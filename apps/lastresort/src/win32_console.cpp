//
// The console on USB, fastboot.exe and the driver - see the header.
//
#ifdef _WIN32

#include "win32_console.h"

#include "../../installer/src/win32_platform.h" // programDirectory

extern "C" {
#include "pki.h"
}

#include <ableem/engine/filesystem.h>
#include <ableem/engine/log.h>

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>

#include <cwchar>
#include <fstream>
#include <sstream>

using namespace std;

const char *const ConsoleHardwareId = "USB\\VID_0BB4&PID_0C01";

namespace {

// fastboot.exe finds its devices by this interface (AdbWinApi's ANDROID_USB_CLASS_ID); a driver without it
// binds WinUSB and fastboot still sees nothing
const char *const AndroidInterfaceGuid = "{F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}";
const char *const InfName = "psc_fastboot.inf";
const char *const CatName = "psc_fastboot.cat";
const char *const CertSubject = "CN=PlayStation Classic fastboot (AutoBleem LastResortRecovery)";

wstring wide(const string &utf8) {
    if (utf8.empty())
        return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

string narrow(const wstring &text) {
    if (text.empty())
        return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

string backslashes(string path) {
    for (char &c : path)
        if (c == '/')
            c = '\\';
    return path;
}

string errorText(DWORD code) {
    wchar_t *msg = nullptr;
    DWORD n =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    string text = n ? narrow(wstring(msg, n)) : string();
    if (msg)
        LocalFree(msg);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(code));
    return text.empty() ? string("error ") + hex : text + " (" + hex + ")";
}

bool startsWithNoCase(const wchar_t *text, const wstring &prefix) {
    return _wcsnicmp(text, prefix.c_str(), prefix.size()) == 0;
}

// one argument as CreateProcess's command line parser reads it back (quotes, and the backslashes before them)
wstring quoted(const wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == wstring::npos)
        return arg;
    wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            slashes++;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            slashes = 0;
        } else {
            slashes = 0;
        }
        out += c;
    }
    out.append(slashes, L'\\');
    return out + L"\"";
}

bool writeWholeFile(const string &path, const string &text, string &error) {
    HANDLE f =
        CreateFileW(wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "could not write " + path + ": " + errorText(GetLastError());
        return false;
    }
    DWORD wrote = 0;
    bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
    if (!ok)
        error = "could not write " + path + ": " + errorText(GetLastError());
    CloseHandle(f);
    return ok;
}

// Microsoft's WinUSB install for Windows 8 and later (Include=winusb.inf, no co-installers), bound to the
// console's id, with the interface fastboot.exe enumerates
string infText() {
    const string id = ConsoleHardwareId;
    string t;
    t += "; PlayStation Classic in fastboot mode - WinUSB, for Android's fastboot client.\r\n";
    t += "; Written and signed on this PC by AutoBleem LastResortRecovery.\r\n";
    t += "[Version]\r\n";
    t += "Signature   = \"$Windows NT$\"\r\n";
    t += "Class       = USBDevice\r\n";
    t += "ClassGuid   = {88BAE032-5A81-49f0-BC3D-A4FF138216D6}\r\n";
    t += "Provider    = %ProviderName%\r\n";
    t += "CatalogFile = " + string(CatName) + "\r\n";
    t += "DriverVer   = 09/24/2026,1.0.0.0\r\n\r\n";
    t += "[Manufacturer]\r\n";
    t += "%ProviderName% = Console,NTx86,NTamd64,NTarm64\r\n\r\n";
    for (const char *arch : {"NTx86", "NTamd64", "NTarm64"}) {
        t += "[Console." + string(arch) + "]\r\n";
        t += "%DeviceName% = USB_Install, " + id + "\r\n\r\n";
    }
    t += "[USB_Install]\r\nInclude = winusb.inf\r\nNeeds   = WINUSB.NT\r\n\r\n";
    t += "[USB_Install.Services]\r\nInclude = winusb.inf\r\nNeeds   = WINUSB.NT.Services\r\n\r\n";
    t += "[USB_Install.HW]\r\nAddReg = Dev_AddReg\r\n\r\n";
    t += "[Dev_AddReg]\r\nHKR,,DeviceInterfaceGUIDs,0x10000,\"" + string(AndroidInterfaceGuid) + "\"\r\n\r\n";
    t += "[Strings]\r\nProviderName = \"AutoBleem\"\r\nDeviceName   = \"PlayStation Classic (fastboot)\"\r\n";
    return t;
}

void pkiLine(int level, const char *line, void *context) {
    auto *say = static_cast<const function<void(const string &)> *>(context);
    if (level >= 1 && say && *say)
        (*say)(string(level >= 2 ? "  warning: " : "  ") + line);
    PLOG_DEBUG << "pki: " << line;
}

} // namespace

//*******************************
// ConsoleUsb::describe
//*******************************
string ConsoleUsb::describe() const {
    if (!present)
        return "not connected";
    string s = instanceId;
    s += service.empty() ? ", no driver" : ", driver " + service;
    if (problem)
        s += ", problem code " + to_string(problem);
    return s;
}

