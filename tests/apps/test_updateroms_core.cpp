//
// UpdateRomsJob: a console stick (or a Pi card) in a PC, scanned with the PC's network - a fake stick in a
// TempDir, a fake network as OnlineAssets's CommandRunner.
//
#include "doctest/doctest.h"

#include "support/env_fixture.h"
#include "support/rdb_builder.h"
#include "support/string_maker.h"
#include "support/temp_dir.h"

#include "core/console_log.h"
#include "core/update_roms_job.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/retroarch_playlist.h>
#include <ableem/engine/zip_writer.h>

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using ableem::DirEntry;
using std::string;
using std::vector;

namespace {

const char *const NES = "Nintendo - Nintendo Entertainment System";

// the job spells every path with forward slashes; the temp dir may not (C:\Users\... on the MSYS2 host)
string fwd(string path) {
    for (char &c : path)
        c = c == '\\' ? '/' : c;
    return path;
}

// a stick as the payload lays it out, with one installed core and one ROM; a console's (RetroBoot's
// folder) unless `pi`
struct FakeStick {
    explicit FakeStick(bool pi = false) : tmp("stick") {
        env.setWorkingPath(tmp.path());
        tmp.makeSubDir("Autobleem/bin/autobleem/platform");
        tmp.writeFile(
            "Autobleem/bin/autobleem/platform/psc.ini",
            "retroarch_dir=RetroArch/bin\nretroarch_roms_dir=RetroArch/roms\ndownload_command=\nusb_root=/media\n");
        tmp.writeFile("Autobleem/bin/autobleem/platform/rpi.ini",
                      "retroarch_dir=RetroArch\nretroarch_roms_dir=RetroArch/roms\nusb_root=/media/autobleem\n");
        tmp.writeFile("Autobleem/bin/autobleem/platform/pc.ini", "download_command=fetch %u %o\n");
        tmp.writeFile("Autobleem/bin/autobleem/platform/roms_folders.cfg", "Arcade=FBNeo - Arcade Games\n");
        tmp.makeSubDir("System/Logs");
        ra = pi ? "RetroArch" : "RetroArch/bin";
        if (pi)
            tmp.writeFile("RetroArch/retroarch.cfg", "# generated\n");
        else
            tmp.makeSubDir("RetroArch/bin");
        tmp.writeFile(ra + "/info/nestopia_libretro.info", "display_name = \"Nintendo - NES (Nestopia)\"\n"
                                                           "supported_extensions = \"nes|fds\"\n"
                                                           "database = \"" +
                                                               string(NES) + "\"\n");
        tmp.writeFile(ra + "/cores/nestopia_libretro.so", "core");
        tmp.makeSubDir(ra + "/playlists");
        tmp.makeSubDir(ra + "/database/rdb");
        tmp.makeSubDir(ra + "/thumbnails");
        roms = "RetroArch/roms";
        tmp.makeSubDir(roms + "/" + NES);
    }

    void addRom(const string &name, const string &bytes = "rom") {
        tmp.writeFile(roms + "/" + NES + "/" + name, bytes);
    }

    // the NES database, Adventures of Lolo by crc32("rom")
    void addDatabase() {
        test_support::Bytes records;
        test_support::appendRomRecord(records, "Adventures of Lolo (USA)", "Adventures of Lolo (USA).nes", 0x79520FA1u,
                                      "HAL", 1989, 1);
        records.push_back(0xc0);
        test_support::writeRdb(tmp, ra + "/database/rdb/" + NES + ".rdb", test_support::makeRdb(0, records));
    }

    ableem::RetroArchPlaylistEntries playlist() const {
        ableem::RetroArchPlaylistEntries entries;
        ableem::RetroArchPlaylist::load(tmp.at(ra + "/playlists/" + NES + ".lpl"), entries);
        return entries;
    }

    EnvFixture env;
    TempDir tmp;
    string ra, roms;
};

// the fake network: URL -> bytes; the command template is "fetch %u %o"
struct FakeServer {
    std::map<string, string> files;
    vector<string> urls;
    OnlineAssets::CommandRunner runner() {
        return [this](const string &commandLine) {
            size_t sp = commandLine.find(' ');
            size_t sp2 = commandLine.find(' ', sp + 1);
            string url = commandLine.substr(sp + 1, sp2 - sp - 1);
            string out = commandLine.substr(sp2 + 1);
            urls.push_back(url);
            auto it = files.find(url);
            if (it == files.end())
                return 22;
            std::ofstream o(out, std::ios::binary);
            o << it->second;
            return 0;
        };
    }
};

} // namespace

