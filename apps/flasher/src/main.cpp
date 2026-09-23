//
// AutoBleemFlasher: the PC USB stick's image onto a stick, from Windows. The image is a channel's from the
// download site (release, testing or nightly) or an .img.xz on this PC; it is decoded as it is written,
// and read back and compared afterwards. The job is autobleem-core's FlasherJob; this is the program
// around it - the window, or a line for scripts. Writing a whole disk needs administrator rights, which
// the manifest asks for (the one AutoBleem program that does).
//
//   AutoBleemFlasher.exe                               the window
//   AutoBleemFlasher.exe --list                        the disks it would offer, and those it would not
//   AutoBleemFlasher.exe --quiet --disk N --yes        no window: write disk N
//       [--channel release|testing|nightly | --image FILE.img.xz] [--no-verify] [--repo URL]
//       [--allow-fixed]  a USB hard drive too (left out otherwise: only a stick or a card is offered)
//
#include "installer/flasher_job.h"
#include "core/services/environment.h"
#include "core/version.h"

#include "../../installer/src/win32_platform.h"
#include "flasher_window.h"
#include "win32_disk.h"

#include <ableem/engine/log.h>

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace std;

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
    cout << "USAGE: AutoBleemFlasher [--list]\n"
            "       AutoBleemFlasher --quiet --disk N --yes [--channel release|testing|nightly | --image FILE]\n"
            "                        [--no-verify] [--repo URL] [--allow-fixed]"
         << endl;
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char *argv[]) {
    bool quiet = false, list = false, yes = false, allowFixed = false;
    int diskNumber = -1;
    FlashOptions options;
    // the channel the flasher itself was built on, unless asked
    options.channel = Version::isBetweenTags() ? "nightly" : Version::isPreRelease() ? "testing" : "release";
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        auto value = [&](string &into) {
            if (i + 1 >= argc)
                return false;
            into = argv[++i];
            return true;
        };
        string number;
        if (arg == "--quiet")
            quiet = true;
        else if (arg == "--list")
            list = true;
        else if (arg == "--yes")
            yes = true;
        else if (arg == "--allow-fixed")
            allowFixed = true;
        else if (arg == "--no-verify")
            options.verify = false;
        else if (arg == "--disk" && value(number))
            diskNumber = atoi(number.c_str());
        else if (arg == "--channel" && value(options.channel)) {
        } else if (arg == "--image" && value(options.imageFile)) {
        } else if (arg == "--repo" && value(options.repoUrl)) {
        } else
            return usage();
    }
    for (char &c : options.imageFile)
        if (c == '\\')
            c = '/';

#ifdef _WIN32
    if (quiet || list)
        attachParentConsole();
    ableem::Log::initConsoleOnly();
    PLOG_INFO << "AutoBleem flasher " << Env::productVersion() << " (" << Version::FULL_VERSION << ")";
    if (list) {
        vector<string> notes;
        for (const PhysicalDisk &d : listTargetDisks(&notes, true))
            cout << d.number << ": " << d.describe() << endl;
        for (const string &note : notes)
            cout << "   not offered - " << note << endl;
        return EXIT_SUCCESS;
    }
    if (!quiet)
        return runFlasherWindow(options);
    if (diskNumber < 0 || !yes)
        return usage();
    for (const PhysicalDisk &d : listTargetDisks(nullptr, allowFixed)) {
        if (d.number != diskNumber)
            continue;
        char temp[MAX_PATH] = {0};
        options.scratchDir = string(temp, GetTempPathA(MAX_PATH, temp)) + "AutoBleemFlasher";
        cout << "Writing onto " << d.describe() << endl;
        QuietListener listener;
        WinInetDownloader downloader;
        WindowsDisk target(d);
        string error;
        bool ok = FlasherJob::run(options, downloader, target, listener, []() { return false; }, error);
        if (!ok)
            cout << "FAILED: " << error << endl;
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    cout << "Disk " << diskNumber << " is not one the flasher writes to (see --list)" << endl;
    return EXIT_FAILURE;
#else
    (void)quiet;
    (void)list;
    (void)yes;
    (void)diskNumber;
    (void)allowFixed;
    cout << "the flasher writes disks on Windows only - elsewhere: xzcat IMAGE.img.xz | sudo dd of=/dev/sdX bs=4M"
         << endl;
    return EXIT_FAILURE;
#endif
}
