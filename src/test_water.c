/* test_water.c - 0.5 M3: the BINARY-FILL water CA (sim.c fluid_step + radial down).
 *
 * Verifies the model ported from the validated prototype (scratchpad/water_ca_proto.c):
 * water is fill {0,15}; a whole voxel falls along the chunk's radial-down face and
 * flows-to-descent laterally; it SETTLES to active==0 and CONSERVES (a spring is the
 * one inexhaustible exception). Also checks the radial down works for ALL six face
 * orientations (the sphere), determinism (sim_state_hash), and that water coexists
 * with the heat CA. GL-free; -DVOXEL_DETERMINISM_HARNESS (sim_state_hash). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "voxel.h"
#include "chunk.h"
#include "material.h"
#include "sim.h"
#include "worldgen.h"   /* WG_PLANET_C* constants only (no worldgen.c link dependency) */

static int fails = 0;

/* The SIM_NEIGH down face (0+X 1-X 2+Y 3-Y 4+Z 5-Z) most toward the planet centre for
 * a chunk - mirrors main.c's chunk_down_face, for the radial test below. */
static int down_face_toward(int cx, int cy, int cz)
{
    long dx = (long)cx * CHUNK_DIM + CHUNK_DIM / 2 - WG_PLANET_CX;
    long dy = (long)cy * CHUNK_DIM + CHUNK_DIM / 2 - WG_PLANET_CY;
    long dz = (long)cz * CHUNK_DIM + CHUNK_DIM / 2 - WG_PLANET_CZ;
    long ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, az = dz < 0 ? -dz : dz;
    if (ax >= ay && ax >= az) return dx > 0 ? 1 : 0;   /* toward centre on X */
    if (ay >= az)             return dy > 0 ? 3 : 2;   /* toward centre on Y */
    return dz > 0 ? 5 : 4;                              /* toward centre on Z */
}
#define CHECK(c,msg) do{ if(c) printf("PASS: %s\n",msg); else {printf("FAIL: %s\n",msg); fails++;} }while(0)

/* SIM_NEIGH face order (mirrors sim.c): 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z. */
static const int FACE_OFF[6][3] = {
    {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1}
};

static Voxel mk(uint8_t mat, uint8_t fill)
{ Voxel v = 0; vox_set_mat(&v, mat); vox_set_fill(&v, fill); vox_set_temp_code(&v, 60u); return v; }
static void back(Chunk *c)
{
    int i;
    Voxel air = mk(MAT_AIR, 0);   /* air at AMBIENT temp 60, exactly as worldgen seeds it */
    memset(c, 0, sizeof *c);
    c->voxels = (Voxel *)calloc(CHUNK_VOXELS, sizeof(Voxel));
    c->slab_idx = -1;
    /* Seed the whole chunk at ambient so there is NO spurious heat gradient (a
     * memset-0 chunk has air at temp code 0 = -40 C, which makes the heat CA churn
     * forever and the active set never drains - the water itself settles fine). */
    for (i = 0; i < CHUNK_VOXELS; ++i) c->voxels[i] = air;
}
static void put(Chunk *c, int x, int y, int z, Voxel v) { c->voxels[vox_index(x,y,z)] = v; }
static int  matat(Chunk *c, int x, int y, int z) { return vox_mat(c->voxels[vox_index(x,y,z)]); }
static long water_count(const Chunk *c)
{ long n=0; int i; for(i=0;i<CHUNK_VOXELS;i++) if(vox_mat(c->voxels[i])==MAT_WATER) n++; return n; }
static int run_to_settle(SimState *s, int cap) /* returns ticks to active==0, or -1 */
{ int t; for(t=0;t<cap;t++){ sim_tick(s); if(s->act.count==0) return t+1; } return -1; }

