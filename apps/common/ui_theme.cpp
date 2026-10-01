//
// The PC installers' shared look - see the header.
//
#ifdef _WIN32

#include "ui_theme.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace uitheme {

namespace {

HBRUSH makeBrush(COLORREF colour) {
    return CreateSolidBrush(colour);
}

HBRUSH wellBrush() {
    static HBRUSH brush = makeBrush(Well);
    return brush;
}

int screenDpi() {
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc)
        ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

// the bytes of an RCDATA resource of this exe
const void *resourceBytes(int id, DWORD &size) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res)
        return nullptr;
    HGLOBAL data = LoadResource(nullptr, res);
    size = SizeofResource(nullptr, res);
    const void *bytes = data ? LockResource(data) : nullptr;
    return size ? bytes : nullptr;
}

// the font the system really hands back for this face: GDI substitutes silently when a face is unknown, so
// the name is compared after the font is selected
HFONT makeFont(const wchar_t *face, int weight, int dpi) {
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(12, dpi, 96);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    HFONT font = CreateFontIndirectW(&lf);
    if (!font)
        return nullptr;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t got[LF_FACESIZE] = {0};
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    if (lstrcmpiW(got, face) != 0) {
        DeleteObject(font);
        return nullptr;
    }
    return font;
}

// the system message font (Segoe UI) at the given weight
HFONT systemFont(int weight) {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    LOGFONTW lf = metrics.lfMessageFont;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

// a font file of the exe made available to this process (it goes away with it); true when GDI took it
bool addResourceFont(int id) {
    DWORD size = 0;
    const void *bytes = resourceBytes(id, size);
    if (!bytes)
        return false;
    DWORD count = 0;
    return AddFontMemResourceEx(const_cast<void *>(bytes), size, nullptr, &count) != nullptr;
}

// the Medium and the SemiBold file are two families to GDI ("Red Hat Text Medium" / "... SemiBold", regular
// weight) or one family in two weights, depending on how the file names itself: try both spellings
HFONT redHat(int weight, const wchar_t *styleName, int dpi) {
    HFONT font = makeFont(L"Red Hat Text", weight, dpi);
    if (font)
        return font;
    wchar_t face[LF_FACESIZE];
    wcscpy(face, L"Red Hat Text ");
    wcsncat(face, styleName, LF_FACESIZE - 14);
    return makeFont(face, FW_NORMAL, dpi);
}

typedef HRESULT(WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT(WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);

void fillRect(HDC dc, const RECT &r, COLORREF colour) {
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

} // namespace

HBRUSH graphiteBrush() {
    static HBRUSH brush = makeBrush(Graphite);
    return brush;
}

int px(int value) {
    static const int dpi = screenDpi();
    return MulDiv(value, dpi, 96);
}

int wrappedHeight(HWND control, HFONT font, int width) {
    wchar_t text[1024] = {0};
    GetWindowTextW(control, text, 1023);
    HDC dc = GetDC(control);
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    RECT area = {0, 0, width, 0};
    DrawTextW(dc, text, -1, &area, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    if (old)
        SelectObject(dc, old);
    ReleaseDC(control, dc);
    return area.bottom - area.top;
}

//*******************************
// loadFonts
//*******************************
void loadFonts(HFONT &medium, HFONT &semibold) {
    const int dpi = screenDpi();
    medium = semibold = nullptr;
    if (addResourceFont(UI_RES_FONT_MEDIUM))
        medium = redHat(FW_MEDIUM, L"Medium", dpi);
    if (addResourceFont(UI_RES_FONT_SEMIBOLD))
        semibold = redHat(FW_SEMIBOLD, L"SemiBold", dpi);
    if (!medium)
        medium = systemFont(FW_NORMAL);
    if (!semibold)
        semibold = systemFont(FW_SEMIBOLD);
}

//*******************************
// setWindowIcons
//*******************************
void setWindowIcons(HWND hwnd) {
    HINSTANCE instance = GetModuleHandle(nullptr);
    // the .ico holds 16..256: ask for the sizes the title bar / taskbar / Alt-Tab really draw
    const int sizes[2][2] = {{GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)},
                             {GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON)}};
    const WPARAM kinds[2] = {ICON_BIG, ICON_SMALL};
    for (int i = 0; i < 2; i++) {
        HANDLE icon = LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, sizes[i][0], sizes[i][1], LR_DEFAULTCOLOR);
        if (icon)
            SendMessageW(hwnd, WM_SETICON, kinds[i], reinterpret_cast<LPARAM>(icon));
    }
}

