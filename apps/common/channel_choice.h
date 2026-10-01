//
// The release channels the PC installers offer, read from the download site's channels.json (the Installer and
// the Flasher both): fetched once, quietly falling back to the three channels the programs always knew, with the
// wording each shows and the one a program starts on.
//
#pragma once

#include "installer/installer_job.h"

#include <ableem/engine/update_catalog.h>

#include <string>

namespace channelchoice {

// <repoUrl>/channels.json; the built-in release/testing/nightly when it cannot be fetched or is not a channel list.
// `scratchDir` is where the small file lands first.
ableem::ChannelCatalog fetch(const std::string &repoUrl, Downloader &downloader, const std::string &scratchDir);

// what the channel box shows: the three known channels keep their wording, any other is its label from the site,
// "(unstable)" after it when the site marks it so
std::string label(const ableem::ChannelEntry &entry);

// the channel id this program's own build wants (Version): "preview" for a preview build, else nightly / testing /
// release
std::string builtFor();

// the position of `wanted` in the catalog, else 0 (the first entry)
size_t initialIndex(const ableem::ChannelCatalog &catalog, const std::string &wanted);

} // namespace channelchoice
