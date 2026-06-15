/* voxel.h - The 4-byte voxel word and the temperature codec.
 *
 * Binding source: ARCHITECTURE.md Decision Ledger (voxel = 4 bytes, one u32:
 * mat8 | temp8 | fill4 | light4 | ao4 | flags4) and Section 2 (full field
 * semantics + two-segment round-to-nearest temperature codec).
 *
 * The voxel is ONE uint32_t accessed through masks/shifts, NEVER C bitfields:
 * bitfield layout is implementation-defined and would drift across the
 * cross-compile toolchains (Linux GCC/Clang -> XP MinGW). This exact bit
 * pattern is persisted to disk and uploaded to the GPU, so the layout must be
 * byte-for-byte identical on every compiler. Masks give us that.
 *
 *   bits  0..7   : material id      (8)  index into g_materials[256]
 *   bits  8..15  : temperature code (8)  two-segment non-linear, see codec
 *   bits 16..19  : fluid fill level  (4)  0..15, 0 = empty of fluid
 *   bits 20..23  : baked light       (4)  0..15, max(sky,block), mesh-time
 *   bits 24..27  : ambient occlusion (4)  0..15, corner darkening, mesh-time
 *   bits 28..31  : state flags        (4)  ACTIVE | LIQUID | DIRTY_MESH | RESERVED
 */
#ifndef VOXEL_H
#define VOXEL_H

#include <stdint.h>

/* The entire persistent state of one cubic metre of the world. 4 bytes.
 * Bit-exact layout - DO NOT reorder; persistence and the GPU depend on it. */
typedef uint32_t Voxel;

_Static_assert(sizeof(Voxel) == 4, "Voxel must be exactly 4 bytes (one u32)");

/* ---- Chunk dimensions (restated from the Decision Ledger / Section 2.4) ----
 * 16^3 = 4096 voxels = 16384 bytes = 16 KiB == half the 32 KB L1D. */
#define CHUNK_DIM    16
#define CHUNK_VOXELS (CHUNK_DIM * CHUNK_DIM * CHUNK_DIM)   /* 4096 */

/* ---- Field masks / shifts (compile-time constants) ----------------------- */
#define VOX_MAT_SHIFT     0u
#define VOX_MAT_MASK      0x000000FFu
#define VOX_TEMP_SHIFT    8u
#define VOX_TEMP_MASK     0x0000FF00u
#define VOX_FILL_SHIFT    16u
#define VOX_FILL_MASK     0x000F0000u
#define VOX_LIGHT_SHIFT   20u
#define VOX_LIGHT_MASK    0x00F00000u
#define VOX_AO_SHIFT      24u
#define VOX_AO_MASK       0x0F000000u
#define VOX_FLAGS_SHIFT   28u
#define VOX_FLAGS_MASK    0xF0000000u

/* ---- State flag bits (the 4-bit field at 28..31) ------------------------- */
#define VF_ACTIVE     0x1u  /* in the simulation wake-set this tick            */
#define VF_LIQUID     0x2u  /* phase fast-path: skip solid-only CA branches    */
#define VF_DIRTY_MESH 0x4u  /* per-voxel remesh hint; chunk dirty is authoritative */
#define VF_RESERVED   0x8u  /* unassigned - do not consume without a Section-2 edit */

/* ---- Field accessors (read) ---------------------------------------------- */
/* Each is a shift + and against compile-time constants: a couple of cheap ALU
 * ops in the integer pipeline, trivial next to a cache miss. */
static inline uint8_t vox_mat(Voxel v)   { return (uint8_t)( v                     & 0xFFu); }
static inline uint8_t vox_temp_code(Voxel v){ return (uint8_t)((v >> VOX_TEMP_SHIFT)  & 0xFFu); }
static inline uint8_t vox_fill(Voxel v)  { return (uint8_t)((v >> VOX_FILL_SHIFT)  & 0x0Fu); }
static inline uint8_t vox_light(Voxel v) { return (uint8_t)((v >> VOX_LIGHT_SHIFT) & 0x0Fu); }
static inline uint8_t vox_ao(Voxel v)    { return (uint8_t)((v >> VOX_AO_SHIFT)    & 0x0Fu); }
static inline uint8_t vox_flags(Voxel v) { return (uint8_t)((v >> VOX_FLAGS_SHIFT) & 0x0Fu); }

