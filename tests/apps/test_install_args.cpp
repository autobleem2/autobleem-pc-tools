//
// install_args: the installer's command line into InstallOptions - the flags, the implications, what is refused.
//
#include "doctest/doctest.h"

#include "install_args.h"

#include <string>
#include <vector>

using namespace std;

namespace {

bool parse(const vector<string> &words, InstallArgs &args) {
    vector<char *> argv;
    string program = "AutoBleemInstaller";
    argv.push_back(&program[0]);
    vector<string> copy = words;
    for (auto &w : copy)
        argv.push_back(&w[0]);
    return parseInstallArgs((int)argv.size(), argv.data(), args);
}

} // namespace

TEST_CASE("install_args: no flags leave the options at their defaults") {
    InstallArgs args;
    REQUIRE(parse({}, args));
    CHECK_FALSE(args.quiet);
    CHECK_FALSE(args.options.bios);
    CHECK_FALSE(args.options.ps1BiosOnly);
    CHECK_FALSE(args.options.force);
}

TEST_CASE("install_args: --bios sets bios alone") {
    InstallArgs args;
    REQUIRE(parse({"--bios"}, args));
    CHECK(args.options.bios);
    CHECK_FALSE(args.options.ps1BiosOnly);
}

TEST_CASE("install_args: --ps1-bios-only implies --bios") {
    InstallArgs args;
    REQUIRE(parse({"--ps1-bios-only"}, args));
    CHECK(args.options.bios);
    CHECK(args.options.ps1BiosOnly);
}

TEST_CASE("install_args: --bios --ps1-bios-only in either order") {
    InstallArgs a, b;
    REQUIRE(parse({"--bios", "--ps1-bios-only"}, a));
    REQUIRE(parse({"--ps1-bios-only", "--bios"}, b));
    CHECK(a.options.bios);
    CHECK(a.options.ps1BiosOnly);
    CHECK(b.options.bios);
    CHECK(b.options.ps1BiosOnly);
}

TEST_CASE("install_args: --force sets force and nothing else") {
    InstallArgs args;
    REQUIRE(parse({"--force"}, args));
    CHECK(args.options.force);
    CHECK_FALSE(args.options.bios);
}

TEST_CASE("install_args: the existing flags still parse") {
    InstallArgs args;
    REQUIRE(parse({"--quiet", "--drive", "F:", "--covers", "U", "--retroarch", "--samples", "--channel", "testing"}, args));
    CHECK(args.quiet);
    CHECK(args.options.root == "F:");
    CHECK(args.options.coversUsa);
    CHECK_FALSE(args.options.coversJapan);
    CHECK_FALSE(args.options.coversPal);
    CHECK(args.options.retroarch);
    CHECK(args.options.samples);
    CHECK(args.options.channel == "testing");
}

TEST_CASE("install_args: an unknown flag or a missing value is refused") {
    InstallArgs args;
    CHECK_FALSE(parse({"--nonsense"}, args));
    CHECK_FALSE(parse({"--drive"}, args));
}
