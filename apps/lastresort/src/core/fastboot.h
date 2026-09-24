//
// Android's fastboot client, as the recovery drives it: one command at a time, its output as it comes.
// The Windows program runs platform-tools' fastboot.exe (win32_fastboot.*); the tests pass a script.
// FastbootOutput reads what the client prints - the lines, the bytes the console has taken, a lost device.
//
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

//******************
// Fastboot
//******************
class Fastboot {
public:
    // a piece of what the client printed (stdout and stderr together, not split into lines); false stops
    // the command - the client is ended
    using Output = std::function<bool(const std::string &text)>;
    virtual ~Fastboot() = default;
    // runs `fastboot <args>`; the exit code, -1 when the client could not be started (or was ended)
    virtual int run(const std::vector<std::string> &args, const Output &output) = 0;
};

//******************
// FastbootOutput
//******************
// what one command printed, read as it comes:
//   Sending 'BOOTIMG1' (6654 KB)                 OKAY [  0.215s]
//   Sending sparse 'USRDATA' 1/4 (262140 KB)     OKAY [  8.102s]
//   Writing 'BOOTIMG1'                           OKAY [  0.120s]
//   FAILED (remote: 'partition table doesn't exist')
//   < waiting for any device >
class FastbootOutput {
public:
    std::function<void(const std::string &line)> onLine; // every finished line

    void feed(const std::string &text);
    void finish(); // the command ended: an unfinished line counts as a line

    uint64_t sentBytes = 0;  // what the console acknowledged (every "Sending ... (N KB) ... OKAY")
    bool deviceLost = false; // the client is waiting for a device - the console went away
    std::string failure;     // the last "FAILED (...)" line, "" when none
    std::string all;         // everything, for the parsers below

    // the serials `fastboot devices` lists in fastboot mode
    static std::vector<std::string> devices(const std::string &text);
    // `fastboot getvar NAME` prints "NAME: value"; false when it did not
    static bool getvar(const std::string &text, const std::string &name, std::string &value);
    // "0x1000000" or "16777216"; false when neither
    static bool parseSize(const std::string &text, uint64_t &size);

private:
    std::string pending_;
    void line(const std::string &text);
};
