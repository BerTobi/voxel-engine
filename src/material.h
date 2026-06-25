/* material.h - The global MaterialDef table: where behaviour lives as data.
 *
 * Binding source: ARCHITECTURE.md Section 2.3. The governing principle is that
 * behaviour is NOT stored in the voxel; it is stored in the MaterialDef the
 * voxel's 8-bit material id points at. A voxel says "I am material 47 at
 * 1200 C"; g_materials[47] says "I am copper, melt 1085 C, here is my specific
 * heat". Every behavioural question is answered by indexing this table, NEVER
 * by switch-ing on the id (a switch outside the table's own construction is a
 * design violation - the hardcoded-recipe pattern the game exists to avoid).
 *
 * The table is 256 entries x 64 bytes = exactly 16 KiB, resident always, built
 * once at startup from data, never reallocated. The power-of-two 64-byte stride
 * makes &g_materials[id] a shift-and-add and keeps a hot access to one or two
 * cache lines.
 */
#ifndef MATERIAL_H
#define MATERIAL_H

#include <stdint.h>
#include "voxel.h"

#define MAX_MATERIALS 256

/* ---- Phase class: selects which CA rule-set a voxel obeys ----------------
 * Numeric values are stable (persisted indirectly via the data table). */
typedef enum {
    PHASE_GAS    = 0,  /* air, steam, smoke, oxygen - buoyant, diffuse, fill=density */
    PHASE_LIQUID = 1,  /* water, lava, molten metals - flows, equalizes, fill=column */
    PHASE_POWDER = 2,  /* sand, gravel, ash, ore-dust - falls, piles at rest angle   */
    PHASE_SOLID  = 3   /* stone, ores, ingots, wood  - static until melted/broken    */
} PhaseClass;

/* ---- MaterialDef flag bits (the cheap booleans mesher/renderer read) ----- */
#define MAT_OPAQUE    0x01u  /* does this face cull its neighbour (most-read in mesher) */
#define MAT_EMISSIVE  0x02u  /* injects block-light (lava, fire)                        */
#define MAT_OXIDIZER  0x04u  /* feeds combustion / rusting reactions                    */
#define MAT_SPRING    0x08u  /* inexhaustible liquid SOURCE: sim auto-registers it as a spring */
/* bits 0x10..0x80 reserved */

/* One material. Exactly 64 bytes, cache-line-friendly. Padded so the array
 * stride is a power of two. Fields earn their place by being read by an
 * emergent mechanic (see Section 2.3). */
typedef struct {
    /* --- identity / rendering (read by mesher + renderer) --- */
    char     name[16];          /* "iron", "molten_iron", ... - debug/persistence       */
    uint8_t  atlas_tile;        /* index into the 16x16 = 256 tile atlas (Section 5)    */
    uint8_t  phase;             /* PhaseClass - selects the CA rule-set                 */
    uint8_t  color_rgba[4];     /* placeholder-atlas tile fill + tint over a real tile  */

    /* --- thermal (read by heat diffusion + phase transitions) --- */
    uint16_t density_kg_m3;     /* kg/m^3 (air ~1 .. gold ~19300)                        */
    uint16_t specific_heat;     /* J/(kg*K); water 4186, Fe 449                          */
    uint16_t thermal_conductivity; /* mW/(m*K) scaled; how fast heat crosses faces       */
    int16_t  melt_point_c;      /* solidus/liquidus in C; -1 = does not melt (it burns)  */
    int16_t  boil_point_c;      /* vaporization in C; -1 = sublimes / never boils        */
    uint16_t latent_fusion;     /* kJ/kg absorbed at melt before temp rises again        */
    uint16_t latent_vapor;      /* kJ/kg absorbed at boil                                */
    uint8_t  melts_to;          /* material id of the liquid phase (iron -> molten_iron) */
    uint8_t  freezes_to;        /* material id of the solid phase (molten_iron -> iron)  */
    uint8_t  boils_to;          /* material id of the gas phase (water -> steam)         */
    uint8_t  condenses_to;      /* material id when gas cools (steam -> water)           */

    /* --- fluid dynamics (read by the falling-sand / fluid pass) --- */
    uint8_t  viscosity;         /* 0=instant spread (water) .. 255=barely creeps (lava)  */
    uint8_t  rest_angle;        /* powders: angle-of-repose proxy, slope before sliding  */

    /* --- mechanical (read by mining / structural logic) --- */
    uint8_t  hardness;          /* mining time / tool tier gate; 255 = unbreakable       */
    uint8_t  breaks_to;         /* material id of the drop/rubble (stone -> cobble)      */

    /* --- chemistry / combustion (read by the reaction pass) --- */
    int16_t  ignition_c;        /* auto-ignition temp in C; -1 = non-flammable           */
    uint8_t  burns_to;          /* material id of the ash/residue (wood -> charcoal)     */
    uint8_t  flammability;      /* 0..255 spread propensity; gates the fire CA           */

    /* --- electrical (reserved for the progression tier; deferred) --- */
    uint8_t  conductivity;      /* 0=insulator .. 255=copper; do not build CA on this yet*/

    uint8_t  flags;             /* MAT_OPAQUE | MAT_EMISSIVE | MAT_OXIDIZER | ...         */
    uint8_t  _pad[14];          /* pad to a clean 64; keeps the stride a power of two    */
} MaterialDef;

_Static_assert(sizeof(MaterialDef) == 64, "MaterialDef must be exactly 64 bytes");

/* ---- Starter material ids ------------------------------------------------
 * MAT_AIR MUST be 0 so a memset(chunk,0,16384) yields a valid all-air chunk
 * (air's temp code 0 decodes to -40 C; gen overwrites temperature at once). */
enum {
    MAT_AIR        = 0,
    MAT_STONE      = 1,
    MAT_DIRT       = 2,
    MAT_WATER      = 3,
    MAT_IRON_ORE   = 4,
    MAT_IRON       = 5,
    MAT_COPPER_ORE = 6,
    MAT_COPPER     = 7,
    MAT_SAND       = 8,
    MAT_WOOD       = 9,
    MAT_LAVA          = 10, /* molten rock: PHASE_LIQUID + MAT_EMISSIVE block-light source */
    MAT_MOLTEN_COPPER = 11, /* copper above 1085 C: PHASE_LIQUID, freezes_to MAT_COPPER    */
    MAT_MOLTEN_IRON   = 12, /* iron above 1538 C: PHASE_LIQUID, freezes_to MAT_IRON         */
    MAT_WATER_SOURCE  = 13  /* a held, inexhaustible WATER SPRING (MAT_SPRING); emits MAT_WATER */
    /* ... ~107 committed of 256, ~147 ids of headroom (Section 2.3 census) */
};

/* The one table; resident always, built once at startup from data. */
extern const MaterialDef g_materials[MAX_MATERIALS];

/* Accessor by material id. One indexed load; inlined. */
static inline const MaterialDef *material_get(uint8_t id) {
    return &g_materials[id];
}

/* Convenience: the MaterialDef a voxel points at. */
static inline const MaterialDef *material_of(Voxel v) {
    return &g_materials[vox_mat(v)];
}

#endif /* MATERIAL_H */
