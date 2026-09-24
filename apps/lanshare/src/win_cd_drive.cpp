//
// WinCdDrive - see the header.
//
#ifdef _WIN32

#include "win_cd_drive.h"

#include <ableem/engine/log.h>

#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>

#include <algorithm>
#include <cstring>

using namespace std;

namespace {

const uint32_t Batch = 20; // 20 x 2448 bytes stays under the 64 KB one request may carry
const uint32_t LeadIn = 150;

uint32_t msfToLba(const UCHAR *address) { // TRACK_DATA::Address: [0] reserved, then M, S, F
    const uint32_t frames = (address[1] * 60u + address[2]) * 75u + address[3];
    return frames >= LeadIn ? frames - LeadIn : 0;
}

} // namespace

//*******************************
// WinCdDrive::drives
//*******************************
vector<string> WinCdDrive::drives() {
    vector<string> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i)))
            continue;
        const string root = string(1, static_cast<char>('A' + i)) + ":\\";
        if (GetDriveTypeA(root.c_str()) == DRIVE_CDROM)
            out.push_back(root.substr(0, 2));
    }
    return out;
}

//*******************************
// WinCdDrive::WinCdDrive / ~WinCdDrive / open
//*******************************
WinCdDrive::WinCdDrive(const string &letter) : letter_(letter.substr(0, 1) + ":"), handle_(INVALID_HANDLE_VALUE) {}

WinCdDrive::~WinCdDrive() {
    if (handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(static_cast<HANDLE>(handle_));
}

bool WinCdDrive::open(string &error) {
    if (handle_ != INVALID_HANDLE_VALUE)
        return true;
    const string path = "\\\\.\\" + letter_;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "the drive " + letter_ + " cannot be opened (Windows error " + to_string(GetLastError()) + ")";
        return false;
    }
    handle_ = h;
    return true;
}

//*******************************
// WinCdDrive::readToc
//*******************************
bool WinCdDrive::readToc(CdToc &toc, string &error) {
    if (!open(error))
        return false;
    HANDLE h = static_cast<HANDLE>(handle_);
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, nullptr, 0, &got, nullptr)) {
        error = "there is no disc in " + letter_ + " (or it is still spinning up - try again in a moment)";
        return false;
    }
    CDROM_TOC raw;
    memset(&raw, 0, sizeof(raw));
    if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, nullptr, 0, &raw, sizeof(raw), &got, nullptr)) {
        error = "the disc's table of contents cannot be read (Windows error " + to_string(GetLastError()) + ")";
        return false;
    }
    toc = CdToc();
    const int count = raw.LastTrack - raw.FirstTrack + 1;
    if (count <= 0 || count >= MAXIMUM_NUMBER_TRACKS) {
        error = "the disc's table of contents is empty";
        return false;
    }
    for (int i = 0; i < count; i++) {
        const TRACK_DATA &t = raw.TrackData[i];
        CdTrack track;
        track.number = t.TrackNumber;
        track.audio = (t.Control & 0x04) == 0; // the data bit
        track.start = msfToLba(t.Address);
        toc.tracks.push_back(track);
    }
    toc.leadout = msfToLba(raw.TrackData[count].Address); // the lead-out entry follows the last track
    leadout_ = toc.leadout;

    // does this drive give the subchannel? one sector tells
    vector<uint8_t> probe(bytesPerSector(WithSubCode));
    subchannel_ = rawRead(0, 1, WithSubCode, probe.data());
    cacheCount_ = 0;
    PLOG_INFO << letter_ << ": " << toc.tracks.size() << " tracks, " << toc.leadout << " sectors, subchannel "
              << (subchannel_ ? "yes" : "no");
    return true;
}

//*******************************
// WinCdDrive::readSector
//*******************************
uint32_t WinCdDrive::bytesPerSector(Mode mode) {
    return mode == WithSubCode ? DiscReader::SectorSize + 96 : DiscReader::SectorSize;
}

bool WinCdDrive::rawRead(uint32_t lba, uint32_t count, Mode mode, uint8_t *out) {
    RAW_READ_INFO info;
    info.DiskOffset.QuadPart = static_cast<LONGLONG>(lba) * 2048; // the IOCTL counts in 2048-byte units
    info.SectorCount = count;
    info.TrackMode = static_cast<TRACK_MODE_TYPE>(mode);
    DWORD got = 0;
    const DWORD size = count * bytesPerSector(mode);
    return DeviceIoControl(static_cast<HANDLE>(handle_), IOCTL_CDROM_RAW_READ, &info, sizeof(info), out, size, &got,
                           nullptr) &&
           got == size;
}

bool WinCdDrive::readSector(uint32_t lba, bool audio, uint8_t *data, uint8_t *q) {
    if (handle_ == INVALID_HANDLE_VALUE || lba >= leadout_)
        return false;
    // with the subchannel every sector is read raw, whatever it holds; without, as what it should be
    const vector<Mode> modes = subchannel_ ? vector<Mode>{WithSubCode}
                                           : (audio ? vector<Mode>{Audio} : vector<Mode>{Data, XaForm2});
    for (Mode mode : modes) {
        const uint32_t per = bytesPerSector(mode);
        const bool cached = cacheCount_ > 0 && cacheMode_ == mode && lba >= cacheFirst_ && lba < cacheFirst_ + cacheCount_;
        if (!cached) {
            const uint32_t count = min(Batch, leadout_ - lba);
            cache_.resize(static_cast<size_t>(count) * per);
            if (rawRead(lba, count, mode, cache_.data())) {
                cacheFirst_ = lba;
                cacheCount_ = count;
                cacheMode_ = mode;
            } else {
                cacheCount_ = 0; // the batch failed: this sector alone
                vector<uint8_t> one(per);
                if (!rawRead(lba, 1, mode, one.data()))
                    continue;
                memcpy(data, one.data(), DiscReader::SectorSize);
                if (q != nullptr && mode == WithSubCode)
                    DiscReader::deinterleaveQ(one.data() + DiscReader::SectorSize, q);
                return true;
            }
        }
        const uint8_t *sector = cache_.data() + static_cast<size_t>(lba - cacheFirst_) * per;
        memcpy(data, sector, DiscReader::SectorSize);
        if (q != nullptr && mode == WithSubCode)
            DiscReader::deinterleaveQ(sector + DiscReader::SectorSize, q);
        return true;
    }
    return false;
}

#endif
