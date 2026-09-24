//
// LastResortRecovery's core: an LBOOT.EPB read as the recovery reads it, fastboot's output, and the whole
// recovery against a scripted fastboot - a real zip in a TempDir, the client a fake that records every command.
//
#include "doctest/doctest.h"

#include "support/temp_dir.h"

#include "core/fastboot.h"
#include "core/lboot_image.h"
#include "core/recovery_job.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/zip_writer.h>

#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

using ableem::DirEntry;
using std::string;
using std::vector;

namespace {

string fwd(string path) {
    for (char &c : path)
        c = c == '\\' ? '/' : c;
    return path;
}

// a backup as abflashkit writes one: the images, then (for an AutoBleem one) the trailer ending "autobleem"
string makeBackup(const TempDir &tmp, const std::map<string, string> &entries, bool autobleem = true,
                  const string &name = "LBOOT.EPB") {
    const string path = fwd(tmp.at(name));
    ableem::ZipWriter zip;
    REQUIRE(zip.open(path));
    for (const auto &e : entries)
        REQUIRE(zip.addBytes(e.first, e.second));
    REQUIRE(zip.close());
    if (autobleem) {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << "RAWCAW" << string(4081, '\0') << "autobleem";
    }
    return path;
}

// fastboot, scripted: what each command prints and returns; every command is recorded
class FakeFastboot : public Fastboot {
public:
    struct Reply {
        string text;
        int code = 0;
    };
    vector<vector<string>> commands;
    std::function<Reply(const vector<string> &)> reply;

    int run(const vector<string> &args, const Output &output) override {
        commands.push_back(args);
        Reply r = reply ? reply(args) : Reply{};
        // the text in two pieces, as a pipe delivers it
        const size_t half = r.text.size() / 2;
        if (!output(r.text.substr(0, half)))
            return -1;
        if (!output(r.text.substr(half)))
            return -1;
        return r.code;
    }
    // the commands with their first argument `verb`
    vector<vector<string>> named(const string &verb) const {
        vector<vector<string>> out;
        for (const auto &c : commands)
            if (!c.empty() && c[0] == verb)
                out.push_back(c);
        return out;
    }
};

// a console that answers everything: one device, every partition 64 MB, every flash OKAY
FakeFastboot::Reply healthyConsole(const vector<string> &args) {
    if (args[0] == "devices")
        return {"0123456789ABCDEF\t fastboot\n", 0};
    if (args[0] == "getvar" && args[1] == "product")
        return {"product: aiv8167\nFinished. Total time: 0.001s\n", 0};
    if (args[0] == "getvar")
        return {args[1] + ": 0x4000000\nFinished. Total time: 0.001s\n", 0};
    if (args[0] == "flash") {
        const long long size = DirEntry::fileSize(args[2]);
        return {"Sending '" + args[1] + "' (" + std::to_string((size + 1023) / 1024) +
                    " KB)            OKAY [  0.010s]\nWriting '" + args[1] +
                    "'            OKAY [  0.010s]\nFinished. Total time: 0.030s\n",
                0};
    }
    if (args[0] == "reboot")
        return {"Rebooting                                          OKAY [  0.001s]\nFinished.\n", 0};
    return {"", 1};
}

class Recorder : public InstallListener {
public:
    vector<string> phases;
    vector<string> lines;
    uint64_t lastDone = 0, lastTotal = 0;
    void onPhase(int, int, const string &title) override { phases.push_back(title); }
    void onProgress(uint64_t done, uint64_t total) override {
        lastDone = done;
        lastTotal = total;
    }
    void onLine(const string &line) override { lines.push_back(line); }
    bool said(const string &part) const {
        for (const string &l : lines)
            if (l.find(part) != string::npos)
                return true;
        return false;
    }
};

const std::map<string, string> FlashBackup = {
    {"boot.img", string(3000, 'B')}, {"userdata.ext4", string(5000, 'U')}, {"tz.img", string(700, 'T')}};

} // namespace

