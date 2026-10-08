//
// The installer's command line: the words after the program name into InstallOptions (any platform - the tests in
// tests/apps run it without a window or a stick).
//
#pragma once

#include "installer/installer_job.h"

struct InstallArgs {
    bool quiet = false;
    InstallOptions options;
};

// false for an unknown word or a flag without its value. --ps1-bios-only implies --bios. The covers letters
// (--covers, J U P, default all three) end up in options.coversJapan / coversUsa / coversPal.
bool parseInstallArgs(int argc, char *argv[], InstallArgs &args);
