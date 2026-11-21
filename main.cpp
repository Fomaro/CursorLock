#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <vector>
#include <algorithm>

// ============================================================================
// ИДЕНТИФИКАТОРЫ
// ============================================================================
constexpr UINT WM_TRAYICON = WM_APP + 100;
constexpr UINT_PTR TIMER_ID = 1001;
constexpr UINT TIMER_INTERVAL_MS = 10;
constexpr int HOTKEY_ID = 1;

constexpr UINT ID_TRAY_TOGGLE   = 2001;
constexpr UINT ID_TRAY_SETTINGS = 2002;
constexpr UINT ID_TRAY_EXIT     = 2003;
constexpr UINT ID_TRAY_MON_BASE = 2100;

constexpr int IDC_KEY_BOX       = 3001;
constexpr int IDC_COMBO_MONITOR = 3002;
constexpr int IDC_BTN_SAVE      = 3003;
constexpr int IDC_BTN_RESET     = 3004;
constexpr int IDC_BTN_CANCEL    = 3005;

// ============================================================================
// СТРУКТУРЫ
// ============================================================================
struct MonitorEntry {
    HMONITOR hMon;
    RECT rc;
    RECT rcWork;
    bool isPrimary;
    wchar_t name[128];
    wchar_t resolution[32];
};

// ============================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================================================
static HWND g_hMainWnd = nullptr;
static HWND g_hSettingsWnd = nullptr;
static HWND g_hKeyBox = nullptr;
static HWND g_hComboMonitor = nullptr;

static UINT g_hotkeyModifiers = MOD_CONTROL | MOD_ALT;
static UINT g_hotkeyVk = 'L';
static wchar_t g_hotkeyStr[64] = L"Ctrl + Alt + L";

static int g_targetMonitor = 0;
static int g_tempTargetMonitor = 0;

static UINT g_tempMods = MOD_CONTROL | MOD_ALT;
static UINT g_tempVk = 'L';
static wchar_t g_tempHotkeyStr[64] = L"Ctrl + Alt + L";
static bool g_isRecording = false;

static HHOOK g_hKeyHook = nullptr;
static HHOOK g_hMouseHook = nullptr;
static HWINEVENTHOOK g_hWinEventHook = nullptr;

static bool g_isLocked = false;
static RECT g_lockedRect = { 0, 0, 0, 0 };
static RECT g_safeRect = { 0, 0, 0, 0 };
static NOTIFYICONDATAW g_nid = {};

static HFONT g_hFontUi = nullptr;
static HFONT g_hFontKey = nullptr;

// Прототипы функций
void StopRecording(bool commit);
void FormatHotkeyParts(UINT mods, UINT vk, wchar_t parts[6][32], int& count);
void FormatHotkeyFullString(UINT mods, UINT vk, wchar_t* outStr, size_t maxLen);
void GetKeyNameFriendly(UINT vk, wchar_t* outName, size_t maxLen);
std::vector<MonitorEntry> GetMonitorsList();
void ApplyTargetMonitor(int monIndex);

// ============================================================================
// ОПРЕДЕЛЕНИЕ И ПЕРЕЧИСЛЕНИЕ МОНИТОРОВ
// ============================================================================
BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC /*hdc*/, LPRECT /*lprc*/, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lParam);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(MONITORINFOEXW);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorEntry entry = {};
        entry.hMon = hMon;
        entry.rc = mi.rcMonitor;
        entry.rcWork = mi.rcWork;
        entry.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        int width = mi.rcMonitor.right - mi.rcMonitor.left;
        int height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        wsprintfW(entry.resolution, L"%dx%d", width, height);

        int idx = static_cast<int>(list->size()) + 1;
        if (entry.isPrimary) {
            wsprintfW(entry.name, L"Экран %d (Основной, %s)", idx, entry.resolution);
        } else {
            wsprintfW(entry.name, L"Экран %d (%s)", idx, entry.resolution);
        }

        list->push_back(entry);
    }
    return TRUE;
}

std::vector<MonitorEntry> GetMonitorsList() {
    std::vector<MonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&list));
    return list;
}