TEST_CASE("LbootImage: an AutoBleem backup - every image mapped to its partition, the kernel first") {
    TempDir tmp("lrr_inspect");
    const string path = makeBackup(tmp, FlashBackup);
    LbootImage image = LbootImage::inspect(path);
    CHECK(image.readable);
    CHECK(image.autobleem);
    CHECK(image.usable());
    REQUIRE(image.images.size() == 3);
    CHECK(image.images[0].entry == "boot.img");
    CHECK(image.images[0].partition == "BOOTIMG1");
    CHECK(image.images[0].size == 3000);
    CHECK(image.images[1].partition == "TEE1");
    CHECK(image.images[2].partition == "USRDATA");
    CHECK(image.unknown.empty());
    CHECK(image.totalBytes() == 8700);
}

TEST_CASE("LbootImage: a full backup has the rootfs; another tool's is read too; unknown entries are left alone") {
    TempDir tmp("lrr_inspect2");
    std::map<string, string> full = FlashBackup;
    full["rootfs.ext4"] = string(4000, 'R');
    full["notes.txt"] = "hello";
    const string path = makeBackup(tmp, full, false);
    LbootImage image = LbootImage::inspect(path);
    CHECK(image.readable);
    CHECK_FALSE(image.autobleem);
    REQUIRE(image.images.size() == 4);
    CHECK(image.images[2].entry == "rootfs.ext4");
    CHECK(image.images[2].partition == "ROOTFS1");
    CHECK(image.images[3].entry == "userdata.ext4");
    REQUIRE(image.unknown.size() == 1);
    CHECK(image.unknown[0] == "notes.txt");
}

TEST_CASE("LbootImage: not a zip, or no kernel in it - not usable, with the reason") {
    TempDir tmp("lrr_inspect3");
    tmp.writeFile("junk.epb", "this is not a zip");
    LbootImage junk = LbootImage::inspect(fwd(tmp.at("junk.epb")));
    CHECK_FALSE(junk.readable);
    CHECK_FALSE(junk.usable());
    CHECK_FALSE(junk.error.empty());

    const string noKernel = makeBackup(tmp, {{"tz.img", "T"}}, true, "nokernel.epb");
    LbootImage image = LbootImage::inspect(noKernel);
    CHECK(image.readable);
    CHECK_FALSE(image.usable());
    CHECK(image.error.find("boot.img") != string::npos);
}

TEST_CASE("FastbootOutput: lines, acknowledged bytes, a failure, a lost device") {
    FastbootOutput out;
    vector<string> lines;
    out.onLine = [&lines](const string &l) { lines.push_back(l); };
    out.feed("Sending sparse 'USRDATA' 1/2 (262140 KB)    OK");
    CHECK(out.sentBytes == 0); // not yet acknowledged
    out.feed("AY [  8.1s]\nSending sparse 'USRDATA' 2/2 (100 KB)   OKAY [ 0.1s]\r\n");
    CHECK(out.sentBytes == (262140ull + 100) * 1024);
    out.feed("Writing 'USRDATA'   FAILED (remote: 'write failed')\n");
    CHECK(out.failure == "Writing 'USRDATA'   FAILED (remote: 'write failed')");
    CHECK(lines.size() == 3);
    CHECK_FALSE(out.deviceLost);
    out.feed("< waiting for any device >");
    CHECK(out.deviceLost);
}

TEST_CASE("FastbootOutput: devices and getvar") {
    auto serials = FastbootOutput::devices("0123456789ABCDEF\t fastboot\nXYZ\tdevice\n");
    REQUIRE(serials.size() == 1);
    CHECK(serials[0] == "0123456789ABCDEF");
    string value;
    CHECK(FastbootOutput::getvar("partition-size:MISC: 0x80000\nFinished. Total time: 0.001s\n", "partition-size:MISC",
                                 value));
    CHECK(value == "0x80000");
    uint64_t size = 0;
    CHECK(FastbootOutput::parseSize(value, size));
    CHECK(size == 0x80000);
    CHECK(FastbootOutput::parseSize("4096", size));
    CHECK(size == 4096);
    CHECK_FALSE(FastbootOutput::getvar("getvar:partition-size:MISC FAILED (remote: 'unknown')\n", "partition-size:MISC",
                                       value));
}

