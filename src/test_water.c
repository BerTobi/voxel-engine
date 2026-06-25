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

static int fails = 0;
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

int main(void)
{
    printf("== test_water (0.5 M3: binary-fill water CA) ==\n");
    test_settle_conserve();
    test_radial_any_face();
    test_spring();
    test_bounded_settle();
    test_heat_water_coexist();
    test_determinism();
    printf("=== %d failure(s) ===\n", fails);
    return fails;
}
