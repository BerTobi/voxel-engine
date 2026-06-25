/* units.h - the metres<->voxels split (0.5 fine-grain foundation).
 *
 * 0.1-0.4 hardcoded "1 voxel == 1 metre" everywhere, implicitly. 0.5 makes the
 * grain finer (0.5 m/voxel) WITHOUT touching the integer hot loops: the greedy
 * mesher and the cellular automaton still index in whole voxels exactly as
 * before (CHUNK_DIM stays 16, the von-Neumann 1/6 stability proof is preserved).
 * The grain shows up ONLY at the physics / worldgen / render boundary, where a
 * PHYSICAL length or speed (metres, m/s) must be turned into voxel-grid units.
 * Author those constants in metres and wrap them in M2V() so they keep their
 * real-world magnitude across a grain change.
 *
 * Compile-time + constant-folded: VOX_GRAIN_MM is the single knob, every macro
 * folds to a literal at -O2, so there is ZERO runtime cost and no shape change.
 * A future 0.25 m experiment is VOX_GRAIN_MM=250 and nothing else.
 *
 * 0.5 M0: this header is ADDITIVE and applied NOWHERE yet - it is compiled and
 * its algebra asserted by test_grain (make check). M2 flips the grain and routes
 * every physical constant through M2V/V2M behind an audited grep gate.
 */
#ifndef UNITS_H
#define UNITS_H

/* The grain: millimetres of world per voxel edge. 1000 = 1 m (the 0.1-0.4
 * implicit grain); 500 = 0.5 m (the 0.5 ship grain, k=2). Integer millimetres so
 * the "divides 1 m cleanly" invariant is a compile-time check, not a float
 * compare. M0 leaves this at 500 but applies it nowhere; M2 is the flip. */
#define VOX_GRAIN_MM     500

/* Physical size of one voxel edge, and its reciprocal, as float scale factors.
 * (1000.0f forces FLOAT division, so VOX_GRAIN_MM=500 -> 0.5f, not integer 0.) */
#define METRES_PER_VOXEL (VOX_GRAIN_MM / 1000.0f)   /* 0.5 m/voxel at k=2      */
#define VOXELS_PER_METRE (1000.0f / VOX_GRAIN_MM)   /* 2 voxels/m at k=2       */

/* Convert AT THE BOUNDARY. M2V: a physical length or speed (metres, m/s) ->
 * voxel units (voxels, voxels/s). V2M: the inverse. Author physical constants in
 * metres and wrap them so they survive a grain change with their meaning intact. */
#define M2V(m) ((m) * VOXELS_PER_METRE)
#define V2M(v) ((v) * METRES_PER_VOXEL)

/* The grain must divide one metre exactly, so a 1 m structure is a whole number
 * of voxels and the worldgen strata math stays integer. (gcc accepts
 * _Static_assert under -std=c99 as an extension; mesher.h already relies on it.) */
_Static_assert(1000 % VOX_GRAIN_MM == 0, "VOX_GRAIN_MM must divide 1000 (1 m) exactly");

#endif /* UNITS_H */