TEST_CASE("RecoveryJob: the whole recovery - every image flashed in order, MISC cleared last, then a reboot") {
    TempDir tmp("lrr_run");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = healthyConsole;
    Recorder rec;
    string error;
    REQUIRE_MESSAGE(RecoveryJob::run(o, fb, rec, nullptr, error), error);

    auto flashes = fb.named("flash");
    REQUIRE(flashes.size() == 4);
    CHECK(flashes[0][1] == "BOOTIMG1");
    CHECK(flashes[1][1] == "TEE1");
    CHECK(flashes[2][1] == "USRDATA");
    CHECK(flashes[3][1] == "MISC");
    CHECK(flashes[0][2] == o.workDir + "/images/boot.img");
    REQUIRE(fb.named("reboot").size() == 1);
    CHECK(fb.commands.back()[0] == "reboot");
    // every partition was measured before anything was written
    CHECK(fb.named("getvar").size() == 5); // product + three images + MISC
    CHECK(rec.phases == RecoveryJob::phasesFor(LbootImage::inspect(o.backupFile), true));
    CHECK(rec.said("Product: aiv8167"));
    CHECK(rec.said("> fastboot flash BOOTIMG1"));
    // the scratch images are gone afterwards
    CHECK_FALSE(DirEntry::exists(o.workDir + "/images"));
}

