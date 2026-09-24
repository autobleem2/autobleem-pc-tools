//
// LanShareSettings: what LAN Share remembers between runs, in <settings dir>/settings.ini - plain key=value lines,
// so the file reads and edits by hand. Tested in tests/apps/test_lanshare_core.cpp; the window (win32_window.cpp)
// is the only other user.
//
// LAN Share manages a LAN server that runs elsewhere (docs/lan-share-plan.md, "Remote server"): its address, the
// upload token, and the share its games folder is on when there is one. The folder on this PC is where games are
// published from (and read discs land when there is no server); sharing it from this PC is an option, off by
// default.
//
#pragma once

#include <ableem/lanserver/lan_server.h>

#include <string>

struct LanShareSettings {
    // the server
    std::string serverUrl; // "http://192.168.68.144:8126" (the Store's source URL is fine too)
    std::string token;     // its upload token ("" when publishing through the share only)
    std::string shareDir;  // its games folder as this PC reaches it ("\\raspberrypi\games"; "" = upload over HTTP)
    // this PC
    std::string localFolder; // the games on this PC: published from, and shared when localServer is on
    bool localServer = false;
    int port = 8124;        // the local server's
    std::string name;       // the local server's name in the Store; "" = this PC's name
    std::string coversDir;  // covers{U,P,J}.db: titles and covers for the games here and for read discs
    std::string rdbFile;    // "Sony - PlayStation.rdb": titles and the check of a read disc
    bool keepInTray = true; // closing the window leaves it running in the tray

    // false (and the defaults kept) when there is no file yet. A settings file of the first LAN Share (the
    // app as the server: library=<name>|<folder> lines) gives its first folder as localFolder.
    bool load(const std::string &file);
    bool save(const std::string &file) const;

    // the local server these settings describe: localFolder, the checksums cached in stateDir
    ableem::LanServer::Config serverConfig(const std::string &stateDir, const std::string &version,
                                           const std::string &computerName) const;
};