// ============================================================================
// ХУКИ ЗАЩИТЫ КУРСОРА (WH_MOUSE_LL + SetWinEventHook)
// ============================================================================
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_isLocked) {
        MSLLHOOKSTRUCT* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        bool isOutside = (pMouse->pt.x < g_safeRect.left ||
                          pMouse->pt.x > g_safeRect.right ||
                          pMouse->pt.y < g_safeRect.top  ||
                          pMouse->pt.y > g_safeRect.bottom);

        if (isOutside) {
            int clampedX = (std::max)(g_safeRect.left, (std::min)(pMouse->pt.x, g_safeRect.right));
            int clampedY = (std::max)(g_safeRect.top,  (std::min)(pMouse->pt.y, g_safeRect.bottom));

            SetCursorPos(clampedX, clampedY);
            ClipCursor(&g_lockedRect);

            // Блокируем передачу события в ОС — предотвращаем клик и движение на соседнем экране
            return 1;
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

void CALLBACK WinEventProc(HWINEVENTHOOK /*hWinEventHook*/, DWORD event, HWND /*hWnd*/,
                           LONG /*idObject*/, LONG /*idChild*/, DWORD /*dwEventThread*/, DWORD /*dwmsEventTime*/) {
    if (g_isLocked && event == EVENT_SYSTEM_FOREGROUND) {
        ClipCursor(&g_lockedRect);
    }
}

// ============================================================================
// ПОДДЕРЖКА DPI
// ============================================================================
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);

void InitializeDpiAwareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        auto pSetDpi = reinterpret_cast<SetProcessDpiAwarenessContextProc>(
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext")
        );
        if (pSetDpi && pSetDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

// ============================================================================
// СОХРАНЕНИЕ И ЗАГРУЗКА В РЕЕСТР WINDOWS
// ============================================================================
constexpr const wchar_t* REG_KEY_PATH = L"Software\\CursorLock";

void GetKeyNameFriendly(UINT vk, wchar_t* outName, size_t maxLen) {
    outName[0] = L'\0';
    if (vk >= 'A' && vk <= 'Z') {
        wsprintfW(outName, L"%c", static_cast<wchar_t>(vk));
        return;
    }
    if (vk >= '0' && vk <= '9') {
        wsprintfW(outName, L"%c", static_cast<wchar_t>(vk));
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        wsprintfW(outName, L"F%u", vk - VK_F1 + 1);
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        wsprintfW(outName, L"Num %u", vk - VK_NUMPAD0);
        return;
    }

    switch (vk) {
    case VK_SPACE:    wcscpy_s(outName, maxLen, L"Space"); break;
    case VK_TAB:      wcscpy_s(outName, maxLen, L"Tab"); break;
    case VK_RETURN:   wcscpy_s(outName, maxLen, L"Enter"); break;
    case VK_ESCAPE:   wcscpy_s(outName, maxLen, L"Esc"); break;
    case VK_BACK:     wcscpy_s(outName, maxLen, L"Backspace"); break;
    case VK_DELETE:   wcscpy_s(outName, maxLen, L"Delete"); break;
    case VK_INSERT:   wcscpy_s(outName, maxLen, L"Insert"); break;
    case VK_HOME:     wcscpy_s(outName, maxLen, L"Home"); break;
    case VK_END:      wcscpy_s(outName, maxLen, L"End"); break;
    case VK_PRIOR:    wcscpy_s(outName, maxLen, L"Page Up"); break;
    case VK_NEXT:     wcscpy_s(outName, maxLen, L"Page Down"); break;
    case VK_UP:       wcscpy_s(outName, maxLen, L"Up"); break;
    case VK_DOWN:     wcscpy_s(outName, maxLen, L"Down"); break;
    case VK_LEFT:     wcscpy_s(outName, maxLen, L"Left"); break;
    case VK_RIGHT:    wcscpy_s(outName, maxLen, L"Right"); break;
    case VK_OEM_3:    wcscpy_s(outName, maxLen, L"~"); break;
    case VK_OEM_PLUS: wcscpy_s(outName, maxLen, L"+"); break;
    case VK_OEM_MINUS:wcscpy_s(outName, maxLen, L"-"); break;
    case VK_OEM_1:    wcscpy_s(outName, maxLen, L";"); break;
    case VK_OEM_2:    wcscpy_s(outName, maxLen, L"/"); break;
    case VK_OEM_4:    wcscpy_s(outName, maxLen, L"["); break;
    case VK_OEM_5:    wcscpy_s(outName, maxLen, L"\\"); break;
    case VK_OEM_6:    wcscpy_s(outName, maxLen, L"]"); break;
    case VK_OEM_7:    wcscpy_s(outName, maxLen, L"'"); break;
    case VK_CAPITAL:  wcscpy_s(outName, maxLen, L"Caps Lock"); break;
    case VK_SNAPSHOT: wcscpy_s(outName, maxLen, L"PrintScreen"); break;
    case VK_PAUSE:    wcscpy_s(outName, maxLen, L"Pause"); break;
    default: {
        LONG sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (sc != 0 && GetKeyNameTextW(sc << 16, outName, static_cast<int>(maxLen)) > 0) {
            return;
        }
        wsprintfW(outName, L"Key(0x%X)", vk);
        break;
    }
    }
}

void FormatHotkeyParts(UINT mods, UINT vk, wchar_t parts[6][32], int& count) {
    count = 0;
    if (mods & MOD_CONTROL) { wcscpy_s(parts[count++], 32, L"Ctrl"); }
    if (mods & MOD_ALT)     { wcscpy_s(parts[count++], 32, L"Alt"); }
    if (mods & MOD_SHIFT)   { wcscpy_s(parts[count++], 32, L"Shift"); }
    if (mods & MOD_WIN)     { wcscpy_s(parts[count++], 32, L"Win"); }

    if (vk != 0) {
        GetKeyNameFriendly(vk, parts[count++], 32);
    }
}

void FormatHotkeyFullString(UINT mods, UINT vk, wchar_t* outStr, size_t maxLen) {
    wchar_t parts[6][32];
    int count = 0;
    FormatHotkeyParts(mods, vk, parts, count);

    outStr[0] = L'\0';
    for (int i = 0; i < count; ++i) {
        wcsncat_s(outStr, maxLen, parts[i], _TRUNCATE);
        if (i < count - 1) {
            wcsncat_s(outStr, maxLen, L" + ", _TRUNCATE);
        }
    }
}

void LoadConfig() {
    constexpr UINT defMods = MOD_CONTROL | MOD_ALT;
    constexpr UINT defVk = 'L';

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD mods = defMods;
        DWORD vk = defVk;
        DWORD targetMon = 0;
        DWORD size = sizeof(DWORD);
        DWORD type = REG_DWORD;

        if (RegQueryValueExW(hKey, L"Modifiers", nullptr, &type, reinterpret_cast<LPBYTE>(&mods), &size) == ERROR_SUCCESS) {
            g_hotkeyModifiers = mods;
        } else {
            g_hotkeyModifiers = defMods;
        }

        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"VkCode", nullptr, &type, reinterpret_cast<LPBYTE>(&vk), &size) == ERROR_SUCCESS) {
            g_hotkeyVk = vk;
        } else {
            g_hotkeyVk = defVk;
        }

        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"TargetMonitor", nullptr, &type, reinterpret_cast<LPBYTE>(&targetMon), &size) == ERROR_SUCCESS) {
            g_targetMonitor = static_cast<int>(targetMon);
        } else {
            g_targetMonitor = 0;
        }

        RegCloseKey(hKey);
    } else {
        g_hotkeyModifiers = defMods;
        g_hotkeyVk = defVk;
        g_targetMonitor = 0;
    }

    FormatHotkeyFullString(g_hotkeyModifiers, g_hotkeyVk, g_hotkeyStr, sizeof(g_hotkeyStr) / sizeof(wchar_t));
    g_tempMods = g_hotkeyModifiers;
    g_tempVk = g_hotkeyVk;
    FormatHotkeyFullString(g_tempMods, g_tempVk, g_tempHotkeyStr, sizeof(g_tempHotkeyStr) / sizeof(wchar_t));
    g_tempTargetMonitor = g_targetMonitor;
}

