@echo off
title Vespera Online - keep this window open
cd /d "%~dp0"
echo.
echo  Starting Vespera...
start "" "%~dp0Vespera.exe"

set "CF=cloudflared"
where cloudflared >nul 2>nul
if errorlevel 1 (
  if exist "%ProgramFiles(x86)%\cloudflared\cloudflared.exe" (set "CF=%ProgramFiles(x86)%\cloudflared\cloudflared.exe") else if exist "%ProgramFiles%\cloudflared\cloudflared.exe" (set "CF=%ProgramFiles%\cloudflared\cloudflared.exe") else (
    echo  Installing the free Cloudflare tunnel (one time only^)...
    winget install --id Cloudflare.cloudflared -e --accept-source-agreements --accept-package-agreements
    if exist "%ProgramFiles(x86)%\cloudflared\cloudflared.exe" (set "CF=%ProgramFiles(x86)%\cloudflared\cloudflared.exe") else if exist "%ProgramFiles%\cloudflared\cloudflared.exe" (set "CF=%ProgramFiles%\cloudflared\cloudflared.exe")
  )
)

timeout /t 3 /nobreak >nul
echo.
echo  ================================================================
echo   Your public Vespera link appears below in a box, ending in
echo   .trycloudflare.com  -  share that link with anyone.
echo   Keep this window AND the Vespera window open.
echo   The link changes each time you start this file.
echo  ================================================================
echo.
"%CF%" tunnel --url http://localhost:8080
echo.
echo  The tunnel stopped. Close this window, then open this file again.
pause