/* ---- Field accessors (write): clear-then-set the target field ------------ */
static inline void vox_set_mat(Voxel *v, uint8_t m) {
    *v = (*v & ~VOX_MAT_MASK) | ((uint32_t)m << VOX_MAT_SHIFT);
}
static inline void vox_set_temp_code(Voxel *v, uint8_t t) {
    *v = (*v & ~VOX_TEMP_MASK) | ((uint32_t)t << VOX_TEMP_SHIFT);
}
static inline void vox_set_fill(Voxel *v, uint8_t f) {
    *v = (*v & ~VOX_FILL_MASK) | (((uint32_t)f & 0x0Fu) << VOX_FILL_SHIFT);
}
static inline void vox_set_light(Voxel *v, uint8_t l) {
    *v = (*v & ~VOX_LIGHT_MASK) | (((uint32_t)l & 0x0Fu) << VOX_LIGHT_SHIFT);
}
static inline void vox_set_ao(Voxel *v, uint8_t a) {
    *v = (*v & ~VOX_AO_MASK) | (((uint32_t)a & 0x0Fu) << VOX_AO_SHIFT);
}
static inline void vox_set_flags(Voxel *v, uint8_t s) {
    *v = (*v & ~VOX_FLAGS_MASK) | (((uint32_t)s & 0x0Fu) << VOX_FLAGS_SHIFT);
}

/* ===== Temperature codec - two-segment non-linear, ROUND-TO-NEAREST =======
 * Binding constants - DO NOT change without re-validating the heat sim.
 *   code 0..159   : -40 .. +120 C @ 1.0  C/code  (ambient / biology band)
 *   code 160..255 : 120 .. 2020 C @ 20.0 C/code  (industrial heat band)
 * Boundary: code 160 == exactly 120 C, shared by both segments (continuous).
 * ENCODE ROUNDS TO NEAREST (add half-step before the divide) - not floor. */
#define T_AMB_BASE_C   (-40)   /* celsius at code 0                          */
#define T_AMB_STEP_C     1     /* celsius per code, codes 0..159             */
#define T_HOT_CODE     160     /* first code of the industrial segment       */
#define T_HOT_BASE_C   120     /* celsius at code 160                        */
#define T_HOT_STEP_C    20     /* celsius per code, codes 160..255           */

/* Decode a temperature code to degrees Celsius (one compare + multiply-add).
 * The ambient branch is the common case (most of the world sits near 20 C ->
 * code 60), so the predictor stays warm. Returns degrees C as double. */
static inline double temp_decode_c(uint8_t code) {
    if (code < T_HOT_CODE)
        return (double)(T_AMB_BASE_C + (int)code * T_AMB_STEP_C);
    return (double)(T_HOT_BASE_C + (int)(code - T_HOT_CODE) * T_HOT_STEP_C);
}

/* Encode degrees Celsius to a temperature code, round-to-nearest, clamped.
 * Round-to-nearest (the +step/2 before the integer divide) is WHY the binding
 * anchors match: Fe 1538 C -> code 231 (decodes 1540), Cu 1085 C -> code 208
 * (decodes 1080). Floor division would undershoot Fe to code 230 (1520 C). */
static inline uint8_t temp_encode_c(double celsius) {
    if (celsius <= (double)T_HOT_BASE_C) {                  /* ambient segment */
        int code = (int)((celsius - (double)T_AMB_BASE_C
                          + (double)T_AMB_STEP_C / 2.0) / (double)T_AMB_STEP_C);
        if (code < 0)   code = 0;                           /* clamp: -40 C floor */
        if (code > 255) code = 255;
        return (uint8_t)code;
    }
    int code = T_HOT_CODE
             + (int)((celsius - (double)T_HOT_BASE_C
                      + (double)T_HOT_STEP_C / 2.0) / (double)T_HOT_STEP_C);
    if (code < 0)   code = 0;
    if (code > 255) code = 255;                             /* clamp: code 255 == 2020 C ceiling */
    return (uint8_t)code;
}

#endif /* VOXEL_H */
