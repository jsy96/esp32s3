@echo off
setlocal

rem ============================================================
rem  flash1.bat - one click compile + upload firmware
rem  board : ESP32-S3 N16R8 (16MB flash / OPI PSRAM)
rem  usb   : native USB port with TinyUSB CDC
rem  usage : flash1.bat [COMx]     (port auto-detected if omitted)
rem ============================================================

set "FQBN=esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,UploadMode=cdc,FlashSize=16M,PSRAM=opi"
set "SKETCH=%~dp0firmware\tilt_gamepad"
set "CLI=%~dp0tools\arduino-cli\arduino-cli.exe"
set "MAX_RETRY=3"

if not exist "%CLI%" (
    echo [flash] ERROR: %CLI% not found.
    echo [flash] run setup1.bat first, or put arduino-cli.exe there.
    exit /b 1
)

rem ---- port: argument or auto-detect ----
set "PORT=%~1"
if "%PORT%"=="" (
    for /f "tokens=1" %%p in ('"%CLI%" board list 2^>nul ^| findstr /i "COM"') do (
        set "PORT=%%p"
        goto :gotport
    )
    echo [flash] no serial port found.
    echo [flash] plug the board "USB" port, or run: flash1.bat COM7
    exit /b 1
)
:gotport
echo [flash] port   : %PORT%
echo [flash] sketch : firmware\tilt_gamepad

set RETRY=0
:loop
if %RETRY% GEQ %MAX_RETRY% goto fail
echo [flash] build + upload, attempt %RETRY% + 1 ...
"%CLI%" compile --upload -p %PORT% --fqbn %FQBN% "%SKETCH%"
if not errorlevel 1 goto ok
set /a RETRY=RETRY+1
echo [flash] failed. if it keeps failing: hold BOOT, tap RST, release BOOT
echo [flash] to enter download mode, then run again. wait 2s ...
ping -n 3 127.0.0.1 >nul
goto loop

:fail
echo [flash] ERROR: upload failed after %MAX_RETRY% attempts.
exit /b 1

:ok
echo [flash] done. open joy.cpl to test the gamepad.
endlocal
