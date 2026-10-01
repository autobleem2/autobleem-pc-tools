//
// channel_choice: the channels the PC Installer and Flasher list - read from the site's channels.json through a fake
// downloader (no network), the built-in three when that fails, the labels, and the channel a program starts on.
//
#include "doctest/doctest.h"
#include "support/temp_dir.h"

#include "channel_choice.h"

#include <fstream>
#include <map>
#include <string>

using namespace std;
using ableem::ChannelCatalog;
using ableem::ChannelEntry;

namespace {

const char *const Site = "http://site";

// a site that is a map of url -> text; anything else is a 404
class FakeSite : public Downloader {
public:
    map<string, string> pages;
    int fetches = 0;
    bool fetch(const string &url, const string &destFile, const Progress &, string &error) override {
        fetches++;
        auto it = pages.find(url);
        if (it == pages.end()) {
            error = url + ": HTTP 404";
            return false;
        }
        ofstream(destFile, ios::binary) << it->second;
        return true;
    }
};

const char *const FourChannels = R"json({"version": 1, "channels": [
    {"id": "release", "label": "Release", "index": "releases/latest.json", "unstable": false},
    {"id": "testing", "label": "Testing", "index": "releases/unstable.json", "unstable": false},
    {"id": "nightly", "label": "Nightly", "index": "nightly/latest.json", "unstable": true},
    {"id": "preview", "label": "Preview (feature-x)", "index": "preview/latest.json", "unstable": true}]})json";

} // namespace

TEST_CASE("the site's channels.json fills the list, preview included") {
    TempDir tmp("channel_choice");
    FakeSite site;
    site.pages[string(Site) + "/channels.json"] = FourChannels;
    const ChannelCatalog c = channelchoice::fetch(Site, site, tmp.path());
    REQUIRE(c.channels.size() == 4);
    CHECK(c.channels[3].id == "preview");
    CHECK(c.lists("preview", false).front() == "preview/latest.json");
}

TEST_CASE("no channels.json, or a bad one: the built-in three, quietly") {
    TempDir tmp("channel_choice_fallback");
    FakeSite offline; // every request is a 404
    ChannelCatalog c = channelchoice::fetch(Site, offline, tmp.path());
    REQUIRE(c.channels.size() == 3);
    CHECK(c.channels[0].id == "release");
    CHECK(c.channels[1].id == "testing");
    CHECK(c.channels[2].id == "nightly");
    CHECK(c.lists("nightly", false).front() == "nightly/latest.json");

    for (const char *bad : {"<html>not json</html>", "{}", R"({"channels": []})", R"({"channels": "x"})", ""}) {
        FakeSite broken;
        broken.pages[string(Site) + "/channels.json"] = bad;
        c = channelchoice::fetch(Site, broken, tmp.path());
        CHECK(c.channels.size() == 3);
    }
}

TEST_CASE("the labels: the known three as before, others from the site with (unstable) when it says so") {
    ChannelCatalog c;
    REQUIRE(c.parse(FourChannels));
    CHECK(channelchoice::label(c.channels[0]) == "Release");
    CHECK(channelchoice::label(c.channels[1]) == "Testing (the next release)");
    CHECK(channelchoice::label(c.channels[2]) == "Nightly (development build)");
    CHECK(channelchoice::label(c.channels[3]) == "Preview (feature-x) (unstable)");
    ChannelEntry stable;
    stable.id = "beta";
    stable.label = "Beta";
    CHECK(channelchoice::label(stable) == "Beta");
}

TEST_CASE("the starting channel: the one this build is, else the first listed") {
    ChannelCatalog c;
    REQUIRE(c.parse(FourChannels));
    CHECK(channelchoice::initialIndex(c, "preview") == 3);
    CHECK(channelchoice::initialIndex(c, "testing") == 1);
    CHECK(channelchoice::initialIndex(c, "gone") == 0); // not on the site: the first entry
    CHECK(channelchoice::initialIndex(ChannelCatalog(), "release") == 0);
    // what a build wants: a preview-... version is the preview channel, whatever else is true of it
    CHECK(ChannelCatalog::wantedFor("preview-feature-ab-gui-a880c7", true, false) == "preview");
    CHECK(ChannelCatalog::wantedFor("v2.0.0-alpha2-6-gba7365c", true, true) == "nightly");
    CHECK(ChannelCatalog::wantedFor("v2.0.0-alpha2", false, true) == "testing");
    CHECK(ChannelCatalog::wantedFor("v2.0.0", false, false) == "release");
    CHECK_FALSE(channelchoice::builtFor().empty());
}
