//
// RecoveryJob - see the header.
//
#include "recovery_job.h"

#include "installer/install_job_base.h" // humanSize

#include <ableem/engine/filesystem.h>
#include <ableem/engine/zip_archive.h>

#include <fstream>

using namespace std;
using ableem::DirEntry;

const char *const RecoveryJob::MiscPartition = "MISC";
const char *const RecoveryJob::RecoveryOffFile = "recovery-off.img";

namespace {

// the room the unpacked images need beyond their own size
const uint64_t SpareBytes = 64ull * 1024 * 1024;

// one fastboot command: echoed, its lines to the listener, the bytes it sent to `sent` as they are
// acknowledged; a lost console ends it. The exit code; `out` holds what it printed
int command(Fastboot &fastboot, InstallListener &listener, const vector<string> &args, FastbootOutput &out,
            const function<void(uint64_t)> &sent = nullptr) {
    string shown = "> fastboot";
    for (const string &a : args)
        shown += " " + (a.find(' ') != string::npos ? "\"" + a + "\"" : a);
    listener.onLine(shown);
    out.onLine = [&listener](const string &line) { listener.onLine("  " + line); };
    uint64_t reported = 0;
    int code = fastboot.run(args, [&](const string &text) {
        out.feed(text);
        if (sent && out.sentBytes != reported) {
            reported = out.sentBytes;
            sent(reported);
        }
        return !out.deviceLost;
    });
    out.finish();
    return code;
}

string reasonOf(const FastbootOutput &out, int code, const string &what) {
    if (out.deviceLost)
        return what + ": the console disconnected (fastboot is waiting for a device)";
    if (code < 0)
        return what + ": fastboot could not be started";
    return what + ": " + (out.failure.empty() ? "fastboot ended with code " + to_string(code) : out.failure);
}

} // namespace

//*******************************
// RecoveryJob::phasesFor
//*******************************
vector<string> RecoveryJob::phasesFor(const LbootImage &image, bool reboot) {
    vector<string> phases = {"Checking the backup", "Checking the console", "Unpacking the backup"};
    for (const LbootPartition &p : image.images)
        phases.push_back("Writing " + p.entry + " to " + p.partition);
    phases.push_back("Turning Sony's recovery off");
    if (reboot)
        phases.push_back("Restarting the console");
    return phases;
}

//*******************************
// RecoveryJob::findConsole
//*******************************
bool RecoveryJob::findConsole(Fastboot &fastboot, string &serial, string &error) {
    FastbootOutput out;
    int code = fastboot.run({"devices"}, [&out](const string &text) {
        out.feed(text);
        return true;
    });
    out.finish();
    if (code < 0) {
        error = "fastboot could not be started";
        return false;
    }
    vector<string> serials = FastbootOutput::devices(out.all);
    if (serials.empty()) {
        error = "no console in fastboot mode";
        return false;
    }
    if (serials.size() > 1) {
        error = to_string(serials.size()) + " devices in fastboot mode - connect only the console";
        return false;
    }
    serial = serials.front();
    return true;
}

