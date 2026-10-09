@echo off
rem ---------------------------------------------------------------------------
rem  Diddy Kong Racing for the Nintendo 3DS - builder
rem
rem  Makes the game from your own ROM:
rem    - put the ROM in the "rom" folder and double-click this file, or
rem    - drop the ROM file onto this file.
rem  The result appears in the "output" folder. README.md has the rest.
rem
rem  All the work is done by builder\build.py; this file only finds Python
rem  for it (an installed one, or a private copy it offers to download).
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
title Diddy Kong Racing for the Nintendo 3DS - builder

set "PYKIND="
if exist "builder\python\python.exe" set "PYKIND=own"
if not defined PYKIND py -3 -c "import sys; sys.exit(sys.version_info < (3, 6))" >nul 2>nul && set "PYKIND=py"
if not defined PYKIND python -c "import sys; sys.exit(sys.version_info < (3, 6))" >nul 2>nul && set "PYKIND=python"
if not defined PYKIND call :get_python
if not defined PYKIND goto no_python

if "%PYKIND%"=="own" goto run_own
if "%PYKIND%"=="py" goto run_py
python "%~dp0builder\build.py" %*
goto done
:run_py
py -3 "%~dp0builder\build.py" %*
goto done
:run_own
"%~dp0builder\python\python.exe" "%~dp0builder\build.py" %*
goto done

:done
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%

:no_python
echo.
echo The builder needs Python 3 and could not get it.
echo Install it from https://www.python.org/downloads/ (tick "Add python.exe
echo to PATH" in the installer), then run this file again.
echo.
pause
exit /b 1

:get_python
echo Python was not found on this PC, and the builder needs it.
echo.
echo It can download a private copy from python.org (11 MB) into the folder
echo builder\python. Nothing is installed and nothing else on the PC changes;
echo deleting that folder removes it again.
echo.
choice /c YN /m "Download it now"
if errorlevel 2 exit /b 0
set "PYARCH=amd64"
set "PYSUM=4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" if not defined PROCESSOR_ARCHITEW6432 set "PYARCH=win32"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "PYARCH=arm64"
if "%PYARCH%"=="win32" set "PYSUM=084b9eb24cb848605c895d05b738fbc2572efc8b4c18c415a824065864a2b853"
if "%PYARCH%"=="arm64" set "PYSUM=3065efc3d382d1cda66757ac71ade11904fa6e350f5a97eb74811acd71ba5532"
echo Downloading...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; $zip = 'builder\python.zip'; Invoke-WebRequest -UseBasicParsing -Uri 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-%PYARCH%.zip' -OutFile $zip; if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne '%PYSUM%') { Remove-Item $zip; throw 'the download is not the file expected' }; Expand-Archive -Force $zip 'builder\python'; Remove-Item $zip"
if errorlevel 1 echo The download did not work.
if exist "builder\python\python.exe" set "PYKIND=own"
echo.
exit /b 0
