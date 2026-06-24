/* version.h - the single source of truth for the engine version.
 *
 * Factorio-style MAJOR.MINOR.PATCH. We are pre-1.0 while the game takes shape:
 *   0.1.0  marks the end of the tech-demo FOUNDATION (the full ARCHITECTURE.md
 *          runtime: 4-byte voxel core, greedy meshing, GL2.1 forward renderer,
 *          fixed-point heat/melt/fluid CA, cross-chunk meshing, a streaming
 *          procedural world, region-file persistence, the progression observer,
 *          day/night, and mouse-look) and the START of actual gameplay.
 * Bump policy:
 *   PATCH - fixes / small tweaks, save-compatible.
 *   MINOR - new gameplay or features (the 0.2.x, 0.3.x, ... cadence toward 1.0).
 *   MAJOR - the 1.0 release, or a deliberately breaking overhaul.
 *
 * PRE-RELEASE: while a MINOR is in development the numbers already read the
 * TARGET version (e.g. 0.2.0) and VOXEL_VERSION_PRERELEASE carries a suffix
 * ("-dev") so a work-in-progress build is unmistakably NOT the final release.
 * On release, set the suffix to "" (empty) - that, plus the changelog/archive/
 * tag ritual, is what makes 0.2.0 "final". The PACKED numeric ignores the
 * suffix, so a dev save still stamps as its target version.
 *
 * Everything that needs a version (window title, startup banner, save-file
 * stamp) derives from the values below - change them HERE only.
 */
#ifndef VERSION_H
#define VERSION_H

#define VOXEL_VERSION_MAJOR 0
#define VOXEL_VERSION_MINOR 4
#define VOXEL_VERSION_PATCH 0
#define VOXEL_VERSION_PRERELEASE ""   /* empty on a final release; -dev in progress */

/* Build the "0.2.0-dev" string from the numbers + suffix (one source of truth). */
#define VOXEL__STR2(x) #x
#define VOXEL__STR(x)  VOXEL__STR2(x)
#define VOXEL_VERSION_STRING                 \
    VOXEL__STR(VOXEL_VERSION_MAJOR) "."      \
    VOXEL__STR(VOXEL_VERSION_MINOR) "."      \
    VOXEL__STR(VOXEL_VERSION_PATCH)          \
    VOXEL_VERSION_PRERELEASE

/* Packed 0x00MMmmpp - stamped into save files for forward migration, and cheap
 * to compare numerically (a save's stamp < the running version => older save). */
#define VOXEL_VERSION_PACK(maj, min, pat)    \
    (((unsigned)(maj) << 16) | ((unsigned)(min) << 8) | (unsigned)(pat))
#define VOXEL_VERSION_PACKED                 \
    VOXEL_VERSION_PACK(VOXEL_VERSION_MAJOR, VOXEL_VERSION_MINOR, VOXEL_VERSION_PATCH)

/* Display name + window-title / banner string ("Voxel Engine 0.1.0"). */
#define VOXEL_NAME  "Voxel Engine"
#define VOXEL_TITLE VOXEL_NAME " " VOXEL_VERSION_STRING

#endif /* VERSION_H */
