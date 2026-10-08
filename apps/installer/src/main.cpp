//
// The AutoBleem installer for the PlayStation Classic: a stick from a PC. The stick's file system is the
// chosen channel's package on the download site (release, testing, nightly, preview - the site's channels.json
// says which), the rest - the cover databases,
// RetroArch and what goes with it, the BIOS files, the sample games - from the download repository, as
// asked. Run again over a stick that has AutoBleem, it updates it and leaves the user's games and data.
//
//   AutoBleemInstaller.exe                       the window: pick the stick, answer the questions, Install
//   AutoBleemInstaller.exe --quiet --drive F:    no window, the same on the console it was started from:
//       [--channel release|testing|nightly|preview] [--covers JUP] [--retroarch] [--bios] [--samples]
//       [--ps1-bios-only] [--force] [--package FILE] [--repo URL]
//       --ps1-bios-only (implies --bios) fetches only the PlayStation BIOS files, nothing else
//       --force reinstalls even when the stick already carries the package's version
//       --channel defaults to the installer's own kind of build; --package installs a local file instead
//       --covers names the cover databases to fetch (J, U, P - the default is all three; "" for none)
//
// On Windows a plain Win32 window (win32_window.cpp), statically linked: one file, nothing to install.
// Elsewhere (a Linux or macOS build) there is the --quiet path only, and a download command instead of
// WinINet.
//
#include "installer/installer_job.h"
#include "channel_choice.h"
#include "install_args.h"
#include "core/services/environment.h"
#include "core/version.h"
#include "win32_platform.h"
#include "win32_window.h"

#include <ableem/engine/log.h>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace std;

namespace {

//******************
// QuietListener
//******************
class QuietListener : public InstallListener {
public:
    void onPhase(int index, int total, const string &title) override {
        cout << "[" << index << "/" << total << "] " << title << endl;
    }
    void onProgress(uint64_t, uint64_t) override {}
    void onLine(const string &line) override { cout << line << endl; }
};

#ifndef _WIN32
//******************
// CommandDownloader
//******************
// curl, which every Linux and macOS box has
class CommandDownloader : public Downloader {
public:
    bool fetch(const string &url, const string &destFile, const Progress &, string &error) override {
        string cmd = "curl -sfL -o \"" + destFile + "\" \"" + url + "\"";
        if (system(cmd.c_str()) == 0)
            return true;
        error = "curl failed for " + url;
        return false;
    }
};
#endif

int usage() {
    cout << "USAGE: AutoBleemInstaller [--quiet --drive F: [--covers JUP] [--retroarch] [--bios] [--samples]]\n"
            "                          [--ps1-bios-only] [--force]\n"
            "                          [--channel release|testing|nightly|preview] [--package FILE] [--repo URL]"
         << endl;
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char *argv[]) {
    InstallArgs args;
    if (!parseInstallArgs(argc, argv, args))
        return usage();
    const bool quiet = args.quiet;
    InstallOptions &options = args.options;
    // the channel the stick package comes from: the one the installer itself was built on, unless asked
    // (the window offers the site's others); a --package file is installed as it is
    if (options.channel.empty())
        options.channel = channelchoice::builtFor();
    if (!options.packageFile.empty())
        options.channel.clear();

#ifdef _WIN32
    if (quiet)
        attachParentConsole();
#endif
    ableem::Log::initConsoleOnly();
    PLOG_INFO << "AutoBleem installer " << Env::productVersion() << " (" << Version::FULL_VERSION << ")" << ", "
              << (options.packageFile.empty() ? "the " + options.channel + " channel" : options.packageFile);

    if (!quiet) {
#ifdef _WIN32
        return runInstallerWindow(options);
#else
        cout << "no window on this platform - use --quiet --drive <root>" << endl;
        return usage();
#endif
    }
    if (options.root.empty())
        return usage();
    QuietListener listener;
    string error;
#ifdef _WIN32
    WinInetDownloader downloader;
    if (options.root.size() >= 2 && options.root[1] == ':' && !ensureVolumeLabel(options.root, "SONY", error))
        cout << "Note: " << error << " - the console expects a stick named SONY" << endl;
#else
    CommandDownloader downloader;
#endif
    if (options.packageFile.empty()) {
        // the channel's lists from the site's channels.json (any id it names); not there = the built-in names
        const string scratch =
            (options.scratchDir.empty() ? InstallerJob::normalizeRoot(options.root) + "/System/Install"
                                        : options.scratchDir);
        options.channelIndexes =
            channelchoice::fetch(options.repoUrl, downloader, scratch).lists(options.channel, false);
    }
    bool ok = InstallerJob::run(options, downloader, listener, []() { return false; }, error);
    if (!ok)
        cout << "FAILED: " << error << endl;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
