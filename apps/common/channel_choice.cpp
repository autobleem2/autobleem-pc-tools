//
// channel_choice - see the header.
//
#include "channel_choice.h"

#include "core/version.h"

using namespace std;
using ableem::ChannelCatalog;
using ableem::ChannelEntry;

namespace channelchoice {

ChannelCatalog fetch(const string &repoUrl, Downloader &downloader, const string &scratchDir) {
    string text, error;
    ChannelCatalog catalog;
    if (downloader.fetchText(repoUrl + "/channels.json", scratchDir + "/channels.json", text, error) &&
        catalog.parse(text))
        return catalog;
    return ChannelCatalog::builtIn();
}

string label(const ChannelEntry &entry) {
    if (entry.id == "release")
        return "Release";
    if (entry.id == "testing")
        return "Testing (the next release)";
    if (entry.id == "nightly")
        return "Nightly (development build)";
    return entry.label + (entry.unstable ? " (unstable)" : "");
}

string builtFor() {
    // a preview build carries its name in the tag or in the describe
    const string version = string(Version::VERSION).rfind("preview-", 0) == 0 ? Version::VERSION : Version::DESCRIBE;
    return ChannelCatalog::wantedFor(version, Version::isBetweenTags(), Version::isPreRelease());
}

size_t initialIndex(const ChannelCatalog &catalog, const string &wanted) {
    for (size_t i = 0; i < catalog.channels.size(); i++)
        if (catalog.channels[i].id == wanted)
            return i;
    return 0;
}

} // namespace channelchoice
