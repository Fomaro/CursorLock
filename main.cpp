#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <algorithm>

// ============================================================================
// ИДЕНТИФИКАТОРЫ
// ============================================================================
constexpr UINT WM_TRAYICON = WM_APP + 100;
constexpr UINT_PTR TIMER_ID = 1001;
constexpr UINT TIMER_INTERVAL_MS = 10; // Высокочастотный таймер-heartbeat 10 мс
constexpr int HOTKEY_ID = 1;

constexpr UINT ID_TRAY_TOGGLE   = 2001;
constexpr UINT ID_TRAY_SETTINGS = 2002;
constexpr UINT ID_TRAY_EXIT     = 2003;
constexpr UINT ID_TRAY_MON_BASE = 2100; // 2100 = Auto, 2101 = Mon 1, 2102 = Mon 2, ...

constexpr int IDC_COMBO_MONITOR = 3010;

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
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ СОСТОЯНИЯ
// ============================================================================
static HWND g_hMainWnd = nullptr;
static HWND g_hSettingsWnd = nullptr;
static HWND g_hComboMonitor = nullptr;

// Текущая активная горячая клавиша (по умолчанию Ctrl + Alt + L)
static UINT g_hotkeyModifiers = MOD_CONTROL | MOD_ALT;
static UINT g_hotkeyVk = 'L';
static wchar_t g_hotkeyStr[64] = L"Ctrl + Alt + L";

// Целевой монитор для блокировки: 0 = Авто (под курсором), 1 = Монитор 1, 2 = Монитор 2...
static int g_targetMonitor = 0;
static int g_tempTargetMonitor = 0;

// Временное состояние при интерактивной записи в окне настроек
static UINT g_tempMods = MOD_CONTROL | MOD_ALT;
static UINT g_tempVk = 'L';
static bool g_isRecording = false;
static HHOOK g_hKeyHook = nullptr;

// Хуки для абсолютной непроницаемости курсора
static HHOOK g_hMouseHook = nullptr;
static HWINEVENTHOOK g_hWinEventHook = nullptr;

// Состояние блокировки курсора
static bool g_isLocked = false;
static RECT g_lockedRect = { 0, 0, 0, 0 };
static RECT g_safeRect = { 0, 0, 0, 0 };
static NOTIFYICONDATAW g_nid = {};

// Шрифты для интерфейса
static HFONT g_hFontTitle = nullptr;
static HFONT g_hFontNormal = nullptr;
static HFONT g_hFontKey = nullptr;
static HFONT g_hFontSmall = nullptr;
static HFONT g_hFontUi = nullptr;

// Состояние наведения мыши для кнопок в окне настроек
enum HoverState {
    HOVER_NONE = 0,
    HOVER_RECORDER,
    HOVER_BTN_SAVE,
    HOVER_BTN_RESET,
    HOVER_BTN_CANCEL
};
static HoverState g_hoverState = HOVER_NONE;

