//
// LbootImage - see the header.
//
#include "lboot_image.h"

#include "installer/install_job_base.h" // humanSize

#include <ableem/engine/zip_archive.h>

#include <cstring>
#include <fstream>

using namespace std;

namespace {

// the images a backup can hold and the partitions they came from (abflashkit's LbootBackup), in the order
// they are flashed: the kernel first - it is what AutoBleem changes - and the big user data last
struct Known {
    const char *entry;
    const char *partition;
};
const Known KnownImages[] = {
    {"boot.img", "BOOTIMG1"},
    {"tz.img", "TEE1"},
    {"rootfs.ext4", "ROOTFS1"},
    {"userdata.ext4", "USRDATA"},
};

// AutoBleem's trailer ends in these nine bytes (abflashkit's lboot_signature.h)
const char Mark[] = "autobleem";
const size_t MarkLength = sizeof(Mark) - 1;

bool endsInMark(const string &path) {
    ifstream in(path, ios::binary);
    if (!in)
        return false;
    in.seekg(0, ios::end);
    const streamoff size = in.tellg();
    if (size < static_cast<streamoff>(MarkLength))
        return false;
    in.seekg(size - static_cast<streamoff>(MarkLength));
    char tail[MarkLength];
    in.read(tail, static_cast<streamsize>(MarkLength));
    return in.gcount() == static_cast<streamsize>(MarkLength) && memcmp(tail, Mark, MarkLength) == 0;
}

} // namespace

//*******************************
// LbootImage::partitionFor
//*******************************
string LbootImage::partitionFor(const string &entry) {
    for (const Known &k : KnownImages)
        if (entry == k.entry)
            return k.partition;
    return "";
}

//*******************************
// LbootImage::inspect
//*******************************
LbootImage LbootImage::inspect(const string &path) {
    LbootImage image;
    image.path = path;
    vector<ableem::ZipEntry> entries;
    if (!ableem::ZipArchive::listEntries(path, entries)) {
        image.error = "not a backup the recovery can read (it is not a zip file, or it is damaged)";
        return image;
    }
    image.readable = true;
    image.autobleem = endsInMark(path);
    for (const Known &k : KnownImages)
        for (const ableem::ZipEntry &e : entries)
            if (!e.isDir && e.name == k.entry)
                image.images.push_back({e.name, k.partition, e.size});
    for (const ableem::ZipEntry &e : entries)
        if (!e.isDir && partitionFor(e.name).empty())
            image.unknown.push_back(e.name);
    if (!image.usable())
        image.error = image.images.empty() ? "the backup holds no partition image" : "the backup has no boot.img";
    return image;
}

//*******************************
// LbootImage::usable
//*******************************
bool LbootImage::usable() const {
    if (!readable)
        return false;
    for (const LbootPartition &p : images)
        if (p.entry == "boot.img" && p.size > 0)
            return true;
    return false;
}

//*******************************
// LbootImage::totalBytes
//*******************************
uint64_t LbootImage::totalBytes() const {
    uint64_t total = 0;
    for (const LbootPartition &p : images)
        total += p.size;
    return total;
}

//*******************************
// LbootImage::describe
//*******************************
vector<string> LbootImage::describe() const {
    vector<string> lines;
    for (const LbootPartition &p : images)
        lines.push_back(p.entry + " -> " + p.partition + " (" + humanSize(p.size) + ")");
    for (const string &name : unknown)
        lines.push_back(name + " - not a partition image the recovery knows; left alone");
    return lines;
}