void SaveConfig() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, nullptr,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD mods = g_hotkeyModifiers;
        DWORD vk = g_hotkeyVk;
        DWORD targetMon = static_cast<DWORD>(g_targetMonitor);

        RegSetValueExW(hKey, L"Modifiers", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&mods), sizeof(DWORD));
        RegSetValueExW(hKey, L"VkCode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vk), sizeof(DWORD));
        RegSetValueExW(hKey, L"TargetMonitor", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&targetMon), sizeof(DWORD));
        RegSetValueExW(hKey, L"HotkeyStr", 0, REG_SZ, reinterpret_cast<const BYTE*>(g_hotkeyStr),
                      static_cast<DWORD>((wcslen(g_hotkeyStr) + 1) * sizeof(wchar_t)));

        RegCloseKey(hKey);
    }
}

// ============================================================================
// УПРАВЛЕНИЕ БЛОКИРОВКОЙ И ТРЕЕМ
// ============================================================================
void UpdateTrayStatus(HWND hWnd) {
    g_nid.uFlags = NIF_TIP | NIF_INFO;

    if (g_isLocked) {
        wsprintfW(g_nid.szTip, L"CursorLock: Заблокировано (%s)", g_hotkeyStr);
        wsprintfW(g_nid.szInfoTitle, L"CursorLock");
        wsprintfW(g_nid.szInfo, L"Курсор надёжно зафиксирован на выбранном мониторе");
        g_nid.dwInfoFlags = NIIF_INFO;
    } else {
        wsprintfW(g_nid.szTip, L"CursorLock: Свободно (%s)", g_hotkeyStr);
        wsprintfW(g_nid.szInfoTitle, L"CursorLock");
        wsprintfW(g_nid.szInfo, L"Курсор разблокирован");
        g_nid.dwInfoFlags = NIIF_INFO;
    }

    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void ToggleCursorLock(HWND hWnd) {
    if (!g_isLocked) {
        auto monitors = GetMonitorsList();
        RECT targetRect = {};
        bool found = false;

        if (g_targetMonitor > 0 && g_targetMonitor <= static_cast<int>(monitors.size())) {
            targetRect = monitors[g_targetMonitor - 1].rc;
            found = true;

            POINT pt = {};
            GetCursorPos(&pt);
            if (pt.x < targetRect.left || pt.x >= targetRect.right ||
                pt.y < targetRect.top  || pt.y >= targetRect.bottom) {
                int cx = targetRect.left + (targetRect.right - targetRect.left) / 2;
                int cy = targetRect.top  + (targetRect.bottom - targetRect.top) / 2;
                SetCursorPos(cx, cy);
            }
        } else {
            POINT pt = {};
            if (GetCursorPos(&pt)) {
                HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {};
                mi.cbSize = sizeof(MONITORINFO);
                if (GetMonitorInfoW(hMonitor, &mi)) {
                    targetRect = mi.rcMonitor;
                    found = true;
                }
            }
        }

        if (found) {
            g_safeRect.left = targetRect.left;
            g_safeRect.top = targetRect.top;
            g_safeRect.right = targetRect.right - 1;
            g_safeRect.bottom = targetRect.bottom - 1;

            g_lockedRect = g_safeRect;
            g_isLocked = true;

            ClipCursor(&g_lockedRect);

            HINSTANCE hInst = GetModuleHandleW(nullptr);
            if (!g_hMouseHook) {
                g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInst, 0);
            }

            if (!g_hWinEventHook) {
                g_hWinEventHook = SetWinEventHook(
                    EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                    nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
                );
            }

            SetTimer(hWnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
            UpdateTrayStatus(hWnd);
        }
    } else {
        g_isLocked = false;
        KillTimer(hWnd, TIMER_ID);

        if (g_hMouseHook) {
            UnhookWindowsHookEx(g_hMouseHook);
            g_hMouseHook = nullptr;
        }
        if (g_hWinEventHook) {
            UnhookWinEvent(g_hWinEventHook);
            g_hWinEventHook = nullptr;
        }

        ClipCursor(nullptr);
        UpdateTrayStatus(hWnd);
    }
}

void ApplyTargetMonitor(int monIndex) {
    g_targetMonitor = monIndex;
    SaveConfig();

    if (g_isLocked) {
        auto monitors = GetMonitorsList();
        if (g_targetMonitor > 0 && g_targetMonitor <= static_cast<int>(monitors.size())) {
            RECT targetRect = monitors[g_targetMonitor - 1].rc;

            g_safeRect.left = targetRect.left;
            g_safeRect.top = targetRect.top;
            g_safeRect.right = targetRect.right - 1;
            g_safeRect.bottom = targetRect.bottom - 1;
            g_lockedRect = g_safeRect;

            POINT pt = {};
            GetCursorPos(&pt);
            if (pt.x < g_safeRect.left || pt.x > g_safeRect.right ||
                pt.y < g_safeRect.top  || pt.y > g_safeRect.bottom) {
                int cx = g_safeRect.left + (g_safeRect.right - g_safeRect.left) / 2;
                int cy = g_safeRect.top  + (g_safeRect.bottom - g_safeRect.top) / 2;
                SetCursorPos(cx, cy);
            }
            ClipCursor(&g_lockedRect);
        }
    }
}

// ============================================================================
// ХУК КЛАВИАТУРЫ ДЛЯ ЗАХВАТА НАЖАТИЙ В ОКНЕ НАСТРОЕК
// ============================================================================
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_isRecording) {
        KBDLLHOOKSTRUCT* pKbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        DWORD vk = pKbd->vkCode;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (vk == VK_ESCAPE && (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0 &&
                                   (GetAsyncKeyState(VK_MENU) & 0x8000) == 0 &&
                                   (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0 &&
                                   (GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
                                   (GetAsyncKeyState(VK_RWIN) & 0x8000) == 0) {
                StopRecording(false);
                return 1;
            }

            bool isCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
            bool isAlt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
            bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
            bool isWin   = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0 || vk == VK_LWIN || vk == VK_RWIN;

            UINT mods = 0;
            if (isCtrl)  mods |= MOD_CONTROL;
            if (isAlt)   mods |= MOD_ALT;
            if (isShift) mods |= MOD_SHIFT;
            if (isWin)   mods |= MOD_WIN;

            bool isModifierKey = (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                                  vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
                                  vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
                                  vk == VK_LWIN    || vk == VK_RWIN);

            g_tempMods = mods;

            if (!isModifierKey) {
                g_tempVk = vk;
                StopRecording(true);
            } else {
                g_tempVk = 0;
                wchar_t parts[6][32];
                int count = 0;
                FormatHotkeyParts(g_tempMods, 0, parts, count);
                wchar_t heldText[128] = L"";
                for (int i = 0; i < count; ++i) {
                    lstrcatW(heldText, parts[i]);
                    lstrcatW(heldText, L" + ");
                }
                lstrcatW(heldText, L"...");
                if (g_hKeyBox) {
                    SetWindowTextW(g_hKeyBox, heldText);
                }
            }
            return 1;
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            bool isCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool isAlt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool isWin   = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

            UINT mods = 0;
            if (isCtrl)  mods |= MOD_CONTROL;
            if (isAlt)   mods |= MOD_ALT;
            if (isShift) mods |= MOD_SHIFT;
            if (isWin)   mods |= MOD_WIN;

            g_tempMods = mods;
            wchar_t parts[6][32];
            int count = 0;
            FormatHotkeyParts(g_tempMods, 0, parts, count);
            if (count > 0) {
                wchar_t heldText[128] = L"";
                for (int i = 0; i < count; ++i) {
                    lstrcatW(heldText, parts[i]);
                    lstrcatW(heldText, L" + ");
                }
                lstrcatW(heldText, L"...");
                if (g_hKeyBox) {
                    SetWindowTextW(g_hKeyBox, heldText);
                }
            } else {
                if (g_hKeyBox) {
                    SetWindowTextW(g_hKeyBox, L"Нажмите комбинацию клавиш... (Esc — отмена)");
                }
            }
            return 1;
        }
    }
    return CallNextHookEx(g_hKeyHook, nCode, wParam, lParam);
}

void StartRecording() {
    if (!g_isRecording) {
        g_isRecording = true;
        g_tempMods = 0;
        g_tempVk = 0;
        if (g_hKeyBox) {
            SetWindowTextW(g_hKeyBox, L"Нажмите комбинацию клавиш... (Esc — отмена)");
            InvalidateRect(g_hKeyBox, nullptr, TRUE);
        }
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        g_hKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    }
}

void StopRecording(bool commit) {
    if (g_hKeyHook) {
        UnhookWindowsHookEx(g_hKeyHook);
        g_hKeyHook = nullptr;
    }
    g_isRecording = false;

    if (!commit) {
        g_tempMods = g_hotkeyModifiers;
        g_tempVk = g_hotkeyVk;
    }

    FormatHotkeyFullString(g_tempMods, g_tempVk, g_tempHotkeyStr, sizeof(g_tempHotkeyStr) / sizeof(wchar_t));
    if (g_hKeyBox) {
        SetWindowTextW(g_hKeyBox, g_tempHotkeyStr);
        InvalidateRect(g_hKeyBox, nullptr, TRUE);
    }
}

// ============================================================================
// ОКНО НАСТРОЕК В КЛАССИЧЕСКОЙ ТЕМАТИКЕ WINDOWS
// ============================================================================
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_hFontUi = CreateFontIndirectW(&ncm.lfMessageFont);

        LOGFONTW lfKey = ncm.lfMessageFont;
        lfKey.lfWeight = FW_BOLD;
        lfKey.lfHeight = static_cast<LONG>(lfKey.lfHeight * 1.15);
        g_hFontKey = CreateFontIndirectW(&lfKey);

        // 1. Группа "Горячая клавиша"
        HWND hGrpKey = CreateWindowExW(
            0, L"BUTTON", L" Горячая клавиша ",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            16, 12, 388, 95,
            hWnd, nullptr, GetModuleHandleW(nullptr), nullptr
        );

        HWND hLblKey = CreateWindowExW(
            0, L"STATIC", L"Нажмите на поле ниже для назначения комбинации клавиш:",
            WS_CHILD | WS_VISIBLE,
            30, 34, 360, 18,
            hWnd, nullptr, GetModuleHandleW(nullptr), nullptr
        );

        g_hKeyBox = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"STATIC", g_tempHotkeyStr,
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY | WS_TABSTOP,
            30, 56, 360, 34,
            hWnd, reinterpret_cast<HMENU>(IDC_KEY_BOX), GetModuleHandleW(nullptr), nullptr
        );

        // 2. Группа "Целевой экран"
        HWND hGrpMon = CreateWindowExW(
            0, L"BUTTON", L" Монитор для фиксации курсора ",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            16, 118, 388, 88,
            hWnd, nullptr, GetModuleHandleW(nullptr), nullptr
        );

        HWND hLblMon = CreateWindowExW(
            0, L"STATIC", L"Выберите экран для блокировки:",
            WS_CHILD | WS_VISIBLE,
            30, 140, 360, 18,
            hWnd, nullptr, GetModuleHandleW(nullptr), nullptr
        );

        g_hComboMonitor = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            30, 162, 360, 220,
            hWnd, reinterpret_cast<HMENU>(IDC_COMBO_MONITOR), GetModuleHandleW(nullptr), nullptr
        );

        // Подсказка снизу
        HWND hLblHint = CreateWindowExW(
            0, L"STATIC", L"Совет: режим «Авто» блокирует курсор на экране под курсором.",
            WS_CHILD | WS_VISIBLE,
            18, 216, 384, 18,
            hWnd, nullptr, GetModuleHandleW(nullptr), nullptr
        );

        // Кнопки управления
        HWND hBtnSave = CreateWindowExW(
            0, L"BUTTON", L"Сохранить",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            106, 244, 92, 28,
            hWnd, reinterpret_cast<HMENU>(IDC_BTN_SAVE), GetModuleHandleW(nullptr), nullptr
        );

        HWND hBtnReset = CreateWindowExW(
            0, L"BUTTON", L"По умолч.",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            206, 244, 92, 28,
            hWnd, reinterpret_cast<HMENU>(IDC_BTN_RESET), GetModuleHandleW(nullptr), nullptr
        );

        HWND hBtnCancel = CreateWindowExW(
            0, L"BUTTON", L"Отмена",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            306, 244, 92, 28,
            hWnd, reinterpret_cast<HMENU>(IDC_BTN_CANCEL), GetModuleHandleW(nullptr), nullptr
        );

        // Применяем стандартный шрифт Segoe UI
        const HWND controls[] = {
            hGrpKey, hLblKey, hGrpMon, hLblMon, g_hComboMonitor, hLblHint,
            hBtnSave, hBtnReset, hBtnCancel
        };
        for (HWND hCtrl : controls) {
            SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontUi), TRUE);
        }
        if (g_hKeyBox) {
            SendMessageW(g_hKeyBox, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontKey), TRUE);
        }

        // Заполняем выпадающий список мониторов
        if (g_hComboMonitor) {
            int idxAuto = static_cast<int>(SendMessageW(g_hComboMonitor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Авто: определять экран под курсором")));
            SendMessageW(g_hComboMonitor, CB_SETITEMDATA, idxAuto, 0);

            auto monitors = GetMonitorsList();
            int curSel = 0;
            for (size_t i = 0; i < monitors.size(); ++i) {
                int idx = static_cast<int>(SendMessageW(g_hComboMonitor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(monitors[i].name)));
                SendMessageW(g_hComboMonitor, CB_SETITEMDATA, idx, static_cast<LPARAM>(i + 1));
                if (g_tempTargetMonitor == static_cast<int>(i + 1)) {
                    curSel = idx;
                }
            }
            SendMessageW(g_hComboMonitor, CB_SETCURSEL, curSel, 0);
        }

        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);

        if (hCtrl == g_hKeyBox) {
            SetBkMode(hdc, TRANSPARENT);
            if (g_isRecording) {
                SetTextColor(hdc, RGB(0, 102, 204));
            } else {
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            }
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
        }

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_SETCURSOR: {
        HWND hCtrl = reinterpret_cast<HWND>(wParam);
        if (hCtrl == g_hKeyBox) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        WORD notifyCode = HIWORD(wParam);
        WORD controlId = LOWORD(wParam);

        if (controlId == IDC_KEY_BOX && (notifyCode == STN_CLICKED || notifyCode == STN_DBLCLK)) {
            StartRecording();
            return 0;
        }

        switch (controlId) {
        case IDC_BTN_SAVE: {
            if (g_isRecording) {
                StopRecording(false);
            }

            if (g_tempMods == 0 && !(g_tempVk >= VK_F1 && g_tempVk <= VK_F24)) {
                MessageBoxW(hWnd,
                    L"Для предотвращения перехвата стандартного ввода текста\n"
                    L"выберите комбинацию с хотя бы одним модификатором (Ctrl, Alt, Shift, Win)\n"
                    L"либо используйте функциональную клавишу F1–F24.",
                    L"CursorLock", MB_OK | MB_ICONWARNING);
                return 0;
            }

            int selectedMon = 0;
            if (g_hComboMonitor) {
                int curSel = static_cast<int>(SendMessageW(g_hComboMonitor, CB_GETCURSEL, 0, 0));
                if (curSel != CB_ERR) {
                    selectedMon = static_cast<int>(SendMessageW(g_hComboMonitor, CB_GETITEMDATA, curSel, 0));
                }
            }

            UnregisterHotKey(g_hMainWnd, HOTKEY_ID);
            if (!RegisterHotKey(g_hMainWnd, HOTKEY_ID, g_tempMods, g_tempVk)) {
                RegisterHotKey(g_hMainWnd, HOTKEY_ID, g_hotkeyModifiers, g_hotkeyVk);
                MessageBoxW(hWnd,
                    L"Не удалось зарегистрировать эту комбинацию клавиш.\n"
                    L"Она уже используется другой программой или самой Windows.\n\n"
                    L"Пожалуйста, выберите другое сочетание клавиш.",
                    L"Ошибка регистрации горячей клавиши", MB_OK | MB_ICONERROR);
                return 0;
            }

            g_hotkeyModifiers = g_tempMods;
            g_hotkeyVk = g_tempVk;
            FormatHotkeyFullString(g_hotkeyModifiers, g_hotkeyVk, g_hotkeyStr, sizeof(g_hotkeyStr) / sizeof(wchar_t));

            ApplyTargetMonitor(selectedMon);
            UpdateTrayStatus(g_hMainWnd);

            wchar_t msgBuf[128];
            wsprintfW(msgBuf, L"Настройки сохранены!\nГорячая клавиша: %s", g_hotkeyStr);
            MessageBoxW(hWnd, msgBuf, L"CursorLock", MB_OK | MB_ICONINFORMATION);

            DestroyWindow(hWnd);
            return 0;
        }

        case IDC_BTN_RESET: {
            if (g_isRecording) {
                StopRecording(false);
            }
            g_tempMods = MOD_CONTROL | MOD_ALT;
            g_tempVk = 'L';
            FormatHotkeyFullString(g_tempMods, g_tempVk, g_tempHotkeyStr, sizeof(g_tempHotkeyStr) / sizeof(wchar_t));
            if (g_hKeyBox) {
                SetWindowTextW(g_hKeyBox, g_tempHotkeyStr);
            }
            if (g_hComboMonitor) {
                SendMessageW(g_hComboMonitor, CB_SETCURSEL, 0, 0);
            }
            return 0;
        }

        case IDC_BTN_CANCEL:
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        StopRecording(false);
        if (g_hFontUi)  { DeleteObject(g_hFontUi);  g_hFontUi = nullptr; }
        if (g_hFontKey) { DeleteObject(g_hFontKey); g_hFontKey = nullptr; }
        g_hKeyBox = nullptr;
        g_hComboMonitor = nullptr;
        g_hSettingsWnd = nullptr;
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void OpenSettingsWindow(HINSTANCE hInstance) {
    if (g_hSettingsWnd) {
        SetForegroundWindow(g_hSettingsWnd);
        return;
    }

    g_tempMods = g_hotkeyModifiers;
    g_tempVk = g_hotkeyVk;
    FormatHotkeyFullString(g_tempMods, g_tempVk, g_tempHotkeyStr, sizeof(g_tempHotkeyStr) / sizeof(wchar_t));
    g_tempTargetMonitor = g_targetMonitor;
    g_isRecording = false;

    const wchar_t SETTINGS_CLASS[] = L"CursorLock_NativeSettingsDialog";

    WNDCLASSEXW swc = {};
    swc.cbSize = sizeof(WNDCLASSEXW);
    swc.lpfnWndProc = SettingsWndProc;
    swc.hInstance = hInstance;
    swc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    swc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    swc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
    swc.lpszClassName = SETTINGS_CLASS;

    RegisterClassExW(&swc);

    constexpr int CLIENT_W = 420;
    constexpr int CLIENT_H = 286;

    POINT pt = {};
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(hMon, &mi);

    RECT adjRc = { 0, 0, CLIENT_W, CLIENT_H };
    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&adjRc, dwStyle, FALSE);

    int winW = adjRc.right - adjRc.left;
    int winH = adjRc.bottom - adjRc.top;
    int posX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - winW) / 2;
    int posY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - winH) / 2;

    g_hSettingsWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        SETTINGS_CLASS,
        L"Настройки CursorLock",
        dwStyle | WS_VISIBLE,
        posX, posY, winW, winH,
        g_hMainWnd,
        nullptr,
        hInstance,
        nullptr
    );

    if (g_hSettingsWnd) {
        SetForegroundWindow(g_hSettingsWnd);
    }
}

