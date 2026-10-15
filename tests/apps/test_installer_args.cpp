//
// installer_args: AutoBleemInstaller's command line - --bios with RetroArch is kept, without it a warning and a
// no-op, and --ps1-bios-only (removed) is an unknown option.
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

TEST_CASE("--ps1-bios-only is rejected as unknown") {
    Parsed p;
    CHECK_FALSE(installerargs::parse({"--quiet", "--drive", "F:", "--ps1-bios-only"}, p));
    Parsed q;
    CHECK_FALSE(installerargs::parse({"--ps1-bios-only", "--retroarch", "--bios"}, q));
}

TEST_CASE("a missing value and an unknown option are rejected") {
    Parsed p;
    CHECK_FALSE(installerargs::parse({"--drive"}, p));
    Parsed q;
    CHECK_FALSE(installerargs::parse({"--nonsense"}, q));
}
