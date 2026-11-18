@echo off
echo Building CursorLock...
windres resource.rc -O coff -o resource.res
g++ -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ main.cpp resource.res -o CursorLock.exe -luser32 -lshell32 -lgdi32 -ladvapi32
if %ERRORLEVEL% EQU 0 (
    echo Build successful: CursorLock.exe
) else (
    echo Build failed!
)
