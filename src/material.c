/* material.c - The global MaterialDef table, built once from data.
 *
 * Binding source: ARCHITECTURE.md Section 2.3. This file is the table's *own
 * construction* - the single sanctioned place where a material's behaviour is
 * spelled out literally. Everywhere else in the engine, behaviour is answered
 * by indexing g_materials[id]; a switch on the id anywhere outside this file is
 * a design violation (Section 2.3 / the hardcoded-recipe pattern the game
 * exists to avoid).
 *
 * The table is 256 entries x 64 bytes = exactly 16 KiB, resident always. It is
 * `const` and statically initialised, so it lives in .rodata, costs no startup
 * work, and is never reallocated.
 *
 * Pure C, no GL/OS dependency: this compiles standalone with
 *   gcc -fsyntax-only -std=c99 -Wall -Isrc src/material.c
 *
 * Physical values are realistic-ish (SI units as documented in material.h), not
 * laboratory-exact: the 8-bit temperature quantum (Section 2.2) and the
 * fixed-point CA already coarsen them. One unit note: thermal_conductivity is
 * the "scaled" uint16_t field from material.h, and the scale chosen here is
 * centi-W/(m*K) (i.e. W/(m*K) * 100). mW/(m*K) - the literal reading of the
 * header comment - would overflow the 16-bit field for copper (401 W/(m*K) ->
 * 401000), so cW/(m*K) is the largest round scale that keeps copper (40100) in
 * range while preserving the binding ordering air < wood < stone < metals that
 * makes a stone cavity work as a furnace (Section 2.3). Temperature anchors that the heat sim
 * pins against come from the Decision Ledger: iron melt ~1538 C, copper melt
 * ~1085 C. Each material gets a distinct color_rgba so the placeholder atlas
 * (a flat-filled tile per id) stays legible while a real atlas is authored.
 *
 * Conventions used in the initialisers below:
 *   - melt_point_c / boil_point_c / ignition_c == -1 means "no such transition"
 *     (stone does not melt in our -40..2020 C range; iron does not burn).
 *   - *_to id fields point at the sibling material for that transition. Where a
 *     starter material's product is not yet a committed id (e.g. water's steam,
 *     molten metals, ash), the field points at MAT_AIR (0) as the neutral
 *     "unassigned / vanishes" target. These are wired up as those ids land;
 *     leaving them 0 is harmless because the matching transition is gated by a
 *     valid melt/boil/ignition threshold, several of which are -1 here.
 *   - color_rgba alpha: 0 for AIR (fully transparent, never meshed), 255 for
 *     opaque solids/powders, ~200 for water (the liquid pass reads this).
 *   - Unused-by-this-material fields are left 0 via the designated initialiser,
 *     which is the correct neutral for every one of them (fill held at 15 for
 *     solids is a voxel-state convention, not a MaterialDef field).
 */

#include "material.h"

/* Designated initialisers keep each row readable and self-documenting, and let
 * the many zero fields (reserved electrical conductivity, unused latent heats,
 * phase-target ids that don't apply) default cleanly to 0. Any id not named
 * below is an all-zero entry: name "", phase GAS, non-opaque - i.e. it behaves
 * like a second air until a real material is appended. That is the intended
 * "empty slot" state for the ~246 ids of headroom (Section 2.3 census). */