//*******************************
// applyDarkTitleBar
//*******************************
bool applyDarkTitleBar(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm)
        return false;
    DwmSetWindowAttributeFn setAttribute =
        reinterpret_cast<DwmSetWindowAttributeFn>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    bool ok = false;
    if (setAttribute) {
        const BOOL on = TRUE;
        // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 2004+, 11); 19 is the same switch before 2004's final
        // build. An older Windows answers an error and keeps its light bar.
        ok = SUCCEEDED(setAttribute(hwnd, 20, &on, sizeof(on))) || SUCCEEDED(setAttribute(hwnd, 19, &on, sizeof(on)));
    }
    FreeLibrary(dwm);
    return ok;
}

//*******************************
// hero
//*******************************
Gdiplus::Image *loadHero() {
    DWORD size = 0;
    const void *bytes = nullptr;
    if (screenDpi() > 96)
        bytes = resourceBytes(UI_RES_HERO_2X, size);
    if (!bytes)
        bytes = resourceBytes(UI_RES_HERO, size);
    if (!bytes)
        return nullptr;
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy)
        return nullptr;
    memcpy(GlobalLock(copy), bytes, size);
    GlobalUnlock(copy);
    IStream *stream = nullptr;
    if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK) {
        GlobalFree(copy);
        return nullptr;
    }
    Gdiplus::Image *image = new Gdiplus::Image(stream);
    stream->Release();
    if (image->GetLastStatus() != Gdiplus::Ok) {
        delete image;
        return nullptr;
    }
    return image;
}

void paintHero(HDC dc, int width, Gdiplus::Image *hero, HFONT semibold) {
    const int heroHeight = px(HeroHeight);
    RECT area = {0, 0, width, heroHeight};
    if (hero) {
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.DrawImage(hero, Gdiplus::Rect(0, 0, width, heroHeight), 0, 0, static_cast<INT>(hero->GetWidth()),
                    static_cast<INT>(hero->GetHeight()), Gdiplus::UnitPixel);
    } else {
        fillRect(dc, area, Graphite);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Text);
        HGDIOBJ old = semibold ? SelectObject(dc, semibold) : nullptr;
        DrawTextW(dc, L"AutoBleem 2", -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old)
            SelectObject(dc, old);
    }
    RECT line = {0, heroHeight, width, heroHeight + 1};
    fillRect(dc, line, Cyan);
}

//*******************************
// controlColor
//*******************************
bool controlColor(UINT msg, HDC dc, HWND control, LRESULT &result) {
    wchar_t cls[32] = {0};
    GetClassNameW(control, cls, 31);
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        if (lstrcmpiW(cls, L"Static") != 0)
            return false;
        SetTextColor(dc, IsWindowEnabled(control) ? Text : TextDim);
        SetBkColor(dc, Graphite);
        result = reinterpret_cast<LRESULT>(graphiteBrush());
        return true;
    case WM_CTLCOLORBTN:
        SetTextColor(dc, Text);
        SetBkColor(dc, Graphite);
        result = reinterpret_cast<LRESULT>(graphiteBrush());
        return true;
    case WM_CTLCOLORLISTBOX: // the log; a combo's drop-down list is class ComboLBox and stays native
        if (lstrcmpiW(cls, L"ListBox") != 0)
            return false;
        SetTextColor(dc, TextDim);
        SetBkColor(dc, Well);
        result = reinterpret_cast<LRESULT>(wellBrush());
        return true;
    case WM_CTLCOLOREDIT:
        if (lstrcmpiW(cls, L"Edit") != 0)
            return false;
        SetTextColor(dc, Text);
        SetBkColor(dc, Well);
        result = reinterpret_cast<LRESULT>(wellBrush());
        return true;
    default:
        return false;
    }
}

