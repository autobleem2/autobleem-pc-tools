//
// The command line of AutoBleemInstaller, parsed apart from main() so the tests can run it: the options it sets,
// the one it rejects (anything not listed is unknown - --ps1-bios-only is gone), and the warning for --bios without
// --retroarch.
//
#pragma once

#include "installer/installer_job.h"

#include <string>
#include <vector>

namespace installerargs {

struct Parsed {
    bool quiet = false;
    std::string covers = "JUP";
    InstallOptions options;
    std::vector<std::string> warnings; // printed before the install starts
};

// args = argv[1..]; false on an unknown option or a missing value (the caller prints the usage)
bool parse(const std::vector<std::string> &args, Parsed &out);

// --bios without --retroarch fetches nothing: the PlayStation BIOS comes from the console (the bootstrap, the
// payload's OpenBIOS) and the packs go only into RetroArch's system folder. "" when there is nothing to warn about.
std::string biosWithoutRetroArchWarning(bool bios, bool retroarch);

} // namespace installerargs