TEST_CASE("rootFromProgramPath: the first parent with Autobleem/bin/autobleem, else nothing") {
    TempDir tmp("root");
    tmp.makeSubDir("stick/Autobleem/bin/autobleem");
    tmp.makeSubDir("stick/UpdateRoms");
    CHECK(UpdateRomsJob::rootFromProgramPath(tmp.at("stick/UpdateRoms/UpdateRoms.exe")) == fwd(tmp.at("stick")));
    CHECK(UpdateRomsJob::rootFromProgramPath(tmp.at("stick/UpdateRoms.exe")) == fwd(tmp.at("stick")));
    CHECK(UpdateRomsJob::rootFromProgramPath(tmp.at("stick\\UpdateRoms\\UpdateRoms.exe")) == fwd(tmp.at("stick")));
    CHECK(UpdateRomsJob::rootFromProgramPath(tmp.at("elsewhere/UpdateRoms.exe")) == "");
    CHECK(UpdateRomsJob::rootFromProgramPath("UpdateRoms.exe") == "");
}

TEST_CASE("detect: a console stick by its RetroArch/bin, a Pi card by its retroarch.cfg, --target over both") {
    FakeStick console;
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(console.tmp.path(), setup, error));
    CHECK(setup.target == "psc");
    CHECK(setup.targetRoot == "/media");
    CHECK(setup.romsDir == fwd(console.tmp.at("RetroArch/roms")));
    CHECK(setup.playlistsDir == fwd(console.tmp.at("RetroArch/bin/playlists")));
    CHECK(setup.targetRomsDir == "/media/RetroArch/roms");
    CHECK(setup.targetRetroarchDir == "/media/RetroArch/bin");
    CHECK(setup.downloadCommand == "fetch %u %o"); // the PC's, never the console's empty one
    CHECK(setup.coresCfg == fwd(console.tmp.at("Autobleem/bin/autobleem/platform/psc.cores.cfg")));

    FakeStick pi(true);
    REQUIRE(UpdateRomsJob::detect(pi.tmp.path(), setup, error));
    CHECK(setup.target == "rpi");
    CHECK(setup.targetRoot == "/media/autobleem");
    CHECK(setup.romsDir == fwd(pi.tmp.at("RetroArch/roms")));
    CHECK(setup.targetRomsDir == "/media/autobleem/RetroArch/roms");

    // --target psc on a Pi card: the console layout says RetroArch/bin, the card has no bin/ - detect
    // rightly finds no RetroArch there
    CHECK_FALSE(UpdateRomsJob::detect(pi.tmp.path(), setup, error, "psc"));
    CHECK(error.find("No RetroArch") != string::npos);

    TempDir notAStick("nope");
    CHECK_FALSE(UpdateRomsJob::detect(notAStick.path(), setup, error));
    CHECK(error.find("Not an AutoBleem stick") != string::npos);
}

TEST_CASE("run: the playlists name the target's paths - ROMs and cores - and the databases name the games") {
    FakeStick stick;
    stick.addRom("lolo.nes");
    stick.addDatabase();
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));

    FakeServer server; // nothing on it: offline
    vector<string> said;
    UpdateRomsJob::Report report =
        UpdateRomsJob::run(setup, nullptr, nullptr, [&said](const string &l) { said.push_back(l); }, server.runner());
    CHECK_FALSE(report.online);
    CHECK(report.databases == 1);
    CHECK(report.games == 1);
    CHECK(report.identified == 1);
    CHECK(report.playlistsWritten == vector<string>{string(NES) + ".lpl"});
    CHECK(report.boxArtFetched == 0);
    CHECK(said.size() == report.lines.size());
    // no device named: detect() cannot tell a PC stick from a Pi card
    CHECK(said[0].find("Stick at ") == 0);
    CHECK(said[0].find("Raspberry Pi") == string::npos);
    CHECK(said[0].find("PlayStation Classic") == string::npos);
    CHECK(said[0].find("playlists will say /media)") != string::npos);

    ableem::RetroArchPlaylistEntries entries = stick.playlist();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == string("/media/RetroArch/roms/") + NES + "/lolo.nes");
    CHECK(entries[0].label == "Adventures of Lolo (USA)");
    CHECK(entries[0].core_path == "/media/RetroArch/bin/cores/nestopia_libretro.so");
    CHECK(entries[0].crc32 == "79520FA1|crc");
}