TEST_CASE("RecoveryJob: the MISC image is abflashkit's recovery-off.img - sixteen zero bytes") {
    TempDir tmp("lrr_misc");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    string miscBytes;
    fb.reply = [&miscBytes](const vector<string> &args) {
        if (args[0] == "flash" && args[1] == "MISC") {
            std::ifstream in(args[2], std::ios::binary);
            miscBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        return healthyConsole(args);
    };
    Recorder rec;
    string error;
    REQUIRE(RecoveryJob::run(o, fb, rec, nullptr, error));
    CHECK(miscBytes == string(16, '\0'));
}

TEST_CASE("RecoveryJob: no console, or two - nothing flashed") {
    TempDir tmp("lrr_noconsole");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = [](const vector<string> &args) {
        if (args[0] == "devices")
            return FakeFastboot::Reply{"", 0};
        return healthyConsole(args);
    };
    Recorder rec;
    string error;
    CHECK_FALSE(RecoveryJob::run(o, fb, rec, nullptr, error));
    CHECK(error.find("fastboot mode") != string::npos);
    CHECK(fb.named("flash").empty());

    FakeFastboot two;
    two.reply = [](const vector<string> &args) {
        if (args[0] == "devices")
            return FakeFastboot::Reply{"AAA\tfastboot\nBBB\tfastboot\n", 0};
        return healthyConsole(args);
    };
    CHECK_FALSE(RecoveryJob::run(o, two, rec, nullptr, error));
    CHECK(error.find("2 devices") != string::npos);
    CHECK(two.named("flash").empty());
}

TEST_CASE("RecoveryJob: an image bigger than its partition - refused before anything is written") {
    TempDir tmp("lrr_toobig");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = [](const vector<string> &args) {
        if (args[0] == "getvar" && args[1] == "partition-size:USRDATA")
            return FakeFastboot::Reply{"partition-size:USRDATA: 0x100\n", 0};
        return healthyConsole(args);
    };
    Recorder rec;
    string error;
    CHECK_FALSE(RecoveryJob::run(o, fb, rec, nullptr, error));
    CHECK(error.find("does not fit USRDATA") != string::npos);
    CHECK(fb.named("flash").empty());
}

TEST_CASE("RecoveryJob: a console that does not answer partition-size is flashed all the same") {
    TempDir tmp("lrr_nosize");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = [](const vector<string> &args) {
        if (args[0] == "getvar")
            return FakeFastboot::Reply{"getvar:" + args[1] + " FAILED (remote: 'unknown variable')\n", 1};
        return healthyConsole(args);
    };
    Recorder rec;
    string error;
    REQUIRE_MESSAGE(RecoveryJob::run(o, fb, rec, nullptr, error), error);
    CHECK(fb.named("flash").size() == 4);
    CHECK(rec.said("not checked"));
}

TEST_CASE("RecoveryJob: a failed write stops there - no MISC, no reboot, the reason from fastboot") {
    TempDir tmp("lrr_fail");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = [](const vector<string> &args) {
        if (args[0] == "flash" && args[1] == "TEE1")
            return FakeFastboot::Reply{
                "Sending 'TEE1' (1 KB)   OKAY\nWriting 'TEE1'   FAILED (remote: 'flash write failure')\n", 1};
        return healthyConsole(args);
    };
    Recorder rec;
    string error;
    CHECK_FALSE(RecoveryJob::run(o, fb, rec, nullptr, error));
    CHECK(error.find("flash write failure") != string::npos);
    auto flashes = fb.named("flash");
    REQUIRE(flashes.size() == 2);
    CHECK(flashes[1][1] == "TEE1");
    CHECK(fb.named("reboot").empty());
    CHECK_FALSE(DirEntry::exists(o.workDir + "/images"));
}

TEST_CASE("RecoveryJob: the console unplugged mid-write - the client is ended, the job says so") {
    TempDir tmp("lrr_lost");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = [](const vector<string> &args) {
        if (args[0] == "flash" && args[1] == "USRDATA")
            return FakeFastboot::Reply{
                "Sending 'USRDATA' (5 KB)   FAILED (Write to device failed)\n< waiting for any device >", 0};
        return healthyConsole(args);
    };
    Recorder rec;
    string error;
    CHECK_FALSE(RecoveryJob::run(o, fb, rec, nullptr, error));
    CHECK(error.find("disconnected") != string::npos);
    CHECK(fb.named("reboot").empty());
}

TEST_CASE("RecoveryJob: a stop before the first write is honoured, after it the images are finished") {
    TempDir tmp("lrr_stop");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = healthyConsole;
    Recorder rec;
    string error;
    CHECK_FALSE(RecoveryJob::run(o, fb, rec, []() { return true; }, error));
    CHECK(error.find("Stopped") != string::npos);
    CHECK(fb.named("flash").empty());

    // asked once the first image is on its way: everything is still written
    FakeFastboot later;
    bool stop = false;
    later.reply = [&stop](const vector<string> &args) {
        if (args[0] == "flash")
            stop = true;
        return healthyConsole(args);
    };
    Recorder rec2;
    REQUIRE(RecoveryJob::run(o, later, rec2, [&stop]() { return stop; }, error));
    CHECK(later.named("flash").size() == 4);
    CHECK(rec2.said("A stop was asked for"));
}

TEST_CASE("RecoveryJob: not enough room to unpack - refused before the console is touched") {
    TempDir tmp("lrr_room");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    o.workDirFreeBytes = 1000;
    FakeFastboot fb;
    fb.reply = healthyConsole;
    Recorder rec;
    string error;
    CHECK_FALSE(RecoveryJob::run(o, fb, rec, nullptr, error));
    CHECK(error.find("Not enough room") != string::npos);
    CHECK(fb.commands.empty());
}

TEST_CASE("RecoveryJob: a failed reboot is only reported - the recovery itself is done") {
    TempDir tmp("lrr_reboot");
    RecoveryOptions o;
    o.backupFile = makeBackup(tmp, FlashBackup);
    o.workDir = fwd(tmp.at("work"));
    FakeFastboot fb;
    fb.reply = [](const vector<string> &args) {
        if (args[0] == "reboot")
            return FakeFastboot::Reply{"FAILED (remote: 'unknown command')\n", 1};
        return healthyConsole(args);
    };
    Recorder rec;
    string error;
    CHECK(RecoveryJob::run(o, fb, rec, nullptr, error));
    CHECK(rec.said("did not restart by itself"));
}
