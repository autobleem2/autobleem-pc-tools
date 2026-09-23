//
// The flasher's disks on Windows - see the header.
//
#ifdef _WIN32

#include "win32_disk.h"

#include "installer/install_job_base.h" // humanSize

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <map>

using namespace std;

namespace {

string lastError() {
    DWORD code = GetLastError();
    char *text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<char *>(&text), 0, nullptr);
    string out = text ? text : "";
    if (text)
        LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out + " (" + to_string(code) + ")";
}

string trimField(const char *s) {
    string out = s ? s : "";
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    size_t start = out.find_first_not_of(' ');
    return start == string::npos ? "" : out.substr(start);
}

string drivePath(int number) {
    return "\\\\.\\PhysicalDrive" + to_string(number);
}

// the disks a volume spans ("\\?\Volume{...}" without its trailing backslash)
vector<int> disksOfVolume(const string &volume) {
    vector<int> disks;
    HANDLE h = CreateFileA(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return disks;
    vector<uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT));
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()),
                        &got, nullptr)) {
        const VOLUME_DISK_EXTENTS *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buf.data());
        for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i)
            disks.push_back(static_cast<int>(extents->Extents[i].DiskNumber));
    }
    CloseHandle(h);
    return disks;
}

// every volume on every disk, and the drive letters of those that have one
void mapVolumes(map<int, vector<string>> &volumes, map<int, vector<string>> &letters) {
    char name[MAX_PATH];
    HANDLE find = FindFirstVolumeA(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        string volume = name;
        if (!volume.empty() && volume.back() == '\\')
            volume.pop_back();
        vector<string> paths;
        char mounts[1024] = {0};
        DWORD length = 0;
        if (GetVolumePathNamesForVolumeNameA(name, mounts, sizeof(mounts), &length))
            for (const char *p = mounts; *p; p += strlen(p) + 1)
                if (strlen(p) == 3 && p[1] == ':')
                    paths.push_back(string(p, 2));
        for (int disk : disksOfVolume(volume)) {
            volumes[disk].push_back(volume);
            for (const string &letter : paths)
                letters[disk].push_back(letter);
        }
    } while (FindNextVolumeA(find, name, MAX_PATH));
    FindVolumeClose(find);
}

} // namespace

//*******************************
// PhysicalDisk::describe
//*******************************
string PhysicalDisk::describe() const {
    string text = (model.empty() ? "Disk " + to_string(number) : model) + " - " + humanSize(sizeBytes) + " (" + bus +
                  (removable ? "" : " hard drive") + ", disk " + to_string(number) + ")";
    if (!letters.empty()) {
        text += " -";
        for (const string &l : letters)
            text += " " + l;
    }
    return text;
}

//*******************************
// listTargetDisks
//*******************************
vector<PhysicalDisk> listTargetDisks(vector<string> *notes, bool includeFixed) {
    map<int, vector<string>> volumes, letters;
    mapVolumes(volumes, letters);
    char windows[MAX_PATH] = {0};
    GetSystemWindowsDirectoryA(windows, MAX_PATH);
    const string systemLetter = string(windows, 2);

    vector<PhysicalDisk> disks;
    for (int n = 0; n < 64; ++n) {
        HANDLE h = CreateFileA(drivePath(n).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        PhysicalDisk d;
        d.number = n;
        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        vector<uint8_t> buf(1024);
        DWORD got = 0;
        STORAGE_BUS_TYPE busType = BusTypeUnknown;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buf.data(),
                            static_cast<DWORD>(buf.size()), &got, nullptr)) {
            const STORAGE_DEVICE_DESCRIPTOR *desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buf.data());
            busType = desc->BusType;
            d.removable = desc->RemovableMedia != 0;
            auto field = [&](DWORD offset) {
                return offset && offset < got ? trimField(reinterpret_cast<const char *>(buf.data()) + offset) : "";
            };
            string vendor = field(desc->VendorIdOffset), product = field(desc->ProductIdOffset);
            d.model = vendor.empty() ? product : product.empty() ? vendor : vendor + " " + product;
        }
        DISK_GEOMETRY_EX geometry = {};
        bool media = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof(geometry), &got,
                                     nullptr) != 0;
        CloseHandle(h);
        if (busType == BusTypeUsb)
            d.bus = "USB";
        else if (busType == BusTypeSd)
            d.bus = "SD";
        else if (busType == BusTypeMmc)
            d.bus = "MMC";
        d.volumes = volumes[n];
        d.letters = letters[n];
        sort(d.letters.begin(), d.letters.end());
        const string who = "disk " + to_string(n) + (d.model.empty() ? "" : " (" + d.model + ")");
        if (d.bus.empty()) {
            if (notes)
                notes->push_back(who + ": not a USB or SD disk");
            continue;
        }
        if (!media || geometry.DiskSize.QuadPart <= 0) {
            if (notes)
                notes->push_back(who + ": no media in it");
            continue;
        }
        if (find(d.letters.begin(), d.letters.end(), systemLetter) != d.letters.end()) {
            if (notes)
                notes->push_back(who + ": Windows runs from it");
            continue;
        }
        if (!d.removable && !includeFixed) {
            if (notes)
                notes->push_back(who + ": a USB hard drive, not a stick (shown with \"USB hard drives too\")");
            continue;
        }
        d.sizeBytes = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
        d.sectorSize = geometry.Geometry.BytesPerSector ? geometry.Geometry.BytesPerSector : 512;
        disks.push_back(d);
    }
    return disks;
}