TEST_CASE("run: online, the databases bundle comes first and the box art after the scan") {
    FakeStick stick(true);
    stick.addRom("lolo.nes");
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));

    // the bundle: rdb/<system>.rdb, as buildbot packs it
    test_support::Bytes records;
    test_support::appendRomRecord(records, "Adventures of Lolo (USA)", "Adventures of Lolo (USA).nes", 0x79520FA1u);
    records.push_back(0xc0);
    test_support::Bytes rdb = test_support::makeRdb(0, records);
    ableem::ZipWriter zip;
    REQUIRE(zip.open(stick.tmp.at("bundle.zip")));
    REQUIRE(zip.addBytes(string("rdb/") + NES + ".rdb", string(rdb.begin(), rdb.end())));
    REQUIRE(zip.close());

    FakeServer server;
    server.files["https://thumbnails.libretro.com/"] = "<html>";
    server.files["https://buildbot.libretro.com/assets/frontend/database-rdb.zip"] = stick.tmp.readFile("bundle.zip");
    server.files[OnlineAssets::boxArtUrl("https://thumbnails.libretro.com", NES, "Adventures of Lolo (USA)")] = "png";

    UpdateRomsJob::Report report = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, server.runner());
    CHECK(report.online);
    CHECK(report.databases == 1);
    CHECK(report.identified == 1);
    CHECK(report.boxArtFetched == 1);
    CHECK(report.boxArtMissing == 0);
    CHECK(DirEntry::exists(
        stick.tmp.at(string("RetroArch/thumbnails/") + NES + "/Named_Boxarts/Adventures of Lolo (USA).png")));
    ableem::RetroArchPlaylistEntries entries = stick.playlist();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == string("/media/autobleem/RetroArch/roms/") + NES + "/lolo.nes");
    CHECK(entries[0].core_path == "/media/autobleem/RetroArch/cores/nestopia_libretro.so");

    // a second run: the databases are there, the cover is there - no request beyond the probe
    server.urls.clear();
    report = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, server.runner());
    CHECK(report.playlistsWritten.empty());
    CHECK(server.urls == vector<string>{"https://thumbnails.libretro.com/"});
}

TEST_CASE("run: the scan's digests live in the stick's state directory, where the launcher reads them") {
    FakeStick stick;
    stick.addRom("lolo.nes");
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));
    // the launcher's state dir on a stick is its working path, Autobleem/bin/autobleem
    CHECK(setup.scanStateFile == fwd(stick.tmp.at("Autobleem/bin/autobleem/roms.scanstate")));
    CHECK_FALSE(DirEntry::exists(setup.scanStateFile));

    UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(DirEntry::exists(stick.tmp.at("Autobleem/bin/autobleem/roms.scanstate")));
}

TEST_CASE("run: a second run leaves a folder that did not change alone, a changed one is scanned") {
    FakeStick stick;
    stick.addRom("lolo.nes");
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));

    UpdateRomsJob::Report first = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(first.systemsSkipped == 0);
    CHECK(first.games == 1);
    CHECK(stick.playlist().size() == 1);

    UpdateRomsJob::Report second = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(second.systemsSkipped == 1);
    CHECK(second.playlistsWritten.empty());
    CHECK(stick.playlist().size() == 1);

    stick.addRom("lolo2.nes");
    UpdateRomsJob::Report third = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(third.systemsSkipped == 0);
    CHECK(third.games == 2);
    CHECK(stick.playlist().size() == 2);
}

TEST_CASE("run: an entry a scan on this PC wrote with this PC's path is made to name the target's") {
    FakeStick stick;
    stick.addRom("lolo.nes");
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));
    ableem::RetroArchPlaylistEntry old;
    old.path = stick.tmp.at(string("RetroArch/roms/") + NES + "/lolo.nes");
    old.label = "lolo";
    old.core_path = "DETECT";
    old.core_name = "DETECT";
    old.crc32 = "00000000|crc";
    old.db_name = string(NES) + ".lpl";
    REQUIRE(ableem::RetroArchPlaylist::save(stick.tmp.at(string("RetroArch/bin/playlists/") + NES + ".lpl"), {old}));

    FakeServer server;
    UpdateRomsJob::Report report = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, server.runner());
    CHECK(report.playlistsWritten.size() == 1);
    ableem::RetroArchPlaylistEntries entries = stick.playlist();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == string("/media/RetroArch/roms/") + NES + "/lolo.nes");
    CHECK(entries[0].label == "lolo"); // kept: no database to say better
}

namespace {
// the occurrences of `what` in `text`
int countOf(const string &text, const string &what) {
    int n = 0;
    for (size_t at = text.find(what); at != string::npos; at = text.find(what, at + what.size()))
        n++;
    return n;
}
} // namespace