/* ---- (1) a pour falls, SETTLES to active==0, and CONSERVES ---- */
static void test_settle_conserve(void)
{
    static Chunk c; SimState s; long before; int settled;
    int x,y,z;
    back(&c);
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++) for (y=0;y<2;y++)
        put(&c,x,y,z, mk(MAT_STONE,15));                 /* floor y0..1 */
    for (z=6;z<10;z++) for (x=6;x<10;x++) for (y=8;y<12;y++)
        put(&c,x,y,z, mk(MAT_WATER,15));                 /* a 4x4x4 blob aloft */
    sim_build_conduct_lut(); sim_init(&s, &c);            /* down defaults to -Y (3) */
    before = water_count(&c);
    settled = run_to_settle(&s, 400);
    CHECK(settled > 0, "binary pour SETTLES to active==0");
    CHECK(water_count(&c) == before, "binary pour CONSERVES water (no create/destroy)");
    CHECK(matat(&c,8,2,8) == MAT_WATER, "the blob fell onto the floor (y=2)");
    sim_shutdown(&s); free(c.voxels);
}

/* ---- (2) radial down works for ALL six face orientations ---- */
static void test_radial_any_face(void)
{
    int face, ok = 1;
    for (face = 0; face < 6; ++face) {
        static Chunk c; SimState s;
        back(&c);
        put(&c, 8,8,8, mk(MAT_WATER,15));                 /* one voxel, open chunk */
        sim_build_conduct_lut(); sim_init(&s, &c);
        sim_set_down_face(&s, face);
        sim_tick(&s);                                     /* one step toward `down` */
        {
            int dx=8+FACE_OFF[face][0], dy=8+FACE_OFF[face][1], dz=8+FACE_OFF[face][2];
            if (matat(&c,dx,dy,dz) != MAT_WATER || matat(&c,8,8,8) != MAT_AIR)
                ok = 0;
        }
        sim_shutdown(&s); free(c.voxels);
    }
    CHECK(ok, "water falls toward the chunk's radial-down face for all 6 orientations");
}

/* ---- (3) a spring is inexhaustible; non-spring water conserves ---- */
static void test_spring(void)
{
    static Chunk c; SimState s; long w0, w1; int sli;
    int x,z;
    back(&c);
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++)
        put(&c,x,0,z, mk(MAT_STONE,15));                  /* floor y0 */
    sli = vox_index(8,13,8);
    put(&c, 8,13,8, mk(MAT_WATER,15));                    /* the spring cell */
    sim_build_conduct_lut(); sim_init(&s, &c);
    sim_set_spring(&s, (uint16_t)sli, 60u);               /* inexhaustible */
    w0 = water_count(&c);
    { int t; for (t=0;t<60;t++) sim_tick(&s); }
    w1 = water_count(&c);
    CHECK(matat(&c,8,13,8) == MAT_WATER, "spring cell never drains (inexhaustible)");
    CHECK(w1 > w0, "spring EMITS - water grows as it falls + accumulates");
    CHECK(matat(&c,8,1,8) == MAT_WATER, "spring water reached the floor below it");
    sim_shutdown(&s); free(c.voxels);
}

/* ---- (4) a larger release stays BOUNDED, settles, conserves (dam-break-ish) ---- */
static void test_bounded_settle(void)
{
    static Chunk c; SimState s; long before; int settled; uint16_t peak=0; int t;
    int x,y,z;
    back(&c);
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++)
        put(&c,x,0,z, mk(MAT_STONE,15));                  /* floor y0 */
    for (z=2;z<14;z++) for (x=2;x<14;x++) for (y=6;y<13;y++)
        put(&c,x,y,z, mk(MAT_WATER,15));                  /* a big perched block */
    sim_build_conduct_lut(); sim_init(&s, &c);
    before = water_count(&c);
    for (t=0;t<600;t++){ sim_tick(&s); if(s.act.count>peak) peak=s.act.count; if(s.act.count==0) break; }
    settled = (s.act.count == 0);
    CHECK(settled, "a large release settles to active==0");
    CHECK(water_count(&c) == before, "large release conserves water");
    CHECK(peak <= CHUNK_VOXELS, "active front stays bounded (<= chunk size, no blow-up)");
    sim_shutdown(&s); free(c.voxels);
}

