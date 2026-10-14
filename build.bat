@echo off
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin
set "sources=src\main.cpp src\rapoo_protocol.cpp src\device_manager.cpp src\osd_window.cpp src\tray_menu.cpp src\alert_window.cpp src\alert_settings.cpp src\hid_transport.cpp src\keyboard_manager.cpp src\autostart.cpp"
where g++ >nul 2>nul
if not errorlevel 1 goto mingw
where cl >nul 2>nul
if not errorlevel 1 goto msvc
echo [ERROR] Neither g++ nor cl.exe found.
exit /b 1
:mingw
windres --codepage 65001 -I res -i res\app.rc -o bin\app.o
if errorlevel 1 goto failure
g++ -std=c++17 -O3 -mwindows -municode -s -static %sources% bin\app.o -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32 -luxtheme -ldwmapi -o bin\rapoo-tray.new.exe
if errorlevel 1 goto failure
goto success
:msvc
rc /c 65001 /fo bin\app.res res\app.rc
if errorlevel 1 goto failure
cl /nologo /std:c++17 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN %sources% bin\app.res /Fo:bin\ /Fe:bin\rapoo-tray.new.exe /link /SUBSYSTEM:WINDOWS setupapi.lib hid.lib user32.lib gdi32.lib shell32.lib advapi32.lib uxtheme.lib dwmapi.lib
if errorlevel 1 goto failure
goto success
:failure
echo [ERROR] Build failed; previous EXE retained.
exit /b 1
:success
move /y bin\rapoo-tray.new.exe bin\rapoo-tray.exe >nul
if errorlevel 1 exit /b 1
echo [SUCCESS] bin\rapoo-tray.exe
exit /b 0
