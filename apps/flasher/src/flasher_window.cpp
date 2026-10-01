//
// The flasher window - see the header. Two pages under the picture: the questions and the progress. The
// site's channels (channels.json) are looked up once in the background when the window opens; the write runs on a
// std::thread reporting into a mutex-guarded State, which a 100 ms timer moves into the controls.
//
#ifdef _WIN32

#include "flasher_window.h"
#include "win32_disk.h"
#include "ui_theme.h"
#include "channel_choice.h"

#include "../../installer/src/win32_platform.h"
#include "core/services/environment.h"
#include "core/version.h"
#include "installer/install_job_base.h" // humanSize

#include <ableem/engine/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std;

namespace {

const wchar_t *const WindowClass = L"AutoBleemFlasher";
const int IdTimer = 1;

enum Ids {
    IdChannel = 100,
    IdDisks,
    IdRefresh,
    IdVerify,
    IdFixed,
    IdWrite,
    IdAction, // Stop / Close / Back on the progress page
};
const int Width = 640;
const int HeroHeight = uitheme::HeroHeight; // the 640x150 picture and the 1 px cyan line under it
const int Margin = 14;
// the dropdown lists the site's channels, then this
const wchar_t *const ImageFileLabel = L"An image file on this PC...";
const UINT WmChannelsLooked = WM_APP + 1;

// the folder a channel's image is downloaded to and kept in, for the next stick
string scratchDirectory() {
    char path[MAX_PATH] = {0};
    DWORD n = GetTempPathA(MAX_PATH, path);
    string dir = n > 0 && n < MAX_PATH ? string(path, n) : string("./");
    for (char &c : dir)
        if (c == '\\')
            c = '/';
    return dir + "AutoBleemFlasher";
}

//******************
// State
//******************
struct State {
    mutex m;
    string phase;
    int phaseIndex = 0, phaseTotal = 0;
    uint64_t done = 0, total = 0;
    vector<string> lines;
    size_t shown = 0;
    atomic<bool> stop{false};
    atomic<bool> finished{false};
    bool ok = false;

