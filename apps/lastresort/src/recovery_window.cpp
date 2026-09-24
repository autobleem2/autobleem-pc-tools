//
// The recovery window - see the header. Four pages under the picture; the backup and the console are
// looked at off the UI thread (a stick's zip listed, fastboot asked once a second), the recovery runs on a
// std::thread reporting into a mutex-guarded State that a 100 ms timer moves into the controls - the
// flasher's pattern.
//
#ifdef _WIN32

#include "recovery_window.h"
#include "win32_console.h"

#include "../../installer/src/win32_platform.h"
#include "core/recovery_job.h"
#include "core/services/environment.h"
#include "core/version.h"
#include "installer/install_job_base.h" // humanSize

#include <ableem/engine/filesystem.h>
#include <ableem/engine/log.h>

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
using ableem::DirEntry;

namespace {

const wchar_t *const WindowClass = L"AutoBleemLastResortRecovery";
const int IdTimer = 1;      // the progress, every 100 ms
const int IdProbeTimer = 2; // the console, every second on the fastboot page

enum Ids {
    IdBackups = 100,
    IdRefresh,
    IdDriver,
    IdBack,
    IdNext,
};
enum Page { PageBackup, PageFastboot, PageProgress, PageResult };

const int Width = 640;
const int HeroHeight = 270;
const int Margin = 14;
const int ContentHeight = 330;
const UINT WmProbed = WM_APP + 1;
const UINT WmDriverDone = WM_APP + 2;
const UINT WmBackupsFound = WM_APP + 3;

//******************
// State - the recovery's, as the flasher's
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
    string error;

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
        error.clear();
    }
};

class Listener : public InstallListener {
public:
    explicit Listener(State &state) : state_(state) {}
    void onPhase(int index, int total, const string &title) override {
        PLOG_INFO << "[" << index << "/" << total << "] " << title;
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
        PLOG_INFO << line;
        lock_guard<mutex> lock(state_.m);
        state_.lines.push_back(line);
    }

private:
    State &state_;
};

// a backup found on a drive (or browsed for)
struct Backup {
    string where; // "F: SONY" or "this PC"
    LbootImage image;
    string describe() const {
        string s = where + " - " + DirEntry::getFileNameFromPath(image.path);
        if (!image.usable())
            return s + " (cannot be used: " + image.error + ")";
        return s + " (" + (image.autobleem ? "AutoBleem backup, " : "") + to_string(image.images.size()) + " images, " +
               humanSize(image.totalBytes()) + ")";
    }
};

// what the probe saw of the console
struct Probe {
    ConsoleUsb usb;
    bool fastbootMissing = false;
    string serial; // fastboot sees it: ready
    string error;  // why fastboot does not, when the device is there
};

//******************
// Window
//******************
struct Window {
    HWND hwnd = nullptr;
    HFONT font = nullptr, bold = nullptr, title = nullptr;
    Gdiplus::Image *hero = nullptr, *board = nullptr;
    ULONG_PTR gdiplusToken = 0;
    RecoveryWindowOptions options;
    Page page = PageBackup;

    HWND heading = nullptr, back = nullptr, next = nullptr;
    // the backup
    HWND intro = nullptr, backupsLabel = nullptr, backups = nullptr, refresh = nullptr, contents = nullptr;
    vector<Backup> backupList;
    string browsed;
    thread finder;
    atomic<bool> finding{false};
    mutex foundMutex;
    vector<Backup> found;
    // fastboot mode
    HWND steps = nullptr, status = nullptr, driver = nullptr;
    thread prober;
    atomic<bool> probing{false};
    mutex probeMutex;
    Probe probe;
    thread driverWorker;
    atomic<bool> installingDriver{false};
    mutex driverMutex;
    vector<string> driverLines;
    string driverError;
    bool driverOk = false;
    // the progress
    HWND phaseLabel = nullptr, phaseBar = nullptr, bar = nullptr, log = nullptr;
    State state;
    thread worker;
    // the result
    HWND result = nullptr;
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

// Windows' static controls want CR LF between lines
wstring lines(const string &text) {
    string out;
    for (char c : text) {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return wide(out);
}

HWND make(Window &w, const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 0, 0, w.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
    return h;
}

// a picture from the exe's resources: RCDATA 1 the AutoBleem splash, RCDATA 2 the board
Gdiplus::Image *loadPicture(int id) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), reinterpret_cast<LPCWSTR>(RT_RCDATA));
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

