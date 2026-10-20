//
// retroboot_offer: the installer asks about RetroBoot's leftovers only when the stick has the tree - in the new
// layout (RetroArch/bin/retroboot) or, before the old layout is brought up to date, at retroarch/retroboot. A bare
// RetroArch/bin/apps, a link and an empty stick are no reason to ask. Also the size and the words of the question.
//
#include "doctest/doctest.h"
#include "support/temp_dir.h"

#include "retroboot_offer.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <string>

using namespace std;
using namespace retrobootoffer;

TEST_CASE("RetroArch/bin/retroboot on the stick: the question is offered") {
    TempDir tmp("rbo_new");
    tmp.writeFile("RetroArch/bin/retroboot/es/settings.xml", "x");
    CHECK(offerRemoveRetroBoot(tmp.path(), false));
    CHECK(offerRemoveRetroBoot(tmp.path(), true));
}

TEST_CASE("the folder names are matched without regard to case") {
    TempDir tmp("rbo_case");
    tmp.writeFile("retroarch/BIN/RetroBoot/a", "x");
    CHECK(offerRemoveRetroBoot(tmp.path(), false));
}

TEST_CASE("an old-layout stick: retroarch/retroboot counts only when the layout is the old one") {
    TempDir tmp("rbo_old");
    tmp.writeFile("retroarch/retroboot/bin/launch_rfa.sh", "x");
    CHECK(offerRemoveRetroBoot(tmp.path(), true));
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), false));
}

TEST_CASE("no RetroBoot: no question - an empty stick, RetroArch alone, bin/apps alone") {
    TempDir tmp("rbo_none");
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), false));
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), true));
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path() + "/not-there", false));
    CHECK_FALSE(offerRemoveRetroBoot("", false));
    tmp.writeFile("RetroArch/bin/retroarch", "x");
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), false));
    tmp.writeFile("RetroArch/bin/apps/game/run.sh", "x");
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), false));
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), true));
    CHECK(retroBootBytes(tmp.path(), false) == 0);
}

TEST_CASE("retroboot that is a file is not a tree") {
    TempDir tmp("rbo_file");
    tmp.writeFile("RetroArch/bin/retroboot", "x");
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), false));
}

#ifndef _WIN32
TEST_CASE("a link in place of retroboot is not offered, and a link inside is not counted or followed") {
    TempDir tmp("rbo_link");
    tmp.writeFile("elsewhere/big.bin", string(5000, 'x'));
    tmp.makeSubDir("RetroArch/bin");
    REQUIRE(symlink(tmp.at("elsewhere").c_str(), tmp.at("RetroArch/bin/retroboot").c_str()) == 0);
    CHECK_FALSE(offerRemoveRetroBoot(tmp.path(), false));

    TempDir inner("rbo_link2");
    inner.writeFile("outside/big.bin", string(5000, 'x'));
    inner.writeFile("RetroArch/bin/retroboot/own.bin", string(100, 'y'));
    REQUIRE(symlink(inner.at("outside").c_str(), inner.at("RetroArch/bin/retroboot/link").c_str()) == 0);
    CHECK(offerRemoveRetroBoot(inner.path(), false));
    CHECK(retroBootBytes(inner.path(), false) == 100);
}
#endif

TEST_CASE("the size is the files of the retroboot tree, rounded to MB, never a fixed number") {
    TempDir tmp("rbo_size");
    tmp.writeFile("RetroArch/bin/retroboot/a.bin", string(3 * 1024 * 1024, 'x'));
    tmp.writeFile("RetroArch/bin/retroboot/sub/b.bin", string(1024 * 1024, 'x'));
    tmp.writeFile("RetroArch/bin/retroarch", string(9 * 1024 * 1024, 'x')); // not RetroBoot's
    CHECK(retroBootBytes(tmp.path(), false) == 4u * 1024 * 1024);
    CHECK(sizeText(4u * 1024 * 1024) == "about 4 MB");
    CHECK(sizeText(10) == "about 1 MB");
    CHECK(sizeText(612u * 1024 * 1024 + 300000) == "about 612 MB");
}

TEST_CASE("a non-ASCII file name inside the tree is counted and does not stop the check") {
    TempDir tmp("rbo_utf8");
    tmp.writeFile("RetroArch/bin/retroboot/gry/Pok\xC3\xA9mon \xE2\x80\x93 \xC5\x81\xC3\xB3"
                  "d\xC5\xBA.txt", "abc");
    CHECK(offerRemoveRetroBoot(tmp.path(), false));
    CHECK(retroBootBytes(tmp.path(), false) == 3);
}

TEST_CASE("the words name the folders the job removes, with the drive and the computed size") {
    const string text = confirmationText("F:", 612u * 1024 * 1024);
    CHECK(text.find("from F:?") != string::npos);
    CHECK(text.find("F:\\RetroArch\\bin\\retroboot") != string::npos);
    CHECK(text.find("F:\\RetroArch\\bin\\apps") != string::npos);
    CHECK(text.find("about 612 MB") != string::npos);
    CHECK(text.find("cannot be undone") != string::npos);
    CHECK(text.find("600") == string::npos);
    CHECK(confirmationText("F:\\", 1).find("from F:?") != string::npos);
    CHECK(checkboxLabel(612u * 1024 * 1024).find("about 612 MB") != string::npos);
    CHECK(helperLine().find("RetroArch\\bin\\retroboot") != string::npos);
}
