#ifdef _WIN32

#include "win32_platform.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

using namespace std;

namespace {

wstring wide(const string &utf8) {
    if (utf8.empty())
        return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

string narrow(const wstring &w) {
    if (w.empty())
        return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

string gb(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else
        snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024));
    return buf;
}

string lastErrorText(DWORD code) {
    wchar_t *msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"wininet.dll"), code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    string text = n ? narrow(wstring(msg, n)) : "error " + to_string(code);
    if (msg)
        LocalFree(msg);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    return text;
}

// runs a command line, every line of its output to `say`; the exit code, -1 when it could not start
int runCapturing(const wstring &commandLine, const function<void(const string &)> &say) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
        return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    wstring cmd = commandLine; // CreateProcess may write into it
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return -1;
    }
    CloseHandle(writeEnd);
    string pending;
    char buf[512];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find_first_of("\r\n")) != string::npos) {
            string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty())
                say("  " + line);
        }
    }
    if (!pending.empty())
        say("  " + pending);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

} // namespace

//*******************************
// RemovableDrive::describe
//*******************************
string RemovableDrive::describe() const {
    if (!ready)
        return letter + "  (no readable volume - format it first)";
    string text = letter + "  " + (label.empty() ? "(no label)" : label) + "  (" +
                  (fileSystem.empty() ? "?" : fileSystem) + ", " + gb(sizeBytes) + ", " + gb(freeBytes) + " free)";
    return text;
}

//*******************************
// listRemovableDrives
//*******************************
vector<RemovableDrive> listRemovableDrives() {
    vector<RemovableDrive> drives;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i)))
            continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_REMOVABLE)
            continue;
        RemovableDrive d;
        d.letter = string(1, static_cast<char>('A' + i)) + ":";
        d.root = d.letter + "/";
        wchar_t label[MAX_PATH + 1] = {0}, fs[MAX_PATH + 1] = {0};
        DWORD serial = 0, maxLen = 0, flags = 0;
        UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS); // no "insert a disk" box for an empty reader
        if (GetVolumeInformationW(root, label, MAX_PATH, &serial, &maxLen, &flags, fs, MAX_PATH)) {
            d.label = narrow(label);
            d.fileSystem = narrow(fs);
            ULARGE_INTEGER freeToCaller, total, freeTotal;
            if (GetDiskFreeSpaceExW(root, &freeToCaller, &total, &freeTotal)) {
                d.sizeBytes = total.QuadPart;
                d.freeBytes = freeToCaller.QuadPart;
            }
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_NOT_READY)
                continue;    // a card reader with nothing in it
            d.ready = false; // there, but unformatted (or RAW)
        }
        SetErrorMode(oldMode);
        drives.push_back(d);
    }
    return drives;
}

//*******************************
// ensureVolumeLabel
//*******************************
bool ensureVolumeLabel(const string &root, const string &label, string &error) {
    if (root.size() < 2 || root[1] != ':') {
        error = "not a drive: " + root;
        return false;
    }
    // a folder on a drive ("E:/tmp/stick": --drive pointed at a test target) is no stick - the drive's own label is not
    // the installer's to rename
    if (root.size() > 3 || (root.size() == 3 && root[2] != '/' && root[2] != '\\'))
        return true;
    wchar_t r[] = {static_cast<wchar_t>(root[0]), L':', L'\\', 0};
    wchar_t current[MAX_PATH + 1] = {0};
    if (GetVolumeInformationW(r, current, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) &&
        _wcsicmp(current, wide(label).c_str()) == 0)
        return true;
    if (!SetVolumeLabelW(r, wide(label).c_str())) {
        error = "cannot name " + root.substr(0, 2) + " " + label + ": " + lastErrorText(GetLastError());
        return false;
    }
    return true;
}

//*******************************
// programDirectory
//*******************************
string programDirectory() {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
    string dir = narrow(wstring(path, n));
    for (char &c : dir)
        if (c == '\\')
            c = '/';
    size_t slash = dir.find_last_of('/');
    return slash == string::npos ? "." : dir.substr(0, slash);
}

