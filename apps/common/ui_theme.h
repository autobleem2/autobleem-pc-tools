//
// The PC installers' shared look (the dark v02b one, design: autobleem-design themes/ab2.0.0/design/pcinstall):
// graphite window, dark title bar, Red Hat Text, owner-drawn cut-corner buttons, custom-drawn checkboxes, cyan
// progress bars, the hero on top. One window of one program calls these from its own window procedure; combo
// boxes and message boxes stay native by design.
//
//   at start-up   uitheme::loadFonts(), uitheme::graphiteBrush() as the class background, a GDI+ session for
//                 loadHero()
//   per window    applyDarkTitleBar(hwnd); styleProgress(bar) for each bar; BS_OWNERDRAW on every push button
//   in the proc   WM_CTLCOLOR* -> controlColor(); WM_DRAWITEM -> drawButton(); WM_NOTIFY with NM_CUSTOMDRAW from
//                 a checkbox -> drawCheckbox(); WM_PAINT -> paintHero()
//
#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include "ui_theme_ids.h"

namespace uitheme {

// the palette (COLORREF)
const COLORREF Graphite = RGB(0x21, 0x28, 0x31);  // the window
const COLORREF Panel = RGB(0x2e, 0x37, 0x42);     // button fill
const COLORREF Well = RGB(0x18, 0x1d, 0x25);      // log, edit, checkbox box, bar track
const COLORREF Rim = RGB(0x3a, 0x45, 0x52);       // pressed fill, disabled rim
const COLORREF Cyan = RGB(0x36, 0xd9, 0xe0);      // rims, bars, ticks
const COLORREF Magenta = RGB(0xff, 0x46, 0xaa);   // ONLY the default or focused button
const COLORREF Text = RGB(0xf4, 0xf6, 0xf8);
const COLORREF TextDim = RGB(0x9a, 0xa4, 0xb2);

const int HeroHeight = 150; // the picture's height at 96 dpi; a 1 px cyan line runs under it, across the width

// a size laid out at 96 dpi, scaled to the screen's dpi: every program is system-dpi-aware (the manifest) and its
// fonts are scaled, so its window, hero and controls are too (paintHero takes the scaled width)
int px(int value);

// the height `control`'s text needs when it wraps at `width` pixels in `font` (a multi-line static label)
int wrappedHeight(HWND control, HFONT font, int width);

// the window background brush (shared, never deleted)
HBRUSH graphiteBrush();

// Red Hat Text Medium (labels, checkboxes, log) and SemiBold (status lines, phase label, buttons), 12 px at the
// screen's dpi, loaded from the exe's RCDATA UI_RES_FONT_*. When a font cannot be loaded that one falls back to
// the system message font (Segoe UI), SemiBold as FW_SEMIBOLD of it. The caller deletes the two HFONTs.
void loadFonts(HFONT &medium, HFONT &semibold);

// the exe's icon (resource 1, the multi-size autobleem.ico) on the window at the big and the small size, for the
// taskbar, Alt-Tab and the title bar - the class icon alone is only the 32 px one
void setWindowIcons(HWND hwnd);

// dark title bar on Windows 10 2004+/11 (DWM immersive dark mode); older Windows ignore it. Returns false then.
bool applyDarkTitleBar(HWND hwnd);

// the hero picture: the 1x or, above 96 dpi, the 2x from the RCDATA ids; null when the resource is missing or
// not a picture (no hero in the exe yet) - paintHero() then draws the fallback. Needs a running GDI+.
Gdiplus::Image *loadHero();

// the hero rectangle (0,0,width,HeroHeight) and the cyan line under it; with a null image graphite plus
// "AutoBleem 2" in the semibold font
void paintHero(HDC dc, int width, Gdiplus::Image *hero, HFONT semibold);

// WM_CTLCOLORSTATIC / EDIT / LISTBOX / BTN: returns true and sets `result` when the control is one the look
// paints (a static label, the log list box, an edit); combo boxes and everything else are left to the system.
// `msg` is the WM_CTLCOLOR* message, `dc` and `control` its WPARAM and LPARAM.
bool controlColor(UINT msg, HDC dc, HWND control, LRESULT &result);

// WM_DRAWITEM of a BS_OWNERDRAW push button: the cut-corner shape. `isDefault` = this is the window's default
// button (magenta rim, as is the focused one).
void drawButton(const DRAWITEMSTRUCT &item, HFONT semibold, bool isDefault);

// NM_CUSTOMDRAW of a BS_AUTOCHECKBOX button (comctl32 v6): paints it and returns CDRF_SKIPDEFAULT at
// CDDS_PREPAINT, CDRF_DODEFAULT for the other stages
LRESULT drawCheckbox(const NMCUSTOMDRAW &draw, HFONT medium);

// a progress bar without its visual theme: the cyan bar on the dark track (the marquee one too)
void styleProgress(HWND bar);

} // namespace uitheme

#endif