string logFile(const Window &w) {
    return w.options.workDir + "/LastResortRecovery.log";
}

//******************
// pages
//******************
void layout(Window &w) {
    const int x = Margin, inner = Width - 2 * Margin;
    const int top = HeroHeight + Margin, bottom = HeroHeight + ContentHeight;
    const int buttonH = 28, buttonW = 120;
    const int buttonsY = bottom - Margin - buttonH;
    MoveWindow(w.heading, x, top, inner, 24, TRUE);
    int y = top + 30;
    // the backup
    MoveWindow(w.intro, x, y, inner, 50, TRUE);
    MoveWindow(w.backupsLabel, x, y + 58, 60, 20, TRUE);
    MoveWindow(w.backups, x + 62, y + 54, inner - 62 - 88, 200, TRUE);
    MoveWindow(w.refresh, Width - Margin - 80, y + 54, 80, 24, TRUE);
    MoveWindow(w.contents, x + 62, y + 88, inner - 62, buttonsY - (y + 88) - 8, TRUE);
    // fastboot mode
    MoveWindow(w.steps, x, y, inner, 150, TRUE);
    MoveWindow(w.status, x, y + 156, inner, 58, TRUE);
    MoveWindow(w.driver, x, buttonsY, 150, buttonH, TRUE);
    // the progress
    MoveWindow(w.phaseLabel, x, y, inner, 20, TRUE);
    MoveWindow(w.phaseBar, x, y + 24, inner, 14, TRUE);
    MoveWindow(w.bar, x, y + 44, inner, 14, TRUE);
    MoveWindow(w.log, x, y + 66, inner, buttonsY - (y + 66) - 8, TRUE);
    // the result
    MoveWindow(w.result, x, y, inner, buttonsY - y - 8, TRUE);
    MoveWindow(w.back, Width - Margin - 2 * buttonW - 8, buttonsY, buttonW, buttonH, TRUE);
    MoveWindow(w.next, Width - Margin - buttonW, buttonsY, buttonW, buttonH, TRUE);
    RECT r = {0, 0, Width, bottom};
    AdjustWindowRect(&r, static_cast<DWORD>(GetWindowLongPtr(w.hwnd, GWL_STYLE)), FALSE);
    SetWindowPos(w.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

void describeBackup(Window &w);
void startProbe(Window &w);

void showPage(Window &w, Page page) {
    w.page = page;
    auto show = [](const vector<HWND> &hs, bool on) {
        for (HWND h : hs)
            ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    };
    show({w.intro, w.backupsLabel, w.backups, w.refresh, w.contents}, page == PageBackup);
    show({w.steps, w.status}, page == PageFastboot);
    ShowWindow(w.driver, SW_HIDE);
    show({w.phaseLabel, w.phaseBar, w.bar, w.log}, page == PageProgress);
    show({w.result}, page == PageResult);
    show({w.heading, w.next}, true);
    ShowWindow(w.back, page == PageProgress ? SW_HIDE : SW_SHOW);
    EnableWindow(w.back, page != PageBackup);
    EnableWindow(w.next, TRUE);
    KillTimer(w.hwnd, IdProbeTimer);
    switch (page) {
    case PageBackup:
        SetWindowTextW(w.heading, L"Step 1 of 3 - The backup");
        SetWindowTextW(w.back, L"< Back");
        SetWindowTextW(w.next, L"Next >");
        describeBackup(w);
        break;
    case PageFastboot:
        SetWindowTextW(w.heading, L"Step 2 of 3 - Put the console into fastboot mode");
        SetWindowTextW(w.back, L"< Back");
        SetWindowTextW(w.next, L"Start recovery");
        EnableWindow(w.next, FALSE);
        SetWindowTextW(w.status, L"Looking for the console...");
        startProbe(w);
        SetTimer(w.hwnd, IdProbeTimer, 1000, nullptr);
        break;
    case PageProgress:
        SetWindowTextW(w.heading, L"Step 3 of 3 - Writing the backup to the console");
        SetWindowTextW(w.next, L"Stop");
        break;
    case PageResult:
        SetWindowTextW(w.heading, L"Does the console start again?");
        SetWindowTextW(w.back, L"No, it does not");
        SetWindowTextW(w.next, L"Yes, it starts");
        break;
    }
    InvalidateRect(w.hwnd, nullptr, TRUE);
}

//******************
// the backup
//******************
const Backup *selectedBackup(Window &w) {
    int i = static_cast<int>(SendMessage(w.backups, CB_GETCURSEL, 0, 0));
    if (i < 0 || i >= static_cast<int>(w.backupList.size()))
        return nullptr;
    return &w.backupList[static_cast<size_t>(i)];
}

void describeBackup(Window &w) {
    const Backup *b = selectedBackup(w);
    string text;
    bool usable = false;
    if (w.finding.load() && w.backupList.empty()) {
        text = "Looking for LBOOT.EPB on the USB sticks...";
    } else if (!b) {
        text = "No LBOOT.EPB found on a USB stick. Plug in the stick AutoBleem was installed from (the backup is at "
               "its root), or pick \"A file on this PC...\" if you copied the backup somewhere.";
    } else {
        // a long path elided in the middle: the drive and the file name are what tell backups apart
        const string &path = b->image.path;
        text = (path.size() > 90 ? path.substr(0, 30) + "..." + path.substr(path.size() - 57) : path) + "\n";
        if (!b->image.usable()) {
            text += "This file cannot be used: " + b->image.error + ".";
        } else {
            text += b->image.autobleem
                        ? "Made by AutoBleem when it installed its kernel.\n"
                        : "Not made by AutoBleem - it is written all the same, if it is what you want.\n";
            text += "Written to the console:\n";
            for (const string &line : b->image.describe())
                text += "    " + line + "\n";
            usable = true;
        }
    }
    SetWindowTextW(w.contents, lines(text).c_str());
    EnableWindow(w.next, usable);
}

void fillBackups(Window &w, const vector<Backup> &found) {
    const Backup *current = selectedBackup(w);
    const string keep = current ? current->image.path : w.options.backupFile;
    w.backupList = found;
    bool listed = false;
    for (const Backup &b : found)
        listed = listed || b.image.path == w.browsed;
    if (!w.browsed.empty() && !listed) {
        Backup b;
        b.where = "this PC";
        b.image = LbootImage::inspect(w.browsed);
        w.backupList.push_back(b);
    }
    SendMessage(w.backups, CB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < w.backupList.size(); i++) {
        SendMessageW(w.backups, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(w.backupList[i].describe()).c_str()));
        if (w.backupList[i].image.path == keep)
            select = static_cast<int>(i);
    }
    SendMessageW(w.backups, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"A file on this PC..."));
    SendMessage(w.backups, CB_SETCURSEL, w.backupList.empty() ? -1 : select, 0);
    describeBackup(w);
}