// ============================================================================
// МЕНЮ ТРЕЯ И ОСНОВНОЙ ЦИКЛ
// ============================================================================
void ShowTrayContextMenu(HWND hWnd, HINSTANCE /*hInstance*/) {
    POINT pt = {};
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    wchar_t toggleItemText[128];
    if (g_isLocked) {
        wsprintfW(toggleItemText, L"Разблокировать курсор (%s)", g_hotkeyStr);
    } else {
        wsprintfW(toggleItemText, L"Заблокировать курсор (%s)", g_hotkeyStr);
    }

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE, toggleItemText);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    HMENU hSubMon = CreatePopupMenu();
    if (hSubMon) {
        UINT flagAuto = (g_targetMonitor == 0) ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(hSubMon, MF_STRING | flagAuto, ID_TRAY_MON_BASE, L"Авто (где находится курсор)");
        AppendMenuW(hSubMon, MF_SEPARATOR, 0, nullptr);

        auto monitors = GetMonitorsList();
        for (size_t i = 0; i < monitors.size(); ++i) {
            UINT flagMon = (g_targetMonitor == static_cast<int>(i + 1)) ? MF_CHECKED : MF_UNCHECKED;
            AppendMenuW(hSubMon, MF_STRING | flagMon, ID_TRAY_MON_BASE + 1 + static_cast<UINT>(i), monitors[i].name);
        }

        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSubMon), L"Целевой экран");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Настройки...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HOTKEY:
        if (wParam == HOTKEY_ID) {
            ToggleCursorLock(hWnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == TIMER_ID && g_isLocked) {
            POINT pt = {};
            if (GetCursorPos(&pt)) {
                if (pt.x < g_safeRect.left || pt.x > g_safeRect.right ||
                    pt.y < g_safeRect.top  || pt.y > g_safeRect.bottom) {
                    int cx = (std::max)(g_safeRect.left, (std::min)(pt.x, g_safeRect.right));
                    int cy = (std::max)(g_safeRect.top,  (std::min)(pt.y, g_safeRect.bottom));
                    SetCursorPos(cx, cy);
                }
            }
            ClipCursor(&g_lockedRect);
            return 0;
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hWnd, GWLP_HINSTANCE));
            ShowTrayContextMenu(hWnd, hInst);
            return 0;
        } else if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            ToggleCursorLock(hWnd);
            return 0;
        }
        break;

    case WM_COMMAND: {
        WORD cmdId = LOWORD(wParam);
        if (cmdId == ID_TRAY_TOGGLE) {
            ToggleCursorLock(hWnd);
            return 0;
        } else if (cmdId == ID_TRAY_SETTINGS) {
            HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hWnd, GWLP_HINSTANCE));
            OpenSettingsWindow(hInst);
            return 0;
        } else if (cmdId == ID_TRAY_EXIT) {
            DestroyWindow(hWnd);
            return 0;
        } else if (cmdId >= ID_TRAY_MON_BASE && cmdId < ID_TRAY_MON_BASE + 20) {
            int selected = static_cast<int>(cmdId - ID_TRAY_MON_BASE);
            ApplyTargetMonitor(selected);
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        if (g_hSettingsWnd) {
            DestroyWindow(g_hSettingsWnd);
            g_hSettingsWnd = nullptr;
        }
        if (g_isLocked) {
            if (g_hMouseHook) {
                UnhookWindowsHookEx(g_hMouseHook);
                g_hMouseHook = nullptr;
            }
            if (g_hWinEventHook) {
                UnhookWinEvent(g_hWinEventHook);
                g_hWinEventHook = nullptr;
            }
            ClipCursor(nullptr);
            g_isLocked = false;
        }
        KillTimer(hWnd, TIMER_ID);
        UnregisterHotKey(hWnd, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"CursorLock_SingleInstance_Mutex_Guid_2026");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
            L"Приложение CursorLock уже запущено и находится в системном трее.",
            L"CursorLock",
            MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Инициализация стандартных элементов Windows Common Controls
    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    InitializeDpiAwareness();
    LoadConfig();

    const wchar_t CLASS_NAME[] = L"CursorLock_HiddenMainWindow";

    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return 1;
    }

    g_hMainWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"CursorLock Window",
        0,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hMainWnd) {
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return 1;
    }

    if (!RegisterHotKey(g_hMainWnd, HOTKEY_ID, g_hotkeyModifiers, g_hotkeyVk)) {
        wchar_t errorMsg[256];
        wsprintfW(errorMsg,
            L"Не удалось зарегистрировать глобальную горячую клавишу (%s).\n"
            L"Возможно, она занята другим приложением.\n\n"
            L"Вы можете изменить комбинацию через пункт «Настройки» в трее.",
            g_hotkeyStr);
        MessageBoxW(nullptr, errorMsg, L"CursorLock Предупреждение", MB_OK | MB_ICONWARNING);
    }

    ZeroMemory(&g_nid, sizeof(NOTIFYICONDATAW));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAYICON;
    HICON hTrayIcon = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR
    ));
    g_nid.hIcon = hTrayIcon ? hTrayIcon : (hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION));

    wsprintfW(g_nid.szTip, L"CursorLock: Свободно (%s)", g_hotkeyStr);
    wsprintfW(g_nid.szInfoTitle, L"CursorLock запущен");
    wsprintfW(g_nid.szInfo, L"Используйте %s для блокировки курсора (или ПКМ по трею -> Настройки).", g_hotkeyStr);
    g_nid.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIconW(NIM_ADD, &g_nid);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_hSettingsWnd && IsDialogMessageW(g_hSettingsWnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}