/* ---- (4b) CROSS-CHUNK gravity (M4): water falls across a chunk seam + conserves.
 * Mirrors the engine: a SimXFlowFn reads the down-neighbour chunk + enqueues; the
 * test applies the deferred move atomically (materialise neighbour + revert source),
 * exactly as main.c's WorldCA does. Two chunks A (top) over B (bottom), linked. ---- */
typedef struct { int pending, src_li, face, n_li; } SeamCtx;
static int seam_xfn(void *user, const SimState *s, int src_li, int face, int nlx, int nly, int nlz)
{
    SeamCtx *cx = (SeamCtx *)user;
    Chunk *nc = s->chunk->neigh[face];
    if (nc == NULL) return 0;
    if (vox_mat(chunk_vox(nc, vox_index(nlx, nly, nlz))) != MAT_AIR) return 0;
    cx->pending = 1; cx->src_li = src_li; cx->face = face;
    cx->n_li = vox_index(nlx, nly, nlz);
    return 1;
}
static void test_cross_chunk_seam(void)
{
    static Chunk a, b; SimState sa, sb; SeamCtx ctx; long before; int crossed;
    back(&a); back(&b);
    /* Wire per the PRODUCTION Chunk neigh[]/Face convention (-X,+X,-Y,+Y,-Z,+Z), NOT
     * the SIM_NEIGH order: A's radial-down (-Y) neighbour is neigh[2]. The cross-flow
     * path converts SIM_NEIGH down (3) to the Face index (down^1 == 2) before indexing
     * neigh[], so B (below) must sit at a.neigh[2]; if the ^1 is ever dropped this test
     * fails (face 3 -> a.neigh[3] == NULL -> no cross). */
    a.neigh[2] = &b;   /* A's -Y (FACE_NEG_Y, radial-down) neighbour is B (below) */
    b.neigh[3] = &a;   /* B's +Y (FACE_POS_Y) neighbour is A (above)              */
    put(&a, 8, 0, 8, mk(MAT_WATER, 15));   /* one water voxel on A's bottom boundary */
    sim_build_conduct_lut();
    sim_init(&sa, &a); sim_init(&sb, &b);          /* both default down = -Y (face 3) */
    sa.fluid_xfn = seam_xfn; sa.fluid_xfn_user = &ctx;   /* enable cross-flow on A */
    sim_notify_edit(&sa, vox_index(8, 0, 8));   /* wake the boundary voxel (sim_init NULLed xfn,
                                                 * so it wasn't seeded active; the engine wakes
                                                 * such water as it flows in) */
    before = water_count(&a) + water_count(&b);
    ctx.pending = 0;
    sim_tick(&sa);                                  /* A's fluid pass enqueues the cross-move */
    crossed = 0;
    if (ctx.pending && vox_mat(b.voxels[ctx.n_li]) == MAT_AIR &&
        vox_mat(a.voxels[ctx.src_li]) == MAT_WATER) {
        b.voxels[ctx.n_li]  = mk(MAT_WATER, 15);    /* atomic move: materialise in B ... */
        a.voxels[ctx.src_li] = mk(MAT_AIR, 0);      /* ... revert A (conserves) */
        crossed = 1;
    }
    CHECK(crossed, "boundary water enqueues + crosses a chunk seam (gravity, down-face)");
    CHECK(vox_mat(b.voxels[vox_index(8, 15, 8)]) == MAT_WATER,
          "water landed in the neighbour chunk's top boundary cell");
    CHECK(vox_mat(a.voxels[vox_index(8, 0, 8)]) == MAT_AIR, "source cell vacated");
    CHECK(water_count(&a) + water_count(&b) == before, "cross-chunk move CONSERVES water");
    sim_shutdown(&sa); sim_shutdown(&sb); free(a.voxels); free(b.voxels);
}

