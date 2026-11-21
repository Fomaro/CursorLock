# CursorLock

A lightweight, native Win32 utility for Windows that securely locks and unlocks the mouse cursor within the boundaries of a specific display or the screen currently under the cursor with a single global hotkey.

## Key Highlights

- **Zero-Leak Boundary Lock (WH_MOUSE_LL)**: Uses a low-level hardware mouse hook alongside `ClipCursor` to inspect every mouse packet at zero latency. If the mouse attempts to cross the monitor edge or register clicks on an adjacent display, the coordinates are instantly clamped and the event is dropped. Accidental clicks on neighboring screens are completely impossible.
- **Accurate Physical Coordinate Clamping**: Windows monitor bounds (`rcMonitor.right` and `rcMonitor.bottom`) are exclusive coordinates that belong to the adjacent screen. CursorLock properly clips to `[left, right - 1]` and `[top, bottom - 1]`, preventing edge bleeding onto adjacent screens.
- **Instant Focus Recovery (SetWinEventHook)**: Intercepts `EVENT_SYSTEM_FOREGROUND` events to immediately re-apply cursor clipping whenever you `Alt+Tab` or a game/app window activates, without waiting for timer ticks.
- **Target Display Selection**: Choose to lock the cursor to **Auto** (whichever monitor is under the cursor) or bind it to a **specific display** (Screen 1, Screen 2, etc.) directly via the system tray context menu or the settings dialog. When a specific monitor is selected, CursorLock automatically centers your cursor on that display.
- **Multi-Monitor & DPI-Aware**: Uses `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` to accurately compute physical display rectangles across mixed DPI setups (100%, 125%, 150%, etc.), including negative monitor coordinates.
- **Interactive In-App Key Binder**: Modern dark UI dialog that records physical keyboard input in real time. Simply click the input area and press any key combination (supports Ctrl, Alt, Shift, Win modifiers plus any key).
- **Zero Companion Files**: Hotkey and display configuration are stored directly in the current user's registry (`HKCU\Software\CursorLock`). No `.ini` or config files left in the directory.
- **Single Portable Executable**: Pure C++17 and Win32 API. Statically linked, no external runtime or framework dependencies (~190 KB).

## Usage

1. Launch `CursorLock.exe`. It runs in the background and places an icon in the Windows notification area (system tray).
2. Default toggle shortcut: `Ctrl + Alt + L`.
   - First press: locks cursor to the target screen.
   - Second press: releases the cursor.
3. Right-click the tray icon to:
   - **Target Display (Целевой экран)**: Switch between *Auto (where cursor is)* or a specific physical screen (*Screen 1*, *Screen 2*, etc.).
   - **Settings (Настройки)**: Interactively record a new hotkey and set the default display.
   - **Exit**: Cleanly shut down and restore cursor boundaries.

## Building from Source

### MinGW-w64 (GCC)
```powershell
windres resource.rc -O coff -o resource.res
g++ -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ main.cpp resource.res -o CursorLock.exe -luser32 -lshell32 -lgdi32 -ladvapi32
```

### Microsoft Visual C++ (MSVC)
```cmd
rc resource.rc
cl /O2 /EHsc /DUNICODE /D_UNICODE /MT main.cpp resource.res user32.lib shell32.lib gdi32.lib advapi32.lib /link /SUBSYSTEM:WINDOWS /OUT:CursorLock.exe
```

## License

This software is released under the MIT License.
