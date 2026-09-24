//
// AutoBleem LAN Share: the owner's PS1 games shared with the AutoBleem Store on the home network, and a PS1 disc
// read from the PC's drive into the library (docs/lan-share-plan.md).
//
//   LanShare.exe                         the window
//   LanShare.exe --tray                  the same, started in the tray (Windows' start-up entry)
//   LanShare.exe --list-drives           the CD/DVD drives, one per line
//   LanShare.exe --read-disc D: <library folder> [--covers DIR] [--rdb FILE]
//                [--disc N [--folder DIR --title TITLE]]
//                                        a disc into <library>/<Title>/ (DiscReader over WinCdDrive), the
//                                        progress and the result on the console it was started from
//
#include "core/main.h"
#include "core/services/disc_reader.h"
#include "win32_window.h"
#include "win_cd_drive.h"

#include <ableem/engine/log.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include <windows.h>

using namespace std;

namespace {

// a GUI-subsystem exe has no console: the one it was started from, for the command-line modes - unless stdout
// already goes somewhere (a script's pipe or file), which stays
void attachParentConsole() {
    const DWORD type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    if ((type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    cout.setf(ios::unitbuf);
    cerr.setf(ios::unitbuf);
}

int usage() {
    cout << "USAGE: LanShare [--list-drives | --read-disc D: <library folder> [--covers DIR] [--rdb FILE]\n"
            "                 [--disc N [--folder DIR --title TITLE]]]\n";
    return EXIT_FAILURE;
}

const char *verdict(DiscReader::Verified v) {
    switch (v) {
    case DiscReader::Verified::Matches:
        return "matches the known good dump";
    case DiscReader::Verified::Differs:
        return "does NOT match the known good dump (a scratch, or another release of the game)";
    default:
        return "no known dump to compare with";
    }
}

int readDisc(int argc, char *argv[]) {
    if (argc < 4)
        return usage();
    WinCdDrive drive(argv[2]);
    DiscReader::Options o;
    o.libraryDir = argv[3];
    for (int i = 4; i < argc; i++) {
        const string a = argv[i];
        const string v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--covers")
            o.coversDir = v;
        else if (a == "--rdb")
            o.rdbFile = v;
        else if (a == "--disc")
            o.discNumber = atoi(v.c_str());
        else if (a == "--folder")
            o.gameFolder = v;
        else if (a == "--title")
            o.title = v;
        else
            return usage();
        i++;
    }
    int lastPercent = -1;
    const DiscReader::Result r = DiscReader::read(drive, o, [&](uint32_t done, uint32_t total) {
        const int percent = total == 0 ? 0 : static_cast<int>(100.0 * done / total);
        if (percent != lastPercent) {
            lastPercent = percent;
            cout << "\rreading " << percent << "% (" << done << "/" << total << " sectors)" << flush;
        }
        return true;
    });
    cout << "\n";
    if (!r.ok) {
        cout << "not read: " << r.error << "\n";
        return EXIT_FAILURE;
    }
    cout << r.title << (r.serial.empty() ? "" : " (" + r.serial + ")") << " -> " << r.cueFile << "\n"
         << "  " << r.sectors << " sectors, data track CRC " << hex << r.dataCrc << dec << ": " << verdict(r.verified)
         << "\n"
         << "  subchannel: "
         << (!r.subchannel ? "not given by this drive (a LibCrypt game will not work from this image)"
             : r.subchannelUnreliable ? "unreliable on this drive - no .sbi written"
             : r.sbiSectors > 0       ? to_string(r.sbiSectors) + " LibCrypt sectors, written to the .sbi"
                                      : "read, no LibCrypt marks")
         << "\n";
    if (!r.badSectors.empty())
        cout << "  " << r.badSectors.size() << " sectors could not be read and are zeros - clean the disc and read it "
             << "again\n";
    return r.badSectors.empty() ? EXIT_SUCCESS : 2;
}

int run(int argc, char *argv[]) {
    const string mode = argc > 1 ? argv[1] : "";
    if (mode.empty() || mode == "--tray") {
        ableem::Log::initConsoleOnly(plog::info);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        char utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
        return runLanShareWindow(mode == "--tray", utf8);
    }
    attachParentConsole();
    ableem::Log::initConsoleOnly(plog::warning);
    if (mode == "--list-drives") {
        for (const string &d : WinCdDrive::drives())
            cout << d << "\n";
        return EXIT_SUCCESS;
    }
    if (mode == "--read-disc")
        return readDisc(argc, argv);
    return usage();
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        PLOG_ERROR << "FATAL: unhandled exception: " << e.what();
    } catch (...) {
        PLOG_ERROR << "FATAL: unhandled exception of unknown type";
    }
    return EXIT_FAILURE;
}
