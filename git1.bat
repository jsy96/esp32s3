@echo off
setlocal

rem ============================================================
rem  git1.bat - one click add / commit / push to GitHub
rem  repo   : https://github.com/jsy96/esp32s3.git
rem  branch : main
rem  note   : github.com is routed through local proxy and the
rem           line is unstable, so push retries automatically
rem ============================================================

set "REMOTE_URL=https://github.com/jsy96/esp32s3.git"
set "BRANCH=main"
set "PROXY_HOST=127.0.0.1"
set "PROXY_PORT=7890"
set "MAX_RETRY=5"

rem -- always run from the folder where this script lives --
cd /d "%~dp0"

where git >nul 2>&1
if errorlevel 1 (
    echo [git1] ERROR: git not found in PATH
    exit /b 1
)

rem ---- keep remote origin pointed at the right repo ----
git remote get-url origin >nul 2>&1
if errorlevel 1 (
    git remote add origin %REMOTE_URL%
) else (
    git remote set-url origin %REMOTE_URL%
)
echo [git1] remote origin : %REMOTE_URL%

rem ---- github goes through the local proxy, direct line is blocked ----
git config http.https://github.com.proxy http://%PROXY_HOST%:%PROXY_PORT%
echo [git1] proxy for github.com : %PROXY_HOST%:%PROXY_PORT%

rem ---- stage everything ----
git add -A

rem ---- commit only when there is something to commit ----
git diff --cached --quiet >nul 2>&1
if not errorlevel 1 (
    echo [git1] nothing to commit, working tree clean
    goto push
)

set "MSG="
set /p MSG=Commit message (default: update):
if "%MSG%"=="" set "MSG=update"
git commit -m "%MSG%"
if errorlevel 1 (
    echo [git1] ERROR: commit failed
    exit /b 1
)

:push
set RETRY=0

:pushloop
if %RETRY% GEQ %MAX_RETRY% goto pushfail
echo [git1] pushing to origin/%BRANCH% (attempt %RETRY% + 1) ...
git push -u origin %BRANCH%
if not errorlevel 1 goto pushok
set /a RETRY=RETRY+1
echo [git1] push failed, wait 2s then retry ...
ping -n 3 127.0.0.1 >nul
goto pushloop

:pushfail
echo [git1] ERROR: push failed after %MAX_RETRY% attempts.
echo [git1] The proxy line looks unstable - switch Clash to another
echo [git1] node, then run git1.bat again.
exit /b 1

:pushok
echo [git1] push OK
endlocal
