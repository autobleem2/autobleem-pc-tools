//
// installer_args: AutoBleemInstaller's command line - --bios with RetroArch is kept, without it a warning and a
// no-op; biosWanted decides the same for the Windows setup, where RetroArch may already be installed.
//
#include "doctest/doctest.h"

#include "installer_args.h"

#include <string>
#include <vector>

using namespace std;
using installerargs::Parsed;

TEST_CASE("--bios with --retroarch is kept, with no warning") {
    Parsed p;
    REQUIRE(installerargs::parse({"--quiet", "--drive", "F:", "--retroarch", "--bios"}, p));
    CHECK(p.options.retroarch);
    CHECK(p.options.bios);
    CHECK(p.warnings.empty());
}

TEST_CASE("--bios without --retroarch is a no-op and warns") {
    Parsed p;
    REQUIRE(installerargs::parse({"--quiet", "--drive", "F:", "--bios"}, p));
    CHECK_FALSE(p.options.retroarch);
    CHECK_FALSE(p.options.bios);
    REQUIRE(p.warnings.size() == 1);
    CHECK(p.warnings[0].find("--bios") != string::npos);
    CHECK(p.warnings[0].find("--retroarch") != string::npos);
}

TEST_CASE("no --bios, no warning") {
    Parsed p;
    REQUIRE(installerargs::parse({"--quiet", "--drive", "F:", "--retroarch"}, p));
    CHECK_FALSE(p.options.bios);
    CHECK(p.warnings.empty());
}

TEST_CASE("the BIOS pack is wanted only with RetroArch, chosen or already there") {
    CHECK(installerargs::biosWanted(true, true, false));
    CHECK(installerargs::biosWanted(true, false, true));
    CHECK(installerargs::biosWanted(true, true, true));
    CHECK_FALSE(installerargs::biosWanted(true, false, false));
    CHECK_FALSE(installerargs::biosWanted(false, true, true));
    CHECK_FALSE(installerargs::biosWanted(false, false, false));
}

TEST_CASE("a missing value and an unknown option are rejected") {
    Parsed p;
    CHECK_FALSE(installerargs::parse({"--drive"}, p));
    Parsed q;
    CHECK_FALSE(installerargs::parse({"--nonsense"}, q));
}
