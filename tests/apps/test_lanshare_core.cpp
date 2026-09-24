//
// LAN Share's settings: the file round trip, the first LAN Share's file read, the local server they make.
//
#include "doctest/doctest.h"
#include "support/temp_dir.h"

#include "core/lanshare_settings.h"

#include <string>

using namespace std;

TEST_CASE("LanShareSettings: saved and read back; a missing file keeps the defaults") {
    TempDir tmp("lanshare");
    LanShareSettings s;
    CHECK_FALSE(s.load(tmp.at("none.ini")));
    CHECK(s.port == 8124);
    CHECK_FALSE(s.localServer); // the server is remote unless asked
    CHECK(s.keepInTray);

    s.serverUrl = "http://192.168.68.144:8126";
    s.token = "abc123";
    s.shareDir = "\\\\raspberrypi\\games";
    s.localFolder = "D:\\AB\\Games";
    s.localServer = true;
    s.port = 9000;
    s.name = "Living room";
    s.coversDir = "C:/AB/db";
    s.rdbFile = "C:/AB/Sony - PlayStation.rdb";
    s.keepInTray = false;
    REQUIRE(s.save(tmp.at("settings.ini")));

    LanShareSettings back;
    REQUIRE(back.load(tmp.at("settings.ini")));
    CHECK(back.serverUrl == "http://192.168.68.144:8126");
    CHECK(back.token == "abc123");
    CHECK(back.shareDir == "\\\\raspberrypi\\games");
    CHECK(back.localFolder == "D:\\AB\\Games");
    CHECK(back.localServer);
    CHECK(back.port == 9000);
    CHECK(back.name == "Living room");
    CHECK(back.coversDir == "C:/AB/db");
    CHECK(back.rdbFile == "C:/AB/Sony - PlayStation.rdb");
    CHECK_FALSE(back.keepInTray);

    // a hand-edited file: comments, CRLF, a bad port are passed over
    tmp.writeFile("hand.ini", "# mine\r\nport=99999\r\nserver=http://pi:8126\r\n");
    LanShareSettings hand;
    REQUIRE(hand.load(tmp.at("hand.ini")));
    CHECK(hand.port == 8124);
    CHECK(hand.serverUrl == "http://pi:8126");
}

TEST_CASE("LanShareSettings: the first LAN Share's folders give the folder on this PC") {
    TempDir tmp("lanshare1");
    tmp.writeFile("old.ini", "port=8125\ntray=1\nlibrary=Games|D:\\AB\\Games\nlibrary=Other|E:\\x\n");
    LanShareSettings s;
    REQUIRE(s.load(tmp.at("old.ini")));
    CHECK(s.localFolder == "D:\\AB\\Games");
    CHECK(s.port == 8125);
    CHECK_FALSE(s.localServer);
}

TEST_CASE("LanShareSettings: the local server they describe") {
    LanShareSettings s;
    s.localFolder = "C:\\games";
    s.coversDir = "C:\\db";
    ableem::LanServer::Config c = s.serverConfig("C:\\state", "2.0", "DESKTOP-1");
    CHECK(c.port == 8124);
    CHECK(c.name == "DESKTOP-1"); // no name set: the PC's
    CHECK(c.version == "2.0");
    CHECK(c.library.gamesDir == "C:\\games");
    CHECK(c.library.coversDir == "C:\\db");
    CHECK(c.library.stateDir == "C:\\state");
    CHECK_FALSE(c.uploads); // a PC shares read only
    s.name = "Mine";
    CHECK(s.serverConfig("", "", "DESKTOP-1").name == "Mine");
}
