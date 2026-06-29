voxel-engine — ROLLING 0.5.0-dev build (branch 0.5-dev)

  voxel      Linux x86-64 (native dev build)
  voxel.exe  Windows PE32 / Intel 80386 — runs on Windows XP SP3+
             (cross-built with i686-w64-mingw32-gcc; -Wall -Wextra clean,
             determinism-verified vs the native build under wine)

This is the CURRENT development build, refreshed on each source commit so the
latest is always grabbable from the repo (it is NOT a tagged release — 0.5 is
still in development and pending playtest). For a stable archived release see
the numbered archive/<version>/ directories.

STARTUP DIAGNOSTICS: if the game fails to start on Windows it now shows a dialog
explaining why, and writes voxel_log.txt next to voxel.exe (includes the OpenGL
driver/version). If it won't run, send that file. A GL_VERSION line like
"1.1.0 ... GDI Generic" means the graphics driver is missing (install the
NVIDIA driver); "2.1 NVIDIA ..." means the driver is fine.

Built 2026-06-26.