/* ---- (4c) CROSS-CHUNK LATERAL flow-to-descent (M4b): on the round planet a valley
 * descends TANGENTIAL to a chunk's dominant down-face, so a river must cross a seam
 * SIDEWAYS, not just straight down. Two chunks A and its -X neighbour B whose floor is
 * one voxel LOWER; a water voxel on A's -X boundary CANNOT fall in-chunk (solid floor
 * under it, and every in-chunk lateral's own-down is solid too) but CAN step across the
 * seam into B, from where it descends. With no planet centre set the potential is the
 * down-axis (-Y) coordinate, so B's sub-seam cell is strictly lower. This is the exact
 * "fills one chunk, never crosses" bug: it FAILS (crossed==0) if the lateral seam-
 * descent path (fluid_seam_descent) is removed - only the down-face would cross. ---- */
static void test_cross_chunk_lateral(void)
{
    static Chunk a, b; SimState sa, sb; SeamCtx ctx; long before; int crossed; int x,y,z;
    back(&a); back(&b);
    a.neigh[0] = &b;   /* A's -X (FACE_NEG_X) neighbour is B; SIM -X (1) -> neigh[1^1]=neigh[0] */
    b.neigh[1] = &a;   /* B's +X (FACE_POS_X) neighbour is A                                    */
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++) for (y=0;y<=4;y++)
        put(&a,x,y,z, mk(MAT_STONE,15));       /* A floor solid up to y=4 (no in-chunk descent) */
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++) for (y=0;y<=3;y++)
        put(&b,x,y,z, mk(MAT_STONE,15));       /* B floor one LOWER: B's (15,4,8) is air+lower  */
    put(&a, 0, 5, 8, mk(MAT_WATER, 15));       /* water on A's -X boundary, resting on A's floor */
    sim_build_conduct_lut();
    sim_init(&sa, &a); sim_init(&sb, &b);      /* down = -Y (face 3); -X is a lateral face       */
    sa.fluid_xfn = seam_xfn; sa.fluid_xfn_user = &ctx;
    sim_notify_edit(&sa, vox_index(0, 5, 8));
    before = water_count(&a) + water_count(&b);
    ctx.pending = 0;
    sim_tick(&sa);                              /* A's fluid pass enqueues the LATERAL cross-move */
    crossed = 0;
    if (ctx.pending && vox_mat(b.voxels[ctx.n_li]) == MAT_AIR &&
        vox_mat(a.voxels[ctx.src_li]) == MAT_WATER) {
        b.voxels[ctx.n_li]   = mk(MAT_WATER, 15);
        a.voxels[ctx.src_li] = mk(MAT_AIR, 0);
        crossed = 1;
    }
    CHECK(crossed, "M4b: boundary water crosses a seam SIDEWAYS (lateral flow-to-descent)");
    CHECK(vox_mat(b.voxels[vox_index(15,5,8)]) == MAT_WATER,
          "M4b: water landed in the -X neighbour at the seam");
    CHECK(vox_mat(a.voxels[vox_index(0,5,8)]) == MAT_AIR, "M4b: lateral source cell vacated");
    CHECK(water_count(&a) + water_count(&b) == before, "M4b: lateral cross-chunk move CONSERVES water");
    sim_shutdown(&sa); sim_shutdown(&sb); free(a.voxels); free(b.voxels);
}