//*******************************
// drawButton
//*******************************
void drawButton(const DRAWITEMSTRUCT &item, HFONT semibold, bool isDefault) {
    HDC dc = item.hDC;
    const RECT rc = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    fillRect(dc, rc, Graphite); // what shows in the two cut corners

    const COLORREF rim = disabled ? Rim : (isDefault || focused) ? Magenta : Cyan;
    const LONG l = rc.left + 1, t = rc.top + 1, r = rc.right - 2, b = rc.bottom - 2;
    const LONG cut = (rc.bottom - rc.top) * 30 / 100; // the top-right and bottom-left corners
    const POINT shape[6] = {{l, t}, {r - cut, t}, {r, t + cut}, {r, b}, {l + cut, b}, {l, b - cut}};

    HBRUSH fill = CreateSolidBrush(pressed && !disabled ? Rim : Panel);
    LOGBRUSH lb = {BS_SOLID, rim, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT | PS_JOIN_MITER, 2, &lb, 0, nullptr);
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Polygon(dc, shape, 6);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(fill);

    wchar_t text[128] = {0};
    GetWindowTextW(item.hwndItem, text, 127);
    HGDIOBJ oldFont = semibold ? SelectObject(dc, semibold) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? TextDim : Text);
    RECT label = rc;
    DrawTextW(dc, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont)
        SelectObject(dc, oldFont);
}

//*******************************
// drawCheckbox
//*******************************
LRESULT drawCheckbox(const NMCUSTOMDRAW &draw, HFONT medium) {
    if (draw.dwDrawStage != CDDS_PREPAINT)
        return CDRF_DODEFAULT;
    HDC dc = draw.hdc;
    const RECT rc = draw.rc;
    const bool disabled = (draw.uItemState & CDIS_DISABLED) != 0;
    const bool focused = (draw.uItemState & CDIS_FOCUS) != 0;
    const bool on = SendMessageW(draw.hdr.hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED;
    fillRect(dc, rc, Graphite);

    // the 14x14 box: a 1 px rim around the dark well
    const int size = px(14);
    const int x = rc.left, y = rc.top + (rc.bottom - rc.top - size) / 2;
    const RECT box = {x, y, x + size, y + size};
    const RECT inner = {x + 1, y + 1, x + size - 1, y + size - 1};
    fillRect(dc, box, disabled ? Rim : Cyan);
    fillRect(dc, inner, Well);
    if (on) {
        HPEN pen = CreatePen(PS_SOLID, px(2), disabled ? Rim : Cyan);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, x + size * 3 / 14, y + size / 2, nullptr);
        LineTo(dc, x + size * 6 / 14, y + size * 10 / 14);
        LineTo(dc, x + size * 11 / 14, y + size * 4 / 14);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    wchar_t text[256] = {0};
    GetWindowTextW(draw.hdr.hwndFrom, text, 255);
    HGDIOBJ oldFont = medium ? SelectObject(dc, medium) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? TextDim : Text);
    RECT label = {x + size + px(6), rc.top, rc.right, rc.bottom};
    DrawTextW(dc, text, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (focused) {
        RECT extent = label;
        DrawTextW(dc, text, -1, &extent, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        RECT focus = {label.left - 2, rc.top + 1, std::min<LONG>(extent.right + 2, rc.right), rc.bottom - 1};
        DrawFocusRect(dc, &focus);
    }
    if (oldFont)
        SelectObject(dc, oldFont);
    return CDRF_SKIPDEFAULT;
}

//*******************************
// styleProgress
//*******************************
void styleProgress(HWND bar) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (ux) {
        SetWindowThemeFn setTheme =
            reinterpret_cast<SetWindowThemeFn>(reinterpret_cast<void *>(GetProcAddress(ux, "SetWindowTheme")));
        if (setTheme)
            setTheme(bar, L"", L"");
        FreeLibrary(ux);
    }
    SendMessageW(bar, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(Cyan));
    SendMessageW(bar, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(Well));
}

} // namespace uitheme

#endif
