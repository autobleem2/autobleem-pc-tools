//
// WinCdDrive: DiscReader's CdDrive on Windows - a CD drive by its letter, read through the storage stack's own
// CD-ROM IOCTLs, so no driver, no admin rights and no SCSI pass-through are needed:
//   IOCTL_STORAGE_CHECK_VERIFY  is there a disc
//   IOCTL_CDROM_READ_TOC        the tracks (data or audio by the control bits) and the lead-out
//   IOCTL_CDROM_RAW_READ        2352-byte sectors - RawWithSubCode (2352 + 96 bytes of P-W subcode) when the
//                               drive does it, which gives the Q channel for LibCrypt; else YellowMode2 (then
//                               XAForm2) for data and CDDA for audio
// Sectors are read 20 at a time (under the 64 KB a request may carry) and handed out one by one; a batch that
// fails is read again sector by sector, so one bad sector costs only itself.
//
#pragma once

#ifdef _WIN32

#include "core/services/disc_reader.h"

#include <string>
#include <vector>

class WinCdDrive : public CdDrive {
public:
    // the CD/DVD drives this PC has, as "D:", "E:", ...
    static std::vector<std::string> drives();

    explicit WinCdDrive(const std::string &letter); // "D:" or "D"
    ~WinCdDrive() override;
    WinCdDrive(const WinCdDrive &) = delete;
    WinCdDrive &operator=(const WinCdDrive &) = delete;

    bool readToc(CdToc &toc, std::string &error) override;
    bool readSector(uint32_t lba, bool audio, uint8_t *data, uint8_t *q) override;
    bool hasSubchannel() const override { return subchannel_; }

private:
    enum Mode { Data = 0, XaForm2 = 1, Audio = 2, WithSubCode = 5 }; // TRACK_MODE_TYPE's values
    bool open(std::string &error);
    bool rawRead(uint32_t lba, uint32_t count, Mode mode, uint8_t *out);
    static uint32_t bytesPerSector(Mode mode);

    std::string letter_;
    void *handle_; // HANDLE, INVALID_HANDLE_VALUE when not open
    bool subchannel_ = false;
    uint32_t leadout_ = 0;
    // the last batch read
    std::vector<uint8_t> cache_;
    uint32_t cacheFirst_ = 0;
    uint32_t cacheCount_ = 0;
    Mode cacheMode_ = Data;
};

#endif
