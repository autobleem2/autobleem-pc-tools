//
// The flasher's disks on Windows: the physical drives a stick image may go onto, and a DiskTarget over one
// of them - \\.\PhysicalDriveN, every volume on it locked and dismounted for the write (the ones without a
// drive letter too: a stick written before has an EFI partition Windows mounts letterless, and a write into
// a mounted volume's sectors is refused), unbuffered I/O through a page-aligned buffer. Needs administrator
// rights (the manifest asks for them).
//
#pragma once

#include "installer/flasher_job.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32

//******************
// PhysicalDisk
//******************
struct PhysicalDisk {
    int number = -1;   // N of \\.\PhysicalDriveN
    std::string model; // vendor + product, as the drive reports them
    std::string bus;   // "USB", "SD", "MMC"
    uint64_t sizeBytes = 0;
    uint32_t sectorSize = 512;
    bool removable = true;            // the drive says its media is removable: a stick, a card - not a USB hard drive
    std::vector<std::string> letters; // "F:", ... - the volumes on it that have one
    std::vector<std::string> volumes; // "\\?\Volume{...}" - every volume on it

    std::string describe() const; // "SanDisk Cruzer Blade - 14.9 GB (USB, disk 2) - F:"
};

// the disks an image may be written to: on a USB, SD or MMC bus, with media in, not holding Windows itself,
// and - unless `includeFixed` - removable: a USB hard drive (the owner's backup disk, as often as not) is
// on the same bus as a stick and only its removable-media flag tells them apart, as Rufus does it. Some
// big fast sticks call themselves fixed; the window has a box for that. `notes` gets a line for each disk
// left out and why (for the log)
std::vector<PhysicalDisk> listTargetDisks(std::vector<std::string> *notes = nullptr, bool includeFixed = false);

//******************
// WindowsDisk
//******************
class WindowsDisk : public DiskTarget {
public:
    explicit WindowsDisk(const PhysicalDisk &disk);
    ~WindowsDisk() override;
    WindowsDisk(const WindowsDisk &) = delete;
    WindowsDisk &operator=(const WindowsDisk &) = delete;

    uint64_t size() const override { return disk_.sizeBytes; }
    uint32_t sectorSize() const override { return disk_.sectorSize; }
    bool open(std::string &error) override;
    bool write(uint64_t offset, const uint8_t *data, size_t size, std::string &error) override;
    bool read(uint64_t offset, uint8_t *data, size_t size, std::string &error) override;
    bool close(std::string &error) override;

private:
    void release();

    PhysicalDisk disk_;
    void *handle_ = nullptr;      // the drive's HANDLE
    std::vector<void *> volumes_; // the locked volumes' HANDLEs, held until close()
    uint8_t *buffer_ = nullptr;   // VirtualAlloc'd, page-aligned (unbuffered I/O needs it)
    size_t bufferSize_ = 0;
};

#endif
