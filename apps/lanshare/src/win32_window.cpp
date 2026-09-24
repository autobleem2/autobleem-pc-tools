//
// The LAN Share window - see the header, and docs/lan-share-plan.md ("Remote server").
//
// Left: the server on the network (its address, the upload token, the share its games are on; Connect) and its
// games and problems, read from its /status.json every 10 s. Right: publishing - the games in a folder on this
// PC, ticked and published to the server (through the share when there is one, else uploaded), and Read a disc
// (read into a staging folder, then published); a progress bar and a log; the databases the titles come from.
// Along the bottom: sharing the folder on this PC as well (a LanServer of its own, off by default), start with
// Windows, keep running in the tray, Quit.
//
// Nothing slow runs on the window's thread: the server's status, the folder's scan, the local server's start and
// the job (a disc read, a publish) each have a thread; a half-second timer moves what they left into the controls.
//
#ifdef _WIN32

#include "win32_window.h"

#include "core/lanshare_settings.h"
#include "core/services/disc_reader.h"
#include "core/services/environment.h"
#include "win_cd_drive.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/log.h>
#include <ableem/lanserver/lan_client.h>
#include <ableem/lanserver/publisher.h>

// every control here is a W one: the commctrl macros (ListView_InsertItem, ...) must send the W messages too
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std;

namespace {

const wchar_t *const WindowClass = L"AutoBleemLanShare";
const wchar_t *const Title = L"AutoBleem LAN Share";
const wchar_t *const RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t *const RunValue = L"AutoBleem LAN Share";
const UINT WmRemote = WM_APP + 1;   // the server's status was read
const UINT WmLocal = WM_APP + 2;    // the folder on this PC was scanned
const UINT WmDisc = WM_APP + 3;     // a disc is read (or not)
const UINT WmJob = WM_APP + 4;      // a publish is done
const UINT WmTray = WM_APP + 5;     // the tray icon was clicked
const UINT WmShow = WM_APP + 6;     // a second start asked this one to show itself
const UINT WmLocalServer = WM_APP + 7; // the local server is done starting
const int IdTimer = 1;
const int RemoteEvery = 10; // seconds between two reads of the server's status

enum Id {
    IdServerLabel = 100,
    IdServer,
    IdTokenLabel,
    IdToken,
    IdShareLabel,
    IdShare,
    IdBrowseShare,
    IdConnect,
    IdServerStatus,
    IdCopy,
    IdOpenPage,
    IdGames,
    IdRemove,
    IdProblems,
    IdFolderLabel,
    IdFolder,
    IdChooseFolder,
    IdLocalGames,
    IdTickNew,
    IdPublish,
    IdDriveLabel,
    IdDrive,
    IdSeveral,
    IdRead,
    IdJobBar,
    IdJobStatus,
    IdLog,
    IdCovers,
    IdChooseCovers,
    IdRdb,
    IdChooseRdb,
    IdLocalServer,
    IdPortLabel,
    IdPort,
    IdLocalStatus,
    IdStartup,
    IdTray,
    IdQuit,
    IdGroupServer,
    IdGroupGames,
    IdGroupPublish,
    IdGroupData,
    IdGroupPc,
    IdMenuOpen = 300,
    IdMenuCopy,
    IdMenuQuit,
};

//******************
// strings
//******************
wstring wide(const string &utf8) {
    if (utf8.empty())
        return wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

string narrow(const wstring &text) {
    if (text.empty())
        return string();
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

string sizeText(uint64_t bytes) {
    char text[32];
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0)
        snprintf(text, sizeof(text), "%.1f GB", mb / 1024.0);
    else
        snprintf(text, sizeof(text), "%.0f MB", mb < 1.0 ? 1.0 : mb);
    return text;
}

string lastSegment(const string &path) {
    string p = path;
    replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    const size_t slash = p.find_last_of('/');
    return slash == string::npos ? p : p.substr(slash + 1);
}

// this PC's IPv4 addresses on the network
vector<string> localAddresses() {
    vector<string> out;
    ULONG size = 16 * 1024;
    vector<uint8_t> buffer(size);
    auto *list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR)
        return out;
    for (IP_ADAPTER_ADDRESSES *a = list; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            char text[INET_ADDRSTRLEN] = {};
            auto *in = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
            inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
            const string address = text;
            if (!address.empty() && address.compare(0, 8, "169.254.") != 0)
                out.push_back(address);
        }
    }
    return out;
}

string computerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &size) ? narrow(name) : "";
}

string timeNow() {
    char text[16];
    const time_t now = time(nullptr);
    strftime(text, sizeof(text), "%H:%M:%S", localtime(&now));
    return text;
}

//******************
// Window
//******************
struct Window {
    HWND hwnd = nullptr;
    HFONT font = nullptr, bold = nullptr;
    HICON icon = nullptr;
    NOTIFYICONDATAW tray{};
    int dpi = 96;
    int tick = 0;

    string exePath, settingsDir, settingsFile, stagingDir;
    LanShareSettings settings;

    // the server's status, read on a thread every RemoteEvery seconds
    thread remoteThread;
    atomic<bool> fetching{false};
    mutex remoteMutex;
    ableem::LanClient::Status remote; // the last one read
    bool remoteRead = false;          // there is one at all
    chrono::steady_clock::time_point lastFetch{};

    // the folder on this PC, scanned on a thread
    unique_ptr<ableem::LanLibrary> local;
    thread localThread;
    atomic<bool> scanning{false};
    shared_ptr<const ableem::LanSnapshot> localShown;
    string localFingerprint;

    // sharing the folder on this PC too (off by default)
    unique_ptr<ableem::LanServer> server;
    thread serverThread;
    string serverError;

    // the job: a disc being read, or games being published
    thread job;
    atomic<bool> busy{false}, stopJob{false};
    atomic<uint64_t> jobDone{0}, jobTotal{0};
    mutex jobMutex;
    string jobStatus;
    vector<string> log; // lines the job said, `logShown` of them in the list already
    size_t logShown = 0;
    // a disc read
    DiscReader::Options readOptions;
    DiscReader::Result readResult;
    string readDrive;
    bool readToServer = false;

