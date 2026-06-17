@echo off
rem Host a multiplayer game (Windows).  Usage: host.bat [port]
rem Place next to voxel.exe (or in the repo root with build\voxel.exe).
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=9001
set VOXEL_HOST=:%PORT%
set BIN=%~dp0voxel.exe
if not exist "%BIN%" set BIN=%~dp0build\voxel.exe
echo Hosting on TCP port %PORT%.  Share your address with players:
echo   - same LAN : your LAN IPv4 (run: ipconfig)
echo   - internet : your Tailscale/ZeroTier IP, or your public IP if you
echo                port-forwarded TCP %PORT% to this PC
"%BIN%"
endlocal