TEST_CASE("runOnConsole: --quiet prints the summary once and no INFO line, a normal run prints each line once") {
    // the harness already logs to the console; ConsoleLog is added beside it, writing to a buffer
    static ConsoleLog consoleLog;
    static bool logAdded = false;
    std::ostringstream captured;
    consoleLog.redirect(&captured);
    if (!logAdded) {
        REQUIRE(plog::get() != nullptr);
        plog::get()->addAppender(&consoleLog);
        logAdded = true;
    }
    FakeStick stick;
    stick.addRom("lolo.nes");
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));

    consoleLog.setQuiet(true);
    UpdateRomsJob::Report quiet = UpdateRomsJob::runOnConsole(setup, true, captured, nullptr, FakeServer().runner());
    PLOG_WARNING << "a warning still shows";
    PLOG_INFO << "an info line does not";
    const string quietOut = captured.str();

    captured.str("");
    consoleLog.setQuiet(false);
    UpdateRomsJob::Report loud = UpdateRomsJob::runOnConsole(setup, false, captured, nullptr, FakeServer().runner());
    const string loudOut = captured.str();
    consoleLog.redirect(nullptr);

    CHECK(quiet.summary == "1 games in 1 systems, 0 named by the databases, 1 playlists written, 0 folders unchanged");
    CHECK(countOf(quietOut, quiet.summary) == 1);
    CHECK(countOf(quietOut, "INFO") == 0);
    CHECK(countOf(quietOut, "an info line") == 0);
    CHECK(countOf(quietOut, "Stick at") == 0);
    CHECK(countOf(quietOut, "a warning still shows") == 1);

    // normal: every line of the job once (the log is the console), the summary among them
    CHECK(loud.summary == "1 games in 1 systems, 0 named by the databases, 0 playlists written, 1 folders unchanged");
    CHECK(countOf(loudOut, loud.summary) == 1);
    CHECK(countOf(loudOut, "Stick at ") == 1);
    for (const string &line : loud.lines)
        CHECK(countOf(loudOut, line) == 1);
}

TEST_CASE("run: a playlist follows its folder - a file added is in it, the file removed again is not") {
    FakeStick stick;
    stick.addRom("lolo.nes");
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));
    UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    REQUIRE(stick.playlist().size() == 1);

    stick.addRom("probe.nes", "0123456789abcdefg"); // 17 bytes
    UpdateRomsJob::Report added = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(added.playlistsWritten.size() == 1);
    CHECK(stick.playlist().size() == 2);

    REQUIRE(DirEntry::removeFile(stick.tmp.at(stick.roms + "/" + NES + "/probe.nes")));
    UpdateRomsJob::Report removed = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(removed.systemsSkipped == 0);
    CHECK(removed.playlistsWritten.size() == 1);
    ableem::RetroArchPlaylistEntries entries = stick.playlist();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == string("/media/RetroArch/roms/") + NES + "/lolo.nes");

    // and the run after that has nothing to do
    UpdateRomsJob::Report again = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(again.systemsSkipped == 1);
    CHECK(again.playlistsWritten.empty());

    // the last file gone: the playlist goes with it
    REQUIRE(DirEntry::removeFile(stick.tmp.at(stick.roms + "/" + NES + "/lolo.nes")));
    UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK_FALSE(DirEntry::exists(stick.tmp.at(stick.ra + "/playlists/" + NES + ".lpl")));
}

TEST_CASE("run: two folders feeding one playlist both stay 'unchanged' on the next run") {
    // "NES" is an alias of the NES system: its folder and the full-name one write the same .lpl, the
    // first-scanned folder's digest was taken before the second one changed the playlist's size
    FakeStick stick;
    stick.tmp.writeFile("Autobleem/bin/autobleem/platform/roms_folders.cfg", string("NES=") + NES + "\n");
    stick.tmp.makeSubDir("RetroArch/roms/NES");
    stick.tmp.writeFile("RetroArch/roms/NES/other.nes", "other rom bytes");
    stick.addRom("lolo.nes");
    UpdateRomsJob::Setup setup;
    string error;
    REQUIRE(UpdateRomsJob::detect(stick.tmp.path(), setup, error));

    UpdateRomsJob::Report first = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(first.systemsSkipped == 0);
    CHECK(stick.playlist().size() == 2);

    UpdateRomsJob::Report second = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(second.systemsSkipped == 2);
    CHECK(second.playlistsWritten.empty());

    // a file into one folder: the folders of that playlist are looked at, then it settles again
    stick.tmp.writeFile("RetroArch/roms/NES/third.nes", "third rom bytes!");
    UpdateRomsJob::Report third = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(third.playlistsWritten.size() == 1);
    CHECK(stick.playlist().size() == 3);
    UpdateRomsJob::Report fourth = UpdateRomsJob::run(setup, nullptr, nullptr, nullptr, FakeServer().runner());
    CHECK(fourth.systemsSkipped == 2);
    CHECK(fourth.playlistsWritten.empty());
}
