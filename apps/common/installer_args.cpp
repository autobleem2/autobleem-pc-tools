#include "installer_args.h"

using namespace std;

namespace installerargs {

//*******************************
// biosWithoutRetroArchWarning
//*******************************
string biosWithoutRetroArchWarning(bool bios, bool retroarch) {
    if (!bios || retroarch)
        return "";
    return "Warning: --bios has no effect without --retroarch - nothing is fetched (the PlayStation BIOS comes from "
           "the console, not from this installer). Add --retroarch to get the BIOS pack for RetroArch.";
}

//*******************************
// parse
//*******************************
bool parse(const vector<string> &args, Parsed &out) {
    for (size_t i = 0; i < args.size(); i++) {
        const string &arg = args[i];
        auto value = [&](string &into) {
            if (i + 1 >= args.size())
                return false;
            into = args[++i];
            return true;
        };
        if (arg == "--quiet")
            out.quiet = true;
        else if (arg == "--retroarch")
            out.options.retroarch = true;
        else if (arg == "--bios")
            out.options.bios = true;
        else if (arg == "--samples")
            out.options.samples = true;
        else if (arg == "--drive" && value(out.options.root)) {
        } else if (arg == "--covers" && value(out.covers)) {
        } else if (arg == "--package" && value(out.options.packageFile)) {
        } else if (arg == "--channel" && value(out.options.channel)) {
        } else if (arg == "--repo" && value(out.options.repoUrl)) {
        } else
            return false;
    }
    string warning = biosWithoutRetroArchWarning(out.options.bios, out.options.retroarch);
    if (!warning.empty()) {
        out.warnings.push_back(warning);
        out.options.bios = false; // a no-op, as warned
    }
    return true;
}

} // namespace installerargs
