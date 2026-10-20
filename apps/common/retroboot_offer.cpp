#include "retroboot_offer.h"

#include <ableem/engine/filesystem.h>

#include <cctype>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace std;
using ableem::DirEntry;

namespace retrobootoffer {

namespace {

enum class Kind { None, Dir, Link, File };

// what `path` itself is - a link is a link, never what it points to
Kind kindOf(const string &path, uint64_t *size = nullptr) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return Kind::None;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT)
        return Kind::Link;
    if (a & FILE_ATTRIBUTE_DIRECTORY)
        return Kind::Dir;
    if (size) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
            *size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    return Kind::File;
#else
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return Kind::None;
    if (S_ISLNK(st.st_mode))
        return Kind::Link;
    if (S_ISDIR(st.st_mode))
        return Kind::Dir;
    if (size)
        *size = static_cast<uint64_t>(st.st_size);
    return Kind::File;
#endif
}

// the entry of `dir` named `name` without regard to case ("" when none): FAT keeps the case it was given
string findCi(const string &dir, const string &name) {
    for (const DirEntry &e : DirEntry::diru(dir)) {
        if (e.name.size() != name.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < name.size() && same; i++)
            same = tolower(static_cast<unsigned char>(e.name[i])) == tolower(static_cast<unsigned char>(name[i]));
        if (same)
            return dir + "/" + e.name;
    }
    return "";
}

string cleanRoot(string root) {
    for (char &c : root)
        if (c == '\\')
            c = '/';
    if (root.size() == 2 && root[1] == ':')
        root += "/";
    while (root.size() > 1 && root.back() == '/' && !(root.size() == 3 && root[1] == ':'))
        root.pop_back();
    return root;
}

string join(const string &dir, const string &name) {
    return !dir.empty() && dir.back() == '/' ? dir + name : dir + "/" + name;
}

// the retroboot folder or link as the job finds it: RetroArch/bin/retroboot, else (old layout) retroarch/retroboot
string retroBootPath(const string &root, bool legacyLayout) {
    const string ra = findCi(root, "retroarch");
    if (ra.empty())
        return "";
    const string bin = findCi(ra, "bin");
    if (!bin.empty()) {
        const string rb = findCi(bin, "retroboot");
        if (!rb.empty())
            return rb;
    }
    return legacyLayout ? findCi(ra, "retroboot") : "";
}

// the bytes of the files under `dir`, links skipped (counted as nothing, never entered)
uint64_t treeBytes(const string &dir, int depth) {
    if (depth > 64)
        return 0;
    uint64_t total = 0;
    for (const DirEntry &e : DirEntry::diru(dir)) {
        const string path = dir + "/" + e.name;
        uint64_t size = 0;
        switch (kindOf(path, &size)) {
        case Kind::Dir:
            total += treeBytes(path, depth + 1);
            break;
        case Kind::File:
            total += size;
            break;
        default:
            break;
        }
    }
    return total;
}

string megabytes(uint64_t bytes) {
    uint64_t mb = (bytes + (512 * 1024)) / (1024 * 1024);
    if (mb < 1)
        mb = 1;
    return to_string(mb);
}

} // namespace

//*******************************
// offerRemoveRetroBoot
//*******************************
bool offerRemoveRetroBoot(const string &root, bool legacyLayout) {
    const string base = cleanRoot(root);
    if (base.empty())
        return false;
    const string rb = retroBootPath(base, legacyLayout);
    return !rb.empty() && kindOf(rb) == Kind::Dir;
}

//*******************************
// retroBootBytes
//*******************************
uint64_t retroBootBytes(const string &root, bool legacyLayout) {
    if (!offerRemoveRetroBoot(root, legacyLayout))
        return 0;
    const string base = cleanRoot(root);
    // apps/ goes only where it is empty, so it adds nothing
    return treeBytes(retroBootPath(base, legacyLayout), 0);
}

string sizeText(uint64_t bytes) {
    return "about " + megabytes(bytes) + " MB";
}

string checkboxLabel(uint64_t bytes) {
    return "Remove the RetroBoot leftovers (" + sizeText(bytes) + ", no longer used)";
}

string helperLine() {
    return "Deletes RetroArch\\bin\\retroboot and the emptied RetroArch\\bin\\apps from the stick. "
           "This cannot be undone.";
}

string confirmationText(const string &drive, uint64_t bytes) {
    string d = drive.substr(0, 2);
    return "Remove the RetroBoot leftovers from " + d + "?\n\nThese folders will be deleted:\n  " + d +
           "\\RetroArch\\bin\\retroboot\n  " + d +
           "\\RetroArch\\bin\\apps (only the app folders that are already empty)\n(" + sizeText(bytes) +
           ")\n\nA folder that something on the stick still uses is kept, and the log says so.\n\n"
           "This cannot be undone.";
}

} // namespace retrobootoffer
