//
// The flasher's window: the installer's look (the AutoBleem picture on top, then the questions, then the
// progress) over FlasherJob - a channel (or an image file), a stick, Write.
//
#pragma once

#include "installer/flasher_job.h"

#ifdef _WIN32
int runFlasherWindow(const FlashOptions &defaults);
#endif
