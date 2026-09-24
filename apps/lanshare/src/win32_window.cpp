//
// The LAN Share window - see the header.
//
// Left: the sharing (the Store's address, the name, the port), the game folders, the games and the problems the
// scan found. Right: reading a disc into a folder, the databases, the recent requests. Along the bottom: start
// with Windows, keep running in the tray, Quit. The server (core's LanServer) is started on a thread of its own
// - its first scan reads every image's serial - and restarted when the settings change; a disc is read on
// another. A one-second timer moves their state into the controls. Closing the window leaves it in the tray
// when that is ticked; the tray's menu opens it, copies the address or quits.
//
#ifdef _WIN32

#include "win32_window.h"

#include "core/lanshare_settings.h"
#include "core/services/disc_reader.h"
#include "core/services/environment.h"
#include "win_cd_drive.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/log.h>

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

#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std;

namespace {

const wchar_t *const WindowClass = L"AutoBleemLanShare";
const wchar_t *const RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t *const RunValue = L"AutoBleem LAN Share";
const UINT WmServer = WM_APP + 1; // the server thread is done starting
const UINT WmDisc = WM_APP + 2;   // a disc is read (or not)
const UINT WmTray = WM_APP + 3;   // the tray icon was clicked
const UINT WmShow = WM_APP + 4;   // a second start asked this one to show itself
const int IdTimer = 1;

enum Id {
    IdUrl = 100,
    IdStatus,
    IdCopy,
    IdOpenPage,
    IdNameLabel,
    IdName,
    IdPortLabel,
    IdPort,
    IdApply,
    IdLibraries,
    IdAddLibrary,
    IdRemoveLibrary,
    IdGames,
    IdProblems,
    IdDriveLabel,
    IdDrive,
    IdTargetLabel,
    IdTarget,
    IdSeveral,
    IdRead,
    IdReadBar,
    IdReadStatus,
    IdCovers,
    IdChooseCovers,
    IdRdb,
    IdChooseRdb,
    IdActivity,
    IdStartup,
    IdTray,
    IdQuit,
    IdGroupSharing,
    IdGroupFolders,
    IdGroupGames,
    IdGroupDisc,
    IdGroupData,
    IdGroupActivity,
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

// this PC's IPv4 addresses on the network, the ones a console or a Pi would reach it by
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

//******************
// Window
//******************
struct Window {
    HWND hwnd = nullptr;
    HFONT font = nullptr, bold = nullptr;
    HICON icon = nullptr;
    NOTIFYICONDATAW tray{};
    int dpi = 96;

    string exePath, settingsDir, settingsFile;
    LanShareSettings settings;

    // the server, started on a thread of its own
    unique_ptr<ableem::LanServer> server;
    thread starter;
    atomic<bool> starting{false};
    string startError;
    shared_ptr<const ableem::LanSnapshot> shown;
    size_t activityShown = 0;

    // a disc being read
    thread reader;
    atomic<bool> reading{false}, stopReading{false};
    atomic<uint32_t> readDone{0}, readTotal{0};
    DiscReader::Result readResult;
    DiscReader::Options readOptions;
    string readDrive;

