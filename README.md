# CursorLock

A lightweight, native Win32 utility for Windows that locks and unlocks the mouse cursor within the boundaries of a specific display or the screen currently under the cursor with a single global hotkey.

## Key Highlights

- **Target Display Selection**: Choose to lock the cursor to **Auto** (whichever monitor is under the cursor) or bind it to a **specific display** (Screen 1, Screen 2, etc.) directly via the system tray context menu or the settings dialog. When a specific monitor is selected, CursorLock automatically moves the cursor into its center upon activation.
- **Multi-Monitor & DPI-Aware**: Uses `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` to accurately compute physical display rectangles across mixed DPI setups (100%, 125%, 150%, etc.), including negative monitor coordinates.
- **Persistent Boundary Enforcement**: A lightweight 50 ms timer continuously reapplies `ClipCursor`, ensuring the boundary holds even when switching windows with `Alt+Tab` or pressing the `Windows` key.
- **Interactive In-App Key Binder**: Modern dark UI dialog that records physical keyboard input in real time. Simply click the input area and press any key combination (supports Ctrl, Alt, Shift, Win modifiers plus any key).
- **Zero Companion Files**: Hotkey and display configuration are stored directly in the current user's registry (`HKCU\Software\CursorLock`). No `.ini` or config files left in the directory.
- **Single Portable Executable**: Pure C++17 and Win32 API. Statically linked, no external runtime or framework dependencies.

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
