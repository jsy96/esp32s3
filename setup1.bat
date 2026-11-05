@echo off
setlocal

rem ============================================================
rem  setup1.bat - one click toolchain setup (no proxy needed)
rem  1. update arduino board index (esp32 additional url)
rem  2. download esp32 toolchain via China mirrors
rem     (Espressif mirror + ghfast.top, see tools\fetch_esp32_core.py)
rem  3. install esp32 core offline from downloaded packages
rem  prerequisite: tools\arduino-cli\arduino-cli.exe and uv on PATH
rem ============================================================

set "CLI=%~dp0tools\arduino-cli\arduino-cli.exe"
set "URL=https://espressif.github.io/arduino-esp32/package_esp32_index.json"

if not exist "%CLI%" (
    echo [setup] ERROR: %CLI% not found.
    echo [setup] download arduino-cli zip and put arduino-cli.exe into tools\arduino-cli\
    exit /b 1
)
where uv >nul 2>&1
if errorlevel 1 (
    echo [setup] ERROR: uv not found in PATH, it manages python on this machine.
    exit /b 1
)

echo [setup] step 1/3: update board index ...
"%CLI%" core update-index --additional-urls %URL%
if errorlevel 1 (
    echo [setup] ERROR: index update failed, check network.
    exit /b 1
)

echo [setup] step 2/3: download toolchain via China mirrors ...
cd /d "%~dp0"
uv run --no-project python tools\fetch_esp32_core.py
if errorlevel 1 (
    echo [setup] ERROR: toolchain download failed.
    echo [setup] you can retry, or run with proxy: uv run --no-project python tools\fetch_esp32_core.py --proxy http://127.0.0.1:7890
    exit /b 1
)

echo [setup] step 3/3: install esp32 core offline ...
"%CLI%" core install esp32:esp32 --additional-urls %URL%
if errorlevel 1 (
    echo [setup] ERROR: core install failed.
    exit /b 1
)

"%CLI%" core list
echo [setup] done. now run flash1.bat to build and upload.
endlocal