    HWND item(int id) const { return GetDlgItem(hwnd, id); }
    int px(int v) const { return MulDiv(v, dpi, 96); }
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
    return narrow(text);
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

void addRow(HWND list, const vector<string> &cells) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(list);
    wstring first = wide(cells[0]);
    item.pszText = &first[0];
    const int row = ListView_InsertItem(list, &item);
    for (size_t i = 1; i < cells.size(); i++) {
        wstring cell = wide(cells[i]);
        ListView_SetItemText(list, row, static_cast<int>(i), &cell[0]);
    }
}

//*******************************
// start with Windows
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

//*******************************
// the Store's address
//*******************************
string storeUrl(const Window &w) {
    const vector<string> addresses = localAddresses();
    return "http://" + (addresses.empty() ? string("<this PC>") : addresses.front()) + ":" +
           to_string(w.settings.port) + "/store.tsv";
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

//*******************************
// the server
//*******************************
void fillLibraries(Window &w);

void stopServer(Window &w) {
    if (w.starter.joinable())
        w.starter.join();
    if (w.server)
        w.server->stop();
    w.server.reset();
    w.shown.reset();
    w.activityShown = 0;
}

void startServer(Window &w) {
    stopServer(w);
    ListView_DeleteAllItems(w.item(IdGames));
    SendMessageW(w.item(IdProblems), LB_RESETCONTENT, 0, 0);
    fillLibraries(w);
    setText(w, IdUrl, storeUrl(w));
    if (w.settings.libraries.empty()) {
        setText(w, IdStatus, "Add a folder of games to share it with the AutoBleem Store.");
        return;
    }
    setText(w, IdStatus, "Starting - reading the game folders...");
    w.starting = true;
    const ableem::LanServer::Config config =
        w.settings.serverConfig(w.settingsDir + "\\state", Env::productVersion(), computerName());
    w.server = make_unique<ableem::LanServer>(config);
    ableem::LanServer *server = w.server.get();
    HWND hwnd = w.hwnd;
    w.starter = thread([&w, server, hwnd] {
        string error;
        const bool ok = server->start(error);
        w.startError = ok ? "" : error;
        w.starting = false;
        PostMessageW(hwnd, WmServer, ok ? 1 : 0, 0);
    });
}

// the games and the problems, when a scan gave a new snapshot; the requests since the last tick
void refreshServer(Window &w) {
    if (!w.server || !w.server->running())
        return;
    const auto snap = w.server->library().snapshot();
    if (snap != w.shown) {
        w.shown = snap;
        HWND games = w.item(IdGames);
        SendMessageW(games, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(games);
        for (const ableem::LanGame &g : snap->games) {
            int discs = 0;
            for (const ableem::LanFile &f : g.files)
                discs += f.disc > 0 ? 1 : 0;
            addRow(games, {g.title, g.serial, to_string(discs), sizeText(g.size()), g.id});
        }
        SendMessageW(games, WM_SETREDRAW, TRUE, 0);
        HWND problems = w.item(IdProblems);
        SendMessageW(problems, LB_RESETCONTENT, 0, 0);
        int errors = 0;
        for (const ableem::LanProblem &p : snap->problems) {
            errors += p.error ? 1 : 0;
            const string line = string(p.error ? "error: " : "warning: ") + (p.path.empty() ? "" : p.path + ": ") + p.what;
            SendMessageW(problems, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(line).c_str()));
        }
        fillLibraries(w);
        setText(w, IdStatus,
                "Sharing " + to_string(snap->games.size()) + " games from " +
                    to_string(w.settings.libraries.size()) + (w.settings.libraries.size() == 1 ? " folder" : " folders") +
                    (snap->problems.empty() ? "" : " - " + to_string(snap->problems.size()) + " problems below") +
                    (w.server->hashing() ? " - checksums still being worked out" : ""));
    }
    const auto activity = w.server->activity();
    if (activity.size() < w.activityShown)
        w.activityShown = 0;
    HWND list = w.item(IdActivity);
    for (size_t i = w.activityShown; i < activity.size(); i++) {
        char when[16];
        const tm *t = localtime(&activity[i].when);
        strftime(when, sizeof(when), "%H:%M:%S", t);
        const string line = string(when) + "  " + activity[i].peer + "  " + activity[i].what;
        const LRESULT at = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(line).c_str()));
        SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(at), 0);
    }
    w.activityShown = activity.size();
}

