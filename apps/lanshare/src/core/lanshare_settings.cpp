//
// LanShareSettings - see the header.
//
#include "lanshare_settings.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/strings.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

using namespace std;

namespace {

string lower(string s) {
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

// a folder compared as Windows compares it: any case, either slash, no trailing one
string key(const string &folder) {
    string k = lower(folder);
    replace(k.begin(), k.end(), '\\', '/');
    while (k.size() > 1 && k.back() == '/')
        k.pop_back();
    return k;
}

bool within(const string &inner, const string &outer) {
    return inner.size() > outer.size() && inner.compare(0, outer.size(), outer) == 0 && inner[outer.size()] == '/';
}

} // namespace

//*******************************
// LanShareSettings::load / save
//*******************************
bool LanShareSettings::load(const string &file) {
    ifstream in(file, ios::binary);
    if (!in)
        return false;
    LanShareSettings s;
    string line;
    while (getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == string::npos)
            continue;
        const string k = ableem::Strings::trim(line.substr(0, eq));
        const string v = ableem::Strings::trim(line.substr(eq + 1));
        if (k == "port") {
            const int p = atoi(v.c_str());
            if (p > 0 && p <= 65535)
                s.port = p;
        } else if (k == "name") {
            s.name = v;
        } else if (k == "covers") {
            s.coversDir = v;
        } else if (k == "rdb") {
            s.rdbFile = v;
        } else if (k == "tray") {
            s.keepInTray = v != "0";
        } else if (k == "library") {
            const size_t bar = v.find('|');
            if (bar != string::npos && bar > 0 && bar + 1 < v.size())
                s.libraries.push_back({v.substr(0, bar), v.substr(bar + 1)});
        }
    }
    *this = s;
    return true;
}

bool LanShareSettings::save(const string &file) const {
    const string tmp = file + ".tmp";
    {
        ofstream out(tmp, ios::binary | ios::trunc);
        out << "# AutoBleem LAN Share\n"
            << "port=" << port << "\n"
            << "name=" << name << "\n"
            << "covers=" << coversDir << "\n"
            << "rdb=" << rdbFile << "\n"
            << "tray=" << (keepInTray ? 1 : 0) << "\n";
        for (const ableem::LanLibrary::Root &r : libraries)
            out << "library=" << r.name << "|" << r.dir << "\n";
        if (!out)
            return false;
    }
    return ableem::DirEntry::replaceFile(tmp, file);
}

//*******************************
// LanShareSettings::nameFor / canAdd
//*******************************
string LanShareSettings::nameFor(const string &folder) const {
    string path = folder;
    replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    const size_t slash = path.find_last_of('/');
    string base = slash == string::npos ? path : path.substr(slash + 1);
    base.erase(remove(base.begin(), base.end(), '|'), base.end());
    if (base.empty() || base.back() == ':')
        base = "Games"; // a drive's root: "D:"
    string name = base;
    for (int n = 2;; n++) {
        bool taken = false;
        for (const ableem::LanLibrary::Root &r : libraries)
            taken = taken || lower(r.name) == lower(name);
        if (!taken)
            return name;
        name = base + " (" + to_string(n) + ")";
    }
}

bool LanShareSettings::canAdd(const string &folder, string &why) const {
    const string k = key(folder);
    for (const ableem::LanLibrary::Root &r : libraries) {
        const string existing = key(r.dir);
        if (k == existing) {
            why = "this folder is already shared as \"" + r.name + "\"";
            return false;
        }
        if (within(k, existing)) {
            why = "this folder is inside \"" + r.name + "\", which is shared already";
            return false;
        }
        if (within(existing, k)) {
            why = "\"" + r.name + "\" is inside this folder - remove it first, then add this one";
            return false;
        }
    }
    return true;
}

//*******************************
// LanShareSettings::serverConfig
//*******************************
ableem::LanServer::Config LanShareSettings::serverConfig(const string &stateDir, const string &version,
                                                         const string &computerName) const {
    ableem::LanServer::Config c;
    c.port = port;
    c.name = name.empty() ? (computerName.empty() ? "My games" : computerName) : name;
    c.version = version;
    c.library.roots = libraries;
    c.library.coversDir = coversDir;
    c.library.rdbFile = rdbFile;
    c.library.stateDir = stateDir;
    return c;
}