const MaterialDef g_materials[MAX_MATERIALS] = {

    /* ---- 0: AIR - the empty/transparent material, never meshed -----------
     * MUST be id 0 so memset(chunk,0,16384) yields a valid all-air chunk
     * (material.h / Section 2.1). GAS phase, not opaque, alpha 0. */
    [MAT_AIR] = {
        .name                = "air",
        .atlas_tile          = 0,
        .phase               = PHASE_GAS,
        .color_rgba          = { 0, 0, 0, 0 },     /* fully transparent */
        .density_kg_m3       = 1,                  /* ~1.2 kg/m^3, rounded */
        .specific_heat       = 1005,               /* J/(kg*K), dry air */
        .thermal_conductivity= 3,                  /* cW/(m*K): ~0.026 W/(m*K) -> 2.6, rounded */
        .melt_point_c        = -1,
        .boil_point_c        = -1,
        .ignition_c          = -1,
        /* all *_to ids 0, flammability 0, hardness 0: air is inert */
        .flags               = MAT_OXIDIZER,       /* feeds combustion (the O2 proxy) */
    },

    /* ---- 1: STONE - the bulk solid; insulating furnace wall --------------
     * Does not melt in our range (-1). Mines to itself for now (no cobble id
     * yet). Stone's low conductivity is *why* a stone cavity works as a
     * furnace (Section 2.3). */
    [MAT_STONE] = {
        .name                = "stone",
        .atlas_tile          = MAT_STONE,
        .phase               = PHASE_SOLID,
        .color_rgba          = { 128, 128, 128, 255 },   /* mid grey */
        .density_kg_m3       = 2700,
        .specific_heat       = 840,
        .thermal_conductivity= 250,                /* cW/(m*K): ~2.5 W/(m*K) */
        .melt_point_c        = -1,                 /* does not melt in range */
        .boil_point_c        = -1,
        .latent_fusion       = 0,
        .viscosity           = 0,
        .rest_angle          = 0,
        .hardness            = 150,
        .breaks_to           = MAT_STONE,          /* -> cobble id when it lands */
        .ignition_c          = -1,
        .flags               = MAT_OPAQUE,
    },

    /* ---- 2: DIRT - soft cover solid; falls under structural rules later -- */
    [MAT_DIRT] = {
        .name                = "dirt",
        .atlas_tile          = MAT_DIRT,
        .phase               = PHASE_SOLID,
        .color_rgba          = { 134, 96, 67, 255 },     /* earthy brown */
        .density_kg_m3       = 1500,
        .specific_heat       = 800,
        .thermal_conductivity= 150,                /* cW/(m*K): ~1.5 W/(m*K) */
        .melt_point_c        = -1,
        .boil_point_c        = -1,
        .hardness            = 40,
        .breaks_to           = MAT_DIRT,
        .ignition_c          = -1,
        .flags               = MAT_OPAQUE,
    },

    /* ---- 3: WATER - the reference liquid and primary coolant -------------
     * LIQUID phase: viscosity 0 = equalises fast (Section 2.3). Boils at
     * 100 C; boils_to/condenses_to point at AIR until a steam id is added
     * (the boil transition still fires correctly off boil_point_c). Water's
     * huge specific heat is why the player reaches for it as a coolant. */
    [MAT_WATER] = {
        .name                = "water",
        .atlas_tile          = MAT_WATER,
        .phase               = PHASE_LIQUID,
        .color_rgba          = { 40, 90, 200, 200 },     /* translucent blue */
        .density_kg_m3       = 1000,
        .specific_heat       = 4186,               /* the coolant number */
        .thermal_conductivity= 60,                 /* cW/(m*K): ~0.6 W/(m*K) */
        .melt_point_c        = 0,                  /* freezes at 0 C */
        .boil_point_c        = 100,
        .latent_fusion       = 334,                /* kJ/kg */
        .latent_vapor        = 2257,               /* kJ/kg */
        .freezes_to          = MAT_AIR,            /* -> ice id when it lands */
        .boils_to            = MAT_AIR,            /* -> steam id when it lands */
        .condenses_to        = MAT_WATER,
        .viscosity           = 0,                  /* spreads instantly */
        .ignition_c          = -1,
        /* not opaque: water is alpha-blended by the liquid pass */
    },

    /* ---- 4: IRON_ORE - smeltable solid; refines to iron -----------------
     * Treated as a hard rock until smelted in code; it does not "melt" into a
     * pure phase on its own (slag chemistry is out of starter scope), so
     * melt_point_c is -1 and the ore->metal step is a reaction, not a melt. */
    [MAT_IRON_ORE] = {
        .name                = "iron_ore",
        .atlas_tile          = MAT_IRON_ORE,
        .phase               = PHASE_SOLID,
        .color_rgba          = { 120, 100, 90, 255 },    /* grey-brown rock w/ rusty cast */
        .density_kg_m3       = 5000,
        .specific_heat       = 700,
        .thermal_conductivity= 100,                /* cW/(m*K): ~1.0 W/(m*K) */
        .melt_point_c        = -1,
        .boil_point_c        = -1,
        .hardness            = 200,
        .breaks_to           = MAT_IRON_ORE,       /* -> crushed-ore dust id later */
        .ignition_c          = -1,
        .flags               = MAT_OPAQUE,
    },

    /* ---- 5: IRON - refined metal. Binding anchor: melt 1538 C -----------
     * (code 231, decodes 1540 C; Section 2.2). melts_to points at AIR until
     * molten_iron lands; the latent_fusion plateau is what makes a crucible
     * take time (Section 2.3). */
    [MAT_IRON] = {
        .name                = "iron",
        .atlas_tile          = MAT_IRON,
        .phase               = PHASE_SOLID,
        .color_rgba          = { 180, 182, 188, 255 },   /* steel grey */
        .density_kg_m3       = 7870,
        .specific_heat       = 449,                /* the Fe anchor from material.h */
        .thermal_conductivity= 8000,               /* cW/(m*K): ~80 W/(m*K), carries forge heat */
        .melt_point_c        = 1538,               /* Ledger anchor -> code 231 */
        .boil_point_c        = 2020,               /* true ~2862 C; capped at the 2020 C codec ceiling */
        .latent_fusion       = 247,                /* kJ/kg */
        .latent_vapor        = 6090,
        .melts_to            = MAT_MOLTEN_IRON,    /* the freshly-melted liquid phase */
        .freezes_to          = MAT_IRON,
        .viscosity           = 0,
        .hardness            = 220,
        .breaks_to           = MAT_IRON,
        .ignition_c          = -1,
        .conductivity        = 180,                /* electrical, deferred tier */
        .flags               = MAT_OPAQUE,
    },

    /* ---- 6: COPPER_ORE - smeltable solid; refines to copper -------------- */
    [MAT_COPPER_ORE] = {
        .name                = "copper_ore",
        .atlas_tile          = MAT_COPPER_ORE,
        .phase               = PHASE_SOLID,
        .color_rgba          = { 90, 130, 110, 255 },    /* greenish (malachite cast) */
        .density_kg_m3       = 4500,
        .specific_heat       = 600,
        .thermal_conductivity= 120,                /* cW/(m*K): ~1.2 W/(m*K) */
        .melt_point_c        = -1,
        .boil_point_c        = -1,
        .hardness            = 180,
        .breaks_to           = MAT_COPPER_ORE,
        .ignition_c          = -1,
        .flags               = MAT_OPAQUE,
    },

    /* ---- 7: COPPER - refined metal. Binding anchor: melt 1085 C ----------
     * (code 208, decodes 1080 C; Section 2.2). Highest thermal conductivity
     * in the starter set - the contrast against stone is the furnace story. */
    [MAT_COPPER] = {
        .name                = "copper",
        .atlas_tile          = MAT_COPPER,
        .phase               = PHASE_SOLID,
        .color_rgba          = { 184, 115, 51, 255 },    /* copper orange */
        .density_kg_m3       = 8960,
        .specific_heat       = 385,
        .thermal_conductivity= 40100,              /* cW/(m*K): ~401 W/(m*K), Cu is the high end */
        .melt_point_c        = 1085,               /* Ledger anchor -> code 208 */
        .boil_point_c        = 2020,               /* true 2562 C; capped at the 2020 C codec ceiling */
        .latent_fusion       = 209,
        .latent_vapor        = 4730,
        .melts_to            = MAT_MOLTEN_COPPER,  /* the freshly-melted liquid phase  */
        .freezes_to          = MAT_COPPER,
        .viscosity           = 0,
        .hardness            = 120,
        .breaks_to           = MAT_COPPER,
        .ignition_c          = -1,
        .conductivity        = 255,                /* the conductivity reference (Section 2.3) */
        .flags               = MAT_OPAQUE,
    },

    /* ---- 8: SAND - the reference powder; falls and piles ----------------
     * POWDER phase: rest_angle is the angle-of-repose proxy (Section 2.3).
     * fill is ignored for powders (held at 15 in voxel state). */
    [MAT_SAND] = {
        .name                = "sand",
        .atlas_tile          = MAT_SAND,
        .phase               = PHASE_POWDER,
        .color_rgba          = { 219, 203, 142, 255 },   /* pale sand */
        .density_kg_m3       = 1600,
        .specific_heat       = 830,
        .thermal_conductivity= 30,                 /* cW/(m*K): ~0.3 W/(m*K) */
        .melt_point_c        = 1700,               /* silica softens ~1700 C -> glass later */
        .boil_point_c        = -1,
        .latent_fusion       = 50,
        .melts_to            = MAT_AIR,            /* -> molten_glass id when it lands */
        .viscosity           = 0,
        .rest_angle          = 35,                 /* steep, mounds well */
        .hardness            = 30,
        .breaks_to           = MAT_SAND,
        .ignition_c          = -1,
        .flags               = MAT_OPAQUE,
    },

    /* ---- 9: WOOD - structural organic; burns before it melts ------------
     * melt_point_c -1, ignition_c set: wood combusts rather than melting
     * (Section 2.3). burns_to -> AIR until a charcoal/ash id lands; the fire
     * CA is gated by ignition_c + flammability, not by burns_to being valid. */
    [MAT_WOOD] = {
        .name                = "wood",
        .atlas_tile          = MAT_WOOD,
        .phase               = PHASE_SOLID,
        .color_rgba          = { 120, 81, 45, 255 },     /* warm timber brown */
        .density_kg_m3       = 650,
        .specific_heat       = 1700,
        .thermal_conductivity= 15,                 /* cW/(m*K): ~0.15 W/(m*K), insulator */
        .melt_point_c        = -1,                 /* burns, never melts */
        .boil_point_c        = -1,
        .hardness            = 60,
        .breaks_to           = MAT_WOOD,
        .ignition_c          = 300,                /* auto-ignites ~300 C */
        .burns_to            = MAT_AIR,            /* -> charcoal/ash id when it lands */
        .flammability        = 200,                /* spreads readily */
        .flags               = MAT_OPAQUE,
    },

    /* ---- 10: LAVA - molten rock; the block-light source ------------------
     * LIQUID phase like water, but the inverse hydraulics: high viscosity so it
     * barely creeps (Section 2.3 names lava as the viscosity high end, ~255),
     * and the only starter material carrying MAT_EMISSIVE - it injects block
     * light so the light pass has something to flood from (light.c step 3). Hot
     * by construction: it sits well above its own freeze point, cooling/freezing
     * to stone (freezes_to MAT_STONE) once the heat sim drains it. boils_to AIR
     * until a rock-vapor id lands; the boil gate fires off boil_point_c. Glowing
     * orange so the placeholder atlas reads as a light source even unlit. */
    [MAT_LAVA] = {
        .name                = "lava",
        .atlas_tile          = MAT_LAVA,
        .phase               = PHASE_LIQUID,
        .color_rgba          = { 255, 102, 0, 255 },     /* glowing molten orange */
        .density_kg_m3       = 2700,               /* molten basalt, ~stone density */
        .specific_heat       = 1450,               /* J/(kg*K), silicate melt */
        .thermal_conductivity= 130,                /* cW/(m*K): ~1.3 W/(m*K) */
        .melt_point_c        = 1200,               /* solidus of basaltic rock        */
        .boil_point_c        = 2020,               /* capped at the 2020 C codec ceiling */
        .latent_fusion       = 400,                /* kJ/kg, the cool-to-stone plateau */
        .freezes_to          = MAT_STONE,          /* chills back into solid rock     */
        .boils_to            = MAT_AIR,            /* -> rock-vapor id when it lands  */
        .viscosity           = 250,                /* barely creeps (Section 2.3 high end) */
        .ignition_c          = -1,                 /* already molten; does not ignite */
        /* not opaque: liquid pass alpha-blends it, matching water's convention.
         * MAT_EMISSIVE is the load-bearing flag here - light.c seeds blocklight
         * from every MAT_EMISSIVE voxel. */
        .flags               = MAT_EMISSIVE,
    },

    /* ---- 11: MOLTEN_COPPER - copper past its 1085 C melt point -----------
     * The emergent-demo product (Section 3.5): solid copper touching the held
     * lava block heats past 1085 C, banks its latent_fusion, then flips to this
     * IN PLACE - no scripted smelt, pure physics. PHASE_LIQUID, freezes_to back
     * to solid copper once it cools below 1085 C (its solidus == its melt point,
     * which the freeze branch reads as the threshold). specific_heat matches
     * solid copper (385) so the sensible thermal mass is continuous across the
     * change and the energy accounting stays clean; latent_fusion matches solid
     * copper (209) so freezing releases exactly what melting absorbed. Glowing
     * orange, distinct from solid copper {184,115,51} and lava {255,102,0} so
     * the molten pool reads as its own hot liquid. NOT MAT_OPAQUE - matches the
     * water/lava liquid convention; the mesher still meshes it (is_air gates on
     * material==AIR, not opacity) so it is visible this milestone. Fluid FLOW is
     * DEFERRED, so viscosity is stored but not yet read - molten copper stays in
     * place. Conductivity ~165 W/(m*K) is lower than solid copper's 401 (molten
     * metal conducts worse) and stays well below the solid-copper ceiling, so it
     * cannot threaten the 1/6 FTCS stability normalization keyed to 40100. */
    [MAT_MOLTEN_COPPER] = {
        .name                = "molten_copper",
        .atlas_tile          = MAT_MOLTEN_COPPER,
        .phase               = PHASE_LIQUID,
        .color_rgba          = { 255, 140, 40, 255 },    /* glowing orange-copper liquid */
        .density_kg_m3       = 8960,
        .specific_heat       = 385,                /* same as solid Cu: continuous thermal mass */
        .thermal_conductivity= 16500,              /* cW/(m*K): ~165 W/(m*K), below solid Cu's 40100 */
        .melt_point_c        = 1085,               /* solidus == freeze point; the freeze branch reads this */
        .boil_point_c        = 2020,               /* true 2562 C; capped at the 2020 C codec ceiling */
        .latent_fusion       = 209,                /* same as solid Cu: freezing releases what melting absorbed */
        .latent_vapor        = 4730,
        .melts_to            = MAT_MOLTEN_COPPER,  /* self: already molten, the melt guard (melts_to != self) disables re-melt */
        .freezes_to          = MAT_COPPER,         /* re-solidifies to solid copper when it cools below 1085 C */
        .viscosity           = 200,                /* oozes; fluid flow DEFERRED so this is only stored */
        .hardness            = 0,
        .ignition_c          = -1,
        /* not opaque: liquid convention (water/lava). flags 0: not emissive -
         * its glow comes from temperature via sim_temp_glow, not a light source. */
        .flags               = 0,
    },

    /* ---- 12: MOLTEN_IRON - iron past its 1538 C melt point ---------------
     * Added for symmetry / to prove the rule is data-driven, not copper-special;
     * NOT exercised by the demo. Iron melts at 1538 C, above the 1150 C lava hold
     * (code 212 -> 1160 C), so iron next to lava equilibrates below its melt
     * point and never melts. Mirrors molten_copper: PHASE_LIQUID, freezes_to back
     * to solid iron, specific_heat (449) and latent_fusion (247) continuous with
     * solid iron for clean energy accounting, glowing yellow-white, conductivity
     * lower than solid iron's 8000. */
    [MAT_MOLTEN_IRON] = {
        .name                = "molten_iron",
        .atlas_tile          = MAT_MOLTEN_IRON,
        .phase               = PHASE_LIQUID,
        .color_rgba          = { 255, 220, 130, 255 },   /* glowing yellow-white liquid */
        .density_kg_m3       = 7870,
        .specific_heat       = 449,                /* same as solid Fe: continuous thermal mass */
        .thermal_conductivity= 3500,               /* cW/(m*K): ~35 W/(m*K), below solid Fe's 8000 */
        .melt_point_c        = 1538,               /* solidus == freeze point */
        .boil_point_c        = 2020,               /* capped at the 2020 C codec ceiling */
        .latent_fusion       = 247,                /* same as solid Fe */
        .latent_vapor        = 6090,
        .melts_to            = MAT_MOLTEN_IRON,    /* self: disables re-melt */
        .freezes_to          = MAT_IRON,           /* re-solidifies to solid iron */
        .viscosity           = 230,                /* oozes; fluid flow DEFERRED */
        .hardness            = 0,
        .ignition_c          = -1,
        .flags               = 0,                  /* not opaque, not emissive */
    },
};
