@echo off
rem Join a multiplayer game (Windows).  Usage: join.bat [host-ip] [port]
rem Double-click to be prompted for the host IP. Place next to voxel.exe.
setlocal
set HOST=%1
if "%HOST%"=="" set /p HOST=Host IP:
set PORT=%2
if "%PORT%"=="" set PORT=9001
set VOXEL_CONNECT=%HOST%:%PORT%
set BIN=%~dp0voxel.exe
if not exist "%BIN%" set BIN=%~dp0build\voxel.exe
echo Connecting to %HOST%:%PORT% ...
"%BIN%"
endlocal
