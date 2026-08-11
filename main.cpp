// PowerPilot v0.2 - Idle-based Auto Shutdown with transparent OSD overlay
// Native Win32, no framework, no external deps.
//
// Behavior:
//   - User sets duration N (minutes) + overlay text size in the main window.
//   - Click Mulai -> transparent countdown overlay appears, main window auto-minimizes.
//   - Countdown only decreases while the user is IDLE (no keyboard/mouse activity).
//   - Any activity resets the countdown AND flashes a brief sub-line on the overlay.
//   - Overlay text can be dragged anywhere on screen (click + drag directly on it).
//   - Right-click the overlay for a one-item "Berhenti" menu.
//   - At 0 -> full power-off (EWX_POWEROFF, bypasses Fast Startup hybrid shutdown).
//   - No Windows toast/balloon notifications are used anywhere.

#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>

#define ID_EDIT_MINUTES    101
#define ID_EDIT_FONTSIZE   102
#define ID_BUTTON_START    103
#define ID_BUTTON_STOP     104
#define ID_STATIC_STATUS   105
#define ID_RADIO_EN        106
#define ID_RADIO_ID        107
#define ID_TIMER_TICK      1
#define ID_MENU_STOP       201

enum Language { LANG_EN = 0, LANG_ID = 1 };

enum StrId {
    STR_LABEL_DURATION = 0,
    STR_LABEL_FONTSIZE,
    STR_BTN_START,
    STR_BTN_STOP,
    STR_STATUS_INACTIVE,
    STR_STATUS_ACTIVE_FMT,   // used with swprintf, has %02d:%02d
    STR_STATUS_STOPPED_USER,
    STR_STATUS_CLOSED,
    STR_STATUS_SHUTDOWN_EXECUTED,
    STR_WARN_INVALID_MINUTES,
    STR_OVERLAY_SUBTEXT,
    STR_MENU_STOP,
    STR_COUNT
};

static const wchar_t* STRINGS_EN[STR_COUNT] = {
    L"Idle duration before shutdown (minutes):",
    L"Overlay text size (px):",
    L"Start",
    L"Stop",
    L"Inactive.",
    L"Active — shutdown in %02d:%02d if no activity",
    L"Stopped by user.",
    L"Closed.",
    L"Shutdown executed.",
    L"Enter a valid duration in minutes (greater than 0).",
    L"Activity detected",
    L"Stop",
};

static const wchar_t* STRINGS_ID[STR_COUNT] = {
    L"Durasi idle sebelum shutdown (menit):",
    L"Ukuran teks overlay (px):",
    L"Mulai",
    L"Berhenti",
    L"Tidak aktif.",
    L"Aktif — shutdown dalam %02d:%02d jika tidak ada aktivitas",
    L"Dihentikan oleh user.",
    L"Ditutup.",
    L"Shutdown dieksekusi.",
    L"Masukkan durasi menit yang valid (lebih dari 0).",
    L"Aktivitas terdeteksi",
    L"Berhenti",
};

static Language g_language = LANG_EN; // default per README/global-audience decision

static const wchar_t* T(StrId id) {
    return (g_language == LANG_EN) ? STRINGS_EN[id] : STRINGS_ID[id];
}

static HINSTANCE g_hInstance = nullptr;
static HWND g_hMainWnd = nullptr;
static HWND g_hEdit, g_hEditFontSize, g_hBtnStart, g_hBtnStop, g_hStatus;
static HWND g_hLabelDuration, g_hLabelFontSize;
static HWND g_hRadioEN, g_hRadioID;
static HWND g_hOverlay = nullptr;
static bool g_active = false;
static int  g_totalSeconds = 0;
static int  g_remainingSeconds = 0;
static int  g_overlayFontSize = 28;
static HFONT g_hFont = nullptr;

static POINT g_overlayPos = {20, 20}; // default: top-left corner
static bool  g_dragging = false;
static POINT g_dragStartCursor;
static POINT g_dragStartWindowPos;
static int   g_subTextTicks = 0; // ticks remaining to show the "activity detected" sub-line

static void StopFeature(HWND hwnd, const wchar_t* reasonStatus);

static void UpdateStatus(const wchar_t* text) {
    SetWindowTextW(g_hStatus, text);
}

static void FormatCountdown(wchar_t* buf, size_t bufLen, int seconds) {
    int m = seconds / 60;
    int s = seconds % 60;
    swprintf(buf, bufLen, T(STR_STATUS_ACTIVE_FMT), m, s);
}

