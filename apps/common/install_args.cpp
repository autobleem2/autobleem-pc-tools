#include "install_args.h"

using namespace std;

bool parseInstallArgs(int argc, char *argv[], InstallArgs &args) {
    string covers = "JUP";
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        auto value = [&](string &into) {
            if (i + 1 >= argc)
                return false;
            into = argv[++i];
            return true;
        };
        InstallOptions &options = args.options;
        if (arg == "--quiet")
            args.quiet = true;
        else if (arg == "--retroarch")
            options.retroarch = true;
        else if (arg == "--bios")
            options.bios = true;
        else if (arg == "--ps1-bios-only")
            options.ps1BiosOnly = options.bios = true;
        else if (arg == "--force")
            options.force = true;
        else if (arg == "--samples")
            options.samples = true;
        else if (arg == "--drive" && value(options.root)) {
        } else if (arg == "--covers" && value(covers)) {
        } else if (arg == "--package" && value(options.packageFile)) {
        } else if (arg == "--channel" && value(options.channel)) {
        } else if (arg == "--repo" && value(options.repoUrl)) {
        } else
            return false;
    }
    args.options.coversJapan = covers.find_first_of("Jj") != string::npos;
    args.options.coversUsa = covers.find_first_of("Uu") != string::npos;
    args.options.coversPal = covers.find_first_of("Pp") != string::npos;
    return true;
}
