# CursorLock

A lightweight, native Win32 utility for Windows that locks and unlocks the mouse cursor within the boundaries of the monitor where it currently resides with a single global hotkey.

## Key Highlights

- **Multi-Monitor & DPI-Aware**: Uses `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` to accurately compute physical display rectangles across mixed DPI setups (100%, 125%, 150%, etc.), including negative monitor coordinates.
- **Persistent Boundary Enforcement**: A lightweight 50 ms timer continuously reapplies `ClipCursor`, ensuring the boundary holds even when switching windows with `Alt+Tab` or pressing the `Windows` key.
- **Interactive In-App Key Binder**: Modern dark UI dialog that records physical keyboard input in real time. Simply click the input area and press any key combination (supports Ctrl, Alt, Shift, Win modifiers plus any key).
- **Zero Companion Files**: Hotkey configuration is stored directly in the current user's registry (`HKCU\Software\CursorLock`). No `.ini` or config files left in the directory.
- **Single Portable Executable**: Pure C++17 and Win32 API. No external runtime or framework dependencies.

## Usage

1. Launch `CursorLock.exe`. It runs in the background and places an icon in the Windows notification area (system tray).
2. Default toggle shortcut: `Ctrl + Alt + L`.
   - First press: locks cursor to the current screen.
   - Second press: releases the cursor.
3. Right-click the tray icon to:
   - Toggle cursor lock manually.
   - Open **Settings** to rebind the shortcut interactively.
   - Exit the application cleanly.

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