static DWORD GetIdleMillis() {
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(LASTINPUTINFO);
    if (!GetLastInputInfo(&lii)) return 0;
    return GetTickCount() - lii.dwTime;
}

// --- Overlay rendering -----------------------------------------------
// Draws the countdown (and optional sub-line) into a 32bpp alpha bitmap
// and pushes it to the layered overlay window. Outline color is (1,1,1)
// rather than pure black on purpose: the alpha-fixup pass below detects
// "was this pixel touched by text" via != 0, and pure black would be
// indistinguishable from the untouched transparent background.
// Renders `text` as a grayscale coverage mask (white text on black bg into
// a throwaway bitmap): the resulting pixel luminance IS the anti-aliasing
// coverage at that pixel, which is exactly what we want as an alpha value.
// This avoids the old approach of drawing straight into the alpha canvas
// and force-setting every touched pixel to opaque -- that clipped partial
// (edge) coverage to full opacity, which is what produced the dark fringe.
static void ComputeTextMask(HDC refDC, HFONT font, const wchar_t* text, int width, int height,
                             int textX, int textY, bool dilateOutline, BYTE* outMask) {
    HDC memDC = CreateCompatibleDC(refDC);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    ZeroMemory(bits, (size_t)width * height * 4); // black background

    HFONT oldFont = (HFONT)SelectObject(memDC, font);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255)); // white on black -> luminance == coverage

    RECT r = {textX, textY, width, height};
    if (dilateOutline) {
        static const int offs[8][2] = {{-2,-2},{0,-2},{2,-2},{-2,0},{2,0},{-2,2},{0,2},{2,2}};
        for (auto& o : offs) {
            RECT rr = r; OffsetRect(&rr, o[0], o[1]);
            DrawTextW(memDC, text, -1, &rr, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
    } else {
        DrawTextW(memDC, text, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    SelectObject(memDC, oldFont);

    DWORD* px = (DWORD*)bits;
    for (int i = 0; i < width * height; i++) {
        BYTE v = (BYTE)(px[i] & 0xFF); // R==G==B for grayscale AA white-on-black
        if (v > outMask[i]) outMask[i] = v;
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
}

// Alpha-composites a solid `color` onto `bits` using `mask` as per-pixel
// coverage, with proper premultiplied-alpha "over" blending -- this is
// what makes edge pixels genuinely translucent instead of solid.
static void CompositeOver(void* bits, int width, int height, BYTE* mask, BYTE cr, BYTE cg, BYTE cb) {
    DWORD* px = (DWORD*)bits;
    for (int i = 0; i < width * height; i++) {
        BYTE srcA = mask[i];
        if (srcA == 0) continue;
        BYTE srcR = (BYTE)(cr * srcA / 255);
        BYTE srcG = (BYTE)(cg * srcA / 255);
        BYTE srcB = (BYTE)(cb * srcA / 255);

        DWORD d = px[i];
        BYTE dstA = (BYTE)((d >> 24) & 0xFF);
        BYTE dstR = (BYTE)((d >> 16) & 0xFF);
        BYTE dstG = (BYTE)((d >> 8) & 0xFF);
        BYTE dstB = (BYTE)(d & 0xFF);

        int invA = 255 - srcA;
        BYTE outA = (BYTE)(srcA + dstA * invA / 255);
        BYTE outR = (BYTE)(srcR + dstR * invA / 255);
        BYTE outG = (BYTE)(srcG + dstG * invA / 255);
        BYTE outB = (BYTE)(srcB + dstB * invA / 255);

        px[i] = ((DWORD)outA << 24) | ((DWORD)outR << 16) | ((DWORD)outG << 8) | outB;
    }
}

static void RenderOverlay() {
    if (!g_hOverlay) return;

    wchar_t line1[32];
    int m = g_remainingSeconds / 60, s = g_remainingSeconds % 60;
    swprintf(line1, 32, L"%02d:%02d", m, s);
    bool showSub = g_subTextTicks > 0;
    const wchar_t* subtext = T(STR_OVERLAY_SUBTEXT);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    // ANTIALIASED_QUALITY (grayscale AA), not ClearType: ClearType's
    // colored subpixel rendering assumes a real opaque background and
    // introduces faint red/blue fringing once reinterpreted as a mask.
    HFONT font = CreateFontW(-g_overlayFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    int subSize = g_overlayFontSize / 2;
    if (subSize < 10) subSize = 10;
    HFONT subFont = CreateFontW(-subSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    HFONT oldFont = (HFONT)SelectObject(memDC, font);
    SIZE sz1; GetTextExtentPoint32W(memDC, line1, (int)wcslen(line1), &sz1);
    SIZE sz2 = {0, 0};
    if (showSub) {
        SelectObject(memDC, subFont);
        GetTextExtentPoint32W(memDC, subtext, (int)wcslen(subtext), &sz2);
        SelectObject(memDC, font);
    }
    SelectObject(memDC, oldFont);

    const int padding = 6;
    int width  = (sz1.cx > sz2.cx ? sz1.cx : sz2.cx) + padding * 2;
    int height = sz1.cy + (showSub ? sz2.cy + 2 : 0) + padding * 2;
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    ZeroMemory(bits, (size_t)width * height * 4); // fully transparent start

    BYTE* fillMask = (BYTE*)calloc((size_t)width * height, 1);
    BYTE* outlineMask = (BYTE*)calloc((size_t)width * height, 1);

    ComputeTextMask(screenDC, font, line1, width, height, padding, padding, false, fillMask);
    ComputeTextMask(screenDC, font, line1, width, height, padding, padding, true, outlineMask);
    CompositeOver(bits, width, height, outlineMask, 0, 0, 0);       // outline behind
    CompositeOver(bits, width, height, fillMask, 255, 255, 255);    // fill on top

    if (showSub) {
        ZeroMemory(fillMask, (size_t)width * height);
        ZeroMemory(outlineMask, (size_t)width * height);
        int subY = padding + sz1.cy + 2;
        ComputeTextMask(screenDC, subFont, subtext, width, height, padding, subY, false, fillMask);
        ComputeTextMask(screenDC, subFont, subtext, width, height, padding, subY, true, outlineMask);
        CompositeOver(bits, width, height, outlineMask, 0, 0, 0);
        CompositeOver(bits, width, height, fillMask, 170, 215, 255);
    }

    free(fillMask);
    free(outlineMask);
    DeleteObject(font);
    DeleteObject(subFont);

    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {width, height};
    POINT ptDst = {g_overlayPos.x, g_overlayPos.y};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hOverlay, screenDC, &ptDst, &sizeWnd, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            g_dragging = true;
            GetCursorPos(&g_dragStartCursor);
            g_dragStartWindowPos = g_overlayPos;
            return 0;

        case WM_MOUSEMOVE:
            if (g_dragging) {
                POINT cur; GetCursorPos(&cur);
                g_overlayPos.x = g_dragStartWindowPos.x + (cur.x - g_dragStartCursor.x);
                g_overlayPos.y = g_dragStartWindowPos.y + (cur.y - g_dragStartCursor.y);
                RenderOverlay(); // UpdateLayeredWindow inside also moves the window
            }
            return 0;

        case WM_LBUTTONUP:
            if (g_dragging) { g_dragging = false; ReleaseCapture(); }
            return 0;

        case WM_RBUTTONUP: {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_MENU_STOP, T(STR_MENU_STOP));
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_MENU_STOP) {
                StopFeature(g_hMainWnd, T(STR_STATUS_STOPPED_USER));
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CreateOverlayIfNeeded() {
    if (g_hOverlay) return;
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = g_hInstance;
        wc.lpszClassName = L"PowerPilotOverlay";
        wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
        RegisterClassW(&wc);
        classRegistered = true;
    }
    g_hOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"PowerPilotOverlay", L"", WS_POPUP,
        g_overlayPos.x, g_overlayPos.y, 10, 10,
        nullptr, nullptr, g_hInstance, nullptr);
}

static void StopFeature(HWND hwnd, const wchar_t* reasonStatus) {
    if (g_active) {
        KillTimer(hwnd, ID_TIMER_TICK);
        SetThreadExecutionState(ES_CONTINUOUS);
        g_active = false;
    }
    if (g_hOverlay) ShowWindow(g_hOverlay, SW_HIDE);
    SetWindowTextW(hwnd, L"PowerPilot");
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    EnableWindow(g_hEdit, TRUE);
    EnableWindow(g_hEditFontSize, TRUE);
    EnableWindow(g_hBtnStart, TRUE);
    EnableWindow(g_hBtnStop, FALSE);
    UpdateStatus(reasonStatus);
}

// Normal shutdown.exe /s lets Fast Startup silently hibernate the kernel
// session instead of powering off. EWX_POWEROFF is the API-level bypass
// (what Shift+Shutdown does) -- a real cold power-off, not a resume.
static bool EnableShutdownPrivilege() {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    TOKEN_PRIVILEGES tkp;
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
    CloseHandle(hToken);
    return ok && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

static void DoShutdown(HWND hwnd) {
    StopFeature(hwnd, T(STR_STATUS_SHUTDOWN_EXECUTED));
    if (!EnableShutdownPrivilege()) {
        system("shutdown /s /t 0");
        return;
    }
    if (!ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG,
                        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED)) {
        system("shutdown /s /t 0");
    }
}

static void StartFeature(HWND hwnd) {
    wchar_t buf[16];
    GetWindowTextW(g_hEdit, buf, 16);
    int minutes = _wtoi(buf);
    if (minutes <= 0) {
        MessageBoxW(hwnd, T(STR_WARN_INVALID_MINUTES), L"PowerPilot", MB_ICONWARNING);
        return;
    }

    wchar_t fsBuf[8];
    GetWindowTextW(g_hEditFontSize, fsBuf, 8);
    int fs = _wtoi(fsBuf);
    if (fs < 10) fs = 10;
    if (fs > 96) fs = 96;
    g_overlayFontSize = fs;

    g_totalSeconds = minutes * 60;
    g_remainingSeconds = g_totalSeconds;
    g_active = true;
    g_subTextTicks = 0;

    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    SetTimer(hwnd, ID_TIMER_TICK, 1000, nullptr);

    EnableWindow(g_hEdit, FALSE);
    EnableWindow(g_hEditFontSize, FALSE);
    EnableWindow(g_hBtnStart, FALSE);
    EnableWindow(g_hBtnStop, TRUE);

    wchar_t status[128];
    FormatCountdown(status, 128, g_remainingSeconds);
    UpdateStatus(status);

    CreateOverlayIfNeeded();
    RenderOverlay();
    ShowWindow(g_hOverlay, SW_SHOWNA); // show without stealing focus
    ShowWindow(hwnd, SW_MINIMIZE);
}

static void OnTick(HWND hwnd) {
    if (!g_active) return;

    int previousRemaining = g_remainingSeconds;
    DWORD idleMs = GetIdleMillis();
    if (idleMs < 1000) {
        g_remainingSeconds = g_totalSeconds;
    } else {
        g_remainingSeconds--;
    }

    // Transition-into-reset edge: flash the sub-line for ~2 seconds,
    // not every tick the user happens to be active.
    if (g_remainingSeconds == g_totalSeconds && previousRemaining != g_totalSeconds) {
        g_subTextTicks = 2;
    } else if (g_subTextTicks > 0) {
        g_subTextTicks--;
    }

    if (g_remainingSeconds <= 0) {
        DoShutdown(hwnd);
        return;
    }

    wchar_t status[128];
    FormatCountdown(status, 128, g_remainingSeconds);
    UpdateStatus(status);

    wchar_t title[64];
    int m = g_remainingSeconds / 60, s = g_remainingSeconds % 60;
    swprintf(title, 64, L"PowerPilot — %02d:%02d", m, s);
    SetWindowTextW(hwnd, title);

    // Live font-size read so a change in the (disabled-but-still-readable)
    // field would apply immediately if re-enabled in a future revision.
    wchar_t fsBuf[8];
    GetWindowTextW(g_hEditFontSize, fsBuf, 8);
    int fs = _wtoi(fsBuf);
    if (fs >= 10 && fs <= 96) g_overlayFontSize = fs;

    RenderOverlay();

    // Some apps briefly grab topmost status for their own overlays/dialogs.
    // Re-asserting every tick lets ours recover automatically instead of
    // staying stuck behind whatever last won that race. SWP_NOACTIVATE
    // keeps this from stealing focus from whatever the user is doing.
    if (g_hOverlay) {
        SetWindowPos(g_hOverlay, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Re-labels every UI element to the current g_language. Called once at
// startup and again whenever the EN/ID radio buttons are toggled -- the
// edit fields' actual values are left untouched, only captions change.
static void RefreshUIText(HWND hwnd) {
    SetWindowTextW(g_hLabelDuration, T(STR_LABEL_DURATION));
    SetWindowTextW(g_hLabelFontSize, T(STR_LABEL_FONTSIZE));
    SetWindowTextW(g_hBtnStart, T(STR_BTN_START));
    SetWindowTextW(g_hBtnStop, T(STR_BTN_STOP));

    if (g_active) {
        wchar_t status[128];
        FormatCountdown(status, 128, g_remainingSeconds);
        UpdateStatus(status);
    } else {
        UpdateStatus(T(STR_STATUS_INACTIVE));
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            g_hLabelDuration = CreateWindowW(L"STATIC", L"",
                          WS_CHILD | WS_VISIBLE, 20, 20, 260, 20,
                          hwnd, nullptr, nullptr, nullptr);
            g_hEdit = CreateWindowW(L"EDIT", L"30",
                          WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                          20, 45, 260, 26, hwnd, (HMENU)ID_EDIT_MINUTES, nullptr, nullptr);

            g_hLabelFontSize = CreateWindowW(L"STATIC", L"",
                          WS_CHILD | WS_VISIBLE, 20, 80, 260, 20,
                          hwnd, nullptr, nullptr, nullptr);
            g_hEditFontSize = CreateWindowW(L"EDIT", L"28",
                          WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                          20, 105, 260, 26, hwnd, (HMENU)ID_EDIT_FONTSIZE, nullptr, nullptr);

            // Language toggle -- always enabled, even while active, since
            // it only relabels UI text and doesn't touch running state.
            g_hRadioEN = CreateWindowW(L"BUTTON", L"English",
                          WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                          20, 140, 100, 20, hwnd, (HMENU)ID_RADIO_EN, nullptr, nullptr);
            g_hRadioID = CreateWindowW(L"BUTTON", L"Indonesia",
                          WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                          130, 140, 110, 20, hwnd, (HMENU)ID_RADIO_ID, nullptr, nullptr);
            SendMessageW(g_hRadioEN, BM_SETCHECK, BST_CHECKED, 0);

            g_hBtnStart = CreateWindowW(L"BUTTON", L"",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          20, 175, 125, 32, hwnd, (HMENU)ID_BUTTON_START, nullptr, nullptr);
            g_hBtnStop = CreateWindowW(L"BUTTON", L"",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                          155, 175, 125, 32, hwnd, (HMENU)ID_BUTTON_STOP, nullptr, nullptr);

            g_hStatus = CreateWindowW(L"STATIC", L"",
                          WS_CHILD | WS_VISIBLE, 20, 220, 260, 55,
                          hwnd, (HMENU)ID_STATIC_STATUS, nullptr, nullptr);

            HWND children[] = { g_hLabelDuration, g_hEdit, g_hLabelFontSize, g_hEditFontSize,
                                 g_hRadioEN, g_hRadioID, g_hBtnStart, g_hBtnStop, g_hStatus };
            for (HWND c : children) SendMessageW(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            RefreshUIText(hwnd);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BUTTON_START: StartFeature(hwnd); break;
                case ID_BUTTON_STOP:  StopFeature(hwnd, T(STR_STATUS_STOPPED_USER)); break;
                case ID_RADIO_EN:     g_language = LANG_EN; RefreshUIText(hwnd); break;
                case ID_RADIO_ID:     g_language = LANG_ID; RefreshUIText(hwnd); break;
            }
            return 0;

        case WM_TIMER:
            if (wParam == ID_TIMER_TICK) OnTick(hwnd);
            return 0;

        case WM_CLOSE:
            StopFeature(hwnd, T(STR_STATUS_CLOSED));
            if (g_hOverlay) DestroyWindow(g_hOverlay);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_hFont) DeleteObject(g_hFont);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Without this, Windows treats the app as DPI-unaware and simply bitmap-
// stretches the whole window (including our carefully alpha-blended
// overlay text) to match display scaling -- which reintroduces blur on
// anything above 100% scaling. Tried via runtime GetProcAddress rather
// than a manifest so no extra resource-compile step is needed; falls
// back gracefully on older Windows that lack the newer API.
static void EnableDpiAwareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL(WINAPI* SetCtx_t)(HANDLE);
        auto setCtx = (SetCtx_t)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (setCtx((HANDLE)(-4))) return;
        }
    }
    HMODULE hShcore = LoadLibraryW(L"Shcore.dll");
    if (hShcore) {
        typedef HRESULT(WINAPI* SetAwareness_t)(int);
        auto setAwareness = (SetAwareness_t)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (setAwareness) {
            setAwareness(2); // PROCESS_PER_MONITOR_DPI_AWARE
            FreeLibrary(hShcore);
            return;
        }
        FreeLibrary(hShcore);
    }
    if (hUser32) {
        typedef BOOL(WINAPI* Legacy_t)();
        auto legacy = (Legacy_t)GetProcAddress(hUser32, "SetProcessDPIAware");
        if (legacy) legacy();
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    EnableDpiAwareness();
    g_hInstance = hInstance;
    const wchar_t CLASS_NAME[] = L"PowerPilotWindowClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"PowerPilot",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 330,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;
    g_hMainWnd = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
