@echo off
rem Play what the builder made in a portable Azahar emulator on this PC,
rem without a console. For developers; docs\BUILDING.md says where the
rem emulator goes (tools\azahar\azahar-windows-msvc-...\).
rem
rem A game controller is mapped once with:  python tools\map-controller.py
rem (A/B/X/Y by position, left stick = Circle Pad, right stick = C-Stick,
rem  triggers = ZL/ZR, Back = SELECT for the touch screen menu, which the
rem  mouse works too).
setlocal
set "ROOT=%~dp0.."
set "EMU="
for /d %%D in ("%ROOT%\tools\azahar\azahar-windows-msvc-*") do set "EMU=%%D"
if not defined EMU (
    echo No emulator in tools\azahar - see docs\BUILDING.md
    exit /b 1
)
set "GAME=%ROOT%\output\sdcard\3ds\DKR"
if not exist "%GAME%\DKR.3dsx" (
    echo Nothing built yet - run BUILD.cmd first
    exit /b 1
)
set "SD=%EMU%\user\sdmc\3ds\DKR"
xcopy /e /i /y /q "%GAME%" "%SD%" >nul
rem A test script left over from a test run would play the game by itself.
if exist "%SD%\AUTOTEST.TXT" del "%SD%\AUTOTEST.TXT"
start "" "%EMU%\azahar.exe" "%SD%\DKR.3dsx"