/* ---- (5) water + heat coexist (lava stays hot while water flows) ---- */
static void test_heat_water_coexist(void)
{
    static Chunk c; SimState s; int t;
    int x,z;
    back(&c);
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++)
        put(&c,x,0,z, mk(MAT_STONE,15));
    put(&c, 2,1,2, mk(MAT_LAVA,15));                      /* a heat source (auto-held) */
    for (z=10;z<13;z++) for (x=10;x<13;x++)
        put(&c,x,10,z, mk(MAT_WATER,15));                 /* water far from the lava */
    sim_build_conduct_lut(); sim_init(&s, &c);
    for (t=0;t<200;t++) sim_tick(&s);
    CHECK(matat(&c,2,1,2) == MAT_LAVA, "lava heat source persists alongside flowing water");
    CHECK(matat(&c,11,1,11) == MAT_WATER, "water still flowed to the floor with heat active");
    sim_shutdown(&s); free(c.voxels);
}

/* ---- (6) determinism: two identical water worlds hash-equal after N ticks ---- */
static void test_determinism(void)
{
    static Chunk ca, cb; SimState sa, sb; int n; int x,y,z;
    back(&ca); back(&cb);
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++) for (y=0;y<2;y++) {
        put(&ca,x,y,z, mk(MAT_STONE,15)); put(&cb,x,y,z, mk(MAT_STONE,15));
    }
    for (z=5;z<11;z++) for (x=5;x<11;x++) for (y=8;y<12;y++) {
        put(&ca,x,y,z, mk(MAT_WATER,15)); put(&cb,x,y,z, mk(MAT_WATER,15));
    }
    sim_build_conduct_lut(); sim_init(&sa,&ca); sim_init(&sb,&cb);
    for (n=0;n<120;n++){ sim_tick(&sa); sim_tick(&sb); }
    CHECK(sim_state_hash(&sa) == sim_state_hash(&sb),
          "two identical binary-water worlds hash-equal after 120 ticks (deterministic)");
    sim_shutdown(&sa); sim_shutdown(&sb); free(ca.voxels); free(cb.voxels);
}

/* ---- (7) M4 RADIAL finisher LEVELS a U-TUBE. Two vertical shafts joined ONLY by a
 * bottom channel; all water seeded in shaft A. The local gravity+flow rule alone can
 * never raise water UP the empty arm B (it only moves DOWNHILL / to lower potential),
 * so water reaching arm B PROVES the finisher ran - this test FAILS if the leveller is
 * stubbed out. Asserts both arms reach a flat (within-1) level, conserve, and settle.
 * No planet centre => -Y axis pot (pot == ly). ---- */
static void test_finisher_levels_utube(void)
{
    static Chunk c; SimState s; int x, y, z, t; long before, after; int settled = 0;
    int topA = -1, topB = -1;
    back(&c);
    for (z = 0; z < CHUNK_DIM; z++) for (y = 0; y < CHUNK_DIM; y++) for (x = 0; x < CHUNK_DIM; x++)
        put(&c, x,y,z, mk(MAT_STONE,15));
    for (y = 1; y <= 14; y++) { put(&c, 4,y,8, mk(MAT_AIR,0)); put(&c, 11,y,8, mk(MAT_AIR,0)); }
    for (x = 4; x <= 11; x++) put(&c, x,1,8, mk(MAT_AIR,0));        /* bottom channel joins the arms */
    for (y = 2; y <= 14; y++) put(&c, 4,y,8, mk(MAT_WATER,15));     /* seed ALL water in arm A */
    sim_build_conduct_lut(); sim_init(&s, &c);
    sim_set_down_face(&s, 3);                    /* -Y down: finisher ON, axis pot */
    before = water_count(&c);
    for (t = 0; t < 2000; t++) { sim_tick(&s); if (s.act.count == 0) { settled = 1; break; } }
    after = water_count(&c);
    for (y = 14; y >= 1; y--) {
        if (topA < 0 && matat(&c, 4, y, 8) == MAT_WATER) topA = y;
        if (topB < 0 && matat(&c, 11, y, 8) == MAT_WATER) topB = y;
    }
    CHECK(settled, "M4 U-tube: equalised state SETTLES to active==0");
    CHECK(after == before, "M4 U-tube: leveling CONSERVES water exactly");
    CHECK(topB > 0, "M4 U-tube: water ROSE up the empty arm (proves the finisher ran)");
    CHECK(topA > 0 && topA - topB <= 1 && topB - topA <= 1,
          "M4 U-tube: both arms level to within 1 cell");
    sim_shutdown(&s); free(c.voxels);
}