// the removable drives' LBOOT.EPB, listed off the UI thread (a zip's directory is read for each)
void findBackups(Window &w) {
    if (w.finding.exchange(true))
        return;
    if (w.finder.joinable())
        w.finder.join();
    Window *pw = &w;
    const string given = w.options.backupFile; // the one named on the command line, or browsed for
    w.finder = thread([pw, given]() {
        vector<Backup> found;
        for (const RemovableDrive &d : listRemovableDrives()) {
            if (!d.ready)
                continue;
            const string path = d.root + "LBOOT.EPB";
            if (!DirEntry::exists(path))
                continue;
            Backup b;
            b.where = d.letter + (d.label.empty() ? "" : " " + d.label);
            b.image = LbootImage::inspect(path);
            found.push_back(b);
        }
        bool listed = false;
        for (const Backup &b : found)
            listed = listed || b.image.path == given;
        if (!given.empty() && !listed) {
            Backup b;
            b.where = "this PC";
            b.image = LbootImage::inspect(given);
            found.push_back(b);
        }
        {
            lock_guard<mutex> lock(pw->foundMutex);
            pw->found = found;
        }
        pw->finding.store(false);
        PostMessage(pw->hwnd, WmBackupsFound, 0, 0);
    });
}

void browseBackup(Window &w) {
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = w.hwnd;
    ofn.lpstrFilter = L"Console backups (LBOOT.EPB)\0*.EPB\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"The console's backup (LBOOT.EPB)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        w.browsed = narrow(file);
        for (char &c : w.browsed)
            if (c == '\\')
                c = '/';
        w.options.backupFile = w.browsed;
    }
    vector<Backup> found;
    {
        lock_guard<mutex> lock(w.foundMutex);
        found = w.found;
    }
    fillBackups(w, found);
}