    void reset() {
        lock_guard<mutex> lock(m);
        phase.clear();
        phaseIndex = phaseTotal = 0;
        done = total = 0;
        lines.clear();
        shown = 0;
        stop.store(false);
        finished.store(false);
        ok = false;
    }
};

class Listener : public InstallListener {
public:
    explicit Listener(State &state) : state_(state) {}
    void onPhase(int index, int total, const string &title) override {
        lock_guard<mutex> lock(state_.m);
        state_.phase = title;
        state_.phaseIndex = index;
        state_.phaseTotal = total;
        state_.done = state_.total = 0;
    }
    void onProgress(uint64_t done, uint64_t total) override {
        lock_guard<mutex> lock(state_.m);
        state_.done = done;
        state_.total = total;
    }
    void onLine(const string &line) override {
        lock_guard<mutex> lock(state_.m);
        state_.lines.push_back(line);
    }

private:
    State &state_;
};

//******************
// Window
//******************
struct Window {
    HWND hwnd = nullptr;
    HWND channelLabel = nullptr, channel = nullptr, disksLabel = nullptr, disks = nullptr, refresh = nullptr;
    HWND fixed = nullptr, status = nullptr, verify = nullptr, write = nullptr;
    HWND phaseLabel = nullptr, phaseBar = nullptr, bar = nullptr, log = nullptr, action = nullptr;
    HFONT font = nullptr, bold = nullptr;
    Gdiplus::Image *hero = nullptr;
    ULONG_PTR gdiplusToken = 0;
    FlashOptions defaults;
    string imageFile; // the dropdown's fourth choice
    vector<PhysicalDisk> diskList;
    mutex channelsMutex;
    ableem::ChannelCatalog catalog = ableem::ChannelCatalog::builtIn(); // the site's channels.json once read
    vector<ChannelImage> channels = vector<ChannelImage>(3);            // what each offers, in the catalog's order
    vector<string> channelErrors = vector<string>(3);
    bool channelPicked = false; // the user chose in the box: the lookup does not move the selection
    atomic<bool> looked{false};
    thread lookup;
    State state;
    thread worker;
    bool progressPage = false;
};

wstring wide(const string &utf8) {
    if (utf8.empty())
        return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

string narrow(const wstring &text) {
    if (text.empty())
        return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

HWND make(Window &w, const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 0, 0, w.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
    return h;
}

bool isCheckbox(int id) {
    return id >= IdVerify && id <= IdFixed; // the BS_AUTOCHECKBOX buttons, in the enum's order
}

int selectedChannel(Window &w) {
    int i = static_cast<int>(SendMessage(w.channel, CB_GETCURSEL, 0, 0));
    return i < 0 ? 0 : i;
}

// the box's last choice - the image file - sits after the site's channels
bool isImageChoice(Window &w, int choice) {
    lock_guard<mutex> lock(w.channelsMutex);
    return static_cast<size_t>(choice) >= w.catalog.channels.size();
}

string channelId(Window &w, int choice) {
    lock_guard<mutex> lock(w.channelsMutex);
    return static_cast<size_t>(choice) < w.catalog.channels.size() ? w.catalog.channels[static_cast<size_t>(choice)].id
                                                                   : w.defaults.channel;
}

// the box from the catalog, then the image file; the selection on `wanted` (else the first channel), or on the
// image file when `imageFile` says so
void fillChannels(Window &w, const string &wanted, bool imageFile) {
    lock_guard<mutex> lock(w.channelsMutex);
    SendMessage(w.channel, CB_RESETCONTENT, 0, 0);
    for (const ableem::ChannelEntry &e : w.catalog.channels)
        SendMessageW(w.channel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(channelchoice::label(e)).c_str()));
    SendMessageW(w.channel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ImageFileLabel));
    SendMessage(w.channel, CB_SETCURSEL,
                imageFile ? static_cast<int>(w.catalog.channels.size())
                          : static_cast<int>(channelchoice::initialIndex(w.catalog, wanted)),
                0);
}

const PhysicalDisk *selectedDisk(Window &w) {
    int i = static_cast<int>(SendMessage(w.disks, CB_GETCURSEL, 0, 0));
    if (i < 0 || i >= static_cast<int>(w.diskList.size()))
        return nullptr;
    return &w.diskList[static_cast<size_t>(i)];
}

void showPage(Window &w, bool progress) {
    w.progressPage = progress;
    for (HWND h : {w.channelLabel, w.channel, w.disksLabel, w.disks, w.refresh, w.fixed, w.status, w.verify, w.write})
        ShowWindow(h, progress ? SW_HIDE : SW_SHOW);
    for (HWND h : {w.phaseLabel, w.phaseBar, w.bar, w.log, w.action})
        ShowWindow(h, progress ? SW_SHOW : SW_HIDE);
}

void layout(Window &w);

// what would be written, onto what - under the two boxes
void describe(Window &w) {
    const int ch = selectedChannel(w);
    const PhysicalDisk *disk = selectedDisk(w);
    string what;
    bool canWrite = true;
    if (isImageChoice(w, ch)) {
        what = w.imageFile.empty() ? "No image file chosen." : "The image " + w.imageFile + ".";
        canWrite = !w.imageFile.empty();
    } else if (!w.looked.load()) {
        what = "Asking the download site what each channel offers...";
        canWrite = false;
    } else {
        lock_guard<mutex> lock(w.channelsMutex);
        const string id = static_cast<size_t>(ch) < w.catalog.channels.size()
                              ? w.catalog.channels[static_cast<size_t>(ch)].id
                              : string();
        const ChannelImage ci =
            static_cast<size_t>(ch) < w.channels.size() ? w.channels[static_cast<size_t>(ch)] : ChannelImage();
        if (ci.version.empty()) {
            what = "The " + id + " channel has no stick image: " +
                   (static_cast<size_t>(ch) < w.channelErrors.size() ? w.channelErrors[static_cast<size_t>(ch)]
                                                                     : string()) +
                   ". Pick another channel, or check the internet connection.";
            canWrite = false;
        } else {
            what = "AutoBleem " + ci.version + " from the " + id + " channel - a " + humanSize(ci.image.size) +
                   " download.";
        }
    }
    if (!disk) {
        what += "\nNo USB stick found. Plug one in (8 GB or more) and press Refresh.";
        canWrite = false;
    } else {
        what += "\nEverything on " + (disk->model.empty() ? "disk " + to_string(disk->number) : disk->model) + " (" +
                humanSize(disk->sizeBytes) + ") will be erased.";
        if (disk->sizeBytes < 7000000000ull)
            what += " A stick of 8 GB or more is needed.";
    }
    SetWindowTextW(w.status, wide(what).c_str());
    EnableWindow(w.write, canWrite);
    layout(w); // the window grows with the status text
}

void refreshDisks(Window &w) {
    const PhysicalDisk *current = selectedDisk(w);
    const int keep = current ? current->number : -1;
    vector<string> notes;
    w.diskList = listTargetDisks(&notes, SendMessage(w.fixed, BM_GETCHECK, 0, 0) == BST_CHECKED);
    for (const string &note : notes)
        PLOG_INFO << "not offered: " << note;
    SendMessage(w.disks, CB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < w.diskList.size(); i++) {
        SendMessageW(w.disks, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(w.diskList[i].describe()).c_str()));
        if (w.diskList[i].number == keep)
            select = static_cast<int>(i);
    }
    SendMessage(w.disks, CB_SETCURSEL, select, 0);
    describe(w);
}

