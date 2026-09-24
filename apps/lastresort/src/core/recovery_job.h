//
// The recovery itself, with the console already in fastboot mode: the backup checked, the console's
// partitions checked against it, the images unpacked into a scratch folder, each flashed to its partition,
// then MISC cleared (Sony's recovery off - abflashkit's recovery-off.img, sixteen zero bytes) and the
// console restarted. Nothing is flashed until every check has passed; once the first image is being
// written a stop request is refused, since a half-written console is worse than either end.
//
#pragma once

#include "fastboot.h"
#include "lboot_image.h"

#include "installer/installer_job.h" // InstallListener

#include <functional>
#include <string>
#include <vector>

//******************
// RecoveryOptions
//******************
struct RecoveryOptions {
    std::string backupFile;        // the LBOOT.EPB
    std::string workDir;           // scratch: the images go into <workDir>/images, removed afterwards
    uint64_t workDirFreeBytes = 0; // room left there; 0 = not known, not checked
    bool reboot = true;            // `fastboot reboot` at the end
};

//******************
// RecoveryJob
//******************
class RecoveryJob {
public:
    using ShouldStop = std::function<bool()>;

    static const char *const MiscPartition;   // "MISC"
    static const char *const RecoveryOffFile; // "recovery-off.img"
    static const size_t RecoveryOffSize = 16; // abflashkit's recovery-off.img: sixteen zero bytes

    // the titles the listener gets, in order - for a preview
    static std::vector<std::string> phasesFor(const LbootImage &image, bool reboot);

    // the console in fastboot mode, as `fastboot devices` lists it: true with its serial, false with why not
    static bool findConsole(Fastboot &fastboot, std::string &serial, std::string &error);

    // the recovery; false with `error` at the first failure (the scratch folder is removed either way)
    static bool run(const RecoveryOptions &options, Fastboot &fastboot, InstallListener &listener,
                    const ShouldStop &shouldStop, std::string &error);
};