//*******************************
// findConsoleUsb
//*******************************
ConsoleUsb findConsoleUsb() {
    ConsoleUsb found;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return found;
    const wstring prefix = wide(ConsoleHardwareId);
    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); i++) {
        wchar_t ids[2048] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE *>(ids),
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr))
            continue;
        bool match = false;
        for (const wchar_t *p = ids; *p; p += wcslen(p) + 1)
            if (startsWithNoCase(p, prefix))
                match = true;
        if (!match)
            continue;
        found.present = true;
        wchar_t id[512] = {0};
        if (SetupDiGetDeviceInstanceIdW(set, &data, id, 512, nullptr))
            found.instanceId = narrow(id);
        wchar_t service[256] = {0};
        if (SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_SERVICE, nullptr, reinterpret_cast<BYTE *>(service),
                                              sizeof(service) - sizeof(wchar_t), nullptr))
            found.service = narrow(service);
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) == CR_SUCCESS && (status & DN_HAS_PROBLEM))
            found.problem = problem;
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

//*******************************
// WindowsFastboot::bundledPath
//*******************************
string WindowsFastboot::bundledPath() {
    return programDirectory() + "/platform-tools/fastboot.exe";
}

//*******************************
// WindowsFastboot::run
//*******************************
int WindowsFastboot::run(const vector<string> &args, const Output &output) {
    wstring cmd = quoted(wide(backslashes(exe_)));
    for (const string &a : args)
        cmd += L" " + quoted(wide(a));
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
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        PLOG_ERROR << "could not start " << exe_ << ": " << errorText(GetLastError());
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return -1;
    }
    CloseHandle(writeEnd);
    bool ended = false;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        if (!ended && output && !output(string(buf, got))) {
            TerminateProcess(pi.hProcess, 1); // the pipe closes with it and the loop ends
            ended = true;
        }
    }
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ended ? -1 : static_cast<int>(code);
}

//*******************************
// installConsoleDriver
//*******************************
bool installConsoleDriver(const string &dir, const function<void(const string &)> &say, string &error) {
    if (!ableem::DirEntry::createDirs(dir)) {
        error = "could not create " + dir;
        return false;
    }
    const string inf = backslashes(dir + "/" + InfName);
    const string cat = backslashes(dir + "/" + CatName);
    say("Writing the driver package: " + inf);
    if (!writeWholeFile(inf, infText(), error))
        return false;
    pki_set_log_sink(pkiLine, const_cast<function<void(const string &)> *>(&say));
    say("Making its security catalog");
    LPCSTR files[] = {InfName};
    const bool catalog = CreateCat(cat.c_str(), ConsoleHardwareId, backslashes(dir).c_str(), files, 1) != FALSE;
    bool signedOk = false;
    if (catalog) {
        say("Signing it with a certificate made for this one package (its private key is deleted afterwards)");
        signedOk = SelfSignFile(cat.c_str(), CertSubject) != FALSE;
    }
    pki_set_log_sink(nullptr, nullptr);
    if (!catalog || !signedOk) {
        error = catalog ? "could not sign the driver package" : "could not make the driver's catalog";
        return false;
    }
    say("Installing the driver for " + string(ConsoleHardwareId));
    BOOL reboot = FALSE;
    if (UpdateDriverForPlugAndPlayDevicesW(nullptr, wide(ConsoleHardwareId).c_str(), wide(inf).c_str(),
                                           INSTALLFLAG_FORCE, &reboot)) {
        say(reboot ? "Driver installed - Windows wants a restart before it is used" : "Driver installed");
        return true;
    }
    DWORD code = GetLastError();
    if (code != ERROR_NO_SUCH_DEVINST) {
        error = "Windows refused the driver: " + errorText(code);
        return false;
    }
    // the console is not plugged in: staged, so it binds the next time it is
    wchar_t dest[MAX_PATH] = {0};
    if (!SetupCopyOEMInfW(wide(inf).c_str(), nullptr, SPOST_PATH, 0, dest, MAX_PATH, nullptr, nullptr)) {
        error = "Windows refused the driver: " + errorText(GetLastError());
        return false;
    }
    say("Driver added to Windows (" + narrow(dest) + ") - it is used when the console is connected");
    return true;
}

//*******************************
// installConsoleDriverElevated
//*******************************
bool installConsoleDriverElevated(const string &dir, const function<void(const string &)> &say, string &error) {
    if (!ableem::DirEntry::createDirs(dir)) {
        error = "could not create " + dir;
        return false;
    }
    const string log = backslashes(dir + "/driver-install.log");
    DeleteFileW(wide(log).c_str());
    wchar_t self[MAX_PATH * 4] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    const wstring params = L"--install-driver " + quoted(wide(backslashes(dir))) + L" --log " + quoted(wide(log));
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = self;
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    say("Asking Windows for administrator rights to install the driver");
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        DWORD code = GetLastError();
        error = code == ERROR_CANCELLED ? "the driver was not installed - Windows' question was answered No"
                                        : "could not start the driver install: " + errorText(code);
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    ifstream in(log);
    string line, lastError;
    while (getline(in, line)) {
        if (line.compare(0, 7, "ERROR: ") == 0)
            lastError = line.substr(7);
        else
            say(line);
    }
    if (code != 0) {
        error = lastError.empty() ? "the driver install ended with code " + to_string(code) : lastError;
        return false;
    }
    return true;
}

#endif