//*******************************
// WindowsDisk
//*******************************
WindowsDisk::WindowsDisk(const PhysicalDisk &disk) : disk_(disk) {}

WindowsDisk::~WindowsDisk() {
    release();
}

void WindowsDisk::release() {
    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
    for (void *v : volumes_) {
        DWORD got = 0;
        DeviceIoControl(v, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        CloseHandle(v);
    }
    volumes_.clear();
    if (buffer_) {
        VirtualFree(buffer_, 0, MEM_RELEASE);
        buffer_ = nullptr;
    }
}

bool WindowsDisk::open(string &error) {
    release();
    // every volume locked (a program holding a file open on it refuses the lock - a few tries, then
    // say which) and dismounted; the handles stay open until close() so nothing mounts them again
    for (const string &volume : disk_.volumes) {
        HANDLE v = CreateFileA(volume.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (v == INVALID_HANDLE_VALUE) {
            error = "cannot open a volume on the stick: " + lastError();
            release();
            return false;
        }
        DWORD got = 0;
        bool locked = false;
        for (int attempt = 0; attempt < 20 && !locked; ++attempt) {
            locked = DeviceIoControl(v, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr) != 0;
            if (!locked)
                Sleep(250);
        }
        if (!locked) {
            error = "the stick is in use (" + lastError() +
                    ") - close every window and program that has a file on it open, then try again";
            CloseHandle(v);
            release();
            return false;
        }
        DeviceIoControl(v, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        volumes_.push_back(v);
    }
    HANDLE h =
        CreateFileA(drivePath(disk_.number).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const bool denied = GetLastError() == ERROR_ACCESS_DENIED;
        error = "cannot open disk " + to_string(disk_.number) + " for writing: " + lastError() +
                (denied ? " - run the flasher as administrator" : "");
        release();
        return false;
    }
    handle_ = h;
    bufferSize_ = FlasherJob::ChunkSize;
    buffer_ = static_cast<uint8_t *>(VirtualAlloc(nullptr, bufferSize_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buffer_) {
        error = "out of memory";
        release();
        return false;
    }
    return true;
}

bool WindowsDisk::write(uint64_t offset, const uint8_t *data, size_t size, string &error) {
    while (size > 0) {
        const size_t n = min(size, bufferSize_);
        memcpy(buffer_, data, n);
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(offset);
        DWORD done = 0;
        if (!SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN) ||
            !WriteFile(handle_, buffer_, static_cast<DWORD>(n), &done, nullptr) || done != n) {
            error = "writing at " + humanSize(offset) + " failed: " + lastError();
            return false;
        }
        offset += n;
        data += n;
        size -= n;
    }
    return true;
}

bool WindowsDisk::read(uint64_t offset, uint8_t *data, size_t size, string &error) {
    while (size > 0) {
        const size_t n = min(size, bufferSize_);
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(offset);
        DWORD done = 0;
        if (!SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN) ||
            !ReadFile(handle_, buffer_, static_cast<DWORD>(n), &done, nullptr) || done != n) {
            error = "reading at " + humanSize(offset) + " failed: " + lastError();
            return false;
        }
        memcpy(data, buffer_, n);
        offset += n;
        data += n;
        size -= n;
    }
    return true;
}

bool WindowsDisk::close(string &error) {
    bool ok = true;
    if (handle_) {
        if (!FlushFileBuffers(handle_)) {
            error = "flushing the stick failed: " + lastError();
            ok = false;
        }
        // the new partition table, read by Windows now rather than at the next plug-in
        DWORD got = 0;
        DeviceIoControl(handle_, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
    }
    release();
    return ok;
}

#endif