/* ---- (7b) M4 NO-CHURN on a DIAGONAL chunk. The regression test for the integration
 * bug an adversarial review caught: with the radial leveller but an axis-based local
 * rule, a water body on an OFF-AXIS chunk (where the radial shell cuts diagonally
 * through the voxel grid - most of the planet) never settled, churning every tick
 * forever (leveller vs local rule fighting). With both keyed off sim_cell_pot it must
 * SETTLE to active==0 and hold a byte-stable fill state. Uses the REAL planet centre
 * and a 45-degree chunk - the worst diagonal. ---- */
static void test_no_churn_diagonal(void)
{
    static Chunk c; SimState s; int x, y, z, t, settled_tick = -1;
    unsigned long long lasth = 0; int stable = 0, maxstable = 0;
    back(&c);
    c.cx = 24; c.cy = 56; c.cz = 24;             /* off all three axes: diagonal radial shell */
    for (z = 0; z < CHUNK_DIM; z++) for (y = 0; y < CHUNK_DIM; y++) for (x = 0; x < CHUNK_DIM; x++)
        put(&c, x,y,z, mk(MAT_STONE,15));
    for (z = 1; z <= 14; z++) for (y = 1; y <= 14; y++) for (x = 1; x <= 14; x++) put(&c, x,y,z, mk(MAT_AIR,0));
    for (y = 1; y <= 12; y++) for (x = 1; x <= 5; x++) for (z = 1; z <= 14; z++) put(&c, x,y,z, mk(MAT_WATER,15));
    sim_set_planet_center(WG_PLANET_CX, WG_PLANET_CY, WG_PLANET_CZ);
    sim_build_conduct_lut(); sim_init(&s, &c);
    sim_set_down_face(&s, down_face_toward(c.cx, c.cy, c.cz));
    for (t = 0; t < 3000; t++) {
        unsigned long long h;
        sim_tick(&s);
        if (s.act.count == 0 && settled_tick < 0) settled_tick = t + 1;
        h = sim_state_hash(&s);
        if (h == lasth) { if (++stable > maxstable) maxstable = stable; } else { stable = 0; lasth = h; }
    }
    CHECK(settled_tick > 0, "M4 diagonal: off-axis water body SETTLES to active==0 (no churn)");
    CHECK(maxstable > 1000, "M4 diagonal: settled state is byte-STABLE (does not churn each tick)");
    sim_shutdown(&s); free(c.voxels);
}

/* ---- (8) M4 spring fill-and-spill: a high spring FILLS a basin bottom-up,
 * LEVELS it, and never fills the open sky ABOVE its own shell. Exercises the
 * RADIAL branch of sim_cell_pot (planet centre set far below => d2 gradient ~ -Y
 * but via the real radial formula) + the spring body-donation. Determinism too. ---- */