//*******************************
// RecoveryJob::run
//*******************************
bool RecoveryJob::run(const RecoveryOptions &options, Fastboot &fastboot, InstallListener &listener,
                      const ShouldStop &shouldStop, string &error) {
    const LbootImage image = LbootImage::inspect(options.backupFile);
    const vector<string> phases = phasesFor(image, options.reboot);
    const int total = static_cast<int>(phases.size());
    int index = 0;
    auto phase = [&]() {
        index++;
        listener.onPhase(index, total, phases[static_cast<size_t>(index - 1)]);
    };
    auto stopped = [&]() {
        if (shouldStop && shouldStop()) {
            error = "Stopped - nothing was written to the console";
            return true;
        }
        return false;
    };

    // 1. the backup
    phase();
    listener.onLine("Backup: " + options.backupFile);
    if (!image.usable()) {
        error = "The backup cannot be used: " + image.error;
        return false;
    }
    listener.onLine(image.autobleem ? "Made by AutoBleem (abflashkit)"
                                    : "Not made by AutoBleem - flashed all the same");
    for (const string &line : image.describe())
        listener.onLine("  " + line);
    const uint64_t needed = image.totalBytes() + SpareBytes;
    if (options.workDirFreeBytes > 0 && options.workDirFreeBytes < needed) {
        error = "Not enough room to unpack the backup in " + options.workDir + ": " + humanSize(needed) + " needed, " +
                humanSize(options.workDirFreeBytes) + " free";
        return false;
    }
    if (stopped())
        return false;

    // 2. the console: there, one of it, and every partition big enough for its image
    phase();
    string serial;
    if (!findConsole(fastboot, serial, error)) {
        error = "The console is not in fastboot mode: " + error;
        return false;
    }
    listener.onLine("Console in fastboot mode: " + serial);
    {
        FastbootOutput out;
        if (command(fastboot, listener, {"getvar", "product"}, out) == 0) {
            string product;
            if (FastbootOutput::getvar(out.all, "product", product))
                listener.onLine("Product: " + product);
        }
    }
    vector<LbootPartition> checked = image.images;
    checked.push_back({RecoveryOffFile, MiscPartition, RecoveryOffSize});
    for (const LbootPartition &p : checked) {
        FastbootOutput out;
        const string var = "partition-size:" + p.partition;
        int code = command(fastboot, listener, {"getvar", var}, out);
        if (out.deviceLost) {
            error = reasonOf(out, code, "Checking " + p.partition);
            return false;
        }
        string value;
        uint64_t size = 0;
        if (code != 0 || !FastbootOutput::getvar(out.all, var, value) || !FastbootOutput::parseSize(value, size)) {
            listener.onLine("  the console does not say how big " + p.partition + " is - not checked");
            continue;
        }
        listener.onLine("  " + p.partition + ": " + humanSize(size) + ", the image " + humanSize(p.size));
        if (p.size > size) {
            error = p.entry + " (" + humanSize(p.size) + ") does not fit " + p.partition + " (" + humanSize(size) +
                    ") - this backup is not from this kind of console";
            return false;
        }
    }
    if (stopped())
        return false;

    // 3. unpacked into a fresh folder
    phase();
    const string imagesDir = options.workDir + "/images";
    if (DirEntry::exists(imagesDir))
        DirEntry::removeDirAndContents(imagesDir);
    if (!DirEntry::createDirs(imagesDir)) {
        error = "Could not create " + imagesDir;
        return false;
    }
    listener.onLine("Unpacking into " + imagesDir);
    auto cleanUp = [&]() { DirEntry::removeDirAndContents(imagesDir); };
    if (!ableem::ZipArchive::extract(options.backupFile, imagesDir,
                                     [&listener](uint64_t done, uint64_t all) { listener.onProgress(done, all); })) {
        error = "Could not unpack the backup (see the log) - is there room in " + options.workDir + "?";
        cleanUp();
        return false;
    }
    for (const LbootPartition &p : image.images) {
        const long long size = DirEntry::fileSize(imagesDir + "/" + p.entry);
        if (size < 0 || static_cast<uint64_t>(size) != p.size) {
            error = p.entry + " did not unpack whole";
            cleanUp();
            return false;
        }
    }
    const string recoveryOff = imagesDir + "/" + RecoveryOffFile;
    {
        ofstream off(recoveryOff, ios::binary);
        const char zeros[RecoveryOffSize] = {0};
        off.write(zeros, RecoveryOffSize);
        if (!off) {
            error = "Could not write " + recoveryOff;
            cleanUp();
            return false;
        }
    }
    if (stopped()) {
        cleanUp();
        return false;
    }

    // 4. the images - from here on a stop is not honoured
    for (const LbootPartition &p : image.images) {
        phase();
        FastbootOutput out;
        const uint64_t size = p.size;
        listener.onProgress(0, size);
        int code = command(fastboot, listener, {"flash", p.partition, imagesDir + "/" + p.entry}, out,
                           [&listener, size](uint64_t sent) { listener.onProgress(sent < size ? sent : size, size); });
        if (code != 0) {
            error = reasonOf(out, code, "Writing " + p.partition);
            cleanUp();
            return false;
        }
        listener.onProgress(size, size);
        if (shouldStop && shouldStop())
            listener.onLine("A stop was asked for - the rest is written first, a half-written console does not start");
    }

    // 5. MISC cleared: the console starts normally instead of into Sony's recovery
    phase();
    {
        FastbootOutput out;
        int code = command(fastboot, listener, {"flash", MiscPartition, recoveryOff}, out);
        if (code != 0) {
            error = reasonOf(out, code, "Clearing MISC") +
                    " - the images are written, but the console may start into Sony's recovery";
            cleanUp();
            return false;
        }
    }
    cleanUp();

    // 6. restarted
    if (options.reboot) {
        phase();
        FastbootOutput out;
        int code = command(fastboot, listener, {"reboot"}, out);
        if (code != 0)
            listener.onLine("The console did not restart by itself (" + reasonOf(out, code, "reboot") +
                            ") - unplug it and plug it in again");
    }
    listener.onLine("Recovery finished");
    return true;
}
