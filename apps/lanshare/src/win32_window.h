//
// The LAN Share window: what is shared and where the Store finds it, the game folders, the games and their
// problems, reading a disc, the recent requests, and the tray. startInTray: the window stays hidden (Windows'
// start-up entry passes --tray).
//
#pragma once

#ifdef _WIN32

#include <string>

int runLanShareWindow(bool startInTray, const std::string &exePath);

#endif
