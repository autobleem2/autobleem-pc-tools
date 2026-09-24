//
// The console on this PC's USB, the Windows side: whether it is there in fastboot mode (0bb4:0c01 - the
// console's bootloader announces itself so, "MediaTek" / "Yocto") and whether a driver is bound to it; the
// fastboot client that talks to it (platform-tools' fastboot.exe, shipped next to the program); and the
// driver fastboot.exe needs, which Windows has none of - WinUSB for exactly that device, with the Android
// interface GUID fastboot.exe looks for, in a package signed on the spot (libwdi's pki.c, as Zadig does).
// Everything here is #ifdef _WIN32.
//
#pragma once

#include "core/fastboot.h"

#include <functional>
#include <string>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

//******************
// ConsoleUsb
//******************
struct ConsoleUsb {
    bool present = false;      // a 0bb4:0c01 device is plugged in
    std::string instanceId;    // Windows' name for it
    std::string service;       // the driver bound to it ("WinUSB"), "" when none
    unsigned long problem = 0; // Device Manager's problem code, 0 when none

    bool hasDriver() const { return !service.empty() && problem == 0; }
    std::string describe() const;
};

extern const char *const ConsoleHardwareId; // "USB\VID_0BB4&PID_0C01"

// the console, as Windows' device list has it now
ConsoleUsb findConsoleUsb();

//******************
// WindowsFastboot
//******************
class WindowsFastboot : public Fastboot {
public:
    explicit WindowsFastboot(std::string exe) : exe_(std::move(exe)) {}
    int run(const std::vector<std::string> &args, const Output &output) override;

    // platform-tools/fastboot.exe next to the program
    static std::string bundledPath();

private:
    std::string exe_;
};

//******************
// the driver
//******************
// writes the driver package (an .inf and its signed catalog) into `dir` and installs it for the console -
// bound at once when the console is plugged in, staged for the next time otherwise. Needs administrator
// rights. Every step to `say`; false with the reason
bool installConsoleDriver(const std::string &dir, const std::function<void(const std::string &)> &say,
                          std::string &error);

// the same from a program without administrator rights: this program again, elevated (Windows asks),
// with --install-driver; its lines come back through a log file. Blocks until it is done - call it off
// the UI thread
bool installConsoleDriverElevated(const std::string &dir, const std::function<void(const std::string &)> &say,
                                  std::string &error);

#endif