// Прототипы функций
void StopRecording(bool commit);
void FormatHotkeyParts(UINT mods, UINT vk, wchar_t parts[6][32], int& count);
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
// ХУКИ ЗАЩИТЫ КУРСОРА (0 MS ЗАДЕРЖКА, БЛОКИРОВКА ВЫЛЕТОВ И СЛУЧАЙНЫХ КЛИКОВ)
// ============================================================================
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_isLocked) {
        MSLLHOOKSTRUCT* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // Проверяем, выходит ли позиция курсора за пределы безопасного диапазона целевого монитора
        bool isOutside = (pMouse->pt.x < g_safeRect.left ||
                          pMouse->pt.x > g_safeRect.right ||
                          pMouse->pt.y < g_safeRect.top  ||
                          pMouse->pt.y > g_safeRect.bottom);

        if (isOutside) {
            // Принудительно удерживаем координаты внутри границ экрана
            int clampedX = (std::max)(g_safeRect.left, (std::min)(pMouse->pt.x, g_safeRect.right));
            int clampedY = (std::max)(g_safeRect.top,  (std::min)(pMouse->pt.y, g_safeRect.bottom));

            SetCursorPos(clampedX, clampedY);
            ClipCursor(&g_lockedRect);

            // МГНОВЕННО БЛОКИРУЕМ событие:
            // Windows не отправит ни движение мыши, ни клик на соседний экран!
            return 1;
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

void CALLBACK WinEventProc(HWINEVENTHOOK /*hWinEventHook*/, DWORD event, HWND /*hWnd*/,
                           LONG /*idObject*/, LONG /*idChild*/, DWORD /*dwEventThread*/, DWORD /*dwmsEventTime*/) {
    // При переключении окон (Alt+Tab, активация окна) Windows сбрасывает ClipCursor.
    // Перехватываем смену активного окна с нулевой задержкой и мгновенно восстанавливаем границы!
    if (g_isLocked && event == EVENT_SYSTEM_FOREGROUND) {
        ClipCursor(&g_lockedRect);
    }
}

// ============================================================================
// ПОДДЕРЖКА DPI И ТЕМНОЙ ТЕМЫ WINDOWS 10 / 11
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

typedef HRESULT (WINAPI *DwmSetWindowAttributeProc)(HWND, DWORD, LPCVOID, DWORD);
void EnableDarkTitleBar(HWND hWnd) {
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        auto pDwmSet = reinterpret_cast<DwmSetWindowAttributeProc>(
            GetProcAddress(hDwm, "DwmSetWindowAttribute")
        );
        if (pDwmSet) {
            BOOL dark = TRUE;
            if (FAILED(pDwmSet(hWnd, 20, &dark, sizeof(dark)))) {
                pDwmSet(hWnd, 19, &dark, sizeof(dark));
            }
        }
        FreeLibrary(hDwm);
    }
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

        // Если задан конкретный монитор (1, 2, ...)
        if (g_targetMonitor > 0 && g_targetMonitor <= static_cast<int>(monitors.size())) {
            targetRect = monitors[g_targetMonitor - 1].rc;
            found = true;

            // Если курсор за пределами целевого монитора, переносим его в центр целевого экрана
            POINT pt = {};
            GetCursorPos(&pt);
            if (pt.x < targetRect.left || pt.x >= targetRect.right ||
                pt.y < targetRect.top  || pt.y >= targetRect.bottom) {
                int cx = targetRect.left + (targetRect.right - targetRect.left) / 2;
                int cy = targetRect.top  + (targetRect.bottom - targetRect.top) / 2;
                SetCursorPos(cx, cy);
            }
        } else {
            // Режим Авто: определяем монитор под курсором
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
            // В Windows rcMonitor.right и rcMonitor.bottom эксклюзивны (первый пиксель соседнего экрана).
            // Чтобы физически исключить перескок курсора на соседний монитор:
            // безопасный диапазон координат strictly [left, right - 1] и [top, bottom - 1]!
            g_safeRect.left = targetRect.left;
            g_safeRect.top = targetRect.top;
            g_safeRect.right = targetRect.right - 1;
            g_safeRect.bottom = targetRect.bottom - 1;

            g_lockedRect = g_safeRect;
            g_isLocked = true;

            // 1. Активируем системное ограничение ClipCursor
            ClipCursor(&g_lockedRect);

            // 2. Устанавливаем низкоуровневый хук мыши (WH_MOUSE_LL)
            // Ловит любые перемещения на уровне драйвера и БЛОКИРУЕТ случайные клики на соседних экранах
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            if (!g_hMouseHook) {
                g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInst, 0);
            }

            // 3. Устанавливаем хук смены активного окна (WinEventHook)
            // Мгновенно восстанавливает ClipCursor при Alt+Tab без ожидания тика таймера
            if (!g_hWinEventHook) {
                g_hWinEventHook = SetWinEventHook(
                    EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                    nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
                );
            }

            // 4. Высокочастотный таймер 10 мс как фоновая защита
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
// ХУК КЛАВИАТУРЫ ДЛЯ ИНТЕРАКТИВНОГО ЗАХВАТА НАЖАТИЙ
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
                if (g_hSettingsWnd) {
                    InvalidateRect(g_hSettingsWnd, nullptr, FALSE);
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
            if (g_hSettingsWnd) {
                InvalidateRect(g_hSettingsWnd, nullptr, FALSE);
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
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        g_hKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
        if (g_hSettingsWnd) {
            InvalidateRect(g_hSettingsWnd, nullptr, FALSE);
        }
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

    if (g_hSettingsWnd) {
        InvalidateRect(g_hSettingsWnd, nullptr, FALSE);
    }
}

// ============================================================================
// ОТРИСОВКА ОКНА НАСТРОЕК (DARK FLUENT DESIGN)
// ============================================================================
constexpr int DLG_WIDTH = 480;
constexpr int DLG_HEIGHT = 410;

constexpr RECT RC_RECORDER   = { 28, 100, 452, 178 };
constexpr RECT RC_BTN_SAVE   = { 28, 335, 150, 373 };
constexpr RECT RC_BTN_RESET  = { 165, 335, 310, 373 };
constexpr RECT RC_BTN_CANCEL = { 325, 335, 452, 373 };

void DrawRoundedBox(HDC hdc, const RECT& rc, COLORREF bg, COLORREF border, int borderWidth = 1, int radius = 8) {
    HPEN hPen = (borderWidth > 0) ? CreatePen(PS_SOLID, borderWidth, border) : CreatePen(PS_NULL, 0, 0);
    HBRUSH hBrush = CreateSolidBrush(bg);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void PaintSettingsWindow(HWND hWnd, HDC hdc) {
    RECT clientRc;
    GetClientRect(hWnd, &clientRc);

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, clientRc.right, clientRc.bottom);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    HBRUSH hBgBrush = CreateSolidBrush(RGB(24, 24, 27));
    FillRect(memDC, &clientRc, hBgBrush);
    DeleteObject(hBgBrush);

    SetBkMode(memDC, TRANSPARENT);

    // Заголовок
    SelectObject(memDC, g_hFontTitle);
    SetTextColor(memDC, RGB(255, 255, 255));
    RECT rcTitle = { 28, 18, 452, 44 };
    DrawTextW(memDC, L"Настройки CursorLock", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);

    // Подзаголовок
    SelectObject(memDC, g_hFontSmall);
    SetTextColor(memDC, RGB(161, 161, 170));
    RECT rcSub = { 28, 45, 452, 65 };
    DrawTextW(memDC, L"Абсолютная фиксация курсора мыши в границах выбранного монитора", -1, &rcSub, DT_LEFT | DT_SINGLELINE);

    // Метка горячей клавиши
    SelectObject(memDC, g_hFontNormal);
    SetTextColor(memDC, RGB(228, 228, 231));
    RECT rcLabelKey = { 28, 74, 452, 94 };
    DrawTextW(memDC, L"Горячая клавиша для переключения:", -1, &rcLabelKey, DT_LEFT | DT_SINGLELINE);

    // Карточка-рекордер
    COLORREF recBg = g_isRecording ? RGB(30, 41, 59) : (g_hoverState == HOVER_RECORDER ? RGB(45, 45, 52) : RGB(39, 39, 42));
    COLORREF recBorder = g_isRecording ? RGB(56, 189, 248) : (g_hoverState == HOVER_RECORDER ? RGB(99, 102, 241) : RGB(63, 63, 70));
    int recBorderWidth = g_isRecording ? 2 : 1;
    DrawRoundedBox(memDC, RC_RECORDER, recBg, recBorder, recBorderWidth, 10);

    if (g_isRecording) {
        SelectObject(memDC, g_hFontKey);
        SetTextColor(memDC, RGB(56, 189, 248));

        wchar_t parts[6][32];
        int partCount = 0;
        FormatHotkeyParts(g_tempMods, g_tempVk, parts, partCount);

        if (partCount > 0) {
            wchar_t heldText[128] = L"";
            for (int i = 0; i < partCount; ++i) {
                lstrcatW(heldText, parts[i]);
                lstrcatW(heldText, L" + ");
            }
            lstrcatW(heldText, L"...");

            RECT rcRecText = { RC_RECORDER.left, RC_RECORDER.top + 16, RC_RECORDER.right, RC_RECORDER.top + 46 };
            DrawTextW(memDC, heldText, -1, &rcRecText, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            RECT rcRecText = { RC_RECORDER.left, RC_RECORDER.top + 16, RC_RECORDER.right, RC_RECORDER.top + 46 };
            DrawTextW(memDC, L"Нажмите клавиши на клавиатуре...", -1, &rcRecText, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        SelectObject(memDC, g_hFontSmall);
        SetTextColor(memDC, RGB(148, 163, 184));
        RECT rcRecHint = { RC_RECORDER.left, RC_RECORDER.top + 48, RC_RECORDER.right, RC_RECORDER.bottom - 8 };
        DrawTextW(memDC, L"(Зажмите модификаторы и нажмите клавишу. Esc — отмена)", -1, &rcRecHint, DT_CENTER | DT_SINGLELINE);
    } else {
        wchar_t parts[6][32];
        int partCount = 0;
        FormatHotkeyParts(g_tempMods, g_tempVk, parts, partCount);

        SelectObject(memDC, g_hFontKey);

        int badgeH = 34;
        int badgePadX = 14;
        int sepW = 16;
        int gap = 8;
        int totalW = 0;
        int partWidths[6] = {};

        for (int i = 0; i < partCount; ++i) {
            SIZE sz;
            GetTextExtentPoint32W(memDC, parts[i], static_cast<int>(wcslen(parts[i])), &sz);
            partWidths[i] = sz.cx + badgePadX * 2;
            totalW += partWidths[i];
            if (i < partCount - 1) {
                totalW += sepW + gap * 2;
            }
        }

        int startX = RC_RECORDER.left + ((RC_RECORDER.right - RC_RECORDER.left) - totalW) / 2;
        int startY = RC_RECORDER.top + 13;

        int curX = startX;
        for (int i = 0; i < partCount; ++i) {
            RECT rcBadge = { curX, startY, curX + partWidths[i], startY + badgeH };
            DrawRoundedBox(memDC, rcBadge, RGB(63, 63, 70), RGB(82, 82, 91), 1, 6);

            SetTextColor(memDC, RGB(255, 255, 255));
            DrawTextW(memDC, parts[i], -1, &rcBadge, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            curX += partWidths[i];

            if (i < partCount - 1) {
                curX += gap;
                RECT rcSep = { curX, startY, curX + sepW, startY + badgeH };
                SetTextColor(memDC, RGB(161, 161, 170));
                DrawTextW(memDC, L"+", -1, &rcSep, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                curX += sepW + gap;
            }
        }

        SelectObject(memDC, g_hFontSmall);
        SetTextColor(memDC, (g_hoverState == HOVER_RECORDER) ? RGB(165, 180, 252) : RGB(148, 163, 184));
        RECT rcClickHint = { RC_RECORDER.left, RC_RECORDER.top + 52, RC_RECORDER.right, RC_RECORDER.bottom - 4 };
        DrawTextW(memDC, L"Кликните сюда, чтобы задать новую комбинацию", -1, &rcClickHint, DT_CENTER | DT_SINGLELINE);
    }

    // Метка выбора монитора
    SelectObject(memDC, g_hFontNormal);
    SetTextColor(memDC, RGB(228, 228, 231));
    RECT rcLabelMon = { 28, 192, 452, 212 };
    DrawTextW(memDC, L"Монитор для блокировки курсора:", -1, &rcLabelMon, DT_LEFT | DT_SINGLELINE);

    // Подсказка под селектором
    SelectObject(memDC, g_hFontSmall);
    SetTextColor(memDC, RGB(161, 161, 170));
    RECT rcInfo = { 28, 258, 452, 318 };
    DrawTextW(memDC,
        L"🛡️ Двойная защита: хук мыши (WH_MOUSE_LL) полностью блокирует вылет курсора и случайные клики.\n"
        L"Курсор физически изолирован внутри выбранного экрана даже при резких движениях мыши.",
        -1, &rcInfo, DT_LEFT | DT_WORDBREAK);

    // Кнопка «Сохранить»
    COLORREF btnSaveBg = (g_hoverState == HOVER_BTN_SAVE) ? RGB(37, 99, 235) : RGB(29, 78, 216);
    DrawRoundedBox(memDC, RC_BTN_SAVE, btnSaveBg, RGB(59, 130, 246), 1, 7);
    SelectObject(memDC, g_hFontNormal);
    SetTextColor(memDC, RGB(255, 255, 255));
    RECT rcBtnSave = RC_BTN_SAVE;
    DrawTextW(memDC, L"Сохранить", -1, &rcBtnSave, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // Кнопка «По умолчанию»
    COLORREF btnResetBg = (g_hoverState == HOVER_BTN_RESET) ? RGB(63, 63, 70) : RGB(39, 39, 42);
    DrawRoundedBox(memDC, RC_BTN_RESET, btnResetBg, RGB(82, 82, 91), 1, 7);
    SetTextColor(memDC, RGB(228, 228, 231));
    RECT rcBtnReset = RC_BTN_RESET;
    DrawTextW(memDC, L"Сброс (Ctrl+Alt+L)", -1, &rcBtnReset, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // Кнопка «Отмена»
    COLORREF btnCancelBg = (g_hoverState == HOVER_BTN_CANCEL) ? RGB(63, 63, 70) : RGB(39, 39, 42);
    DrawRoundedBox(memDC, RC_BTN_CANCEL, btnCancelBg, RGB(82, 82, 91), 1, 7);
    SetTextColor(memDC, RGB(228, 228, 231));
    RECT rcBtnCancel = RC_BTN_CANCEL;
    DrawTextW(memDC, L"Отмена", -1, &rcBtnCancel, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    BitBlt(hdc, 0, 0, clientRc.right, clientRc.bottom, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

// ============================================================================
// ОБРАБОТЧИК СООБЩЕНИЙ ОКНА НАСТРОЕК
// ============================================================================
bool PtInRectHelper(const RECT& rc, POINT pt) {
    return pt.x >= rc.left && pt.x <= rc.right && pt.y >= rc.top && pt.y <= rc.bottom;
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hFontTitle = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_hFontNormal = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_hFontKey = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_hFontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_hFontUi = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        EnableDarkTitleBar(hWnd);

        // Создаем выпадающий список для выбора монитора
        g_hComboMonitor = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            28, 218, 424, 200,
            hWnd, reinterpret_cast<HMENU>(IDC_COMBO_MONITOR),
            GetModuleHandleW(nullptr), nullptr
        );

        if (g_hComboMonitor) {
            SendMessageW(g_hComboMonitor, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontUi), TRUE);

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

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintSettingsWindow(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        HoverState newHover = HOVER_NONE;

        if (PtInRectHelper(RC_RECORDER, pt))        newHover = HOVER_RECORDER;
        else if (PtInRectHelper(RC_BTN_SAVE, pt))   newHover = HOVER_BTN_SAVE;
        else if (PtInRectHelper(RC_BTN_RESET, pt))  newHover = HOVER_BTN_RESET;
        else if (PtInRectHelper(RC_BTN_CANCEL, pt)) newHover = HOVER_BTN_CANCEL;

        if (newHover != g_hoverState) {
            g_hoverState = newHover;
            InvalidateRect(hWnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hoverState != HOVER_NONE) {
            g_hoverState = HOVER_NONE;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT && g_hoverState != HOVER_NONE) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };

        if (PtInRectHelper(RC_RECORDER, pt)) {
            StartRecording();
            return 0;
        }

        if (g_isRecording) {
            StopRecording(false);
        }

        if (PtInRectHelper(RC_BTN_SAVE, pt)) {
            if (g_tempMods == 0 && !(g_tempVk >= VK_F1 && g_tempVk <= VK_F24)) {
                MessageBoxW(hWnd,
                    L"Для предотвращения перехвата обычного ввода текста\n"
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

        if (PtInRectHelper(RC_BTN_RESET, pt)) {
            g_tempMods = MOD_CONTROL | MOD_ALT;
            g_tempVk = 'L';
            if (g_hComboMonitor) {
                SendMessageW(g_hComboMonitor, CB_SETCURSEL, 0, 0);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        if (PtInRectHelper(RC_BTN_CANCEL, pt)) {
            DestroyWindow(hWnd);
            return 0;
        }

        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        StopRecording(false);
        if (g_hFontTitle)  { DeleteObject(g_hFontTitle);  g_hFontTitle = nullptr; }
        if (g_hFontNormal) { DeleteObject(g_hFontNormal); g_hFontNormal = nullptr; }
        if (g_hFontKey)    { DeleteObject(g_hFontKey);    g_hFontKey = nullptr; }
        if (g_hFontSmall)  { DeleteObject(g_hFontSmall);  g_hFontSmall = nullptr; }
        if (g_hFontUi)     { DeleteObject(g_hFontUi);     g_hFontUi = nullptr; }
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
    g_tempTargetMonitor = g_targetMonitor;
    g_isRecording = false;

    const wchar_t SETTINGS_CLASS[] = L"CursorLock_ModernSettingsWindow";

    WNDCLASSEXW swc = {};
    swc.cbSize = sizeof(WNDCLASSEXW);
    swc.lpfnWndProc = SettingsWndProc;
    swc.hInstance = hInstance;
    swc.hbrBackground = nullptr;
    swc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    swc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
    swc.lpszClassName = SETTINGS_CLASS;

    RegisterClassExW(&swc);

    POINT pt = {};
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(hMon, &mi);

    RECT adjRc = { 0, 0, DLG_WIDTH, DLG_HEIGHT };
    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&adjRc, dwStyle, FALSE);

    int winW = adjRc.right - adjRc.left;
    int winH = adjRc.bottom - adjRc.top;
    int posX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - winW) / 2;
    int posY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - winH) / 2;

    g_hSettingsWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
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

    // Подменю выбора целевого экрана
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
            // Резервная проверка позиции курсора
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