//*******************************
// the folders
//*******************************
void fillLibraries(Window &w) {
    HWND list = w.item(IdLibraries);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(list);
    HWND target = w.item(IdTarget);
    const LRESULT targetIndex = SendMessageW(target, CB_GETCURSEL, 0, 0);
    SendMessageW(target, CB_RESETCONTENT, 0, 0);
    for (const ableem::LanLibrary::Root &r : w.settings.libraries) {
        int games = 0;
        if (w.shown)
            for (const ableem::LanGame &g : w.shown->games)
                games += w.settings.libraries.size() == 1 || g.id.compare(0, r.name.size() + 1, r.name + "/") == 0;
        addRow(list, {r.name, r.dir, w.shown ? to_string(games) : ""});
        SendMessageW(target, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(r.name).c_str()));
    }
    if (selected >= 0 && selected < ListView_GetItemCount(list))
        ListView_SetItemState(list, selected, LVIS_SELECTED, LVIS_SELECTED);
    const LRESULT count = SendMessageW(target, CB_GETCOUNT, 0, 0);
    SendMessageW(target, CB_SETCURSEL, targetIndex >= 0 && targetIndex < count ? targetIndex : 0, 0);
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

void addLibrary(Window &w) {
    const string folder = chooseFolder(w, L"The folder of games to share - one folder per game inside it");
    if (folder.empty())
        return;
    string why;
    if (!w.settings.canAdd(folder, why)) {
        MessageBoxW(w.hwnd, wide("This folder cannot be added: " + why + ".").c_str(), L"AutoBleem LAN Share",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    w.settings.libraries.push_back({w.settings.nameFor(folder), folder});
    w.settings.save(w.settingsFile);
    startServer(w);
}

void removeLibrary(Window &w) {
    const int row = ListView_GetNextItem(w.item(IdLibraries), -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(w.settings.libraries.size()))
        return;
    const string name = w.settings.libraries[static_cast<size_t>(row)].name;
    if (MessageBoxW(w.hwnd,
                    wide("Stop sharing \"" + name + "\"? The folder and its games are not touched.").c_str(),
                    L"AutoBleem LAN Share", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    w.settings.libraries.erase(w.settings.libraries.begin() + row);
    w.settings.save(w.settingsFile);
    startServer(w);
}

void applyNameAndPort(Window &w) {
    const int port = atoi(getText(w, IdPort).c_str());
    if (port <= 0 || port > 65535) {
        MessageBoxW(w.hwnd, L"The port is a number from 1 to 65535 (8124 unless something else uses it).",
                    L"AutoBleem LAN Share", MB_OK | MB_ICONINFORMATION);
        return;
    }
    w.settings.port = port;
    w.settings.name = getText(w, IdName);
    w.settings.save(w.settingsFile);
    startServer(w);
}

//*******************************
// the databases
//*******************************
void showDatabases(Window &w) {
    setText(w, IdCovers, w.settings.coversDir.empty() ? "Covers: none chosen" : "Covers: " + w.settings.coversDir);
    setText(w, IdRdb, w.settings.rdbFile.empty() ? "Titles and checks: none chosen" : "Titles and checks: " + w.settings.rdbFile);
}

void chooseCovers(Window &w) {
    const string folder = chooseFolder(w, L"The folder with coversU.db, coversP.db and coversJ.db");
    if (folder.empty())
        return;
    w.settings.coversDir = folder;
    w.settings.save(w.settingsFile);
    showDatabases(w);
    startServer(w);
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
    w.settings.save(w.settingsFile);
    showDatabases(w);
    startServer(w);
}

//*******************************
// reading a disc
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

void startReading(Window &w) {
    w.stopReading = false;
    w.readDone = 0;
    w.readTotal = 0;
    w.reading = true;
    setText(w, IdRead, "Stop");
    SendMessageW(w.item(IdReadBar), PBM_SETPOS, 0, 0);
    setText(w, IdReadStatus,
            w.readOptions.discNumber > 0 ? "Reading disc " + to_string(w.readOptions.discNumber) + "..." : "Reading the disc...");
    HWND hwnd = w.hwnd;
    const string drive = w.readDrive;
    const DiscReader::Options options = w.readOptions;
    w.reader = thread([&w, hwnd, drive, options] {
        WinCdDrive cd(drive);
        w.readResult = DiscReader::read(cd, options, [&w](uint32_t done, uint32_t total) {
            w.readDone = done;
            w.readTotal = total;
            return !w.stopReading.load();
        });
        w.reading = false;
        PostMessageW(hwnd, WmDisc, 0, 0);
    });
}

void readButton(Window &w) {
    if (w.reading) {
        w.stopReading = true;
        return;
    }
    const string drive = getText(w, IdDrive);
    if (drive.empty()) {
        MessageBoxW(w.hwnd, L"This PC has no CD or DVD drive that Windows sees. Connect one, then try again.",
                    L"AutoBleem LAN Share", MB_OK | MB_ICONINFORMATION);
        fillDrives(w);
        return;
    }
    const LRESULT target = SendMessageW(w.item(IdTarget), CB_GETCURSEL, 0, 0);
    if (w.settings.libraries.empty() || target < 0) {
        MessageBoxW(w.hwnd, L"Add a game folder first: the disc is read into one.", L"AutoBleem LAN Share",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (w.reader.joinable())
        w.reader.join();
    w.readDrive = drive;
    w.readOptions = DiscReader::Options();
    w.readOptions.libraryDir = w.settings.libraries[static_cast<size_t>(target)].dir;
    w.readOptions.coversDir = w.settings.coversDir;
    w.readOptions.rdbFile = w.settings.rdbFile;
    w.readOptions.discNumber = SendMessageW(w.item(IdSeveral), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    EnableWindow(w.item(IdSeveral), FALSE);
    startReading(w);
}

const char *verdict(DiscReader::Verified v) {
    switch (v) {
    case DiscReader::Verified::Matches:
        return "matches the known good dump";
    case DiscReader::Verified::Differs:
        return "does NOT match the known good dump - a scratch, or another release";
    default:
        return "no known dump to compare with (choose the rdb below for that)";
    }
}

void readFinished(Window &w) {
    if (w.reader.joinable())
        w.reader.join();
    setText(w, IdRead, "Read a disc");
    const DiscReader::Result &r = w.readResult;
    if (!r.ok) {
        SendMessageW(w.item(IdReadBar), PBM_SETPOS, 0, 0);
        setText(w, IdReadStatus, r.error == "stopped" ? "Stopped - nothing was kept." : "Not read: " + r.error);
        EnableWindow(w.item(IdSeveral), TRUE);
        return;
    }
    SendMessageW(w.item(IdReadBar), PBM_SETPOS, 1000, 0);
    string text = r.title + (r.serial.empty() ? "" : " (" + r.serial + ")") + "\r\n" + r.folder + "\r\n" +
                  verdict(r.verified) + "\r\n";
    if (!r.subchannel)
        text += "This drive does not give the subchannel: a LibCrypt game will not play from this image.\r\n";
    else if (r.subchannelUnreliable)
        text += "The drive's subchannel was noise: no .sbi written.\r\n";
    else if (r.sbiSectors > 0)
        text += "LibCrypt: " + to_string(r.sbiSectors) + " marked sectors, in the .sbi.\r\n";
    if (!r.badSectors.empty())
        text += to_string(r.badSectors.size()) + " sectors could not be read (zeros) - clean the disc and read it again.";
    setText(w, IdReadStatus, text);
    if (w.server)
        w.server->rescan();

    if (w.readOptions.discNumber > 0) {
        const int next = w.readOptions.discNumber + 1;
        const string ask = "Disc " + to_string(w.readOptions.discNumber) + " of " + r.title +
                           " is read.\n\nPut disc " + to_string(next) +
                           " in the drive and press OK - or Cancel when the game has no more discs.";
        if (MessageBoxW(w.hwnd, wide(ask).c_str(), L"AutoBleem LAN Share", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            w.readOptions.discNumber = next;
            w.readOptions.gameFolder = r.folder;
            w.readOptions.title = r.title;
            startReading(w);
            return;
        }
    }
    EnableWindow(w.item(IdSeveral), TRUE);
}

void refreshReading(Window &w) {
    if (!w.reading)
        return;
    const uint32_t total = w.readTotal, done = w.readDone;
    if (total > 0) {
        SendMessageW(w.item(IdReadBar), PBM_SETPOS, static_cast<WPARAM>(1000.0 * done / total), 0);
        char text[96];
        snprintf(text, sizeof(text), "Reading %s%.0f%% (%u of %u sectors)",
                 w.readOptions.discNumber > 0 ? ("disc " + to_string(w.readOptions.discNumber) + ": ").c_str() : "",
                 100.0 * done / total, done, total);
        setText(w, IdReadStatus, text);
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
    AppendMenuW(menu, MF_STRING, IdMenuQuit, L"Quit (stop sharing)");
    POINT at;
    GetCursorPos(&at);
    SetForegroundWindow(w.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, at.x, at.y, 0, w.hwnd, nullptr);
    DestroyMenu(menu);
}

//*******************************
// layout
//*******************************
void layout(Window &w) {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = w.px(10), g = w.px(8), row = w.px(24), label = w.px(18), groupTop = w.px(20);
    const int bottom = H - m - row;
    const int leftW = (W - 3 * m) * 58 / 100, rightX = 2 * m + leftW, rightW = W - rightX - m;
    auto place = [&](int id, int x, int y, int cx, int cy) { MoveWindow(w.item(id), x, y, cx, cy, TRUE); };

    // left: sharing
    int y = m;
    const int sharingH = groupTop + label + g + label + g + row + g;
    place(IdGroupSharing, m, y, leftW, sharingH);
    int iy = y + groupTop, ix = m + g, iw = leftW - 2 * g;
    const int buttonW = w.px(110);
    place(IdUrl, ix, iy, iw - 2 * buttonW - 2 * g, label + w.px(2));
    place(IdCopy, ix + iw - 2 * buttonW - g, iy - w.px(3), buttonW, row);
    place(IdOpenPage, ix + iw - buttonW, iy - w.px(3), buttonW, row);
    iy += label + g;
    place(IdStatus, ix, iy, iw, label);
    iy += label + g;
    place(IdNameLabel, ix, iy + w.px(4), w.px(45), label);
    place(IdName, ix + w.px(48), iy, w.px(200), row);
    place(IdPortLabel, ix + w.px(260), iy + w.px(4), w.px(35), label);
    place(IdPort, ix + w.px(298), iy, w.px(70), row);
    place(IdApply, ix + w.px(380), iy, w.px(80), row);
    y += sharingH + g;

    // left: the folders
    const int foldersH = groupTop + w.px(110) + g + row + g;
    place(IdGroupFolders, m, y, leftW, foldersH);
    iy = y + groupTop;
    place(IdLibraries, ix, iy, iw, w.px(110));
    iy += w.px(110) + g;
    place(IdAddLibrary, ix, iy, w.px(150), row);
    place(IdRemoveLibrary, ix + w.px(150) + g, iy, w.px(110), row);
    y += foldersH + g;

    // left: the games, then the problems
    const int gamesH = bottom - g - y;
    place(IdGroupGames, m, y, leftW, gamesH);
    iy = y + groupTop;
    const int problemsH = w.px(80);
    place(IdGames, ix, iy, iw, gamesH - groupTop - problemsH - 2 * g);
    place(IdProblems, ix, y + gamesH - g - problemsH, iw, problemsH);

    // right: reading a disc
    y = m;
    ix = rightX + g;
    iw = rightW - 2 * g;
    const int discH = groupTop + 2 * (row + g) + row + g + row + g + w.px(18) + g + w.px(80) + g;
    place(IdGroupDisc, rightX, y, rightW, discH);
    iy = y + groupTop;
    place(IdDriveLabel, ix, iy + w.px(4), w.px(70), label);
    place(IdDrive, ix + w.px(75), iy, w.px(80), w.px(200));
    iy += row + g;
    place(IdTargetLabel, ix, iy + w.px(4), w.px(70), label);
    place(IdTarget, ix + w.px(75), iy, iw - w.px(75), w.px(200));
    iy += row + g;
    place(IdSeveral, ix, iy, iw, row);
    iy += row + g;
    place(IdRead, ix, iy, w.px(130), row);
    iy += row + g;
    place(IdReadBar, ix, iy, iw, w.px(18));
    iy += w.px(18) + g;
    place(IdReadStatus, ix, iy, iw, w.px(80));
    y += discH + g;

    // right: the databases
    const int dataH = groupTop + 2 * (row + g);
    place(IdGroupData, rightX, y, rightW, dataH);
    iy = y + groupTop;
    place(IdCovers, ix, iy + w.px(4), iw - w.px(90) - g, label);
    place(IdChooseCovers, ix + iw - w.px(90), iy, w.px(90), row);
    iy += row + g;
    place(IdRdb, ix, iy + w.px(4), iw - w.px(90) - g, label);
    place(IdChooseRdb, ix + iw - w.px(90), iy, w.px(90), row);
    y += dataH + g;

    // right: the requests
    place(IdGroupActivity, rightX, y, rightW, bottom - g - y);
    place(IdActivity, ix, y + groupTop, iw, bottom - g - y - groupTop - g);

    // the bottom row
    place(IdStartup, m, bottom, w.px(200), row);
    place(IdTray, m + w.px(210), bottom, w.px(330), row);
    place(IdQuit, W - m - w.px(100), bottom, w.px(100), row);
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

    const DWORD group = BS_GROUPBOX;
    add(w, L"BUTTON", L"Sharing with the AutoBleem Store", group, IdGroupSharing);
    add(w, L"BUTTON", L"Game folders", group, IdGroupFolders);
    add(w, L"BUTTON", L"Games shared", group, IdGroupGames);
    add(w, L"BUTTON", L"Read a disc into a game folder", group, IdGroupDisc);
    add(w, L"BUTTON", L"Databases", group, IdGroupData);
    add(w, L"BUTTON", L"Recent requests", group, IdGroupActivity);

    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdUrl);
    SendMessageW(w.item(IdUrl), WM_SETFONT, reinterpret_cast<WPARAM>(w.bold), TRUE);
    add(w, L"BUTTON", L"Copy address", BS_PUSHBUTTON, IdCopy);
    add(w, L"BUTTON", L"Status page", BS_PUSHBUTTON, IdOpenPage);
    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IdStatus);
    add(w, L"STATIC", L"Name:", SS_LEFT, IdNameLabel);
    add(w, L"EDIT", wide(w.settings.name).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IdName, WS_EX_CLIENTEDGE);
    SendMessageW(w.item(IdName), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(wide(computerName()).c_str()));
    add(w, L"STATIC", L"Port:", SS_LEFT, IdPortLabel);
    add(w, L"EDIT", to_wstring(w.settings.port).c_str(), ES_NUMBER | WS_TABSTOP, IdPort, WS_EX_CLIENTEDGE);
    add(w, L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, IdApply);

    HWND libraries =
        add(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IdLibraries, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(libraries, LVS_EX_FULLROWSELECT);
    addColumn(libraries, 0, L"Name in the Store", w.px(150));
    addColumn(libraries, 1, L"Folder", w.px(300));
    addColumn(libraries, 2, L"Games", w.px(60));
    add(w, L"BUTTON", L"Add a folder...", BS_PUSHBUTTON | WS_TABSTOP, IdAddLibrary);
    add(w, L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, IdRemoveLibrary);

    HWND games = add(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, IdGames, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(games, LVS_EX_FULLROWSELECT);
    addColumn(games, 0, L"Title", w.px(230));
    addColumn(games, 1, L"Serial", w.px(90));
    addColumn(games, 2, L"Discs", w.px(45));
    addColumn(games, 3, L"Size", w.px(70));
    addColumn(games, 4, L"Folder", w.px(200));
    add(w, L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, IdProblems, WS_EX_CLIENTEDGE);

    add(w, L"STATIC", L"Drive:", SS_LEFT, IdDriveLabel);
    add(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdDrive);
    add(w, L"STATIC", L"Into:", SS_LEFT, IdTargetLabel);
    add(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdTarget);
    add(w, L"BUTTON", L"The game has more than one disc", BS_AUTOCHECKBOX | WS_TABSTOP, IdSeveral);
    add(w, L"BUTTON", L"Read a disc", BS_PUSHBUTTON | WS_TABSTOP, IdRead);
    add(w, PROGRESS_CLASSW, L"", 0, IdReadBar);
    SendMessageW(w.item(IdReadBar), PBM_SETRANGE32, 0, 1000);
    add(w, L"STATIC", L"A PlayStation disc is read whole into the folder chosen above, named after its title.",
        SS_LEFT | SS_NOPREFIX, IdReadStatus);

    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, IdCovers);
    add(w, L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, IdChooseCovers);
    add(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, IdRdb);
    add(w, L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, IdChooseRdb);

    add(w, L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, IdActivity, WS_EX_CLIENTEDGE);

    add(w, L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, IdStartup);
    SendMessageW(w.item(IdStartup), BM_SETCHECK, startsWithWindows() ? BST_CHECKED : BST_UNCHECKED, 0);
    add(w, L"BUTTON", L"Closing the window keeps sharing (in the tray)", BS_AUTOCHECKBOX | WS_TABSTOP, IdTray);
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
    wcsncpy(w.tray.szTip, L"AutoBleem LAN Share", sizeof(w.tray.szTip) / sizeof(wchar_t) - 1);
    Shell_NotifyIconW(NIM_ADD, &w.tray);

    SetTimer(w.hwnd, IdTimer, 1000, nullptr);
    startServer(w);
}

void shutdown(Window &w) {
    KillTimer(w.hwnd, IdTimer);
    w.stopReading = true;
    if (w.reader.joinable())
        w.reader.join();
    stopServer(w);
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
        info->ptMinTrackSize.x = w->px(900);
        info->ptMinTrackSize.y = w->px(640);
        return 0;
    }
    case WM_TIMER:
        refreshServer(*w);
        refreshReading(*w);
        return 0;
    case WmServer:
        if (w->starter.joinable())
            w->starter.join();
        if (wp == 0) {
            setText(*w, IdStatus, "Not sharing: " + w->startError + " - choose another port and press Apply.");
            w->server.reset();
        } else {
            refreshServer(*w);
        }
        return 0;
    case WmDisc:
        readFinished(*w);
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
        case IdCopy:
        case IdMenuCopy:
            copyToClipboard(*w, storeUrl(*w));
            return 0;
        case IdOpenPage:
            ShellExecuteW(hwnd, L"open", wide("http://127.0.0.1:" + to_string(w->settings.port) + "/").c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IdApply:
            applyNameAndPort(*w);
            return 0;
        case IdAddLibrary:
            addLibrary(*w);
            return 0;
        case IdRemoveLibrary:
            removeLibrary(*w);
            return 0;
        case IdChooseCovers:
            chooseCovers(*w);
            return 0;
        case IdChooseRdb:
            chooseRdb(*w);
            return 0;
        case IdRead:
            readButton(*w);
            return 0;
        case IdDrive:
            if (HIWORD(wp) == CBN_DROPDOWN)
                fillDrives(*w);
            return 0;
        case IdStartup:
            setStartsWithWindows(*w, SendMessageW(w->item(IdStartup), BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IdTray:
            w->settings.keepInTray = SendMessageW(w->item(IdTray), BM_GETCHECK, 0, 0) == BST_CHECKED;
            w->settings.save(w->settingsFile);
            return 0;
        case IdMenuOpen:
            showWindow(*w);
            return 0;
        case IdQuit:
        case IdMenuQuit:
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
    HWND hwnd = CreateWindowExW(0, WindowClass, L"AutoBleem LAN Share", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, MulDiv(1060, dpi, 96), MulDiv(740, dpi, 96), nullptr, nullptr,
                                instance, nullptr);
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
