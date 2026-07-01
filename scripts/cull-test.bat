@echo off
rem Frustum-cull perf test (Windows / XP).  Usage: cull-test.bat [view_radius]
rem Place next to voxel.exe (or in the repo root with build\voxel.exe).
rem Logs "cull: drawn=N culled=M (..% culled) resident=R" every ~120 frames to
rem voxel_log.txt next to the exe (the GUI app has no console). In-game, use the
rem Up/Down keys to push view distance and watch the FPS counter; compare runs
rem with NOCULL=1 set below to feel the draw-call savings directly.
setlocal
set VOXEL_CULL_LOG=1
rem To A/B against no culling, remove "rem" from the next line:
rem set VOXEL_NOCULL=1
if not "%1"=="" set VOXEL_VIEW_RADIUS=%1
set BIN=%~dp0voxel.exe
if not exist "%BIN%" set BIN=%~dp0build\voxel.exe
echo Cull telemetry ON - after playing, read voxel_log.txt next to the exe.
"%BIN%"
endlocal