//******************
// fastboot mode
//******************
const char *const StepsText =
    "1. Unplug the console from everything, take its stick out, and open it to reach its board.\n"
    "2. Find the two round FASTBOOT pads shown above: side A of the board, just right of \"LM-11\".\n"
    "3. Hold the two pads together (tweezers, or a piece of wire across both), and while you hold them\n"
    "    connect the console's micro-USB port (where its power goes) to this PC with a USB data cable.\n"
    "4. Let go after a few seconds. The console needs no other power: this PC's USB port feeds it.\n"
    "This page sees the console come up below - you do not need to press anything until it is ready.";

// the console as Windows and fastboot see it, off the UI thread
void startProbe(Window &w) {
    if (w.probing.exchange(true))
        return;
    if (w.prober.joinable())
        w.prober.join();
    Window *pw = &w;
    w.prober = thread([pw]() {
        Probe p;
        const string exe = WindowsFastboot::bundledPath();
        p.fastbootMissing = !DirEntry::exists(exe);
        p.usb = findConsoleUsb();
        // fastboot decides; Windows' device list only explains why it sees nothing (no driver)
        if (!p.fastbootMissing) {
            WindowsFastboot fastboot(exe);
            string serial, error;
            if (RecoveryJob::findConsole(fastboot, serial, error))
                p.serial = serial;
            else
                p.error = error;
        }
        {
            lock_guard<mutex> lock(pw->probeMutex);
            pw->probe = p;
        }
        pw->probing.store(false);
        PostMessage(pw->hwnd, WmProbed, 0, 0);
    });
}

void showProbe(Window &w) {
    if (w.page != PageFastboot)
        return;
    Probe p;
    {
        lock_guard<mutex> lock(w.probeMutex);
        p = w.probe;
    }
    string text;
    bool ready = false, offerDriver = false;
    if (w.installingDriver.load()) {
        lock_guard<mutex> lock(w.driverMutex);
        text = "Installing the driver... " + (w.driverLines.empty() ? string() : w.driverLines.back());
    } else if (p.fastbootMissing) {
        text = "platform-tools\\fastboot.exe is missing next to LastResortRecovery.exe - unpack the whole zip "
               "again, with its folders.";
    } else if (!p.serial.empty()) {
        text = "Ready: the console is in fastboot mode (" + p.serial + "). Press \"Start recovery\".";
        ready = true;
    } else if (!p.usb.present) {
        text = p.error.find("devices") != string::npos
                   ? "fastboot sees " + p.error + "."
                   : "Waiting for the console... (nothing in fastboot mode on this PC's USB yet)";
    } else if (!p.usb.hasDriver()) {
        text = "The console is connected in fastboot mode, but Windows has no driver for it.\n"
               "Press \"Install driver\" - Windows asks for permission once.";
        offerDriver = true;
    } else {
        text = "The console is connected (" + p.usb.service + "), but fastboot does not see it: " + p.error +
               ".\nIf another tool installed a driver for it before, press \"Install driver\" to replace it.";
        offerDriver = true;
    }
    if (!w.driverError.empty() && !ready && !w.installingDriver.load())
        text += "\nThe last driver install failed: " + w.driverError;
    SetWindowTextW(w.status, lines(text).c_str());
    ShowWindow(w.driver, offerDriver && !w.installingDriver.load() ? SW_SHOW : SW_HIDE);
    EnableWindow(w.next, ready && !w.installingDriver.load());
}

void installDriver(Window &w) {
    if (w.installingDriver.exchange(true))
        return;
    if (w.driverWorker.joinable())
        w.driverWorker.join();
    {
        lock_guard<mutex> lock(w.driverMutex);
        w.driverLines.clear();
        w.driverError.clear();
    }
    Window *pw = &w;
    w.driverWorker = thread([pw]() {
        string error;
        const bool ok = installConsoleDriverElevated(
            pw->options.workDir + "/driver",
            [pw](const string &line) {
                PLOG_INFO << "driver: " << line;
                lock_guard<mutex> lock(pw->driverMutex);
                pw->driverLines.push_back(line);
            },
            error);
        if (!ok) {
            PLOG_WARNING << "driver install failed: " << error;
        }
        {
            lock_guard<mutex> lock(pw->driverMutex);
            pw->driverOk = ok;
            pw->driverError = ok ? "" : error;
        }
        pw->installingDriver.store(false);
        PostMessage(pw->hwnd, WmDriverDone, 0, 0);
    });
    showProbe(w);
}

