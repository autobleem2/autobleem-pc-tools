//
// The flasher window - see the header. Two pages under the picture: the questions and the progress. The
// three channels are looked up once in the background when the window opens; the write runs on a
// std::thread reporting into a mutex-guarded State, which a 100 ms timer moves into the controls.
//
#ifdef _WIN32

#include "flasher_window.h"
#include "win32_disk.h"

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
const int HeroHeight = 270; // the picture: rows 90..630 of the 1280x720 splash, scaled to the width
const int Margin = 14;
const int Channels = 3; // the dropdown's release, testing, nightly - then "An image file on this PC..."
const char *const ChannelNames[Channels] = {"release", "testing", "nightly"};
const wchar_t *const ChannelLabels[Channels + 1] = {L"Release", L"Testing (the next release)",
                                                    L"Nightly (development build)", L"An image file on this PC..."};
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
    ChannelImage channels[Channels];
    string channelErrors[Channels];
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

// the hero picture from the exe's resources (autobleem.jpg as RCDATA 1)
Gdiplus::Image *loadHero() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(1), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res)
        return nullptr;
    HGLOBAL data = LoadResource(nullptr, res);
    DWORD size = SizeofResource(nullptr, res);
    void *bytes = LockResource(data);
    if (!bytes || !size)
        return nullptr;
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    memcpy(GlobalLock(copy), bytes, size);
    GlobalUnlock(copy);
    IStream *stream = nullptr;
    if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK)
        return nullptr;
    Gdiplus::Image *image = new Gdiplus::Image(stream);
    stream->Release();
    if (image->GetLastStatus() != Gdiplus::Ok) {
        delete image;
        return nullptr;
    }
    return image;
}

