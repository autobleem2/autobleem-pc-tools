//
// LastResortRecovery: a PlayStation Classic that no longer starts, brought back from this PC. The console's
// own backup (LBOOT.EPB - abflashkit makes it on the stick before it installs AutoBleem's kernel) is written
// back over USB with Android's fastboot client, the console put into fastboot mode by the two pads on its
// board; then MISC is cleared (Sony's recovery off) and the console restarted. The job is RecoveryJob
// (core/, tested); this is the program around it - the window, a line for scripts, and the driver install
// that runs elevated.
//
//   LastResortRecovery.exe                                   the window
//   LastResortRecovery.exe --probe                           what Windows and fastboot see of the console
//   LastResortRecovery.exe --quiet --backup LBOOT.EPB --yes  no window: the console must be in fastboot mode
//       [--no-reboot]                                          already, with its driver
//   LastResortRecovery.exe --install-driver DIR --log FILE   the driver (what the window runs elevated)
//
#include "core/recovery_job.h"
#include "core/services/environment.h"
#include "core/version.h"

#include "../../installer/src/win32_platform.h"
#include "recovery_window.h"
#include "win32_console.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/log.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace std;
using ableem::DirEntry;

namespace {

class QuietListener : public InstallListener {
public:
    void onPhase(int index, int total, const string &title) override {
        cout << "[" << index << "/" << total << "] " << title << endl;
    }
    void onProgress(uint64_t, uint64_t) override {}
    void onLine(const string &line) override { cout << line << endl; }
};

int usage() {
    cout << "USAGE: LastResortRecovery [--backup LBOOT.EPB]\n"
            "       LastResortRecovery --probe\n"
            "       LastResortRecovery --quiet --backup LBOOT.EPB --yes [--no-reboot]\n"
            "       LastResortRecovery --install-driver DIR --log FILE"
         << endl;
    return EXIT_FAILURE;
}

string forward(string path) {
    for (char &c : path)
        if (c == '\\')
            c = '/';
    return path;
}

#ifdef _WIN32
// %TEMP%\LastResortRecovery: the unpacked images, the driver package, the log
string workDirectory() {
    char path[MAX_PATH] = {0};
    DWORD n = GetTempPathA(MAX_PATH, path);
    string dir = n > 0 && n < MAX_PATH ? forward(string(path, n)) : string("./");
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir + "LastResortRecovery";
}
#endif

} // namespace

int main(int argc, char *argv[]) {
    bool quiet = false, probe = false, yes = false, reboot = true;
    string backup, driverDir, driverLog;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        auto value = [&](string &into) {
            if (i + 1 >= argc)
                return false;
            into = argv[++i];
            return true;
        };
        if (arg == "--quiet")
            quiet = true;
        else if (arg == "--probe")
            probe = true;
        else if (arg == "--yes")
            yes = true;
        else if (arg == "--no-reboot")
            reboot = false;
        else if (arg == "--backup" && value(backup)) {
        } else if (arg == "--install-driver" && value(driverDir)) {
        } else if (arg == "--log" && value(driverLog)) {
        } else
            return usage();
    }
    backup = forward(backup);

#ifdef _WIN32
    // the elevated half of the window's "Install driver": no console, no window - its lines go to the log
    // file the window reads back
    if (!driverDir.empty()) {
        if (driverLog.empty())
            return usage();
        ofstream log(driverLog);
        string error;
        bool ok = installConsoleDriver(forward(driverDir), [&log](const string &line) { log << line << endl; }, error);
        if (!ok)
            log << "ERROR: " << error << endl;
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (quiet || probe)
        attachParentConsole();
    const string work = workDirectory();
    DirEntry::createDirs(work);
    ableem::Log::initConsoleOnly();
    ableem::Log::addFile(work + "/LastResortRecovery.log");
    PLOG_INFO << "LastResortRecovery " << Env::productVersion() << " (" << Version::FULL_VERSION << ")";

    if (probe) {
        const ConsoleUsb usb = findConsoleUsb();
        cout << "USB " << ConsoleHardwareId << ": " << usb.describe() << endl;
        const string exe = WindowsFastboot::bundledPath();
        if (!DirEntry::exists(exe)) {
            cout << "fastboot: " << exe << " is missing" << endl;
            return EXIT_FAILURE;
        }
        WindowsFastboot fastboot(exe);
        string serial, error;
        if (RecoveryJob::findConsole(fastboot, serial, error))
            cout << "fastboot: console " << serial << " - ready" << endl;
        else
            cout << "fastboot: " << error << endl;
        return serial.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (!quiet) {
        RecoveryWindowOptions o;
        o.backupFile = backup;
        o.workDir = work;
        return runRecoveryWindow(o);
    }
    if (backup.empty() || !yes)
        return usage();
    RecoveryOptions o;
    o.backupFile = backup;
    o.workDir = work;
    o.reboot = reboot;
    ULARGE_INTEGER free = {};
    if (GetDiskFreeSpaceExA(work.c_str(), &free, nullptr, nullptr))
        o.workDirFreeBytes = free.QuadPart;
    QuietListener listener;
    WindowsFastboot fastboot(WindowsFastboot::bundledPath());
    string error;
    bool ok = RecoveryJob::run(o, fastboot, listener, []() { return false; }, error);
    if (!ok)
        cout << "FAILED: " << error << endl;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
#else
    (void)quiet;
    (void)probe;
    (void)yes;
    (void)reboot;
    cout << "LastResortRecovery runs on Windows. Elsewhere, with the console in fastboot mode and the backup "
            "unpacked:\n"
            "  fastboot flash BOOTIMG1 boot.img\n"
            "  fastboot flash TEE1 tz.img\n"
            "  fastboot flash ROOTFS1 rootfs.ext4      (a full backup only)\n"
            "  fastboot flash USRDATA userdata.ext4\n"
            "  fastboot flash MISC recovery-off.img    (sixteen zero bytes)\n"
            "  fastboot reboot"
         << endl;
    return EXIT_FAILURE;
#endif
}
