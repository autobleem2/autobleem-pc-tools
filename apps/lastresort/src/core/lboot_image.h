//
// LBOOT.EPB, as the recovery reads it: a plain zip of partition images (abflashkit writes boot.img,
// userdata.ext4 and tz.img, and rootfs.ext4 for a full backup) with a 4 KB trailer after the end record -
// AutoBleem's ends in the nine bytes "autobleem". This says what one holds and where each image goes.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

//******************
// LbootPartition
//******************
struct LbootPartition {
    std::string entry;     // "boot.img" - the file in the zip
    std::string partition; // "BOOTIMG1" - the console's partition, as fastboot names it
    uint64_t size = 0;     // the image's size, unpacked
};

//******************
// LbootImage
//******************
class LbootImage {
public:
    std::string path;
    bool readable = false;              // a zip that could be listed
    std::string error;                  // why not, when not
    bool autobleem = false;             // made by AutoBleem (abflashkit): its trailer is there
    std::vector<LbootPartition> images; // what is flashed, in the order it is flashed
    std::vector<std::string> unknown;   // entries no partition is known for - reported, never flashed

    // lists the zip; never throws
    static LbootImage inspect(const std::string &path);
    // the partition an entry of the zip goes to, "" when the entry is not a partition image we know
    static std::string partitionFor(const std::string &entry);

    // something to flash, and the kernel among it: a backup without boot.img cannot bring a console back
    bool usable() const;
    // the images' bytes together
    uint64_t totalBytes() const;
    // "boot.img -> BOOTIMG1 (6.5 MB), ..." for the window and the log
    std::vector<std::string> describe() const;
};
