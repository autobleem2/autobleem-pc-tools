//
// Resource ids the shared look (ui_theme.h) reads from the program's own .rc - plain #defines, so windres can
// include this file too. Every program that uses ui_theme embeds the two fonts under these ids; the hero
// pictures are the program's own (1x = 640x150, 2x = 1280x300, drawn when the screen is above 96 dpi).
//
#pragma once

#define UI_RES_FONT_MEDIUM 9001   // RCDATA: apps/common/resources/RedHatText-Medium.ttf (OFL)
#define UI_RES_FONT_SEMIBOLD 9002 // RCDATA: apps/common/resources/RedHatText-SemiBold.ttf (OFL)
#define UI_RES_HERO 9003          // RCDATA: the program's hero, 640x150 PNG
#define UI_RES_HERO_2X 9004       // RCDATA: the same at 1280x300
