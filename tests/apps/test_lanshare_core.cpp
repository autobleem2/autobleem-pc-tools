//
// LAN Share's settings: the file round trip, the library names and what may be added, the server they make.
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
    CHECK(s.keepInTray);

    s.port = 9000;
    s.name = "Living room";
    s.coversDir = "C:/AB/db";
    s.rdbFile = "C:/AB/Sony - PlayStation.rdb";
    s.keepInTray = false;
    s.libraries = {{"PS1", "D:\\Games\\PS1"}, {"Rips", "E:\\Rips"}};
    REQUIRE(s.save(tmp.at("settings.ini")));

    LanShareSettings back;
    REQUIRE(back.load(tmp.at("settings.ini")));
    CHECK(back.port == 9000);
    CHECK(back.name == "Living room");
    CHECK(back.coversDir == "C:/AB/db");
    CHECK(back.rdbFile == "C:/AB/Sony - PlayStation.rdb");
    CHECK_FALSE(back.keepInTray);
    REQUIRE(back.libraries.size() == 2);
    CHECK(back.libraries[1].name == "Rips");
    CHECK(back.libraries[1].dir == "E:\\Rips");

    // a hand-edited file: comments, CRLF, a bad port and a broken library line are passed over
    tmp.writeFile("hand.ini", "# mine\r\nport=99999\r\nlibrary=|D:\\x\r\nlibrary=Good|D:\\good\r\n");
    LanShareSettings hand;
    REQUIRE(hand.load(tmp.at("hand.ini")));
    CHECK(hand.port == 8124);
    REQUIRE(hand.libraries.size() == 1);
    CHECK(hand.libraries[0].dir == "D:\\good");
}

TEST_CASE("LanShareSettings: a new library's name, and the folders that may not be added") {
    LanShareSettings s;
    CHECK(s.nameFor("D:\\Games\\PS1\\") == "PS1");
    CHECK(s.nameFor("D:\\") == "Games");
    s.libraries = {{"PS1", "D:\\Games\\PS1"}};
    CHECK(s.nameFor("E:\\Other\\ps1") == "ps1 (2)"); // any case is the same name
    CHECK(s.nameFor("E:\\a|b") == "ab");

    string why;
    CHECK(s.canAdd("E:\\Rips", why));
    CHECK_FALSE(s.canAdd("d:/games/ps1/", why)); // the same folder, spelt otherwise
    CHECK(why.find("already shared") != string::npos);
    CHECK_FALSE(s.canAdd("D:\\Games\\PS1\\RPG", why));
    CHECK(why.find("inside") != string::npos);
    CHECK_FALSE(s.canAdd("D:\\Games", why));
    CHECK(why.find("remove it first") != string::npos);
    CHECK(s.canAdd("D:\\Games\\PS10", why)); // a longer name is not inside
}

TEST_CASE("LanShareSettings: the server they describe") {
    LanShareSettings s;
    s.libraries = {{"A", "C:\\a"}, {"B", "C:\\b"}};
    s.coversDir = "C:\\db";
    ableem::LanServer::Config c = s.serverConfig("C:\\state", "2.0", "DESKTOP-1");
    CHECK(c.port == 8124);
    CHECK(c.name == "DESKTOP-1"); // no name set: the PC's
    CHECK(c.version == "2.0");
    REQUIRE(c.library.roots.size() == 2);
    CHECK(c.library.roots[1].name == "B");
    CHECK(c.library.coversDir == "C:\\db");
    CHECK(c.library.stateDir == "C:\\state");
    s.name = "Mine";
    CHECK(s.serverConfig("", "", "DESKTOP-1").name == "Mine");
}
