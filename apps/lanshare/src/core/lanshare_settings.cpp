//
// LanShareSettings - see the header.
//
#include "lanshare_settings.h"

#include <ableem/engine/filesystem.h>
#include <ableem/engine/strings.h>

#include <cstdlib>
#include <fstream>

using namespace std;

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
        if (k == "server") {
            s.serverUrl = v;
        } else if (k == "token") {
            s.token = v;
        } else if (k == "share") {
            s.shareDir = v;
        } else if (k == "folder") {
            s.localFolder = v;
        } else if (k == "local") {
            s.localServer = v == "1";
        } else if (k == "port") {
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
        } else if (k == "library" && s.localFolder.empty()) { // the first LAN Share's folders
            const size_t bar = v.find('|');
            if (bar != string::npos && bar + 1 < v.size())
                s.localFolder = v.substr(bar + 1);
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
            << "server=" << serverUrl << "\n"
            << "token=" << token << "\n"
            << "share=" << shareDir << "\n"
            << "folder=" << localFolder << "\n"
            << "local=" << (localServer ? 1 : 0) << "\n"
            << "port=" << port << "\n"
            << "name=" << name << "\n"
            << "covers=" << coversDir << "\n"
            << "rdb=" << rdbFile << "\n"
            << "tray=" << (keepInTray ? 1 : 0) << "\n";
        if (!out)
            return false;
    }
    return ableem::DirEntry::replaceFile(tmp, file);
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
    c.library.gamesDir = localFolder;
    c.library.coversDir = coversDir;
    c.library.rdbFile = rdbFile;
    c.library.stateDir = stateDir;
    return c;
}