//*******************************
// formatDrive
//*******************************
bool formatDrive(const string &letter, const string &fileSystem, const string &label,
                 const function<void(const string &)> &say, string &error) {
    if (letter.size() != 2 || letter[1] != ':') {
        error = "not a drive letter: " + letter;
        return false;
    }
    wchar_t root[] = {static_cast<wchar_t>(letter[0]), L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_REMOVABLE) {
        error = letter + " is not a removable drive - only those are formatted from here";
        return false;
    }
    const bool fat32 = fileSystem == "FAT32";
    ULARGE_INTEGER total = {};
    ULARGE_INTEGER dummy = {};
    GetDiskFreeSpaceExW(root, &dummy, &total, &dummy);
    const bool bigForFat32 = fat32 && total.QuadPart > 32ull * 1024 * 1024 * 1024;

    wstring command;
    const string helper = programDirectory() + "/fat32format.exe";
    if (bigForFat32 && GetFileAttributesW(wide(helper).c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Ridgecrop's fat32format: `fat32format -c<sectors per cluster> X:` - it answers a prompt itself
        // with -y in the newer builds; the older takes the drive only and asks, so the answer is piped
        say("Formatting " + letter + " as FAT32 with fat32format.exe (" + gb(total.QuadPart) +
            " is more than format.com will do)");
        command = L"cmd.exe /c echo y| \"" + wide(helper) + L"\" " + wide(letter);
    } else {
        if (bigForFat32)
            say("Note: " + letter + " is " + gb(total.QuadPart) +
                " - Windows formats FAT32 up to 32 GB only; put "
                "fat32format.exe next to the installer for a bigger stick, or use exFAT");
        say("Formatting " + letter + " as " + fileSystem + " (quick), label " + label);
        command =
            L"cmd.exe /c format " + wide(letter) + L" /FS:" + wide(fileSystem) + L" /Q /V:" + wide(label) + L" /Y";
    }
    int code = runCapturing(command, say);
    if (code != 0) {
        error =
            code < 0 ? "could not start the format tool" : "the format tool failed (exit code " + to_string(code) + ")";
        return false;
    }
    // the label: format.com sets it; fat32format does not
    SetVolumeLabelW(root, wide(label).c_str());
    return true;
}

//*******************************
// WinInetDownloader
//*******************************
// One WinINet session for the life of the object, and per thread and host one connection kept open between
// files (INTERNET_FLAG_KEEP_CONNECTION): the BIOS pack's 700 files used to cost a session, a TLS handshake and a
// new connection each. Each thread has connections of its own, so the BIOS workers (connections()) share nothing
// but the cache's map. A file may be continued from a part with an HTTP Range request.
namespace {

string threadKey() {
    ostringstream key;
    key << this_thread::get_id();
    return key.str();
}

bool crack(const string &url, WinInetTarget &t) {
    const wstring w = wide(url);
    URL_COMPONENTSW c = {};
    c.dwStructSize = sizeof(c);
    c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlW(w.c_str(), static_cast<DWORD>(w.size()), 0, &c) || c.dwHostNameLength == 0)
        return false;
    if (c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    t.host.assign(c.lpszHostName, c.dwHostNameLength);
    t.path.assign(c.lpszUrlPath, c.dwUrlPathLength);
    t.path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
    if (t.path.empty())
        t.path = L"/";
    t.port = c.nPort;
    t.secure = c.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

} // namespace

WinInetDownloader::WinInetDownloader() {
    session_ = InternetOpenW(L"AutoBleem-Installer/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!session_)
        return;
    DWORD timeout = 30000;
    InternetSetOptionW(session_, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session_, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    // WinINet allows 2 (older) to 6 connections to one host; the BIOS workers want connections() of them
    DWORD perServer = 8;
    InternetSetOptionW(nullptr, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &perServer, sizeof(perServer));
}

WinInetDownloader::~WinInetDownloader() {
    for (auto &c : connections_)
        InternetCloseHandle(c.second);
    if (session_)
        InternetCloseHandle(session_);
}

void *WinInetDownloader::connection(const WinInetTarget &t, bool fresh) {
    const string key = threadKey() + "|" + narrow(t.host) + ":" + to_string(t.port) + (t.secure ? "s" : "");
    lock_guard<mutex> lock(m_);
    auto it = connections_.find(key);
    if (it != connections_.end()) {
        if (!fresh)
            return it->second;
        InternetCloseHandle(it->second);
        connections_.erase(it);
    }
    HINTERNET c = InternetConnectW(session_, t.host.c_str(), t.port, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (c)
        connections_[key] = c;
    return c;
}

bool WinInetDownloader::fetch(const string &url, const string &destFile, const Progress &progress, string &error) {
    return get(url, destFile, false, progress, error);
}

bool WinInetDownloader::fetchResumable(const string &url, const string &destFile, const Progress &progress,
                                       string &error) {
    return get(url, destFile, true, progress, error);
}

bool WinInetDownloader::get(const string &url, const string &destFile, bool resume, const Progress &progress,
                            string &error) {
    if (!session_) {
        error = "WinINet: " + lastErrorText(GetLastError());
        return false;
    }
    WinInetTarget target;
    if (!crack(url, target)) {
        error = url + ": not an http(s) address";
        return false;
    }
    // what a continued download starts from: the bytes the part already holds
    uint64_t have = 0;
    if (resume) {
        const long long size = ableem::DirEntry::fileSize(destFile);
        have = size > 0 ? static_cast<uint64_t>(size) : 0;
    }
    HINTERNET req = nullptr;
    DWORD status = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        // attempt 1: the kept connection was probably closed by the server meanwhile - a new one; attempt 2: the
        // same without the Range when the server answered 416 (the part is not a prefix of its file)
        HINTERNET conn = static_cast<HINTERNET>(connection(target, attempt == 1));
        if (!conn) {
            error = url + ": " + lastErrorText(GetLastError());
            return false;
        }
        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI |
                      INTERNET_FLAG_KEEP_CONNECTION | (target.secure ? INTERNET_FLAG_SECURE : 0);
        req = HttpOpenRequestW(conn, L"GET", target.path.c_str(), nullptr, nullptr, nullptr, flags, 0);
        if (!req) {
            error = url + ": " + lastErrorText(GetLastError());
            return false;
        }
        if (have > 0) {
            const wstring range = L"Range: bytes=" + to_wstring(have) + L"-\r\n";
            HttpAddRequestHeadersW(req, range.c_str(), static_cast<DWORD>(-1), HTTP_ADDREQ_FLAG_ADD);
        }
        if (!HttpSendRequestW(req, nullptr, 0, nullptr, 0)) {
            const DWORD code = GetLastError();
            InternetCloseHandle(req);
            req = nullptr;
            if (attempt == 0)
                continue; // a stale kept connection looks like this
            error = url + ": " + lastErrorText(code);
            return false;
        }
        DWORD size = sizeof(status), index = 0;
        status = 0;
        HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, &index);
        if (status == 416 && have > 0) {
            InternetCloseHandle(req);
            req = nullptr;
            ableem::DirEntry::removeFile(destFile);
            have = 0;
            continue;
        }
        break;
    }
    if (!req) {
        error = url + ": " + lastErrorText(GetLastError());
        return false;
    }
    bool ok = true;
    if (status != 200 && status != 206 && status != 0) {
        error = url + ": HTTP " + to_string(status);
        ok = false;
    }
    // 206 = the server went on where the part ends; a plain 200 sent the whole file again
    const bool appending = ok && status == 206 && have > 0;
    uint64_t total = 0;
    wchar_t lengthText[64] = {0};
    DWORD size = sizeof(lengthText), index = 0;
    if (ok && HttpQueryInfoW(req, HTTP_QUERY_CONTENT_LENGTH, lengthText, &size, &index))
        total = wcstoull(lengthText, nullptr, 10);
    if (ok) {
        ofstream out(destFile, ios::binary | (appending ? ios::app : ios::trunc));
        if (!out) {
            error = "cannot write " + destFile;
            ok = false;
        }
        const uint64_t before = appending ? have : 0; // the file's size when the body starts
        uint64_t done = 0;
        static thread_local char buf[65536];
        DWORD got = 0;
        bool read = true;
        while (ok && (read = InternetReadFile(req, buf, sizeof(buf), &got) != FALSE) && got > 0) {
            out.write(buf, got);
            done += got;
            if (progress && !progress(before + done, total ? before + total : 0)) {
                error = "Stopped";
                ok = false;
            }
        }
        if (ok && !read) {
            error = url + ": " + lastErrorText(GetLastError());
            ok = false;
        }
        if (ok && !out) {
            error = "cannot write " + destFile;
            ok = false;
        }
        if (ok && total > 0 && done != total) {
            error = url + ": the download stopped short (" + to_string(done) + " of " + to_string(total) + " bytes)";
            ok = false;
        }
        out.close();
        // a resumable fetch keeps what arrived, so the next run goes on from there; any other starts over
        if (!ok && !resume)
            DeleteFileW(wide(destFile).c_str());
    }
    InternetCloseHandle(req);
    return ok;
}

//*******************************
// attachParentConsole
//*******************************
void attachParentConsole() {
    auto redirected = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    const bool outRedirected = redirected(STD_OUTPUT_HANDLE), errRedirected = redirected(STD_ERROR_HANDLE);
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *f = nullptr;
    if (!outRedirected)
        freopen_s(&f, "CONOUT$", "w", stdout);
    if (!errRedirected)
        freopen_s(&f, "CONOUT$", "w", stderr);
}

#endif