    HWND item(int id) const { return GetDlgItem(hwnd, id); }
    int px(int v) const { return MulDiv(v, dpi, 96); }
    void say(const string &line) {
        lock_guard<mutex> lock(jobMutex);
        log.push_back(timeNow() + "  " + line);
    }
    void status(const string &text) {
        lock_guard<mutex> lock(jobMutex);
        jobStatus = text;
    }
};

Window *self = nullptr;

void setText(Window &w, int id, const string &text) {
    SetWindowTextW(w.item(id), wide(text).c_str());
}

string getText(Window &w, int id) {
    const int n = GetWindowTextLengthW(w.item(id));
    wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(w.item(id), &text[0], n + 1);
    text.resize(static_cast<size_t>(n));
    string t = narrow(text);
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    while (!t.empty() && t.front() == ' ')
        t.erase(0, 1);
    return t;
}

bool checked(Window &w, int id) {
    return SendMessageW(w.item(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

HWND add(Window &w, const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, w.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
    return h;
}

void addColumn(HWND list, int index, const wchar_t *title, int width) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = const_cast<wchar_t *>(title);
    c.cx = width;
    ListView_InsertColumn(list, index, &c);
}

int addRow(HWND list, const vector<string> &cells, LPARAM data = 0) {
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = ListView_GetItemCount(list);
    item.lParam = data;
    wstring first = wide(cells[0]);
    item.pszText = &first[0];
    const int row = ListView_InsertItem(list, &item);
    for (size_t i = 1; i < cells.size(); i++) {
        wstring cell = wide(cells[i]);
        ListView_SetItemText(list, row, static_cast<int>(i), &cell[0]);
    }
    return row;
}

void addLine(HWND list, const string &line) {
    const LRESULT at = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(line).c_str()));
    SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(at), 0);
}

void save(Window &w) {
    w.settings.save(w.settingsFile);
}

//*******************************
// start with Windows / the clipboard / pickers
//*******************************
bool startsWithWindows() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const bool there = RegQueryValueExW(key, RunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return there;
}

void setStartsWithWindows(Window &w, bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    if (on) {
        const wstring command = L"\"" + wide(w.exePath) + L"\" --tray";
        RegSetValueExW(key, RunValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, RunValue);
    }
    RegCloseKey(key);
}