// the dropdown's fourth choice: an .img.xz from this PC
void chooseImageFile(Window &w) {
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = w.hwnd;
    ofn.lpstrFilter = L"PC stick images (*.img.xz)\0*.img.xz\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        w.imageFile = narrow(file);
        for (char &c : w.imageFile)
            if (c == '\\')
                c = '/';
    }
}

void layout(Window &w) {
    using uitheme::px;
    const int W = px(Width), margin = px(Margin), heroH = px(HeroHeight);
    const int x = margin, inner = W - 2 * margin;
    int y = heroH + margin;
    MoveWindow(w.channelLabel, x, y + px(4), px(72), px(20), TRUE);
    MoveWindow(w.channel, x + px(74), y, px(280), px(200), TRUE);
    y += px(32);
    MoveWindow(w.disksLabel, x, y + px(4), px(72), px(20), TRUE);
    MoveWindow(w.disks, x + px(74), y, inner - px(74) - px(88), px(200), TRUE);
    MoveWindow(w.refresh, W - margin - px(80), y, px(80), px(24), TRUE);
    y += px(28);
    MoveWindow(w.fixed, x + px(74), y, inner - px(74), px(22), TRUE);
    y += px(30);
    // the status text wraps: its height comes from the text, never less than three lines' room
    const int statusH = max(px(52), uitheme::wrappedHeight(w.status, w.bold, inner) + px(4));
    MoveWindow(w.status, x, y, inner, statusH, TRUE);
    y += statusH + px(6);
    MoveWindow(w.verify, x, y, inner, px(22), TRUE);
    y += px(34);
    MoveWindow(w.write, W - margin - px(110), y, px(110), px(30), TRUE);
    const int questionsBottom = y + px(30) + margin + px(60); // the progress page needs the room for its log
    y = heroH + margin;
    MoveWindow(w.phaseLabel, x, y, inner, px(20), TRUE);
    y += px(24);
    MoveWindow(w.phaseBar, x, y, inner, px(14), TRUE);
    y += px(20);
    MoveWindow(w.bar, x, y, inner, px(14), TRUE);
    y += px(22);
    const int buttonH = px(26), buttonW = px(90);
    MoveWindow(w.log, x, y, inner, questionsBottom - y - buttonH - margin - px(4), TRUE);
    MoveWindow(w.action, W - margin - buttonW, questionsBottom - buttonH - margin, buttonW, buttonH, TRUE);
    RECT r = {0, 0, W, questionsBottom};
    AdjustWindowRect(&r, static_cast<DWORD>(GetWindowLongPtr(w.hwnd, GWL_STYLE)), FALSE);
    SetWindowPos(w.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

// two questions before a whole disk is erased: what, and are you sure
bool confirmed(Window &w, const PhysicalDisk &disk, const string &what) {
    const string name = disk.model.empty() ? "disk " + to_string(disk.number) : disk.model;
    const wstring first =
        wide("Write " + what + " onto\n\n    " + disk.describe() +
             "\n\nEverything on this stick will be erased - every partition and every file." +
             (disk.removable ? ""
                             : "\n\nThis drive calls itself a HARD DRIVE, not a stick. Make sure it is not a "
                               "backup disk or another drive you need."));
    if (MessageBoxW(w.hwnd, first.c_str(), L"AutoBleem - write the stick",
                    MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return false;
    const wstring second =
        wide("Last check: erase " + name + " (" + humanSize(disk.sizeBytes) + ") now?\n\nThis cannot be undone.");
    return MessageBoxW(w.hwnd, second.c_str(), L"AutoBleem - erase the stick",
                       MB_YESNO | MB_ICONEXCLAMATION | MB_DEFBUTTON2) == IDYES;
}

void startWrite(Window &w) {
    const PhysicalDisk *picked = selectedDisk(w);
    if (!picked)
        return;
    const PhysicalDisk disk = *picked;
    FlashOptions o = w.defaults;
    const int ch = selectedChannel(w);
    string what;
    if (isImageChoice(w, ch)) {
        o.imageFile = w.imageFile;
        what = "the image " + w.imageFile.substr(w.imageFile.find_last_of('/') + 1);
    } else {
        o.imageFile.clear();
        o.channel = channelId(w, ch);
        lock_guard<mutex> lock(w.channelsMutex);
        o.channelIndexes = w.catalog.lists(o.channel, true);
        what = "AutoBleem " +
               (static_cast<size_t>(ch) < w.channels.size() ? w.channels[static_cast<size_t>(ch)].version : string()) +
               " (" + o.channel + ")";
    }
    o.verify = SendMessage(w.verify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (o.scratchDir.empty())
        o.scratchDir = scratchDirectory();
    if (!confirmed(w, disk, what))
        return;
    w.state.reset();
    showPage(w, true);
    SendMessage(w.log, LB_RESETCONTENT, 0, 0);
    SetWindowTextW(w.action, L"Stop");
    EnableWindow(w.action, TRUE);
    w.worker = thread([&w, o, disk]() {
        Listener listener(w.state);
        WinInetDownloader downloader;
        WindowsDisk target(disk);
        string error;
        bool ok = FlasherJob::run(o, downloader, target, listener, [&w]() { return w.state.stop.load(); }, error);
        {
            lock_guard<mutex> lock(w.state.m);
            w.state.ok = ok;
            if (!ok) {
                w.state.lines.push_back("Failed: " + error);
                w.state.phase = error.compare(0, 7, "Stopped") == 0 ? "Stopped" : "Failed";
            } else {
                w.state.phase = "Done - the stick is ready";
            }
            w.state.done = w.state.total = 0;
        }
        w.state.finished.store(true);
    });
    SetTimer(w.hwnd, IdTimer, 100, nullptr);
}

void refresh(Window &w) {
    string phase;
    int phaseIndex, phaseTotal;
    uint64_t done, total;
    vector<string> fresh;
    bool finished = w.state.finished.load();
    {
        lock_guard<mutex> lock(w.state.m);
        phase = w.state.phase;
        phaseIndex = w.state.phaseIndex;
        phaseTotal = w.state.phaseTotal;
        done = w.state.done;
        total = w.state.total;
        for (size_t i = w.state.shown; i < w.state.lines.size(); i++)
            fresh.push_back(w.state.lines[i]);
        w.state.shown = w.state.lines.size();
    }
    string title = phase.empty() ? "Starting..." : phase;
    if (phaseTotal > 0 && !finished) {
        title = "Step " + to_string(phaseIndex) + " of " + to_string(phaseTotal) + ": " + phase;
        if (total > 0)
            title += " - " + humanSize(done) + " of " + humanSize(total);
    }
    SetWindowTextW(w.phaseLabel, wide(title).c_str());
    SendMessage(w.phaseBar, PBM_SETRANGE32, 0, phaseTotal > 0 ? phaseTotal : 1);
    SendMessage(w.phaseBar, PBM_SETPOS,
                finished ? (phaseTotal > 0 ? phaseTotal : 1) : (phaseIndex > 0 ? phaseIndex - 1 : 0), 0);
    const bool marquee = (GetWindowLongPtr(w.bar, GWL_STYLE) & PBS_MARQUEE) != 0;
    if (total > 0 || finished) {
        if (marquee) {
            SendMessage(w.bar, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtr(w.bar, GWL_STYLE, GetWindowLongPtr(w.bar, GWL_STYLE) & ~PBS_MARQUEE);
        }
        SendMessage(w.bar, PBM_SETRANGE32, 0, 1000);
        SendMessage(w.bar, PBM_SETPOS, finished ? 1000 : static_cast<int>(done * 1000 / total), 0);
    } else if (!marquee) {
        SetWindowLongPtr(w.bar, GWL_STYLE, GetWindowLongPtr(w.bar, GWL_STYLE) | PBS_MARQUEE);
        SendMessage(w.bar, PBM_SETMARQUEE, TRUE, 0);
    }
    for (const string &line : fresh) {
        int index =
            static_cast<int>(SendMessageW(w.log, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(line).c_str())));
        SendMessage(w.log, LB_SETTOPINDEX, index, 0);
    }
    if (finished) {
        if (w.worker.joinable())
            w.worker.join();
        SetWindowTextW(w.action, w.state.ok ? L"Close" : L"Back");
        EnableWindow(w.action, TRUE);
        KillTimer(w.hwnd, IdTimer);
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window *w = reinterpret_cast<Window *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        w = reinterpret_cast<Window *>(reinterpret_cast<CREATESTRUCT *>(lParam)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->hwnd = hwnd;
        uitheme::loadFonts(w->font, w->bold);
        uitheme::applyDarkTitleBar(hwnd); // older Windows keep their light bar

        w->channelLabel = make(*w, L"STATIC", L"Channel:", SS_LEFT, 0);
        w->channel = make(*w, L"COMBOBOX", nullptr, WS_TABSTOP | CBS_DROPDOWNLIST, IdChannel);
        if (!w->defaults.imageFile.empty())
            w->imageFile = w->defaults.imageFile;
        fillChannels(*w, w->defaults.channel,
                     !w->imageFile.empty()); // the built-in three until the site's list is read
        // the site's channel list and each channel's image, off the UI thread (small downloads)
        w->lookup = thread([w]() {
            WinInetDownloader downloader;
            const ableem::ChannelCatalog catalog =
                channelchoice::fetch(w->defaults.repoUrl, downloader, scratchDirectory());
            vector<ChannelImage> images(catalog.channels.size());
            vector<string> errors(catalog.channels.size());
            for (size_t i = 0; i < catalog.channels.size(); i++) {
                const string &id = catalog.channels[i].id;
                if (!FlasherJob::channelImage(w->defaults.repoUrl, id, catalog.lists(id, true), downloader,
                                              scratchDirectory(), images[i], errors[i]))
                    images[i] = ChannelImage();
            }
            {
                lock_guard<mutex> lock(w->channelsMutex);
                w->catalog = catalog;
                w->channels = images;
                w->channelErrors = errors;
            }
            PostMessage(w->hwnd, WmChannelsLooked, 0, 0);
        });
        w->disksLabel = make(*w, L"STATIC", L"USB stick:", SS_LEFT, 0);
        w->disks = make(*w, L"COMBOBOX", nullptr, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IdDisks);
        w->refresh = make(*w, L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, IdRefresh);
        w->fixed = make(*w, L"BUTTON", L"Show USB hard drives too (some big sticks call themselves one)",
                        WS_TABSTOP | BS_AUTOCHECKBOX, IdFixed);
        w->status = make(*w, L"STATIC", L"", SS_LEFT, 0);
        SendMessage(w->status, WM_SETFONT, reinterpret_cast<WPARAM>(w->bold), TRUE);
        w->verify = make(*w, L"BUTTON", L"Read the stick back afterwards and compare (recommended)",
                         WS_TABSTOP | BS_AUTOCHECKBOX, IdVerify);
        SendMessage(w->verify, BM_SETCHECK, w->defaults.verify ? BST_CHECKED : BST_UNCHECKED, 0);
        w->write = make(*w, L"BUTTON", L"Write", WS_TABSTOP | BS_OWNERDRAW, IdWrite);

        w->phaseLabel = make(*w, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
        SendMessage(w->phaseLabel, WM_SETFONT, reinterpret_cast<WPARAM>(w->bold), TRUE);
        w->phaseBar = make(*w, PROGRESS_CLASSW, nullptr, 0, 0);
        w->bar = make(*w, PROGRESS_CLASSW, nullptr, PBS_MARQUEE, 0);
        uitheme::styleProgress(w->phaseBar);
        uitheme::styleProgress(w->bar);
        w->log = make(*w, L"LISTBOX", nullptr, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 0, WS_EX_CLIENTEDGE);
        w->action = make(*w, L"BUTTON", L"Stop", WS_TABSTOP | BS_OWNERDRAW, IdAction);

        layout(*w);
        showPage(*w, false);
        refreshDisks(*w);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        uitheme::paintHero(dc, uitheme::px(Width), w ? w->hero : nullptr, w ? w->bold : nullptr);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        // the dark look; the combo boxes are not touched (the system draws them)
        LRESULT brush = 0;
        if (uitheme::controlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam), brush))
            return brush;
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
        if (!w || !item || item->CtlType != ODT_BUTTON)
            return FALSE;
        const UINT defaultId = w->progressPage ? IdAction : IdWrite;
        uitheme::drawButton(*item, w->bold, item->CtlID == defaultId);
        return TRUE;
    }
    case WM_NOTIFY: {
        const NMHDR *header = reinterpret_cast<const NMHDR *>(lParam);
        if (w && header && header->code == NM_CUSTOMDRAW && isCheckbox(static_cast<int>(header->idFrom)))
            return uitheme::drawCheckbox(*reinterpret_cast<const NMCUSTOMDRAW *>(lParam), w->font);
        return 0;
    }
    case DM_GETDEFID:
        // Enter presses the default button, as the old BS_DEFPUSHBUTTON did (the owner-drawn buttons carry
        // no default style)
        return w ? MAKELONG(w->progressPage ? IdAction : IdWrite, DC_HASDEFID) : 0;
    case WM_DEVICECHANGE:
        // a stick plugged in or pulled out
        if (w && !w->progressPage && (wParam == 0x8000 /*DBT_DEVICEARRIVAL*/ || wParam == 0x8004 /*REMOVECOMPLETE*/))
            refreshDisks(*w);
        return TRUE;
    case WmChannelsLooked:
        if (w) {
            // the site's list replaces the built-in one; the user's own pick (the image file too) stays, else
            // the program's default
            const int now = selectedChannel(*w);
            const bool image = isImageChoice(*w, now) && !(w->imageFile.empty() && !w->channelPicked);
            fillChannels(*w, w->channelPicked ? channelId(*w, now) : w->defaults.channel, image);
            w->looked.store(true);
            if (!w->progressPage)
                describe(*w);
        }
        return 0;
    case WM_TIMER:
        if (w)
            refresh(*w);
        return 0;
    case WM_COMMAND:
        if (!w)
            return 0;
        switch (LOWORD(wParam)) {
        case IdChannel:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                w->channelPicked = true;
                if (isImageChoice(*w, selectedChannel(*w)))
                    chooseImageFile(*w);
                describe(*w);
            }
            break;
        case IdDisks:
            if (HIWORD(wParam) == CBN_SELCHANGE)
                describe(*w);
            break;
        case IdFixed:
        case IdRefresh:
            refreshDisks(*w);
            break;
        case IdWrite:
            if (!w->progressPage)
                startWrite(*w);
            break;
        case IdAction:
            if (w->state.finished.load()) {
                if (w->state.ok) {
                    DestroyWindow(hwnd);
                } else {
                    showPage(*w, false);
                    refreshDisks(*w);
                }
            } else if (!w->state.stop.load()) {
                w->state.stop.store(true);
                lock_guard<mutex> lock(w->state.m);
                w->state.phase = "Stopping...";
            }
            break;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        if (w && w->progressPage && !w->state.finished.load()) {
            if (MessageBoxW(hwnd, L"Stop writing? The stick will not be usable until it is written again.",
                            L"AutoBleem", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                w->state.stop.store(true);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

//*******************************
// runFlasherWindow
//*******************************
int runFlasherWindow(const FlashOptions &defaults) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Window w;
    w.defaults = defaults;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&w.gdiplusToken, &gdiplusInput, nullptr);
    w.hero = uitheme::loadHero(); // null = no picture in the exe: paintHero draws the fallback

    WNDCLASSW wc = {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = uitheme::graphiteBrush();
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(1)); // the autobleem.ico of the .rc
    if (!wc.hIcon)
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, WindowClass,
                                wide("AutoBleem 2 " + Env::productVersion() + " - write the PC USB stick").c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                uitheme::px(Width), 600, nullptr, nullptr, wc.hInstance, &w);
    if (!hwnd) {
        PLOG_ERROR << "CreateWindow failed: " << GetLastError();
        return 1;
    }
    uitheme::setWindowIcons(hwnd);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    w.state.stop.store(true);
    if (w.worker.joinable())
        w.worker.join();
    if (w.lookup.joinable())
        w.lookup.join();
    delete w.hero;
    Gdiplus::GdiplusShutdown(w.gdiplusToken);
    if (w.font)
        DeleteObject(w.font);
    if (w.bold)
        DeleteObject(w.bold);
    return 0;
}

#endif