int selectedChannel(Window &w) {
    int i = static_cast<int>(SendMessage(w.channel, CB_GETCURSEL, 0, 0));
    return i < 0 || i > Channels ? 0 : i;
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

// what would be written, onto what - under the two boxes
void describe(Window &w) {
    const int ch = selectedChannel(w);
    const PhysicalDisk *disk = selectedDisk(w);
    string what;
    bool canWrite = true;
    if (ch == Channels) {
        what = w.imageFile.empty() ? "No image file chosen." : "The image " + w.imageFile + ".";
        canWrite = !w.imageFile.empty();
    } else if (!w.looked.load()) {
        what = "Asking the download site what each channel offers...";
        canWrite = false;
    } else {
        lock_guard<mutex> lock(w.channelsMutex);
        const ChannelImage &ci = w.channels[ch];
        if (ci.version.empty()) {
            what = "The " + string(ChannelNames[ch]) + " channel has no stick image: " + w.channelErrors[ch] +
                   ". Pick another channel, or check the internet connection.";
            canWrite = false;
        } else {
            what = "AutoBleem " + ci.version + " from the " + ChannelNames[ch] + " channel - a " +
                   humanSize(ci.image.size) + " download.";
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
    const int x = Margin, inner = Width - 2 * Margin;
    int y = HeroHeight + Margin;
    MoveWindow(w.channelLabel, x, y + 4, 72, 20, TRUE);
    MoveWindow(w.channel, x + 74, y, 280, 200, TRUE);
    y += 32;
    MoveWindow(w.disksLabel, x, y + 4, 72, 20, TRUE);
    MoveWindow(w.disks, x + 74, y, inner - 74 - 88, 200, TRUE);
    MoveWindow(w.refresh, Width - Margin - 80, y, 80, 24, TRUE);
    y += 28;
    MoveWindow(w.fixed, x + 74, y, inner - 74, 22, TRUE);
    y += 30;
    MoveWindow(w.status, x, y, inner, 52, TRUE);
    y += 58;
    MoveWindow(w.verify, x, y, inner, 22, TRUE);
    y += 34;
    MoveWindow(w.write, Width - Margin - 110, y, 110, 30, TRUE);
    const int questionsBottom = y + 30 + Margin + 60; // the progress page needs the room for its log
    y = HeroHeight + Margin;
    MoveWindow(w.phaseLabel, x, y, inner, 20, TRUE);
    y += 24;
    MoveWindow(w.phaseBar, x, y, inner, 14, TRUE);
    y += 20;
    MoveWindow(w.bar, x, y, inner, 14, TRUE);
    y += 22;
    const int buttonH = 26, buttonW = 90;
    MoveWindow(w.log, x, y, inner, questionsBottom - y - buttonH - Margin - 4, TRUE);
    MoveWindow(w.action, Width - Margin - buttonW, questionsBottom - buttonH - Margin, buttonW, buttonH, TRUE);
    RECT r = {0, 0, Width, questionsBottom};
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
    if (ch == Channels) {
        o.imageFile = w.imageFile;
        what = "the image " + w.imageFile.substr(w.imageFile.find_last_of('/') + 1);
    } else {
        o.imageFile.clear();
        o.channel = ChannelNames[ch];
        lock_guard<mutex> lock(w.channelsMutex);
        what = "AutoBleem " + w.channels[ch].version + " (" + o.channel + ")";
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
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        w->font = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW boldFont = metrics.lfMessageFont;
        boldFont.lfWeight = FW_SEMIBOLD;
        w->bold = CreateFontIndirectW(&boldFont);

        w->channelLabel = make(*w, L"STATIC", L"Channel:", SS_LEFT, 0);
        w->channel = make(*w, L"COMBOBOX", nullptr, WS_TABSTOP | CBS_DROPDOWNLIST, IdChannel);
        for (const wchar_t *label : ChannelLabels)
            SendMessageW(w->channel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        int initial = 0;
        for (int i = 0; i < Channels; i++)
            if (w->defaults.channel == ChannelNames[i])
                initial = i;
        if (!w->defaults.imageFile.empty()) {
            w->imageFile = w->defaults.imageFile;
            initial = Channels;
        }
        SendMessage(w->channel, CB_SETCURSEL, initial, 0);
        // the three channels' images, off the UI thread (three small downloads)
        w->lookup = thread([w]() {
            for (int i = 0; i < Channels; i++) {
                WinInetDownloader downloader;
                ChannelImage ci;
                string error;
                bool ok = FlasherJob::channelImage(w->defaults.repoUrl, ChannelNames[i], downloader, scratchDirectory(),
                                                   ci, error);
                lock_guard<mutex> lock(w->channelsMutex);
                if (ok)
                    w->channels[i] = ci;
                else
                    w->channelErrors[i] = error;
            }
            w->looked.store(true);
            PostMessage(w->hwnd, WmChannelsLooked, 0, 0);
        });
        w->disksLabel = make(*w, L"STATIC", L"USB stick:", SS_LEFT, 0);
        w->disks = make(*w, L"COMBOBOX", nullptr, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IdDisks);
        w->refresh = make(*w, L"BUTTON", L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, IdRefresh);
        w->fixed = make(*w, L"BUTTON", L"Show USB hard drives too (some big sticks call themselves one)",
                        WS_TABSTOP | BS_AUTOCHECKBOX, IdFixed);
        w->status = make(*w, L"STATIC", L"", SS_LEFT, 0);
        SendMessage(w->status, WM_SETFONT, reinterpret_cast<WPARAM>(w->bold), TRUE);
        w->verify = make(*w, L"BUTTON", L"Read the stick back afterwards and compare (recommended)",
                         WS_TABSTOP | BS_AUTOCHECKBOX, IdVerify);
        SendMessage(w->verify, BM_SETCHECK, w->defaults.verify ? BST_CHECKED : BST_UNCHECKED, 0);
        w->write = make(*w, L"BUTTON", L"Write", WS_TABSTOP | BS_DEFPUSHBUTTON, IdWrite);

        w->phaseLabel = make(*w, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
        SendMessage(w->phaseLabel, WM_SETFONT, reinterpret_cast<WPARAM>(w->bold), TRUE);
        w->phaseBar = make(*w, PROGRESS_CLASSW, nullptr, 0, 0);
        w->bar = make(*w, PROGRESS_CLASSW, nullptr, PBS_MARQUEE, 0);
        w->log = make(*w, L"LISTBOX", nullptr, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 0, WS_EX_CLIENTEDGE);
        w->action = make(*w, L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, IdAction);

        layout(*w);
        showPage(*w, false);
        refreshDisks(*w);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT hero = {0, 0, Width, HeroHeight};
        if (w && w->hero) {
            Gdiplus::Graphics g(dc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            Gdiplus::Rect dest(0, 0, Width, HeroHeight);
            g.DrawImage(w->hero, dest, 0, 90, 1280, 540, Gdiplus::UnitPixel);
        } else {
            HBRUSH navy = CreateSolidBrush(RGB(6, 26, 58));
            FillRect(dc, &hero, navy);
            DeleteObject(navy);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(232, 242, 255));
            if (w)
                SelectObject(dc, w->bold);
            DrawTextW(dc, L"AutoBleem 2", -1, &hero, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case WM_DEVICECHANGE:
        // a stick plugged in or pulled out
        if (w && !w->progressPage && (wParam == 0x8000 /*DBT_DEVICEARRIVAL*/ || wParam == 0x8004 /*REMOVECOMPLETE*/))
            refreshDisks(*w);
        return TRUE;
    case WmChannelsLooked:
        if (w && !w->progressPage)
            describe(*w);
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
                if (selectedChannel(*w) == Channels)
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
    w.hero = loadHero();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, WindowClass,
                                wide("AutoBleem 2 " + Env::productVersion() + " - write the PC USB stick").c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                Width, 600, nullptr, nullptr, wc.hInstance, &w);
    if (!hwnd) {
        PLOG_ERROR << "CreateWindow failed: " << GetLastError();
        return 1;
    }
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
