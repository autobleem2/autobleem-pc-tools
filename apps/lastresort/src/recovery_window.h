//
// LastResortRecovery's window: the installer's look (a picture on top, the page under it) as a wizard of
// four pages - the backup (found on a stick, or browsed for), putting the console into fastboot mode (the
// board with the pads marked, and the console's state as Windows sees it, the driver installed from here),
// the recovery's progress, and the question the whole thing is for: does the console start again.
//
#pragma once

#include <string>

#ifdef _WIN32
struct RecoveryWindowOptions {
    std::string backupFile; // preselected (--backup), "" to look for one
    std::string workDir;    // the scratch folder: the unpacked images, the driver package, the log
};
int runRecoveryWindow(const RecoveryWindowOptions &options);
#endif