void copyToClipboard(Window &w, const string &text) {
    if (!OpenClipboard(w.hwnd))
        return;
    EmptyClipboard();
    const wstring t = wide(text);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (mem != nullptr) {
        memcpy(GlobalLock(mem), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

string chooseFolder(Window &w, const wchar_t *title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = w.hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl == nullptr)
        return "";
    wchar_t path[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? narrow(path) : "";
}

//*******************************
// the server
//*******************************
string storeUrl(Window &w) {
    if (!w.settings.serverUrl.empty()) {
        ableem::LanClient client(w.settings.serverUrl);
        if (client.valid())
            return client.baseUrl() + "/store.tsv";
    }
    const vector<string> addresses = localAddresses();
    return "http://" + (addresses.empty() ? string("<this PC>") : addresses.front()) + ":" +
           to_string(w.settings.port) + "/store.tsv";
}

void fetchRemote(Window &w) {
    if (w.fetching || w.settings.serverUrl.empty())
        return;
    if (w.remoteThread.joinable())
        w.remoteThread.join();
    w.fetching = true;
    w.lastFetch = chrono::steady_clock::now();
    const string url = w.settings.serverUrl;
    HWND hwnd = w.hwnd;
    w.remoteThread = thread([&w, url, hwnd] {
        ableem::LanClient client(url);
        ableem::LanClient::Status s = client.status();
        {
            lock_guard<mutex> lock(w.remoteMutex);
            w.remote = s;
            w.remoteRead = true;
        }
        w.fetching = false;
        PostMessageW(hwnd, WmRemote, 0, 0);
    });
}

void fillLocal(Window &w);

void showRemote(Window &w) {
    ableem::LanClient::Status s;
    {
        lock_guard<mutex> lock(w.remoteMutex);
        s = w.remote;
    }
    if (!s.ok) {
        setText(w, IdServerStatus, "Not connected: " + s.error);
        return;
    }
    uint64_t free = 0;
    for (const auto &l : s.libraries)
        free = max(free, l.free);
    string text = s.name + " (abstored " + s.version + ") - " + to_string(s.games.size()) + " games";
    if (!s.problems.empty())
        text += ", " + to_string(s.problems.size()) + " problems";
    text += ", " + sizeText(free) + " free - ";
    if (!w.settings.shareDir.empty())
        text += ableem::DirEntry::isDirectory(w.settings.shareDir) ? "publishes through the share"
                                                                   : "the share cannot be reached";
    else if (s.uploads)
        text += w.settings.token.empty() ? "uploads on: enter the token" : "publishes by upload";
    else
        text += "read only: give its share, or start it with --allow-uploads";
    setText(w, IdServerStatus, text);

    HWND games = w.item(IdGames);
    SendMessageW(games, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(games);
    for (const auto &g : s.games)
        addRow(games, {g.title, g.serial, to_string(g.discs), sizeText(g.size), g.id});
    SendMessageW(games, WM_SETREDRAW, TRUE, 0);
    HWND problems = w.item(IdProblems);
    SendMessageW(problems, LB_RESETCONTENT, 0, 0);
    for (const auto &p : s.problems)
        SendMessageW(problems, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         wide(string(p.error ? "error: " : "warning: ") + (p.path.empty() ? "" : p.path + ": ") + p.what)
                             .c_str()));
    fillLocal(w); // "on the server" follows the server
}

void connect(Window &w) {
    w.settings.serverUrl = getText(w, IdServer);
    w.settings.token = getText(w, IdToken);
    w.settings.shareDir = getText(w, IdShare);
    save(w);
    if (w.settings.serverUrl.empty()) {
        setText(w, IdServerStatus, "Enter the server's address - http://<its address>:<port>, as the Store has it.");
        return;
    }
    if (!ableem::LanClient(w.settings.serverUrl).valid()) {
        setText(w, IdServerStatus, "That is not a server address: http://<its address>:<port>");
        return;
    }
    setText(w, IdServerStatus, "Connecting...");
    fetchRemote(w);
}

//*******************************
// the folder on this PC
//*******************************
void scanLocal(Window &w) {
    if (w.scanning || w.settings.localFolder.empty() || w.busy)
        return;
    if (w.localThread.joinable())
        w.localThread.join();
    w.scanning = true;
    if (!w.local || w.local->config().gamesDir != w.settings.localFolder) {
        ableem::LanLibrary::Config c;
        c.gamesDir = w.settings.localFolder;
        c.coversDir = w.settings.coversDir;
        c.rdbFile = w.settings.rdbFile;
        w.local = make_unique<ableem::LanLibrary>(c);
        w.localShown.reset();
    }
    ableem::LanLibrary *library = w.local.get();
    w.localFingerprint = library->fingerprint(); // what the timer compares with, taken here
    HWND hwnd = w.hwnd;
    w.localThread = thread([&w, library, hwnd] {
        library->scan();
        w.scanning = false;
        PostMessageW(hwnd, WmLocal, 0, 0);
    });
}

void fillLocal(Window &w) {
    if (!w.local)
        return;
    const auto snap = w.local->snapshot();
    ableem::LanClient::Status remote;
    bool haveRemote;
    {
        lock_guard<mutex> lock(w.remoteMutex);
        remote = w.remote;
        haveRemote = w.remoteRead && w.remote.ok;
    }
    HWND list = w.item(IdLocalGames);
    // keep the ticks across a refresh, by game id
    vector<string> ticked;
    if (w.localShown)
        for (int i = 0; i < ListView_GetItemCount(list); i++)
            if (ListView_GetCheckState(list, i)) {
                LVITEMW item{};
                item.mask = LVIF_PARAM;
                item.iItem = i;
                ListView_GetItem(list, &item);
                if (item.lParam >= 0 && static_cast<size_t>(item.lParam) < w.localShown->games.size())
                    ticked.push_back(w.localShown->games[static_cast<size_t>(item.lParam)].id);
            }
    w.localShown = snap;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < snap->games.size(); i++) {
        const ableem::LanGame &g = snap->games[i];
        const string there =
            haveRemote ? (ableem::Publisher::serverHas(remote, g.serial, g.title) ? "yes" : "no") : "";
        const int row = addRow(list, {g.title, g.serial, sizeText(g.size()), there}, static_cast<LPARAM>(i));
        if (find(ticked.begin(), ticked.end(), g.id) != ticked.end())
            ListView_SetCheckState(list, row, TRUE);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    setText(w, IdFolder, w.settings.localFolder + " - " + to_string(snap->games.size()) + " games" +
                             (snap->problems.empty() ? "" : ", " + to_string(snap->problems.size()) + " problems"));
}

void chooseLocalFolder(Window &w) {
    const string folder = chooseFolder(w, L"The folder of games on this PC - one folder per game inside it");
    if (folder.empty())
        return;
    w.settings.localFolder = folder;
    save(w);
    setText(w, IdFolder, folder + " - reading...");
    scanLocal(w);
}

void tickNew(Window &w) {
    if (!w.localShown)
        return;
    ableem::LanClient::Status remote;
    {
        lock_guard<mutex> lock(w.remoteMutex);
        remote = w.remote;
    }
    HWND list = w.item(IdLocalGames);
    for (int i = 0; i < ListView_GetItemCount(list); i++) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        ListView_GetItem(list, &item);
        const ableem::LanGame &g = w.localShown->games[static_cast<size_t>(item.lParam)];
        ListView_SetCheckState(list, i, !ableem::Publisher::serverHas(remote, g.serial, g.title));
    }
}

//*******************************
// the local server (off by default)
//*******************************
void stopLocalServer(Window &w) {
    if (w.serverThread.joinable())
        w.serverThread.join();
    if (w.server)
        w.server->stop();
    w.server.reset();
}

void startLocalServer(Window &w) {
    stopLocalServer(w);
    if (!w.settings.localServer) {
        setText(w, IdLocalStatus, "");
        return;
    }
    if (w.settings.localFolder.empty()) {
        setText(w, IdLocalStatus, "Choose the folder of games on this PC first.");
        return;
    }
    setText(w, IdLocalStatus, "Starting...");
    w.server = make_unique<ableem::LanServer>(
        w.settings.serverConfig(w.settingsDir + "\\state", Env::productVersion(), computerName()));
    ableem::LanServer *server = w.server.get();
    HWND hwnd = w.hwnd;
    w.serverThread = thread([&w, server, hwnd] {
        string error;
        const bool ok = server->start(error);
        w.serverError = ok ? "" : error;
        PostMessageW(hwnd, WmLocalServer, ok ? 1 : 0, 0);
    });
}

//*******************************
// the job: publishing
//*******************************
// what stands between this PC and publishing; "" when nothing does
string cannotPublish(Window &w) {
    if (w.settings.serverUrl.empty())
        return "Connect to the server first (its address, above).";
    if (!w.settings.shareDir.empty()) {
        if (!ableem::DirEntry::isDirectory(w.settings.shareDir))
            return "The share " + w.settings.shareDir + " cannot be reached from this PC.";
        return "";
    }
    lock_guard<mutex> lock(w.remoteMutex);
    if (!w.remoteRead || !w.remote.ok)
        return "The server does not answer - Connect first.";
    if (!w.remote.uploads)
        return "The server is read only: give the share its games folder is on, or start abstored with "
               "--allow-uploads.";
    if (w.settings.token.empty())
        return "The server takes uploads with its token: enter it next to the address.";
    return "";
}

void startJob(Window &w, const string &label, const function<void()> &work, UINT done) {
    if (w.job.joinable())
        w.job.join();
    w.busy = true;
    w.stopJob = false;
    w.jobDone = 0;
    w.jobTotal = 0;
    w.status(label);
    setText(w, IdPublish, "Stop");
    EnableWindow(w.item(IdRead), FALSE);
    EnableWindow(w.item(IdSeveral), FALSE);
    HWND hwnd = w.hwnd;
    w.job = thread([&w, work, done, hwnd] {
        work();
        w.busy = false;
        PostMessageW(hwnd, done, 0, 0);
    });
}

// the files of games, published one game after the other
void publishGames(Window &w, const vector<pair<string, vector<ableem::Publisher::File>>> &games,
                  const function<void(const string &folder, bool ok)> &each = nullptr) {
    const string url = w.settings.serverUrl, token = w.settings.token, share = w.settings.shareDir;
    startJob(
        w, "Publishing...",
        [&w, games, url, token, share, each] {
            ableem::LanClient client(url, token);
            ableem::Publisher::Target t;
            t.client = &client;
            t.shareDir = share;
            int published = 0;
            for (size_t i = 0; i < games.size() && !w.stopJob; i++) {
                const string &folder = games[i].first;
                w.status("Publishing " + folder + " (" + to_string(i + 1) + " of " + to_string(games.size()) + ")");
                const ableem::Publisher::Result r =
                    ableem::Publisher::publish(games[i].second, folder, t, [&w](uint64_t done, uint64_t total) {
                        w.jobDone = done;
                        w.jobTotal = total;
                        return !w.stopJob.load();
                    });
                if (r.ok) {
                    published++;
                    w.say("published " + r.folder + (r.viaShare ? " (to the share)" : " (uploaded)"));
                } else {
                    w.say(folder + ": not published - " + r.error);
                }
                if (each)
                    each(r.ok ? r.folder : folder, r.ok);
            }
            w.status(w.stopJob ? "Stopped - what was sent is kept on the server for the next try."
                               : "Published " + to_string(published) + " of " + to_string(games.size()) + ".");
        },
        WmJob);
}

void publishTicked(Window &w) {
    if (w.busy) {
        w.stopJob = true;
        return;
    }
    const string why = cannotPublish(w);
    if (!why.empty()) {
        MessageBoxW(w.hwnd, wide(why).c_str(), Title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!w.localShown || !w.local)
        return;
    // a game the server has already (by serial, else by title) is not sent again
    ableem::LanClient::Status remote;
    {
        lock_guard<mutex> lock(w.remoteMutex);
        remote = w.remote;
    }
    vector<pair<string, vector<ableem::Publisher::File>>> games;
    int ticked = 0;
    HWND list = w.item(IdLocalGames);
    for (int i = 0; i < ListView_GetItemCount(list); i++) {
        if (!ListView_GetCheckState(list, i))
            continue;
        ticked++;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        ListView_GetItem(list, &item);
        const ableem::LanGame &g = w.localShown->games[static_cast<size_t>(item.lParam)];
        if (remote.ok && ableem::Publisher::serverHas(remote, g.serial, g.title)) {
            w.say(g.title + ": already on the server - skipped");
            continue;
        }
        games.emplace_back(lastSegment(g.id), ableem::Publisher::filesOf(g, *w.local));
    }
    if (ticked == 0) {
        MessageBoxW(w.hwnd, L"Tick the games to publish first (or \"Tick those not on the server\").", Title,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (games.empty()) {
        w.status("Every ticked game is on the server already.");
        return;
    }
    publishGames(w, games);
}

//*******************************
// the job: removing games from the server
//*******************************
void removeSelected(Window &w) {
    if (w.busy)
        return;
    HWND list = w.item(IdGames);
    vector<pair<string, string>> chosen; // id, title
    for (int i = ListView_GetNextItem(list, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list, i, LVNI_SELECTED)) {
        wchar_t title[512] = {}, id[1024] = {};
        ListView_GetItemText(list, i, 0, title, 512);
        ListView_GetItemText(list, i, 4, id, 1024);
        chosen.emplace_back(narrow(id), narrow(title));
    }
    if (chosen.empty()) {
        MessageBoxW(w.hwnd, L"Select the games to remove in \"Games on the server\" first.", Title,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const string why = cannotPublish(w); // what may publish may remove: the share, or uploads with the token
    if (!why.empty()) {
        MessageBoxW(w.hwnd, wide(why).c_str(), Title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    string names;
    for (size_t i = 0; i < chosen.size() && i < 10; i++)
        names += "  " + chosen[i].second + "\n";
    if (chosen.size() > 10)
        names += "  ... and " + to_string(chosen.size() - 10) + " more\n";
    const string ask = "Take these games off the server?\n\n" + names +
                       "\nThey are not deleted: each is moved into the .removed folder next to the server's games,\n"
                       "and moving it back puts it back.";
    if (MessageBoxW(w.hwnd, wide(ask).c_str(), Title, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    const string url = w.settings.serverUrl, token = w.settings.token, share = w.settings.shareDir;
    startJob(
        w, "Removing...",
        [&w, chosen, url, token, share] {
            ableem::LanClient client(url, token);
            ableem::Publisher::Target t;
            t.client = &client;
            t.shareDir = share;
            int removed = 0;
            for (const auto &c : chosen) {
                string error;
                if (ableem::Publisher::remove(c.first, t, error)) {
                    removed++;
                    w.say("removed " + c.second + " from the server (kept in .removed)");
                } else {
                    w.say(c.second + ": not removed - " + error);
                }
            }
            w.status("Removed " + to_string(removed) + " of " + to_string(chosen.size()) + ".");
        },
        WmJob);
}

//*******************************
// the job: reading a disc
//*******************************
void fillDrives(Window &w) {
    HWND combo = w.item(IdDrive);
    const string current = getText(w, IdDrive);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int select = 0, i = 0;
    for (const string &d : WinCdDrive::drives()) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(d).c_str()));
        if (d == current)
            select = i;
        i++;
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
}

void readDisc(Window &w) {
    const string drive = w.readDrive;
    const DiscReader::Options options = w.readOptions;
    startJob(
        w, options.discNumber > 0 ? "Reading disc " + to_string(options.discNumber) + "..." : "Reading the disc...",
        [&w, drive, options] {
            WinCdDrive cd(drive);
            w.readResult = DiscReader::read(cd, options, [&w](uint32_t done, uint32_t total) {
                w.jobDone = done;
                w.jobTotal = total;
                return !w.stopJob.load();
            });
        },
        WmDisc);
}

void readButton(Window &w) {
    if (w.busy) {
        w.stopJob = true;
        return;
    }
    const string drive = getText(w, IdDrive);
    if (drive.empty()) {
        MessageBoxW(w.hwnd, L"This PC has no CD or DVD drive that Windows sees. Connect one, then try again.", Title,
                    MB_OK | MB_ICONINFORMATION);
        fillDrives(w);
        return;
    }
    // to the server when it can take it, else into the folder on this PC
    w.readToServer = cannotPublish(w).empty();
    if (!w.readToServer && w.settings.localFolder.empty()) {
        MessageBoxW(w.hwnd,
                    wide(cannotPublish(w) + "\n\nOr choose a folder on this PC to read the disc into.").c_str(), Title,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    w.readDrive = drive;
    w.readOptions = DiscReader::Options();
    w.readOptions.libraryDir = w.readToServer ? w.stagingDir : w.settings.localFolder;
    w.readOptions.coversDir = w.settings.coversDir;
    w.readOptions.rdbFile = w.settings.rdbFile;
    w.readOptions.discNumber = checked(w, IdSeveral) ? 1 : 0;
    ableem::DirEntry::createDirs(w.stagingDir);
    readDisc(w);
}

const char *verdict(DiscReader::Verified v) {
    switch (v) {
    case DiscReader::Verified::Matches:
        return "matches the known good dump";
    case DiscReader::Verified::Differs:
        return "does NOT match the known good dump - a scratch, or another release";
    default:
        return "no known dump to compare with (choose the rdb for that)";
    }
}

void jobEnded(Window &w) {
    if (w.job.joinable())
        w.job.join();
    setText(w, IdPublish, "Publish the ticked games");
    EnableWindow(w.item(IdRead), TRUE);
    EnableWindow(w.item(IdSeveral), TRUE);
}

void discFinished(Window &w) {
    jobEnded(w);
    const DiscReader::Result &r = w.readResult;
    if (!r.ok) {
        w.status(r.error == "stopped" ? "Stopped - nothing was kept." : "Not read: " + r.error);
        w.say(r.error == "stopped" ? "disc read stopped" : "disc not read: " + r.error);
        return;
    }
    w.say("read " + r.title + (r.serial.empty() ? "" : " (" + r.serial + ")") + " - " + verdict(r.verified));
    if (!r.subchannel)
        w.say("  this drive gives no subchannel: a LibCrypt game will not play from this image");
    else if (r.sbiSectors > 0)
        w.say("  LibCrypt: " + to_string(r.sbiSectors) + " marked sectors, in the .sbi");
    if (!r.badSectors.empty())
        w.say("  " + to_string(r.badSectors.size()) + " sectors could not be read (zeros) - clean the disc, read it again");

    if (w.readOptions.discNumber > 0) {
        const int next = w.readOptions.discNumber + 1;
        const string ask = "Disc " + to_string(w.readOptions.discNumber) + " of " + r.title + " is read.\n\nPut disc " +
                           to_string(next) + " in the drive and press OK - or Cancel when the game has no more discs.";
        if (MessageBoxW(w.hwnd, wide(ask).c_str(), Title, MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            w.readOptions.discNumber = next;
            w.readOptions.gameFolder = r.folder;
            w.readOptions.title = r.title;
            readDisc(w);
            return;
        }
    }
    if (!w.readToServer) {
        w.status("Read into " + r.folder + ".");
        scanLocal(w);
        return;
    }
    // on the server already: not sent again - kept on this PC when there is a folder for it, else dropped
    bool there = false;
    {
        lock_guard<mutex> lock(w.remoteMutex);
        there = w.remote.ok && ableem::Publisher::serverHas(w.remote, r.serial, r.title);
    }
    if (there) {
        const string kept = w.settings.localFolder.empty() ? "" : w.settings.localFolder + "\\" + lastSegment(r.folder);
        if (!kept.empty() && !ableem::DirEntry::exists(kept) && ableem::DirEntry::renameFile(r.folder, kept)) {
            w.say(r.title + " is on the server already - not published; the disc is kept in " + kept);
            scanLocal(w);
        } else {
            ableem::DirEntry::removeDirAndContents(r.folder);
            w.say(r.title + " is on the server already - not published");
        }
        w.status(r.title + " is on the server already.");
        return;
    }
    // the whole game (every disc) from the staging folder to the server; kept on this PC if that fails
    vector<ableem::Publisher::File> files;
    for (const ableem::DirEntry &e : ableem::DirEntry::diru(r.folder))
        if (!e.isDir)
            files.push_back({r.folder + "\\" + e.name, e.name});
    const string folder = lastSegment(r.folder), staged = r.folder, pcFolder = w.settings.localFolder;
    publishGames(w, {{folder, files}}, [&w, staged, pcFolder](const string &, bool ok) {
        if (ok) {
            ableem::DirEntry::removeDirAndContents(staged);
        } else if (!pcFolder.empty()) {
            const string kept = pcFolder + "\\" + lastSegment(staged);
            if (!ableem::DirEntry::exists(kept) && ableem::DirEntry::renameFile(staged, kept))
                w.say("  the disc is kept in " + kept + " - publish it from the list once the server takes it");
        } else {
            w.say("  the disc is kept in " + staged);
        }
    });
}

//*******************************
// refresh (the timer)
//*******************************
void refresh(Window &w) {
    // the job's progress and its log
    {
        lock_guard<mutex> lock(w.jobMutex);
        setText(w, IdJobStatus, w.jobStatus);
        for (size_t i = w.logShown; i < w.log.size(); i++)
            addLine(w.item(IdLog), w.log[i]);
        w.logShown = w.log.size();
    }
    const uint64_t total = w.jobTotal, done = w.jobDone;
    SendMessageW(w.item(IdJobBar), PBM_SETPOS, total > 0 ? static_cast<WPARAM>(1000.0 * done / total) : 0, 0);
    if (w.busy && total > 0) {
        char text[64];
        snprintf(text, sizeof(text), " - %.0f%%", 100.0 * done / total);
        lock_guard<mutex> lock(w.jobMutex);
        setText(w, IdJobStatus, w.jobStatus + text);
    }
    // every so often: the server's status, and whether the folder here changed
    if (++w.tick % 20 == 0) {
        if (!w.settings.serverUrl.empty() &&
            chrono::steady_clock::now() - w.lastFetch > chrono::seconds(RemoteEvery))
            fetchRemote(w);
        if (w.local && !w.scanning && !w.busy && w.local->fingerprint() != w.localFingerprint)
            scanLocal(w);
    }
}

//*******************************
// the tray
//*******************************
void showWindow(Window &w) {
    ShowWindow(w.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(w.hwnd);
}

void trayMenu(Window &w) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IdMenuOpen, L"Open AutoBleem LAN Share");
    AppendMenuW(menu, MF_STRING, IdMenuCopy, L"Copy the Store's address");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdMenuQuit, L"Quit");
    POINT at;
    GetCursorPos(&at);
    SetForegroundWindow(w.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, at.x, at.y, 0, w.hwnd, nullptr);
    DestroyMenu(menu);
}

//*******************************
// the databases
//*******************************
void showDatabases(Window &w) {
    setText(w, IdCovers, w.settings.coversDir.empty() ? "Covers: none chosen" : "Covers: " + w.settings.coversDir);
    setText(w, IdRdb, w.settings.rdbFile.empty() ? "Titles and checks: none chosen"
                                                 : "Titles and checks: " + w.settings.rdbFile);
}

void chooseCovers(Window &w) {
    const string folder = chooseFolder(w, L"The folder with coversU.db, coversP.db and coversJ.db");
    if (folder.empty())
        return;
    w.settings.coversDir = folder;
    save(w);
    showDatabases(w);
    w.local.reset(); // titles come from them: read the folder again
    scanLocal(w);
}

void chooseRdb(Window &w) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = w.hwnd;
    of.lpstrFilter = L"RetroArch database (*.rdb)\0*.rdb\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Sony - PlayStation.rdb";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of))
        return;
    w.settings.rdbFile = narrow(file);
    save(w);
    showDatabases(w);
    w.local.reset();
    scanLocal(w);
}

//*******************************
// layout
//*******************************
void layout(Window &w) {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = w.px(10), g = w.px(8), row = w.px(24), label = w.px(18), groupTop = w.px(20);
    const int bottomRow = H - m - row;
    const int pcTop = bottomRow - g - (groupTop + row + g);
    const int leftW = (W - 3 * m) / 2, rightX = 2 * m + leftW, rightW = W - rightX - m;
    auto place = [&](int id, int x, int y, int cx, int cy) { MoveWindow(w.item(id), x, y, cx, cy, TRUE); };
    const int lw = w.px(55);

    // left: the server
    int y = m;
    const int serverH = groupTop + 3 * (row + g) + label + g + row + g;
    place(IdGroupServer, m, y, leftW, serverH);
    int ix = m + g, iw = leftW - 2 * g, iy = y + groupTop;
    place(IdServerLabel, ix, iy + w.px(4), lw, label);
    place(IdServer, ix + lw, iy, iw - lw - w.px(100) - g, row);
    place(IdConnect, ix + iw - w.px(100), iy, w.px(100), row);
    iy += row + g;
    place(IdTokenLabel, ix, iy + w.px(4), lw, label);
    place(IdToken, ix + lw, iy, w.px(200), row);
    iy += row + g;
    place(IdShareLabel, ix, iy + w.px(4), lw, label);
    place(IdShare, ix + lw, iy, iw - lw - w.px(40) - g, row);
    place(IdBrowseShare, ix + iw - w.px(40), iy, w.px(40), row);
    iy += row + g;
    place(IdServerStatus, ix, iy, iw, label);
    iy += label + g;
    place(IdCopy, ix, iy, w.px(150), row);
    place(IdOpenPage, ix + w.px(150) + g, iy, w.px(150), row);
    y += serverH + g;

    // left: its games and problems
    const int gamesH = pcTop - g - y;
    place(IdGroupGames, m, y, leftW, gamesH);
    const int problemsH = w.px(70);
    place(IdGames, ix, y + groupTop, iw, gamesH - groupTop - problemsH - row - 3 * g);
    place(IdRemove, ix, y + gamesH - 2 * g - problemsH - row, w.px(190), row);
    place(IdProblems, ix, y + gamesH - g - problemsH, iw, problemsH);

    // right: publishing
    y = m;
    ix = rightX + g;
    iw = rightW - 2 * g;
    const int dataH = groupTop + 2 * (row + g);
    const int publishH = pcTop - g - y - dataH - g;
    place(IdGroupPublish, rightX, y, rightW, publishH);
    iy = y + groupTop;
    place(IdFolderLabel, ix, iy + w.px(4), w.px(110), label);
    place(IdFolder, ix + w.px(110), iy + w.px(4), iw - w.px(110) - w.px(90) - g, label);
    place(IdChooseFolder, ix + iw - w.px(90), iy, w.px(90), row);
    iy += row + g;
    const int logH = w.px(90);
    const int listH = publishH - groupTop - 5 * (row + g) - logH - label - g;
    place(IdLocalGames, ix, iy, iw, listH);
    iy += listH + g;
    place(IdTickNew, ix, iy, w.px(190), row);
    place(IdPublish, ix + iw - w.px(190), iy, w.px(190), row);
    iy += row + g;
    place(IdDriveLabel, ix, iy + w.px(4), w.px(40), label);
    place(IdDrive, ix + w.px(42), iy, w.px(60), w.px(200));
    place(IdSeveral, ix + w.px(110), iy, w.px(230), row);
    place(IdRead, ix + iw - w.px(190), iy, w.px(190), row);
    iy += row + g;
    place(IdJobBar, ix, iy + w.px(3), iw, w.px(16));
    iy += row;
    place(IdJobStatus, ix, iy, iw, label);
    iy += label + g;
    place(IdLog, ix, iy, iw, logH);
    y += publishH + g;

    // right: the databases
    place(IdGroupData, rightX, y, rightW, dataH);
    iy = y + groupTop;
    place(IdCovers, ix, iy + w.px(4), iw - w.px(90) - g, label);
    place(IdChooseCovers, ix + iw - w.px(90), iy, w.px(90), row);
    iy += row + g;
    place(IdRdb, ix, iy + w.px(4), iw - w.px(90) - g, label);
    place(IdChooseRdb, ix + iw - w.px(90), iy, w.px(90), row);

    // this PC, across the window
    place(IdGroupPc, m, pcTop, W - 2 * m, groupTop + row + g);
    iy = pcTop + groupTop;
    place(IdLocalServer, m + g, iy, w.px(310), row);
    place(IdPortLabel, m + g + w.px(315), iy + w.px(4), w.px(35), label);
    place(IdPort, m + g + w.px(350), iy, w.px(60), row);
    place(IdLocalStatus, m + g + w.px(420), iy + w.px(4), W - 2 * m - 2 * g - w.px(420), label);

    // the bottom row
    place(IdStartup, m, bottomRow, w.px(190), row);
    place(IdTray, m + w.px(200), bottomRow, w.px(330), row);
    place(IdQuit, W - m - w.px(100), bottomRow, w.px(100), row);
}

//*******************************
// create
//*******************************
void create(Window &w) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    w.font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW boldFont = metrics.lfMessageFont;
    boldFont.lfWeight = FW_BOLD;
    w.bold = CreateFontIndirectW(&boldFont);
    HDC dc = GetDC(w.hwnd);
    w.dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(w.hwnd, dc);

    add(w, L"BUTTON", L"The server on your network", BS_GROUPBOX, IdGroupServer);
    add(w, L"BUTTON", L"Games on the server", BS_GROUPBOX, IdGroupGames);
    add(w, L"BUTTON", L"Publish to the server", BS_GROUPBOX, IdGroupPublish);
    add(w, L"BUTTON", L"Databases", BS_GROUPBOX, IdGroupData);
    add(w, L"BUTTON", L"This PC", BS_GROUPBOX, IdGroupPc);

    add(w, L"STATIC", L"Address:", SS_LEFT, IdServerLabel);
    add(w, L"EDIT", wide(w.settings.serverUrl).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IdServer, WS_EX_CLIENTEDGE);
    SendMessageW(w.item(IdServer), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"http://192.168.1.20:8124"));
    add(w, L"BUTTON", L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, IdConnect);
    add(w, L"STATIC", L"Token:", SS_LEFT, IdTokenLabel);
    add(w, L"EDIT", wide(w.settings.token).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IdToken, WS_EX_CLIENTEDGE);
    SendMessageW(w.item(IdToken), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"for uploads"));
    add(w, L"STATIC", L"Share:", SS_LEFT, IdShareLabel);
    add(w, L"EDIT", wide(w.settings.shareDir).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IdShare, WS_EX_CLIENTEDGE);
    SendMessageW(w.item(IdShare), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"\\\\raspberrypi\\games - optional, instead of uploading"));
    add(w, L"BUTTON", L"...", BS_PUSHBUTTON | WS_TABSTOP, IdBrowseShare);
    add(w, L"STATIC", L"Enter the server's address and press Connect.", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
        IdServerStatus);
    add(w, L"BUTTON", L"Copy the Store's address", BS_PUSHBUTTON | WS_TABSTOP, IdCopy);
    add(w, L"BUTTON", L"The server's status page", BS_PUSHBUTTON | WS_TABSTOP, IdOpenPage);

    HWND games = add(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, IdGames, WS_EX_CLIENTEDGE);
    add(w, L"BUTTON", L"Remove from the server...", BS_PUSHBUTTON | WS_TABSTOP, IdRemove);
    ListView_SetExtendedListViewStyle(games, LVS_EX_FULLROWSELECT);
    addColumn(games, 0, L"Title", w.px(200));
    addColumn(games, 1, L"Serial", w.px(85));
    addColumn(games, 2, L"Discs", w.px(42));
    addColumn(games, 3, L"Size", w.px(65));
    addColumn(games, 4, L"Folder", w.px(160));
    add(w, L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, IdProblems, WS_EX_CLIENTEDGE);

    add(w, L"STATIC", L"Games on this PC:", SS_LEFT, IdFolderLabel);
    add(w, L"STATIC", L"none chosen", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, IdFolder);
    add(w, L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, IdChooseFolder);
    HWND local = add(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, IdLocalGames, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(local, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
    addColumn(local, 0, L"Title", w.px(220));
    addColumn(local, 1, L"Serial", w.px(85));
    addColumn(local, 2, L"Size", w.px(65));
    addColumn(local, 3, L"On the server", w.px(90));
    add(w, L"BUTTON", L"Tick those not on the server", BS_PUSHBUTTON | WS_TABSTOP, IdTickNew);
    add(w, L"BUTTON", L"Publish the ticked games", BS_PUSHBUTTON | WS_TABSTOP, IdPublish);
    add(w, L"STATIC", L"Drive:", SS_LEFT, IdDriveLabel);
    add(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdDrive);
    add(w, L"BUTTON", L"The game has more than one disc", BS_AUTOCHECKBOX | WS_TABSTOP, IdSeveral);
    add(w, L"BUTTON", L"Read a disc and publish it", BS_PUSHBUTTON | WS_TABSTOP, IdRead);
    add(w, PROGRESS_CLASSW, L"", 0, IdJobBar);
    SendMessageW(w.item(IdJobBar), PBM_SETRANGE32, 0, 1000);
    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IdJobStatus);
    add(w, L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, IdLog, WS_EX_CLIENTEDGE);

    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, IdCovers);
    add(w, L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, IdChooseCovers);
    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, IdRdb);
    add(w, L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, IdChooseRdb);

    add(w, L"BUTTON", L"Also share the games on this PC with the Store", BS_AUTOCHECKBOX | WS_TABSTOP, IdLocalServer);
    SendMessageW(w.item(IdLocalServer), BM_SETCHECK, w.settings.localServer ? BST_CHECKED : BST_UNCHECKED, 0);
    add(w, L"STATIC", L"Port:", SS_LEFT, IdPortLabel);
    add(w, L"EDIT", to_wstring(w.settings.port).c_str(), ES_NUMBER | WS_TABSTOP, IdPort, WS_EX_CLIENTEDGE);
    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IdLocalStatus);

    add(w, L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, IdStartup);
    SendMessageW(w.item(IdStartup), BM_SETCHECK, startsWithWindows() ? BST_CHECKED : BST_UNCHECKED, 0);
    add(w, L"BUTTON", L"Closing the window keeps it running (in the tray)", BS_AUTOCHECKBOX | WS_TABSTOP, IdTray);
    SendMessageW(w.item(IdTray), BM_SETCHECK, w.settings.keepInTray ? BST_CHECKED : BST_UNCHECKED, 0);
    add(w, L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, IdQuit);

    showDatabases(w);
    fillDrives(w);
    layout(w);

    w.tray.cbSize = sizeof(w.tray);
    w.tray.hWnd = w.hwnd;
    w.tray.uID = 1;
    w.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    w.tray.uCallbackMessage = WmTray;
    w.tray.hIcon = w.icon;
    wcsncpy(w.tray.szTip, Title, sizeof(w.tray.szTip) / sizeof(wchar_t) - 1);
    Shell_NotifyIconW(NIM_ADD, &w.tray);

    SetTimer(w.hwnd, IdTimer, 500, nullptr);
    if (!w.settings.localFolder.empty()) {
        setText(w, IdFolder, w.settings.localFolder + " - reading...");
        scanLocal(w);
    }
    if (!w.settings.serverUrl.empty())
        connect(w);
    startLocalServer(w);
}

void shutdown(Window &w) {
    KillTimer(w.hwnd, IdTimer);
    w.stopJob = true;
    for (thread *t : {&w.job, &w.remoteThread, &w.localThread})
        if (t->joinable())
            t->join();
    stopLocalServer(w);
    Shell_NotifyIconW(NIM_DELETE, &w.tray);
}

//*******************************
// the window procedure
//*******************************
LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window *w = self;
    if (w == nullptr)
        return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        w->hwnd = hwnd;
        create(*w);
        return 0;
    case WM_SIZE:
        layout(*w);
        return 0;
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lp);
        info->ptMinTrackSize.x = w->px(1000);
        info->ptMinTrackSize.y = w->px(660);
        return 0;
    }
    case WM_TIMER:
        refresh(*w);
        return 0;
    case WmRemote:
        if (w->remoteThread.joinable())
            w->remoteThread.join();
        showRemote(*w);
        return 0;
    case WmLocal:
        if (w->localThread.joinable())
            w->localThread.join();
        fillLocal(*w);
        return 0;
    case WmLocalServer:
        if (w->serverThread.joinable())
            w->serverThread.join();
        if (wp == 0) {
            setText(*w, IdLocalStatus, "Not sharing: " + w->serverError);
            w->server.reset();
        } else {
            const vector<string> addresses = localAddresses();
            setText(*w, IdLocalStatus, "Shared at http://" +
                                           (addresses.empty() ? string("<this PC>") : addresses.front()) + ":" +
                                           to_string(w->settings.port) + "/store.tsv");
        }
        return 0;
    case WmDisc:
        discFinished(*w);
        return 0;
    case WmJob:
        jobEnded(*w);
        fetchRemote(*w);
        return 0;
    case WmShow:
        showWindow(*w);
        return 0;
    case WmTray:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP)
            showWindow(*w);
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            trayMenu(*w);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IdConnect:
            connect(*w);
            return 0;
        case IdBrowseShare: {
            const string folder = chooseFolder(*w, L"The server's games folder, as it is shared on the network");
            if (!folder.empty())
                setText(*w, IdShare, folder);
            return 0;
        }
        case IdCopy:
        case IdMenuCopy:
            copyToClipboard(*w, storeUrl(*w));
            return 0;
        case IdOpenPage: {
            ableem::LanClient client(w->settings.serverUrl);
            if (client.valid())
                ShellExecuteW(hwnd, L"open", wide(client.baseUrl() + "/").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IdChooseFolder:
            chooseLocalFolder(*w);
            return 0;
        case IdTickNew:
            tickNew(*w);
            return 0;
        case IdRemove:
            removeSelected(*w);
            return 0;
        case IdPublish:
            publishTicked(*w);
            return 0;
        case IdRead:
            readButton(*w);
            return 0;
        case IdDrive:
            if (HIWORD(wp) == CBN_DROPDOWN)
                fillDrives(*w);
            return 0;
        case IdChooseCovers:
            chooseCovers(*w);
            return 0;
        case IdChooseRdb:
            chooseRdb(*w);
            return 0;
        case IdLocalServer: {
            const int port = atoi(getText(*w, IdPort).c_str());
            if (port > 0 && port <= 65535)
                w->settings.port = port;
            w->settings.localServer = checked(*w, IdLocalServer);
            save(*w);
            startLocalServer(*w);
            return 0;
        }
        case IdStartup:
            setStartsWithWindows(*w, checked(*w, IdStartup));
            return 0;
        case IdTray:
            w->settings.keepInTray = checked(*w, IdTray);
            save(*w);
            return 0;
        case IdMenuOpen:
            showWindow(*w);
            return 0;
        case IdQuit:
        case IdMenuQuit:
            if (w->busy && MessageBoxW(hwnd, L"A disc is being read or games published. Stop it and quit?", Title,
                                       MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return 0;
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (w->settings.keepInTray) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        shutdown(*w);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

//*******************************
// runLanShareWindow
//*******************************
int runLanShareWindow(bool startInTray, const string &exePath) {
    // one LAN Share at a time: a second start shows the first
    HANDLE single = CreateMutexW(nullptr, TRUE, L"AutoBleem.LanShare.Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(WindowClass, nullptr);
        if (other != nullptr && !startInTray)
            PostMessageW(other, WmShow, 0, 0);
        return 0;
    }

    Window w;
    w.exePath = exePath;
    wchar_t local[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
        w.settingsDir = narrow(local) + "\\AutoBleem LAN Share";
    else
        w.settingsDir = ".";
    w.stagingDir = w.settingsDir + "\\staging";
    ableem::DirEntry::createDirs(w.settingsDir + "\\state");
    w.settingsFile = w.settingsDir + "\\settings.ini";
    w.settings.load(w.settingsFile);
    ableem::Log::addFile(w.settingsDir + "\\lanshare.log");
    PLOG_INFO << "AutoBleem LAN Share " << Env::productVersion() << ", settings in " << w.settingsDir;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    w.icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (w.icon == nullptr)
        w.icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hIcon = w.icon;
    wc.hIconSm = w.icon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = WindowClass;
    RegisterClassExW(&wc);

    self = &w;
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    HWND hwnd = CreateWindowExW(0, WindowClass, Title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(1180, dpi, 96), MulDiv(780, dpi, 96), nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr)
        return 1;
    ShowWindow(hwnd, startInTray ? SW_HIDE : SW_SHOWNORMAL);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    self = nullptr;
    CoUninitialize();
    if (single != nullptr)
        CloseHandle(single);
    return 0;
}

#endif