//******************
// the recovery
//******************
bool confirmed(Window &w, const LbootImage &image) {
    string text = "Write the backup to the console now?\n\n" + image.path + "\n";
    for (const string &line : image.describe())
        text += "    " + line + "\n";
    text += "\nThe console's system and its saved data are replaced by what the backup holds. Leave the console "
            "connected until the recovery says it is done.";
    return MessageBoxW(w.hwnd, wide(text).c_str(), L"LastResortRecovery",
                       MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

void startRecovery(Window &w) {
    const Backup *b = selectedBackup(w);
    if (!b || !b->image.usable() || !confirmed(w, b->image))
        return;
    KillTimer(w.hwnd, IdProbeTimer);
    RecoveryOptions o;
    o.backupFile = b->image.path;
    o.workDir = w.options.workDir;
    ULARGE_INTEGER free = {};
    if (GetDiskFreeSpaceExW(wide(o.workDir).c_str(), &free, nullptr, nullptr))
        o.workDirFreeBytes = free.QuadPart;
    w.state.reset();
    showPage(w, PageProgress);
    SendMessage(w.log, LB_RESETCONTENT, 0, 0);
    Window *pw = &w;
    w.worker = thread([pw, o]() {
        Listener listener(pw->state);
        WindowsFastboot fastboot(WindowsFastboot::bundledPath());
        string error;
        const bool ok = RecoveryJob::run(o, fastboot, listener, [pw]() { return pw->state.stop.load(); }, error);
        if (!ok) {
            PLOG_ERROR << "recovery failed: " << error;
        }
        {
            lock_guard<mutex> lock(pw->state.m);
            pw->state.ok = ok;
            pw->state.error = error;
            if (!ok) {
                pw->state.lines.push_back("Failed: " + error);
                pw->state.phase = error.compare(0, 7, "Stopped") == 0 ? "Stopped" : "Failed";
            } else {
                pw->state.phase = "Done - the backup is on the console";
            }
            pw->state.done = pw->state.total = 0;
        }
        pw->state.finished.store(true);
    });
    SetTimer(w.hwnd, IdTimer, 100, nullptr);
}

// the images are being written: no stop, no close
bool writing(Window &w) {
    if (w.page != PageProgress || w.state.finished.load())
        return false;
    lock_guard<mutex> lock(w.state.m);
    return w.state.phase.compare(0, 8, "Writing ") == 0 || w.state.phase.compare(0, 7, "Turning") == 0;
}

void refresh(Window &w) {
    string phase, error;
    int phaseIndex, phaseTotal;
    uint64_t done, total;
    vector<string> fresh;
    bool finished = w.state.finished.load();
    {
        lock_guard<mutex> lock(w.state.m);
        phase = w.state.phase;
        error = w.state.error;
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
    if (!finished) {
        const bool busy = writing(w);
        SetWindowTextW(w.next, busy ? L"Writing..." : L"Stop");
        EnableWindow(w.next, !busy && !w.state.stop.load());
        return;
    }
    if (w.worker.joinable())
        w.worker.join();
    KillTimer(w.hwnd, IdTimer);
    if (w.state.ok) {
        SetWindowTextW(w.next, L"Next >");
        EnableWindow(w.next, TRUE);
        return;
    }
    SetWindowTextW(w.next, L"< Back");
    EnableWindow(w.next, TRUE);
    const bool stopped = error.compare(0, 7, "Stopped") == 0;
    if (!stopped)
        MessageBoxW(w.hwnd,
                    wide("The recovery did not finish:\n\n" + error +
                         "\n\nPut the console into fastboot mode again and start the recovery once more. The whole "
                         "log is in\n" +
                         logFile(w))
                        .c_str(),
                    L"LastResortRecovery", MB_OK | MB_ICONERROR);
}

const char *const ResultText =
    "The backup is on the console, and the console was told to restart.\n\n"
    "1. Disconnect the console from this PC.\n"
    "2. Close the console up again, connect its own power supply (a PC's USB port may not give it enough power to "
    "start) and the TV, and switch it on - with no stick in it the first time.\n\n"
    "Does the console start - the PlayStation logo, then its menu?";

void answered(Window &w, bool starts) {
    if (starts) {
        PLOG_INFO << "the user says the console starts";
        MessageBoxW(w.hwnd,
                    L"The console is recovered.\n\nIt runs the system it had when the backup was made. Put the "
                    L"AutoBleem stick back in to use AutoBleem again.",
                    L"LastResortRecovery", MB_OK | MB_ICONINFORMATION);
    } else {
        PLOG_INFO << "the user says the console does not start";
        MessageBoxW(w.hwnd,
                    wide("Some things to try:\n\n"
                         "- Switch it on once more with its own power supply and HDMI, and give it a minute.\n"
                         "- If it does nothing at all, run LastResortRecovery again: fastboot mode, then the "
                         "recovery. If fastboot reported an error, the log says which partition.\n"
                         "- If the backup is not from this console, or it is damaged, it cannot bring the console "
                         "back.\n\nThe whole log is in\n" +
                         logFile(w) + "\n- keep it if you ask for help.")
                        .c_str(),
                    L"LastResortRecovery", MB_OK | MB_ICONWARNING);
    }
    DestroyWindow(w.hwnd);
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
        LOGFONTW titleFont = boldFont;
        titleFont.lfHeight = titleFont.lfHeight * 5 / 4;
        w->title = CreateFontIndirectW(&titleFont);

        w->heading = make(*w, L"STATIC", L"", SS_LEFT, 0);
        SendMessage(w->heading, WM_SETFONT, reinterpret_cast<WPARAM>(w->title), TRUE);
        w->back = make(*w, L"BUTTON", L"< Back", WS_TABSTOP | BS_PUSHBUTTON, IdBack);
        w->next = make(*w, L"BUTTON", L"Next >", WS_TABSTOP | BS_DEFPUSHBUTTON, IdNext);

        w->intro = make(*w, L"STATIC",
                        L"LastResortRecovery writes the console's own backup - LBOOT.EPB, which AutoBleem made on the "
                        L"stick before it installed its kernel - back to the console over USB, for a console that no "
                        L"longer starts and that Sony's recovery cannot fix.",
                        SS_LEFT, 0);
        w->backupsLabel = make(*w, L"STATIC", L"Backup:", SS_LEFT, 0);
        w->backups = make(*w, L"COMBOBOX", nullptr, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IdBackups);
        w->refresh = make(*w, L"BUTTON", L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, IdRefresh);
        w->contents = make(*w, L"STATIC", L"", SS_LEFT, 0);

        w->steps = make(*w, L"STATIC", lines(StepsText).c_str(), SS_LEFT, 0);
        w->status = make(*w, L"STATIC", L"", SS_LEFT, 0);
        SendMessage(w->status, WM_SETFONT, reinterpret_cast<WPARAM>(w->bold), TRUE);
        w->driver = make(*w, L"BUTTON", L"Install driver", WS_TABSTOP | BS_PUSHBUTTON, IdDriver);

        w->phaseLabel = make(*w, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
        SendMessage(w->phaseLabel, WM_SETFONT, reinterpret_cast<WPARAM>(w->bold), TRUE);
        w->phaseBar = make(*w, PROGRESS_CLASSW, nullptr, 0, 0);
        w->bar = make(*w, PROGRESS_CLASSW, nullptr, PBS_MARQUEE, 0);
        w->log = make(*w, L"LISTBOX", nullptr, WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 0,
                      WS_EX_CLIENTEDGE);
        SendMessage(w->log, LB_SETHORIZONTALEXTENT, 1600, 0);

        w->result = make(*w, L"STATIC", lines(ResultText).c_str(), SS_LEFT, 0);

        layout(*w);
        showPage(*w, PageBackup);
        findBackups(*w);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT band = {0, 0, Width, HeroHeight};
        Gdiplus::Image *picture = w ? (w->page == PageFastboot ? w->board : w->hero) : nullptr;
        if (picture) {
            Gdiplus::Graphics g(dc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            Gdiplus::Rect dest(0, 0, Width, HeroHeight);
            if (picture == w->hero)
                g.DrawImage(picture, dest, 0, 90, 1280, 540, Gdiplus::UnitPixel);
            else
                g.DrawImage(picture, dest, 0, 0, static_cast<INT>(picture->GetWidth()),
                            static_cast<INT>(picture->GetHeight()), Gdiplus::UnitPixel);
        } else {
            HBRUSH navy = CreateSolidBrush(RGB(6, 26, 58));
            FillRect(dc, &band, navy);
            DeleteObject(navy);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        // the window's own face behind the text, not the default white box
        SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case WM_DEVICECHANGE:
        if (w && w->page == PageBackup && (wParam == 0x8000 /*DBT_DEVICEARRIVAL*/ || wParam == 0x8004))
            findBackups(*w);
        return TRUE;
    case WmBackupsFound:
        if (w) {
            vector<Backup> found;
            {
                lock_guard<mutex> lock(w->foundMutex);
                found = w->found;
            }
            fillBackups(*w, found);
        }
        return 0;
    case WmProbed:
    case WmDriverDone:
        if (w) {
            showProbe(*w);
            if (msg == WmDriverDone && w->page == PageFastboot)
                startProbe(*w);
        }
        return 0;
    case WM_TIMER:
        if (w && wParam == IdTimer)
            refresh(*w);
        else if (w && wParam == IdProbeTimer && w->page == PageFastboot && !w->installingDriver.load())
            startProbe(*w);
        return 0;
    case WM_COMMAND:
        if (!w)
            return 0;
        switch (LOWORD(wParam)) {
        case IdBackups:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int i = static_cast<int>(SendMessage(w->backups, CB_GETCURSEL, 0, 0));
                if (i == static_cast<int>(w->backupList.size()))
                    browseBackup(*w);
                else
                    describeBackup(*w);
            }
            break;
        case IdRefresh:
            findBackups(*w);
            break;
        case IdDriver:
            installDriver(*w);
            break;
        case IdBack:
            if (w->page == PageFastboot)
                showPage(*w, PageBackup);
            else if (w->page == PageResult)
                answered(*w, false);
            break;
        case IdNext:
            switch (w->page) {
            case PageBackup:
                if (selectedBackup(*w) && selectedBackup(*w)->image.usable())
                    showPage(*w, PageFastboot);
                break;
            case PageFastboot:
                startRecovery(*w);
                break;
            case PageProgress:
                if (!w->state.finished.load()) {
                    if (!writing(*w)) {
                        w->state.stop.store(true);
                        lock_guard<mutex> lock(w->state.m);
                        w->state.phase = "Stopping...";
                    }
                } else if (w->state.ok) {
                    showPage(*w, PageResult);
                } else {
                    showPage(*w, PageFastboot);
                }
                break;
            case PageResult:
                answered(*w, true);
                break;
            }
            break;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        if (w && w->page == PageProgress && !w->state.finished.load()) {
            if (writing(*w)) {
                MessageBoxW(hwnd,
                            L"The backup is being written to the console. Closing now would leave it half written - "
                            L"wait until the recovery is done.",
                            L"LastResortRecovery", MB_OK | MB_ICONWARNING);
                return 0;
            }
            w->state.stop.store(true);
            return 0;
        }
        if (w && w->installingDriver.load())
            return 0; // Windows' own question is open
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
// runRecoveryWindow
//*******************************
int runRecoveryWindow(const RecoveryWindowOptions &options) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Window w;
    w.options = options;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&w.gdiplusToken, &gdiplusInput, nullptr);
    w.hero = loadPicture(1);
    w.board = loadPicture(2);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd =
        CreateWindowExW(0, WindowClass, wide("AutoBleem 2 " + Env::productVersion() + " - LastResortRecovery").c_str(),
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, Width,
                        640, nullptr, nullptr, wc.hInstance, &w);
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
    for (thread *t : {&w.worker, &w.finder, &w.prober, &w.driverWorker})
        if (t->joinable())
            t->join();
    delete w.hero;
    delete w.board;
    Gdiplus::GdiplusShutdown(w.gdiplusToken);
    for (HFONT f : {w.font, w.bold, w.title})
        if (f)
            DeleteObject(f);
    return 0;
}

#endif