static long fill_in_yrange(const Chunk *c, int ylo, int yhi)
{
    long n = 0; int x, y, z;
    for (z = 0; z < CHUNK_DIM; z++)
        for (y = ylo; y <= yhi; y++)
            for (x = 0; x < CHUNK_DIM; x++)
                if (vox_mat(c->voxels[vox_index(x, y, z)]) == MAT_WATER) n++;
    return n;
}
static void build_cavity(Chunk *c, int *sli)
{
    int x, y, z;
    back(c);
    /* solid block with a small open shaft (x,z in 6..10, y in 1..13) */
    for (z = 0; z < CHUNK_DIM; z++) for (y = 0; y < CHUNK_DIM; y++) for (x = 0; x < CHUNK_DIM; x++)
        put(c, x,y,z, mk(MAT_STONE, 15));
    for (z = 6; z <= 10; z++) for (y = 1; y <= 13; y++) for (x = 6; x <= 10; x++)
        put(c, x,y,z, mk(MAT_AIR, 0));
    *sli = vox_index(8, 12, 8);
    /* The spring is a MAT_WATER_SOURCE voxel (id 13) - EXACTLY as the engine places it
     * (main.c make_liquid(MAT_WATER_SOURCE)), so this exercises the real donate path:
     * a spring must emit plain MAT_WATER, never copy its own MAT_WATER_SOURCE id. */
    put(c, 8, 12, 8, mk(MAT_WATER_SOURCE, 15));  /* the spring, high in the shaft */
}
static long count_mat(const Chunk *c, uint8_t m)
{ long n = 0; int i; for (i = 0; i < CHUNK_VOXELS; ++i) if (vox_mat(c->voxels[i]) == m) n++; return n; }
static unsigned long long run_spring_fill(Chunk *c, int sli, int ticks)
{
    SimState s;
    sim_set_planet_center(8, -2000, 8);          /* far below: radial pot, ~-Y down */
    sim_build_conduct_lut(); sim_init(&s, c);
    sim_set_down_face(&s, 3);                     /* finisher ON */
    sim_set_spring(&s, (uint16_t)sli, 60u);
    { int t; for (t = 0; t < ticks; t++) sim_tick(&s); }
    { unsigned long long h = sim_state_hash(&s); sim_shutdown(&s); return h; }
}
static void test_spring_fill_and_spill(void)
{
    static Chunk c, c2; int sli, sli2; long below, above, springs; unsigned long long h1, h2;
    build_cavity(&c, &sli);
    (void)run_spring_fill(&c, sli, 4000);
    below   = fill_in_yrange(&c, 1, 11);             /* the shaft below the spring's y=12 shell */
    above   = fill_in_yrange(&c, 13, CHUNK_DIM - 1); /* the open sky above the spring           */
    springs = count_mat(&c, MAT_WATER_SOURCE);       /* must stay exactly ONE (the spring)      */
    CHECK(below > 150, "M4 spring: donation FILLED the basin below the spring bottom-up");
    CHECK(above == 0,  "M4 spring: water never fills the SKY above the spring's shell");
    CHECK(springs == 1, "M4 spring: donated cells are plain WATER, not a field of SPRINGS");
    free(c.voxels);
    /* determinism: two FRESH identical worlds reach a byte-identical state. */
    build_cavity(&c, &sli);
    build_cavity(&c2, &sli2);
    h1 = run_spring_fill(&c, sli, 1200);
    h2 = run_spring_fill(&c2, sli2, 1200);
    CHECK(h1 == h2, "M4 spring: two identical spring worlds hash-equal (deterministic)");
    free(c.voxels); free(c2.voxels);
}

int main(void)
{
    printf("== test_water (0.5 M3: binary-fill water CA) ==\n");
    test_settle_conserve();
    test_radial_any_face();
    test_spring();
    test_bounded_settle();
    test_cross_chunk_seam();
    test_cross_chunk_lateral();      /* M4b: water crosses a seam SIDEWAYS (axis pot; before centre) */
    test_heat_water_coexist();
    test_determinism();
    /* ORDER MATTERS: sim_set_planet_center sets a GLOBAL (g_pc_set) that has no reset.
     * Every test ABOVE leaves it unset (axis-gravity fallback); the two M4 tests that
     * set a centre run LAST, and the one M4 test relying on the axis fallback
     * (test_finisher_levels_utube) runs BEFORE either sets it. Keep this order. */
    test_finisher_levels_utube();    /* M4: leveling requires the finisher (U-tube); axis pot */
    test_no_churn_diagonal();        /* M4: off-axis chunk must SETTLE, not churn (sets centre) */
    test_spring_fill_and_spill();    /* M4: spring donation + radial pot (sets centre LAST) */
    printf("=== %d failure(s) ===\n", fails);
    return fails;
}
