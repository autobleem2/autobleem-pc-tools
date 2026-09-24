//
// LanShareSettings: what LAN Share remembers between runs, in <settings dir>/settings.ini, and the LanServer it
// makes of them. Plain key=value lines (a library a line of its own, `library=<name>|<folder>`), so the file reads
// and edits by hand. Tested in tests/apps/test_lanshare_core.cpp; the window (win32_window.cpp) is the only
// other user.
//
#pragma once

#include <ableem/lanserver/lan_library.h>
#include <ableem/lanserver/lan_server.h>

#include <string>
#include <vector>

struct LanShareSettings {
    int port = 8124;
    std::string name;       // the source's name in the Store; "" = this PC's name
    std::string coversDir;  // covers{U,P,J}.db, for titles and covers
    std::string rdbFile;    // "Sony - PlayStation.rdb", for titles and the check of a read disc
    bool keepInTray = true; // closing the window leaves it serving from the tray
    std::vector<ableem::LanLibrary::Root> libraries;

    // false (and the defaults kept) when there is no file yet
    bool load(const std::string &file);
    bool save(const std::string &file) const;

    // a name for a new library from its folder ("D:\Games\PS1" -> "PS1"), unlike every existing one: " (2)"
    // and on, and never with a '/' (it starts every game's path in the Store)
    std::string nameFor(const std::string &folder) const;
    // false (and why) when the folder is already a library, or inside one, or holds one
    bool canAdd(const std::string &folder, std::string &why) const;

    // the server these settings describe: the libraries as its roots, the checksums cached in stateDir
    ableem::LanServer::Config serverConfig(const std::string &stateDir, const std::string &version,
                                           const std::string &computerName) const;
};
