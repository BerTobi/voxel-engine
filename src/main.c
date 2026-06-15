/* main.c - The engine entry point and the frame loop (ARCHITECTURE.md
 * Section 6), now driving the STREAMING world (Section 7 + the WorldStore of
 * Section 1.5/2.5). This is the bring-up driver, not the shipping Engine struct
 * of Section 1.5: it stands up a window + GL context, creates the WorldStore,
 * primes a player-centred window of PROCEDURAL chunks, and then runs the
 * cooperative single-core frame loop at the binding 30 FPS / 33.33 ms - calling
 * the budgeted streaming update around the camera each frame so chunks load on
 * the leading edge and evict on the trailing edge as the player roams.
 *
 * What this file is and is NOT:
 *   - IS:  plat_create_window -> gl_load -> render_init -> world_init (slab pool
 *          + residency hash) -> world_prime (synchronous first window) -> the
 *          Section 6 loop (measure clock, poll input, fly the camera, stream the
 *          window around the camera under a per-frame budget, tick the heat sim
 *          on the resident HOME chunk, drain the dirty remesh, build MVP, draw
 *          every LIVE resident render slot, swap, cap ~30 FPS, exit on
 *          close/ESC). Meshing is NEIGHBOUR-AWARE and stays seamless as the
 *          window slides: the WorldStore wires each chunk's neigh[6] on
 *          insert/evict and re-meshes boundary chunks whose seam faces changed,
 *          all under the per-frame remesh budget, so no internal seam walls
 *          appear at the moving window edge.
 *   - NOW (this milestone, ARCHITECTURE Section 8 World Persistence): an evicted
 *          chunk flagged CHUNK_MODIFIED is WRITTEN BACK to disk before its slab
 *          returns to the pool, and re-entry LOADS it from disk in preference to
 *          regenerating; an UNMODIFIED chunk still just returns its slab and is
 *          REGENERATED FROM SEED on re-entry (worldgen is deterministic, so the
 *          regenerated chunk is byte-identical). This driver opens a PersistStore
 *          (save dir from VOXEL_SAVE, else saves/<seed>), injects it into the
 *          WorldStore so streaming load/evict/shutdown route through disk, and
 *          flags the demo HOME chunk CHUNK_MODIFIED so the EVOLVED home (decorated
 *          + sim-settled) persists across eviction AND a process restart: run once
 *          to evolve+save HOME, run again to LOAD it. world_shutdown flushes every
 *          resident modified chunk; persist_close then closes the region handles.
 *   - NOT: the heat/fluid CA is still single-chunk (one SimState bound to a fixed
 *          HOME world chunk); cross-chunk diffusion / cross-chunk light bleed /
 *          cross-chunk fluid flow remain deferred follow-ups.
 *
 * The WorldStore owns chunk storage, residency, neigh[6] wiring, render-slot
 * assignment and the per-frame gen/remesh budget. To keep world.c GL-free and
 * unit-testable, the renderer steps are passed to it as function-pointer
 * callbacks (gen / mesh-upload / slot-free); this file supplies those callbacks
 * and the reusable mesh scratch buffers they use. The sim binds to the HOME
 * chunk only while that chunk is resident and detaches safely otherwise (the
 * crash-avoidance rule of the streaming design: never tick a recycled slab).
 *
 * Everything here talks ONLY to the engine-portable headers (platform.h,
 * render.h, chunk.h, mesher.h, voxel.h, material.h, light.h, sim.h, world.h,
 * worldgen.h) and the manually-loaded GL pointers (gl_loader.h). It never
 * includes a windowing or GL system header - the platform layer owns the
 * context. The matrix/vector math is a tiny inline column-major kit (incidental
 * float, per Appendix A; -mfpmath=sse governs it), kept local.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv - optional headless screenshot + camera control */
#include <math.h>

/* <math.h> M_PI is a POSIX/GNU extension, not ISO C99; define it ourselves so
 * a strict -std=c99 build (MinGW-w64 or Mesa/Linux GCC) compiles either way. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "platform.h"
#include "gl_loader.h"
#include "render.h"
#include "mesher.h"
#include "chunk.h"
#include "voxel.h"
#include "material.h"
#include "light.h"
#include "sim.h"
#include "world.h"
#include "worldgen.h"
#include "persist.h"
#include "progress.h"
#include "raycast.h"
#include "version.h"

/* ---- Frame scheduling constants (binding, ARCHITECTURE.md Section 6) ------ */
#define FRAME_TARGET_MS   33.333    /* 30 FPS, binding TARGET_FRAMERATE        */

/* ---- World seed ---------------------------------------------------------- *
 * The whole world is a deterministic function of this one seed (Section 7):
 * same seed -> byte-identical terrain on every machine and every re-entry.
 * A fixed default keeps the headless screenshot reproducible; VOXEL_SEED lets a
 * developer roll a different landscape without a recompile. */
#define WORLD_SEED_DEFAULT  0x5EED1234ABCD0001ull

/* ---- The fixed HOME demo chunk (heat/fluid sim binds here) ---------------- *
 * The single-chunk heat+fluid SimState binds to ONE fixed world chunk so the
 * lava pool / copper columns / water demo always lands at a known location the
 * camera can frame. We pick a near-origin chunk in the UPPER band layer (cy 1,
 * world-Y 16..31) which sits ENTIRELY ABOVE the procedural surface (the
 * heightmap tops out at WG_HEIGHT_MAX=18, in world-Y 16..18 -> local rows 0..2
 * of cy 1), so demo_decorate can build a clean stone pedestal and stand the
 * lava/water demo on top of it in open air, fully visible regardless of the
 * rolling terrain underneath. HOME is in the resident band [WORLD_BAND_Y0..Y1]
 * (0..1), so it streams in like any other chunk. With Section 8 persistence now
 * live, HOME is flagged CHUNK_MODIFIED after decoration (and whenever the sim
 * materially changes it), so flying away EVICTS it to disk and flying back LOADS
 * the evolved (decorated + settled) state rather than a fresh undecorated chunk;
 * the edits also survive a process restart. (If HOME re-loads from disk already
 * decorated, the attach path's demo_decorate is idempotent over the same
 * footprint, so re-decoration is a harmless no-op overwrite.) */
#define HOME_CX           0
#define HOME_CY           1
#define HOME_CZ           0

/* Local-Y of the pedestal top the demo sits on (also the camera's look height
 * once offset by the HOME chunk's world origin). The decoration footprint is
 * filled solid stone below this and the demo placed on top, recreating the flat
 * floor the heat/fluid demo was authored against - but raised clear of the
 * rolling terrain so it reads in frame. */
#define DEMO_FLOOR_Y      8

/* World-Y the camera aims at: HOME chunk origin + the pedestal floor. */
#define DEMO_WORLD_Y      ((float)(HOME_CY * CHUNK_DIM + DEMO_FLOOR_Y))
/* World X/Z of the HOME chunk centre - the default player look point so the
 * demo is framed on startup with no env overrides. */
#define DEMO_WORLD_X      ((float)(HOME_CX * CHUNK_DIM) + (float)(CHUNK_DIM / 2))
#define DEMO_WORLD_Z      ((float)(HOME_CZ * CHUNK_DIM) + (float)(CHUNK_DIM / 2))

/* ---- Day/night cycle period (visual goal: live sun, zero remeshes) -------- *
 * Wall-clock seconds for one full dawn->noon->dusk->night->dawn cycle. The
 * frame loop maps wall time onto this period and drives the live u_sun uniform
 * the vertex shader folds per frame with NO remeshes (the architecture's
 * selling point). Demo-tunable; kept short so the cycle is visible while flying. */
#define DAY_LENGTH_SEC    120.0

/* Worst-case greedy-mesh scratch: a 16^3 chunk tops out far under the 65,535
 * vertex ceiling (Section 4). A checkerboard degenerate fill is the pessimum;
 * 4 verts + 6 indices per quad. These caps comfortably cover any 16^3 chunk
 * and are reused across all chunks (one scratch buffer, the mesher idiom). */
#define MESH_VERT_CAP     65536u
#define MESH_INDEX_CAP    98304u

/* ---- Fly camera (FPS free-look: mouse turns, WASD moves relative) --------- *
 * The mouse drives yaw/pitch (platform.h's relative-motion API) and WASD moves
 * relative to where the player faces: W/S along the look direction flattened to
 * the XZ plane, A/D strafe, SPACE/LSHIFT world up/down. The look direction is
 * now a DERIVED value (cam_target = cam_pos + forward) rebuilt every frame from
 * yaw/pitch, not a fixed tilt. The PLAYER's eye XZ (cam_pos) is the streaming
 * anchor: the loaded window centres on the eye position (NOT the look point,
 * which now swings with the gaze), starting exactly over the HOME demo chunk.
 *
 * Mouse-look is LIVE-only: in VOXEL_SHOT mode capture stays OFF and yaw/pitch
 * are frozen at the VOXEL_YAW/VOXEL_PITCH env values, so headless captures use a
 * fixed, reproducible orientation. */
#define CAM_MOVE_PER_SEC  12.0f     /* metres (voxels) per second              */
#define CAM_MOUSE_SENS    0.12f     /* degrees of look per pixel of motion     */
#define CAM_PITCH_LIMIT   89.0f     /* clamp pitch to avoid gimbal flip        */

typedef struct {
    float x, y, z;
} Vec3;

/* ---- Tiny column-major 4x4 matrix kit (incidental render float) ----------- *
 * Column-major to match glUniformMatrix4fv(loc, 1, GL_FALSE, m): element m[c*4+r]
 * is row r, column c. All inline, all local; no external math dependency. */

static void mat4_identity(float *m)
{
    int i;
    for (i = 0; i < 16; ++i)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b  (column-major: applies b first, then a, to a column vector). */
static void mat4_mul(float *out, const float *a, const float *b)
{
    float r[16];
    int c, row, k;
    for (c = 0; c < 4; ++c) {
        for (row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    }
    for (c = 0; c < 16; ++c)
        out[c] = r[c];
}

/* Right-handed perspective, mapping z to [-1,1] (the GL clip convention).
 * fovy in radians; aspect = width/height. Column-major. */
static void mat4_perspective(float *m, float fovy, float aspect,
                             float znear, float zfar)
{
    float f = 1.0f / (float)tan((double)fovy * 0.5);
    int i;
    for (i = 0; i < 16; ++i)
        m[i] = 0.0f;
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

static Vec3 vec3_sub(Vec3 a, Vec3 b)
{
    Vec3 r;
    r.x = a.x - b.x;
    r.y = a.y - b.y;
    r.z = a.z - b.z;
    return r;
}

static Vec3 vec3_cross(Vec3 a, Vec3 b)
{
    Vec3 r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

static Vec3 vec3_normalize(Vec3 a)
{
    float len = (float)sqrt((double)(a.x * a.x + a.y * a.y + a.z * a.z));
    Vec3 r = a;
    if (len > 1e-6f) {
        float inv = 1.0f / len;
        r.x = a.x * inv;
        r.y = a.y * inv;
        r.z = a.z * inv;
    }
    return r;
}

/* Right-handed look-at view matrix, column-major. eye looks toward center with
 * the given up vector. Equivalent to the standard gluLookAt construction. */
static void mat4_lookat(float *m, Vec3 eye, Vec3 center, Vec3 up)
{
    Vec3 f = vec3_normalize(vec3_sub(center, eye));   /* forward  */
    Vec3 s = vec3_normalize(vec3_cross(f, up));       /* right    */
    Vec3 u = vec3_cross(s, f);                        /* true up  */

    mat4_identity(m);
    /* rotation (rows are the basis vectors), column-major storage */
    m[0]  =  s.x;  m[4]  =  s.y;  m[8]  =  s.z;
    m[1]  =  u.x;  m[5]  =  u.y;  m[9]  =  u.z;
    m[2]  = -f.x;  m[6]  = -f.y;  m[10] = -f.z;
    /* translation: -dot(basis, eye) in the 4th column */
    m[12] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
    m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
    m[14] =  (f.x * eye.x + f.y * eye.y + f.z * eye.z);
    m[15] =  1.0f;
}

/* ---- Streaming render callback context ----------------------------------- *
 * The WorldStore is GL-free (world.h / world.c include only mesher.h + render.h
 * for the slot count, never gl_loader.h), so it calls OUT to the renderer via
 * function pointers we install at world_init. This struct is the `user` opaque
 * threaded into every callback: it carries the two reusable greedy-mesh scratch
 * buffers (one opaque stream, one liquid stream) so the mesh-upload callback can
 * mesh a chunk without allocating per call. Single-threaded, so one shared pair
 * of scratch buffers serves every callback, the HOME decorate-remesh, and the
 * per-frame dirty-mesh drain. */
typedef struct {
    MeshBuffer *opaque;   /* greedy_mesh scratch (non-liquid stream)            */
    MeshBuffer *liquid;   /* greedy_mesh_liquid scratch (PHASE_LIQUID stream)   */
} StreamCtx;

/* Light + mesh (both streams) + upload one chunk into its render slot. This is
 * the WorldMeshUploadFn the WorldStore invokes for a freshly-generated chunk and
 * for any boundary chunk whose neigh-set changed (re-mesh to keep seams correct
 * as the window slides). light_compute MUST run before greedy_mesh (the mesher
 * reads vox_light to key faces). greedy_mesh now reads neigh[6], so interior
 * seams against resident neighbours cull and only true window-edge faces emit. */
static void cb_mesh_upload(Chunk *c, int slot, void *user)
{
    StreamCtx *ctx = (StreamCtx *)user;
    if (slot < 0)
        return;                                    /* no render slot (headless)  */
    light_compute(c);                              /* bake light before meshing  */
    greedy_mesh(c, ctx->opaque);                   /* opaque stream              */
    render_upload_chunk(slot, ctx->opaque, c->cx, c->cy, c->cz);
    greedy_mesh_liquid(c, ctx->liquid);            /* liquid (blended) stream    */
    render_upload_chunk_liquid(slot, ctx->liquid, c->cx, c->cy, c->cz);
}

/* Mark a render slot drawable-as-nothing on evict: upload an empty mesh to both
 * the opaque and liquid VBOs for the slot (index_count 0 -> draws nothing) so a
 * reclaimed slot does not draw a stale chunk before it is reused. The MeshBuffer
 * count fields are zeroed via mesh_buffer_reset; the upload orphans the VBOs. */
static void cb_slot_free(int slot, void *user)
{
    StreamCtx *ctx = (StreamCtx *)user;
    if (slot < 0)
        return;
    mesh_buffer_reset(ctx->opaque);
    mesh_buffer_reset(ctx->liquid);
    render_upload_chunk(slot, ctx->opaque, 0, 0, 0);
    render_upload_chunk_liquid(slot, ctx->liquid, 0, 0, 0);
}

/* The WorldGenFn: fill a freshly-popped slab from seed. A thin wrapper over the
 * deterministic worldgen so the WorldStore stays decoupled from worldgen.h's
 * exact signature; it sets c->cx/cy/cz and raises CHUNK_DIRTY_MESH|CHUNK_GEN. */
static void cb_gen(Chunk *c, int cx, int cy, int cz, uint64_t seed, void *user)
{
    (void)user;
    worldgen_fill_chunk(c, cx, cy, cz, seed);
}

/* ---- Demo decoration (M2 lighting + M3/M4 heat + M6 fluid) --------------- *
 * Decorate ONLY the HOME chunk (the one the single-chunk sim binds to). The
 * lava pool + copper columns + water blob sit INTERIOR to this chunk, well clear
 * of its 0/15 boundary planes, so a sim edit never touches a chunk face and the
 * deferred cross-chunk dirty/fluid propagation produces no visible artefact.
 *
 * Unlike the old flat-grid demo (which stood on the world's GROUND_HEIGHT
 * floor), HOME now sits above ROLLING procedural terrain, so this routine first
 * builds a solid stone PEDESTAL filling the demo footprint up to DEMO_FLOOR_Y,
 * then stands the original demo on top of it. The pedestal recreates the flat
 * floor the heat/fluid demo was authored against and lifts the whole thing clear
 * of the hills below, so it reads cleanly in frame. The footprint union is
 * x in 2..11, z in 2..7, so the pedestal covers every piece placed below. */
static Voxel make_voxel(uint8_t mat)
{
    Voxel v = 0;
    vox_set_mat(&v, mat);
    vox_set_fill(&v, 15);
    vox_set_temp_code(&v, temp_encode_c(20.0));
    return v;
}

/* A flowing-liquid voxel for the fluid demo (ARCHITECTURE Section 3 fluid sim):
 * make_voxel + VF_LIQUID, exactly the state sim.c's melt branch leaves on a
 * freshly-molten voxel (full cubic metre, fill 15, flagged flowing). Held at
 * ambient 20 C so the water stays clear of its 0 C freeze / 100 C boil points;
 * the fluid pass reads phase from MaterialDef, never an id, so any PHASE_LIQUID
 * id works. */
static Voxel make_liquid(uint8_t mat)
{
    Voxel v = 0;
    vox_set_mat(&v, mat);
    vox_set_fill(&v, 15);
    vox_set_temp_code(&v, temp_encode_c(20.0));
    vox_set_flags(&v, VF_LIQUID);
    return v;
}

/* ---- Block break / place (0.2) ------------------------------------------ *
 * A voxel ray (raycast.c) from the eye along the look direction finds the first
 * solid block: left-click breaks it, right-click places the selected material in
 * the empty cell before it. Number keys 1..5 pick the placement material; the
 * targeted block is highlighted. Edits go through world_edit_voxel (remesh +
 * persist) and, when they land in the simulated chunk, wake the sim. */
#define BLOCK_REACH   6.0f      /* player edit reach, in voxels */

/* Placement palette, selected by number keys 1..5. */
static const uint8_t PLACE_MATS[5] = {
    MAT_STONE, MAT_DIRT, MAT_COPPER, MAT_WATER, MAT_LAVA
};

/* floor(w/16) without relying on implementation-defined signed right-shift. */
static int floordiv16(int w)
{
    return (w >= 0) ? (w >> 4) : -(((-w) + 15) >> 4);
}

/* A clean AIR voxel at ambient 20 C (NOT the cold code-0 a zeroed voxel decodes
 * to): breaking a block must not drop a -40 C cold sink into the simulated chunk. */
static Voxel air_voxel(void)
{
    Voxel v = 0;
    vox_set_temp_code(&v, temp_encode_c(20.0));
    return v;
}

/* Raycast solid predicate: the ray stops at OPAQUE blocks; air and (translucent)
 * liquids are passed through, so you target the first real surface. ctx is the
 * WorldStore. */
static int ray_solid(void *ctx, int x, int y, int z)
{
    Voxel v = world_get_voxel((const WorldStore *)ctx, x, y, z);
    return (material_get(vox_mat(v))->flags & MAT_OPAQUE) != 0;
}

/* Build the voxel to PLACE for material id m (liquids carry VF_LIQUID + full
 * fill; solids a full opaque cube), reusing the demo voxel makers. */
static Voxel place_voxel(uint8_t m)
{
    return (material_get(m)->phase == (uint8_t)PHASE_LIQUID) ? make_liquid(m)
                                                             : make_voxel(m);
}

/* Apply a player edit at world voxel (wx,wy,wz); if that voxel lives in the
 * chunk the sim is bound to, wake the sim around it so heat/fluid react. */
static void edit_and_notify(WorldStore *w, SimState *s, int wx, int wy, int wz, Voxel v)
{
    if (!world_edit_voxel(w, wx, wy, wz, v))
        return;
    if (s != NULL && s->chunk != NULL) {
        int cx = floordiv16(wx), cy = floordiv16(wy), cz = floordiv16(wz);
        if (s->chunk->cx == cx && s->chunk->cy == cy && s->chunk->cz == cz)
            sim_notify_edit(s, vox_index(wx - cx * 16, wy - cy * 16, wz - cz * 16));
    }
}

static void demo_decorate(Chunk *c)
{
    int x, y, z;

    if (c->cx != HOME_CX || c->cy != HOME_CY || c->cz != HOME_CZ)
        return;

    /* Pedestal: fill the demo footprint (x 2..11, z 2..7) solid stone from the
     * chunk floor up to DEMO_FLOOR_Y-1, giving the demo a flat floor at local-Y
     * DEMO_FLOOR_Y clear of the rolling terrain below. */
    for (x = 2; x <= 11; ++x)
        for (z = 2; z <= 7; ++z)
            for (y = 0; y < DEMO_FLOOR_Y; ++y)
                chunk_set(c, x, y, z, make_voxel(MAT_STONE));

    /* Emergent-smelter demo. A lava pool (held at ~1160 C, surface at the floor
     * +2) with copper columns standing in it. A column voxel ringed by lava on
     * its 4 sides is driven toward the lava temperature and, once it banks its
     * latent heat of fusion, MELTS into molten copper - no scripted recipe, just
     * heat crossing copper's 1085 C melt point. A stone column never melts. Lava
     * is also the M2 block-light source (MAT_EMISSIVE); the sim auto-holds every
     * emissive voxel. */

    /* Lava BASIN walls. The held-heat/held-fill decoupling means the lava now
     * holds its TEMPERATURE (Dirichlet heat source, still drives melting) but its
     * fill is CONSERVED - it is no longer an inexhaustible spring. So a physical
     * WALL is what keeps the pool in place (without it the finite lava would still
     * flow off the open pedestal once and drain). Extend the pedestal under a 7x7
     * tub (x,z 1..7) and raise a 1-thick stone wall ring around the 5x5 pool, one
     * voxel above the lava surface (to floor+3) so nothing spills over. */
    for (x = 1; x <= 7; ++x)
        for (z = 1; z <= 7; ++z) {
            for (y = 0; y < DEMO_FLOOR_Y; ++y)            /* extend pedestal floor */
                chunk_set(c, x, y, z, make_voxel(MAT_STONE));
            if (x == 1 || x == 7 || z == 1 || z == 7)     /* perimeter wall ring   */
                for (y = DEMO_FLOOR_Y; y <= DEMO_FLOOR_Y + 3; ++y)
                    chunk_set(c, x, y, z, make_voxel(MAT_STONE));
        }

    /* Lava pool: 5x5 footprint (x,z 2..6), 3 deep (y floor..floor+2), inside the
     * walled tub above so it stays contained. */
    for (x = 2; x <= 6; ++x)
        for (z = 2; z <= 6; ++z)
            for (y = DEMO_FLOOR_Y; y <= DEMO_FLOOR_Y + 2; ++y)
                chunk_set(c, x, y, z, make_voxel(MAT_LAVA));

    /* Four 1x1 copper columns inside the pool (y floor..floor+3) - their
     * submerged, lava-ringed voxels melt - plus one stone column that never
     * melts. */
    {
        static const int cols[4][2] = { {3,3}, {3,5}, {5,3}, {5,5} };
        int i;
        for (i = 0; i < 4; ++i)
            for (y = DEMO_FLOOR_Y; y <= DEMO_FLOOR_Y + 3; ++y)
                chunk_set(c, cols[i][0], y, cols[i][1], make_voxel(MAT_COPPER));
    }
    for (y = DEMO_FLOOR_Y; y <= DEMO_FLOOR_Y + 3; ++y)
        chunk_set(c, 6, y, 2, make_voxel(MAT_STONE));

    /* Water demo (ARCHITECTURE Section 3): a raised 3x3x5 block of water (fill 15,
     * 45 cells) that FALLS and settles into a level pool inside a WALLED stone
     * BASIN, east of the lava tub. The basin is essential: the demo HOME chunk
     * sits at cy 1, so its local-Y 0 is a CHUNK-BOUNDARY face. Cross-chunk
     * vertical fluid flow is deferred, so water that runs off an open pedestal
     * edge cascades to that boundary and pools there - visually "floating" a full
     * chunk above the terrain below, never touching ground. Containing the water
     * (like the lava tub) makes it a clean puddle resting on the pedestal floor.
     * Basin footprint x8..12, z4..8 (extends the pedestal a little east/south,
     * staying interior to the chunk); a 1-thick wall ring rises to floor+6, tall
     * enough to hold the settled pool (45 cells / 3x3 = 5 deep -> floor..floor+4)
     * with freeboard, so nothing spills. The pool is clear of the lava tub (x<=7)
     * and the chunk's 0/15 faces; different liquids never merge anyway. */
    for (x = 8; x <= 12; ++x)
        for (z = 4; z <= 8; ++z) {
            for (y = 0; y < DEMO_FLOOR_Y; ++y)              /* extend pedestal floor */
                chunk_set(c, x, y, z, make_voxel(MAT_STONE));
            if (x == 8 || x == 12 || z == 4 || z == 8)      /* perimeter wall ring   */
                for (y = DEMO_FLOOR_Y; y <= DEMO_FLOOR_Y + 6; ++y)
                    chunk_set(c, x, y, z, make_voxel(MAT_STONE));
        }
    /* The raised water column inside the basin (interior x9..11, z5..7), starting
     * a couple of cells above the floor so it visibly FALLS and equalises into a
     * level pool that rests on - and touches - the pedestal floor. */
    for (x = 9; x <= 11; ++x)
        for (z = 5; z <= 7; ++z)
            for (y = DEMO_FLOOR_Y + 2; y <= DEMO_FLOOR_Y + 6; ++y)
                chunk_set(c, x, y, z, make_liquid(MAT_WATER));
}

/* True iff HOME already carries the demo (either decorated this run, or LOADED
 * from a previous run's save). Probes the lava-pool corner at (2,DEMO_FLOOR_Y,2):
 * a fresh worldgen HOME is open air there (cy 1 sits entirely above the surface),
 * while a decorated/persisted HOME has the lava source there - and lava is a held
 * heat+fill source the sim NEVER drains, so the marker survives any amount of sim
 * evolution. This is the guard that makes the cross-restart demo correct: on a
 * re-run HOME loads its EVOLVED (decorated + settled) voxels from disk and we must
 * NOT re-stamp the pristine authored decoration over them. demo_decorate itself is
 * idempotent for the static pieces, but re-stamping would CLOBBER melted copper /
 * settled water / banked heat the player (sim) produced and we just reloaded. */
static int home_is_decorated(const Chunk *c)
{
    if (c == NULL)
        return 0;
    return vox_mat(chunk_get(c, 2, DEMO_FLOOR_Y, 2)) == MAT_LAVA;
}

/* ===================================================================== */

/* Read a float world-coordinate from the environment, or fall back to a
 * default. Used by VOXEL_CAM_X / VOXEL_CAM_Z for headless camera placement. */
static float env_float(const char *name, float fallback)
{
    const char *s = getenv(name);
    if (s == NULL || s[0] == '\0')
        return fallback;
    return (float)atof(s);
}

/* Resolve the save directory for this world (ARCHITECTURE Section 8: a save
 * belongs to a seed). VOXEL_SAVE names an explicit directory; otherwise the
 * default is "saves/<seed>" rendered in hex so each seed gets its own save and
 * two different seeds never share region files. Written into `buf` (caller-
 * sized); persist_open() creates the directory if it is missing. Kept tiny and
 * stdio-only so it is portable to the XP MinGW target. */
static const char *resolve_save_dir(char *buf, size_t buf_sz, uint64_t seed)
{
    const char *env = getenv("VOXEL_SAVE");
    if (env != NULL && env[0] != '\0')
        return env;
    snprintf(buf, buf_sz, "saves/%016llx", (unsigned long long)seed);
    return buf;
}

/* Flag the HOME chunk CHUNK_MODIFIED so the streaming evict/shutdown path
 * persists it (ARCHITECTURE Section 8 modified policy: only CHUNK_MODIFIED
 * chunks are written; everything else regenerates from seed). Called after the
 * demo decoration is applied and whenever a sim tick has materially changed
 * HOME's voxels (a melt, a fluid move, banked heat) - exactly the player-edit
 * analogue the persistence rule was written for. NULL-safe. */
static void mark_home_modified(Chunk *c)
{
    if (c != NULL)
        c->flags |= CHUNK_MODIFIED;
}

int main(void)
{
    const int win_w = 1024;
    const int win_h = 768;

    MeshBuffer    scratch;        /* opaque (non-liquid) mesh stream scratch     */
    MeshBuffer    liq_scratch;    /* liquid (PHASE_LIQUID) mesh stream scratch   */
    StreamCtx     stream_ctx;     /* `user` ctx threaded into the store callbacks */
    int           rc;

    /* The streaming world store. It OWNS the chunk slab pool, the residency
     * hash, the neigh[6] wiring, the render-slot assignment, and the per-frame
     * gen/remesh budget. Large (the ~6.3 MiB slab pool dominates), so it lives on
     * the heap, not this stack frame. */
    WorldStore   *world = NULL;
    uint64_t      seed;
    WorldCallbacks cb;

    /* World persistence (ARCHITECTURE Section 8). One PersistStore for the whole
     * run, opened on a save dir derived from the seed (or VOXEL_SAVE), injected
     * into the WorldStore so streaming load/evict/shutdown route CHUNK_MODIFIED
     * chunks through disk. NULL = persistence disabled (open failed) - the engine
     * then behaves exactly like the pre-Section-8 ephemeral path, never wedging
     * on a bad save dir. */
    PersistStore *persist = NULL;
    char          save_dir_buf[512];
    const char   *save_dir;

    /* Heat simulation (ARCHITECTURE Section 3). Single-chunk: one SimState bound
     * to the resident HOME chunk that carries the lava block. It is bound when
     * HOME is resident and DETACHED (sim->chunk = NULL) when HOME scrolls out of
     * the window, so no tick ever dereferences a recycled slab. SimState is large
     * (it embeds the per-tick double-buffer), so it lives on the heap. */
    SimState     *sim = NULL;
    double        sim_accum_ms = 0.0;     /* Section 3.7 ms tick accumulator     */

    /* Progression observer (ARCHITECTURE Section 9; progress.h). The sim is the
     * single PRODUCER, pushing a ProgressEvent onto this bounded ring on each
     * emergent transition it ALREADY computes (a melt, a freeze, a temp-tier
     * crossing). The observer is the single CONSUMER, drained once per frame in
     * the slack band (never the sim budget) into prog_state, which fires the
     * deduped discovery flash to its log (stderr) and tightens the empirical
     * journal. The ring is OWNED here and handed to the sim by BORROWED pointer
     * (sim->progress) AFTER sim_init - the binding read-only contract: the sim
     * only ever pushes, never reads back, so removing this layer leaves the world
     * BYTE-IDENTICAL (the emit hook is a no-op when sim->progress is NULL). The
     * ProgressState is large-ish (~7 KiB: journal + dedup bitset + discovery log),
     * so it lives on the heap alongside the ring to keep this stack frame small. */
    ProgressRing  *prog_ring  = NULL;     /* sim->observer SPSC event queue       */
    ProgressState *prog_state = NULL;     /* the observer's discovery/journal memory */

    /* Fly camera state + streaming anchor. cam_pos is the eye AND the streaming
     * anchor: the loaded window centres on the player's eye XZ (it starts over
     * the HOME demo chunk so the lava/water demo is framed). The look direction
     * is held as yaw/pitch (degrees) and cam_target is DERIVED each frame as
     * cam_pos + forward(yaw,pitch); only cam_pos translates, the gaze comes from
     * yaw/pitch. world_up is the +Y reference for the look-at + strafe frame. */
    Vec3   cam_pos;
    Vec3   cam_target;
    Vec3   world_up;
    float  yaw;     /* degrees; yaw=0 looks toward -Z (matches today's framing) */
    float  pitch;   /* degrees; <0 looks down, clamped to +/-CAM_PITCH_LIMIT    */

    /* Headless camera control (the streaming-capture knobs of the design):
     *   VOXEL_CAM_X / VOXEL_CAM_Z : starting world look position (override demo)
     *   VOXEL_FLY                 : voxels/frame, signed, advanced along +X each
     *                               frame so successive VOXEL_SHOT frames show the
     *                               player moving through streamed, varying
     *                               terrain (leading-edge load, trailing-edge
     *                               evict become visible). */
    float  cam_look_x = env_float("VOXEL_CAM_X", DEMO_WORLD_X);
    float  cam_look_z = env_float("VOXEL_CAM_Z", DEMO_WORLD_Z);
    float  fly_per_frame = env_float("VOXEL_FLY", 0.0f);

    /* Day/night sun term (live uniform; zero remeshes). Now ANIMATED in the live
     * frame loop from wall time (see just before render_begin) so lighting moves
     * with zero remeshes - the headless screenshot path leaves it at this fixed
     * full-day value so VOXEL_SHOT captures stay byte-reproducible - overridable
     * via VOXEL_SUN (0..1) to capture a specific time of day in a screenshot. */
    float  sun = env_float("VOXEL_SUN", 1.0f);

    /* Initial look orientation (degrees). The defaults reproduce TODAY's demo
     * framing exactly: the old fixed pose had the eye 24 up / 28 back (+Z) of the
     * look point, i.e. forward = normalize(0,-24,-28), which is yaw 0 (toward -Z)
     * and pitch = asin(-24/sqrt(24^2+28^2)) ~= -40.6 deg. VOXEL_YAW / VOXEL_PITCH
     * override them so a headless VOXEL_SHOT capture can freeze any orientation
     * (mouse-look is OFF in shot mode, so these stay the orientation for the whole
     * capture run). They compose with VOXEL_CAM_X/Z (eye XZ) and VOXEL_SUN. */
    float  init_yaw   = env_float("VOXEL_YAW",   0.0f);
    float  init_pitch = env_float("VOXEL_PITCH", -40.6f);

    /* Frame clock (Section 6): one monotonic source read at the frame top. */
    double frame_prev_ms;

    /* Optional headless one-shot screenshot: if VOXEL_SHOT names a path, render
     * frames, capture the back buffer to that PPM on frame VOXEL_FRAMES, and
     * exit. With a non-zero VOXEL_FLY a larger VOXEL_FRAMES captures the camera
     * deeper into freshly-streamed terrain. Clamped to at least 1. */
    const char *shot_path   = getenv("VOXEL_SHOT");
    const char *frames_env  = getenv("VOXEL_FRAMES");
    long        shot_frame  = (frames_env != NULL) ? (long)atoi(frames_env) : 3;
    long        frame_no    = 0;
    const char *seed_env;

    if (shot_frame < 1)
        shot_frame = 1;

    /* Startup banner (Factorio-style): identify the build on the console. */
    fprintf(stderr, "%s\n", VOXEL_TITLE);

    /* ---- 1. Window + GL 2.1 context -------------------------------------- */
    if (plat_create_window(win_w, win_h, VOXEL_TITLE) != 0) {
        fprintf(stderr, "main: plat_create_window failed\n");
        return 1;
    }

    /* ---- 2. Load the post-1.1 GL entry points through the platform ------- */
    rc = gl_load(plat_gl_getproc);
    if (rc != 0) {
        fprintf(stderr, "main: gl_load failed at entry point #%d\n", rc);
        return 1;
    }

    /* ---- 3. Renderer: shaders, atlas, once-per-run GL state -------------- */
    if (render_init() != 0) {
        fprintf(stderr, "main: render_init failed\n");
        return 1;
    }

    /* ---- 4. Two reusable greedy-mesh scratch buffers --------------------- *
     * The mesher emits TWO streams per chunk (ARCHITECTURE 5.1/5.6): the opaque
     * stream (greedy_mesh) and a separate liquid stream (greedy_mesh_liquid).
     * Each needs its own scratch; both reuse the same worst-case caps and are
     * shared by the store's mesh-upload/slot-free callbacks, the HOME decorate-
     * remesh, and the per-frame dirty drain (single-threaded, so one pair is
     * enough). They are reset by the mesher each call and freed at shutdown. */
    if (mesh_buffer_init(&scratch, MESH_VERT_CAP, MESH_INDEX_CAP) != 0) {
        fprintf(stderr, "main: mesh_buffer_init failed (opaque)\n");
        return 1;
    }
    if (mesh_buffer_init(&liq_scratch, MESH_VERT_CAP, MESH_INDEX_CAP) != 0) {
        fprintf(stderr, "main: mesh_buffer_init failed (liquid)\n");
        return 1;
    }
    stream_ctx.opaque = &scratch;
    stream_ctx.liquid = &liq_scratch;

    /* ---- 5. Build the WorldStore + prime the player-centred window ------- *
     * world_init reserves the slab pool (one big calloc), zeroes the hash and
     * fills the free stacks, and installs our gen/mesh-upload/slot-free
     * callbacks. cb.gen is REQUIRED (the store cannot invent terrain) and binds
     * the deterministic worldgen; mesh_upload + slot_free wire the renderer.
     * world_prime then force-fills the ENTIRE window around the start position
     * synchronously, so the first frame shows full rolling terrain like the old
     * static grid did - generating, neighbour-wiring, lighting, meshing and
     * uploading every in-range chunk now (the per-frame budget governs only the
     * live loop). */
    seed = WORLD_SEED_DEFAULT;
    seed_env = getenv("VOXEL_SEED");
    if (seed_env != NULL && seed_env[0] != '\0')
        seed = (uint64_t)strtoull(seed_env, NULL, 0);

    cb.gen         = cb_gen;
    cb.mesh_upload = cb_mesh_upload;
    cb.slot_free   = cb_slot_free;
    cb.user        = &stream_ctx;

    world = (WorldStore *)malloc(sizeof(WorldStore));
    if (world == NULL) {
        fprintf(stderr, "main: WorldStore malloc failed\n");
        return 1;
    }
    if (world_init(world, seed, &cb) != 0) {
        fprintf(stderr, "main: world_init failed (slab pool alloc)\n");
        return 1;
    }

    /* ---- 5b. World persistence (ARCHITECTURE Section 8) ------------------ *
     * Open the save (creating its directory if missing) and INJECT it into the
     * WorldStore so the streaming load/evict/shutdown points route through disk:
     * world_insert tries persist_load_chunk() before cb.gen, world_evict writes
     * back any CHUNK_MODIFIED chunk, and world_shutdown flushes all resident
     * modified chunks. The save belongs to THIS seed (stamped + checked per
     * region) and THIS generator (WG_GEN_VERSION, refused on mismatch). MUST be
     * injected BEFORE world_prime so the very first window loads persisted edits
     * in preference to regenerating from seed. A failed open is non-fatal: the
     * store stays persistence-less (ephemeral, the pre-Section-8 behaviour). */
    save_dir = resolve_save_dir(save_dir_buf, sizeof(save_dir_buf), seed);
    persist = persist_open(save_dir, seed, WG_GEN_VERSION);
    if (persist == NULL)
        fprintf(stderr, "main: persist_open(%s) failed - edits will be "
                        "ephemeral this run\n", save_dir);
    else
        world_set_persist(world, persist);

    world_prime(world, cam_look_x, cam_look_z);

    /* ---- 6. Heat sim allocation (bound lazily when HOME is resident) ----- *
     * SimState is large (it embeds the double-buffer + latent/heat arrays), so
     * it lives on the heap. It is NOT bound yet: sim->chunk == NULL until the
     * frame loop sees HOME resident, decorates it, and sim_init()s. We zero
     * sim->chunk explicitly because sim_init owns the rest of the struct. */
    sim = (SimState *)malloc(sizeof(SimState));
    if (sim == NULL) {
        fprintf(stderr, "main: SimState malloc failed\n");
        world_shutdown(world);   /* flushes through the borrowed persist store    */
        free(world);
        persist_close(persist);  /* then close it (NULL-safe), the owner's job     */
        return 1;
    }
    sim->chunk = NULL;   /* unbound: nothing to tick until HOME is resident      */

    /* Build the conductance LUT once up front (sim_init would do this lazily,
     * but doing it here keeps the first attach cheap). Idempotent. */
    sim_build_conduct_lut();

    /* ---- 6b. Progression observer (ARCHITECTURE Section 9) --------------- *
     * Stand up the event ring and the observer state, then HAND the ring to the
     * sim as its (borrowed) event sink. Both are heap-allocated (the observer's
     * journal + dedup tables are several KiB) and owned by this frame. The ring is
     * zero-initialised to empty (prog_ring_init); the observer is initialised with
     * stderr as its journal log, so the deduped discovery flash ("DISCOVERY:
     * copper melts (observed ~1080 C)") and the shutdown journal dump land on the
     * console (the engine has no font/UI yet - this milestone is console-journal).
     *
     * The attach is the read-only contract in action: sim->progress is a NULLABLE
     * borrowed pointer the sim only ever PUSHES to. We set it AFTER sim_build_
     * conduct_lut and BEFORE the sim is ticked anywhere (the warm-up soak below,
     * then the frame loop), so every emergent transition - including those caused
     * by a headless VOXEL_WARMUP_TICKS soak - is observed. A malloc failure here
     * is NON-FATAL: we simply leave the sink NULL (the sim then emits nothing and
     * runs byte-identically), so the engine still works, just without the journal.
     *
     * The ring is handed to the sim with sim_set_progress_sink() AFTER each
     * sim_init (sim_init resets s->progress to NULL, the safe default), so the
     * attach happens at the warm-up bind and at every loop re-attach below - here
     * we only allocate + initialise. The ring is NOT bound to sim->chunk: it
     * survives HOME detach/attach cycles and process-long roaming, accumulating
     * discoveries across the whole run, exactly as the design intends (discovery
     * is first-occurrence-of-a-kind over the lifetime of play, not per residency). */
    prog_ring  = (ProgressRing *)malloc(sizeof(ProgressRing));
    prog_state = (ProgressState *)malloc(sizeof(ProgressState));
    if (prog_ring != NULL && prog_state != NULL) {
        prog_ring_init(prog_ring);
        prog_init(prog_state, stderr);    /* stderr = the console journal sink     */
    } else {
        /* Allocation failed: run without observation (sim stays byte-identical).
         * Free whichever half succeeded so neither leaks, and null both so the
         * sink-attach / drain / dump / teardown below all short-circuit safely. */
        free(prog_ring);
        free(prog_state);
        prog_ring  = NULL;
        prog_state = NULL;
        fprintf(stderr, "main: progression observer alloc failed - "
                        "running without the discovery journal\n");
    }

    /* ---- 7. Camera initial pose ----------------------------------------- *
     * Place the EYE up and back of the demo (the same spot as the old fixed
     * pose: 24 up, 28 back of the look point) and seed yaw/pitch from the env
     * (defaults reproduce that old down-tilted framing exactly). The look point
     * cam_target is DERIVED from yaw/pitch below; the EYE XZ is the streaming
     * anchor and starts over HOME. */
    cam_pos.x    = cam_look_x;
    cam_pos.y    = DEMO_WORLD_Y + 24.0f;     /* up                              */
    cam_pos.z    = cam_look_z + 28.0f;       /* back (+Z)                       */

    world_up.x = 0.0f;
    world_up.y = 1.0f;
    world_up.z = 0.0f;

    yaw   = init_yaw;
    pitch = init_pitch;
    if (pitch >  CAM_PITCH_LIMIT) pitch =  CAM_PITCH_LIMIT;
    if (pitch < -CAM_PITCH_LIMIT) pitch = -CAM_PITCH_LIMIT;

    /* Derive the initial look point from yaw/pitch so cam_target is consistent
     * before the first frame (the warm-up below builds no view, but keep it
     * coherent). forward: yaw=0 -> -Z, pitch<0 -> looking down. */
    {
        float ry  = yaw   * (float)(M_PI / 180.0);
        float rp  = pitch * (float)(M_PI / 180.0);
        float cp  = cosf(rp);
        cam_target.x = cam_pos.x + sinf(ry) * cp;
        cam_target.y = cam_pos.y + sinf(rp);
        cam_target.z = cam_pos.z - cosf(ry) * cp;
    }

    /* Enable mouse-look for a LIVE session only. In VOXEL_SHOT mode capture
     * stays OFF (the cursor is never grabbed and plat_mouse_delta is never read),
     * so yaw/pitch remain frozen at the env values for a reproducible capture.
     * Idempotent + safe now the window exists; released in teardown below. */
    if (shot_path == NULL)
        plat_set_mouse_capture(1);

    /* ---- 8. Optional sim warm-up ---------------------------------------- *
     * If HOME primed in (it does, by default - the start is centred on it) and
     * sim is unbound, decorate + bind now so VOXEL_WARMUP_TICKS can soak the
     * heat/fluid demo before the first frame for a one-shot screenshot. The
     * attach-and-warmup mirrors the per-frame attach below, kept here so the
     * warmed state is uploaded before the loop. */
    {
        Chunk *home = world_get(world, HOME_CX, HOME_CY, HOME_CZ);
        const char *warm = getenv("VOXEL_WARMUP_TICKS");
        long wt = (warm != NULL) ? atol(warm) : 0L;

        if (home != NULL) {
            int slot;
            /* Attach: decorate the resident HOME chunk (UNLESS it loaded from a
             * previous run's save already carrying the evolved demo - then keep
             * the persisted state, see home_is_decorated), re-mesh it (the
             * decoration replaced voxels primed by worldgen), then bind the sim
             * (sim_init scans for the emissive lava source and seeds the front).
             * Either way flag HOME CHUNK_MODIFIED so the evolved home is written
             * back on eviction / shutdown (Section 8 modified policy). */
            if (!home_is_decorated(home))
                demo_decorate(home);
            mark_home_modified(home);
            slot = world_render_slot(world, home);
            cb_mesh_upload(home, slot, &stream_ctx);
            if (sim_init(sim, home) != 0) {
                fprintf(stderr, "main: sim_init failed\n");
                /* Non-fatal: leave sim unbound and continue without the demo. */
                sim->chunk = NULL;
            }
            /* Install the progression sink AFTER sim_init (which reset progress to
             * NULL): the sim now PUSHES emergent transitions into our ring so the
             * warm-up soak below is observed. NULL prog_ring (alloc failed) detaches
             * cleanly - the sim then emits nothing and is byte-identical. */
            if (sim->chunk != NULL)
                sim_set_progress_sink(sim, prog_ring);

            /* Soak N ticks, then re-light + re-mesh both streams so the settled
             * fluid / heat plume is uploaded for the first frame. Guarded by the
             * bind succeeding. The soak materially changes HOME's voxels (heat,
             * melt, fluid), so re-flag CHUNK_MODIFIED to persist the warmed state.
             *
             * The soak DRAINS the observer every tick (prog_observe_drain), so a
             * headless one-shot run (VOXEL_WARMUP_TICKS + VOXEL_SHOT, no live loop)
             * still logs every discovery the soak caused - a copper melt during
             * warm-up prints its "DISCOVERY:" flash here, and the journal it builds
             * is dumped at shutdown. Draining every tick (rather than once after
             * the loop) keeps the 256-entry ring from drop-oldest discarding a
             * NOVEL first-occurrence under a long, melt-heavy soak. NULL-safe: a
             * failed observer alloc leaves prog_state NULL and this is a no-op. */
            if (sim->chunk != NULL && wt > 0L) {
                long k;
                for (k = 0; k < wt; ++k) {
                    sim_tick(sim);
                    if (prog_state != NULL)
                        prog_observe_drain(prog_state, prog_ring);
                }
                cb_mesh_upload(home, slot, &stream_ctx);
                sim->dirty_mesh = 0;
                mark_home_modified(home);
            }
        }
    }

    /* ---- 9. The frame loop (Section 6 shape) ----------------------------- */
    frame_prev_ms = plat_time_ms();

    int sel_mat = 0;   /* placement-material index into PLACE_MATS (0 = stone) */

    for (;;) {
        double frame_start = plat_time_ms();
        double real_dt_ms  = frame_start - frame_prev_ms;
        float  dt_s;
        double frame_used;
        double sleep_ms;
        int    hl_have = 0;          /* block-target highlight valid this frame  */
        int    hl_x = 0, hl_y = 0, hl_z = 0;

        frame_prev_ms = frame_start;
        if (real_dt_ms > 250.0)        /* guard: post-stall / debugger pause   */
            real_dt_ms = 250.0;
        if (real_dt_ms < 0.0)
            real_dt_ms = 0.0;
        dt_s = (float)(real_dt_ms / 1000.0);

        /* --- Input: pump the OS queue once per frame, then sample keys --- */
        plat_poll();
        if (plat_should_close() || plat_key_down(PLAT_KEY_ESC))
            break;

        /* --- FPS free-look: mouse turns, WASD moves relative to facing ---- *
         * On a LIVE session read the relative mouse motion accumulated since the
         * last poll and fold it into yaw/pitch: +dx (right) yaws right, +dy
         * (down) lowers pitch (the screen-natural dy is negated into pitch).
         * Pitch is clamped to avoid the look vector aligning with world_up; yaw
         * wraps to keep it bounded. In VOXEL_SHOT mode we SKIP this entirely so
         * yaw/pitch stay frozen at the env values (capture is off, so the delta
         * would be (0,0) anyway, but skipping makes the frozen intent explicit).
         *
         * Movement is in a horizontal frame derived from the (now yaw/pitch-
         * driven) forward flattened to the XZ plane: W/S dolly toward/away from
         * where we look, A/D strafe, SPACE/LSHIFT raise/lower (world up/down).
         * Only the EYE (cam_pos) translates - the look direction comes from
         * yaw/pitch, so cam_target is recomputed as cam_pos + forward AFTER the
         * move. VOXEL_FLY adds a constant +X advance each frame for headless
         * streaming capture (the player roaming so leading/trailing edges show).*/
        {
            Vec3 fwd;        /* full 3D look direction from yaw/pitch          */
            Vec3 fwd_flat;   /* fwd flattened to XZ for ground-plane movement  */
            Vec3 right;
            float move = CAM_MOVE_PER_SEC * dt_s;
            float ry, rp, cp;
            Vec3 delta;

            if (shot_path == NULL) {
                int mdx = 0, mdy = 0;
                plat_mouse_delta(&mdx, &mdy);
                yaw   += (float)mdx * CAM_MOUSE_SENS;
                pitch -= (float)mdy * CAM_MOUSE_SENS;   /* +down -> look down  */
                if (pitch >  CAM_PITCH_LIMIT) pitch =  CAM_PITCH_LIMIT;
                if (pitch < -CAM_PITCH_LIMIT) pitch = -CAM_PITCH_LIMIT;
                /* Keep yaw in [0,360) so it never drifts to a huge magnitude. */
                if (yaw >= 360.0f) yaw -= 360.0f;
                if (yaw <    0.0f) yaw += 360.0f;
            }

            /* Build the look direction from yaw/pitch (degrees -> radians).
             * yaw=0 -> -Z, pitch<0 -> looking down (matches the env defaults). */
            ry = yaw   * (float)(M_PI / 180.0);
            rp = pitch * (float)(M_PI / 180.0);
            cp = cosf(rp);
            fwd.x = sinf(ry) * cp;
            fwd.y = sinf(rp);
            fwd.z = -cosf(ry) * cp;

            /* Horizontal move frame: flatten forward to XZ, derive right. */
            fwd_flat.x = fwd.x;
            fwd_flat.y = 0.0f;
            fwd_flat.z = fwd.z;
            fwd_flat = vec3_normalize(fwd_flat);

            right = vec3_normalize(vec3_cross(fwd_flat, world_up));

            delta.x = fly_per_frame;     /* env-driven +X advance (0 when unset)*/
            delta.y = 0.0f;
            delta.z = 0.0f;

            if (plat_key_down(PLAT_KEY_W)) {
                delta.x += fwd_flat.x * move;
                delta.z += fwd_flat.z * move;
            }
            if (plat_key_down(PLAT_KEY_S)) {
                delta.x -= fwd_flat.x * move;
                delta.z -= fwd_flat.z * move;
            }
            if (plat_key_down(PLAT_KEY_D)) {
                delta.x += right.x * move;
                delta.z += right.z * move;
            }
            if (plat_key_down(PLAT_KEY_A)) {
                delta.x -= right.x * move;
                delta.z -= right.z * move;
            }
            if (plat_key_down(PLAT_KEY_SPACE))
                delta.y += move;
            if (plat_key_down(PLAT_KEY_LSHIFT))
                delta.y -= move;

            /* Only the eye translates; the look comes from yaw/pitch. */
            cam_pos.x += delta.x;
            cam_pos.y += delta.y;
            cam_pos.z += delta.z;

            /* Derive the look point from the (possibly moved) eye + forward. */
            cam_target.x = cam_pos.x + fwd.x;
            cam_target.y = cam_pos.y + fwd.y;
            cam_target.z = cam_pos.z + fwd.z;

            /* --- Block break / place (live sessions only) ----------------- *
             * Cast from the eye along the look dir to the first solid block.
             * Left-click breaks it; right-click places the selected material in
             * the empty cell before it; number keys 1..5 pick the material. The
             * hit block is highlighted (hl_*). A VOXEL_SHOT capture skips all of
             * this so headless frames stay clean and reproducible. */
            if (shot_path == NULL) {
                RayHit ray = raycast_voxel(cam_pos.x, cam_pos.y, cam_pos.z,
                                           fwd.x, fwd.y, fwd.z,
                                           BLOCK_REACH, ray_solid, world);
                int lc = 0, rc = 0, i;

                if (ray.hit) { hl_have = 1; hl_x = ray.hx; hl_y = ray.hy; hl_z = ray.hz; }

                for (i = 0; i < 5; ++i) {
                    if (plat_key_down(PLAT_KEY_1 + i) && sel_mat != i) {
                        sel_mat = i;
                        fprintf(stderr, "[place] slot %d -> material id %d\n",
                                i + 1, (int)PLACE_MATS[i]);
                    }
                }

                plat_mouse_buttons(&lc, &rc);
                if (ray.hit && lc > 0)
                    edit_and_notify(world, sim, ray.hx, ray.hy, ray.hz, air_voxel());
                if (ray.hit && rc > 0) {
                    /* Don't place a block into the voxel the eye occupies. */
                    int ex = (int)floorf(cam_pos.x);
                    int ey = (int)floorf(cam_pos.y);
                    int ez = (int)floorf(cam_pos.z);
                    if (!(ray.px == ex && ray.py == ey && ray.pz == ez))
                        edit_and_notify(world, sim, ray.px, ray.py, ray.pz,
                                        place_voxel(PLACE_MATS[sel_mat]));
                }
            }
        }

        /* --- Stream the loaded window around the camera (Section 6/7) ----- *
         * ONCE per frame, before drawing: convert the PLAYER eye's XZ (cam_pos)
         * to a chunk coord, evict the trailing edge, enqueue the leading edge,
         * and drain a BUDGETED slice of gen + remesh (newly resident chunks get
         * light + mesh + upload into an assigned render slot via cb_mesh_upload;
         * evicted chunks free their slot via cb_slot_free; boundary chunks whose
         * neigh-set changed are re-meshed so seams stay seamless as the window
         * slides). Anchoring on the EYE (not the look point) keeps the window
         * centred on the player: with free-look cam_target swings with the gaze
         * and shoots far away near the horizon, which would yank the streamed
         * window off the player. A stationary player with no pending work costs
         * nothing. */
        world_stream_update(world, cam_pos.x, cam_pos.z);

        /* --- HOME sim attach / detach (crash-avoidance rule) -------------- *
         * The sim binds to the FIXED HOME chunk only while it is resident. After
         * the stream update (which may have evicted or regenerated HOME), re-test
         * residency by pointer identity:
         *   - sim BOUND but world_get(HOME) != sim->chunk  -> HOME scrolled out
         *     (or its slab was recycled): DETACH (sim_shutdown, sim->chunk=NULL)
         *     so no tick dereferences a recycled slab.
         *   - sim UNBOUND but HOME resident                -> ATTACH: re-mesh it
         *     and sim_init() to bind. If HOME came back as FRESH terrain (it was
         *     evicted UNMODIFIED, or no save existed) re-decorate; if it came back
         *     LOADED from disk already carrying the evolved demo, keep the
         *     persisted state (home_is_decorated). Either way re-flag
         *     CHUNK_MODIFIED so the next eviction writes it back.
         * Pointer identity is the authoritative test: a reloaded/regenerated HOME
         * occupies a (possibly different) slab, and only the current resident
         * pointer is safe to tick. */
        {
            Chunk *home = world_get(world, HOME_CX, HOME_CY, HOME_CZ);

            if (sim->chunk != NULL && home != sim->chunk) {
                /* DETACH: HOME left the window (or was recycled). */
                sim_shutdown(sim);
                sim->chunk = NULL;
            }
            if (sim->chunk == NULL && home != NULL) {
                /* ATTACH: (decorate if fresh) + re-mesh + bind the resident HOME. */
                int slot = world_render_slot(world, home);
                if (!home_is_decorated(home))
                    demo_decorate(home);
                mark_home_modified(home);
                cb_mesh_upload(home, slot, &stream_ctx);
                if (sim_init(sim, home) != 0) {
                    fprintf(stderr, "main: sim_init re-attach failed\n");
                    sim->chunk = NULL;   /* stay detached; demo just won't run   */
                }
                /* Re-install the progression sink after re-attach (sim_init reset
                 * progress to NULL): the re-bound HOME resumes emitting into our
                 * ring so discoveries keep flowing as the player roams away and
                 * back. NULL prog_ring (alloc failed) detaches cleanly. */
                if (sim->chunk != NULL)
                    sim_set_progress_sink(sim, prog_ring);
            }
        }

        /* --- Heat sim: fixed 15 Hz tick decoupled from render (Sec 3.7) --- *
         * A millisecond accumulator separate from the render cadence: each frame
         * add this frame's already-clamped real dt, then drain whole SIM_TICK_MS
         * slices (one sim_tick each) up to SIM_MAX_TICKS_PER_FRAME. The cap stops
         * a catch-up spiral. Guarded by the sim being BOUND to the still-resident
         * HOME chunk (the crash-avoidance rule: never tick a recycled slab). */
        if (sim->chunk != NULL
            && world_get(world, HOME_CX, HOME_CY, HOME_CZ) == sim->chunk) {
            sim_accum_ms += real_dt_ms;
            {
                int ticks = 0;
                while (sim_accum_ms >= SIM_TICK_MS
                       && ticks < SIM_MAX_TICKS_PER_FRAME) {
                    sim_accum_ms -= SIM_TICK_MS;
                    sim_tick(sim);
                    ++ticks;
                }
                if (ticks == SIM_MAX_TICKS_PER_FRAME && sim_accum_ms > SIM_TICK_MS)
                    sim_accum_ms = SIM_TICK_MS;
            }

            /* --- Dirty-chunk remesh drain (the sim's CHUNK_DIRTY_MESH path) --- *
             * The sim sets s->dirty_mesh when a tick changed visible state (a
             * glow, a melt, a fluid move). Re-run the full per-chunk mesh-time
             * pipeline on the resident HOME chunk into its current render slot.
             * The sim's edits sit interior to HOME (never a boundary voxel), so
             * no neighbour remesh is needed. greedy_mesh reads neigh[6], so HOME
             * still culls its seams against its resident neighbours every remesh.
             * dirty_mesh means a committed temp / material / fill change (sim.h),
             * i.e. HOME's persistable voxels MATERIALLY changed - so flag it
             * CHUNK_MODIFIED (Section 8 modified policy) to write the evolved
             * state back on the next eviction / shutdown. */
            if (sim->dirty_mesh) {
                Chunk *sc = sim->chunk;
                int slot = world_render_slot(world, sc);
                sc->flags |= CHUNK_DIRTY_MESH;
                cb_mesh_upload(sc, slot, &stream_ctx);
                sc->flags &= (uint8_t)~CHUNK_DIRTY_MESH;
                mark_home_modified(sc);
                sim->dirty_mesh = 0;
            }
        }

        /* --- Progression observer drain (ARCHITECTURE Section 9) --------- *
         * Empty the sim's event ring into the observer ONCE per frame, AFTER the
         * sim-tick loop and in the FRAME SLACK band (never the 14 ms sim budget).
         * For each drained event prog_observe_drain tightens the empirical
         * material journal and, on the FIRST occurrence of a (kind,material) pair,
         * fires the deduped discovery flash to the observer's log (stderr) - e.g.
         * "DISCOVERY: copper melts (observed ~1080 C)". This is OUTSIDE the
         * sim-bound guard on purpose: the ring carries VALUES, so events the sim
         * pushed this frame are still drainable even if HOME just detached, and a
         * drain of an empty ring is a couple of cheap loads. Read-only contract:
         * the observer only CONSUMES the ring + reads the const MaterialDef table;
         * it never writes a voxel or a sim field, so removing this call (or running
         * with prog_state == NULL) leaves the simulation byte-identical. NULL-safe
         * when the observer alloc failed. */
        if (prog_state != NULL)
            prog_observe_drain(prog_state, prog_ring);

        /* --- Build the model-view-projection (column-major) -------------- */
        {
            float proj[16];
            float view[16];
            float mvp[16];
            /* Use the LIVE window size for the aspect ratio so a user resize /
             * maximize doesn't distort the view (the backend has already resized
             * the GL viewport via WM_SIZE / ConfigureNotify). Falls back to the
             * creation size if the platform reports nothing yet; headless
             * VOXEL_SHOT never resizes, so this stays the fixed capture size. */
            int cur_w = win_w, cur_h = win_h;
            float aspect;
            uint32_t n, ri;

            plat_get_size(&cur_w, &cur_h);
            if (cur_w <= 0 || cur_h <= 0) { cur_w = win_w; cur_h = win_h; }
            aspect = (float)cur_w / (float)cur_h;

            mat4_perspective(proj, (float)(70.0 * M_PI / 180.0),
                             aspect, 0.1f, 1000.0f);
            mat4_lookat(view, cam_pos, cam_target, world_up);
            mat4_mul(mvp, proj, view);   /* mvp = proj * view (model = I)     */

            /* --- Render: begin opaque pass, draw every LIVE resident slot --- *
             * Walk the WorldStore's dense resident list (NOT the sparse hash or a
             * fixed slot range): for each resident chunk fetch the render slot it
             * owns and draw it. Frustum culling is deferred (Section 5.2) - the
             * window is small (<= 338 chunks) and the G70 eats the draw calls.
             * One glDrawElements per chunk inside render_draw_chunk; the atlas/
             * program/state are bound once in render_begin. Model matrix is
             * identity (positions are chunk-local; the shader adds u_chunk_origin
             * per chunk), so mvp == proj*view. */

            /* --- Live day/night sun (visual goal: animate u_sun, zero remeshes) -
             * Drive the sun term from the monotonic wall clock already read at the
             * frame top (frame_start, ms). One smooth dawn->noon->dusk->night cycle
             * every DAY_LENGTH_SEC: phase sweeps 0..2pi and sun = 0.5 + 0.5*sin,
             * giving a 0..1 ramp the shader folds live into v_bright with NO
             * remeshes. A small night FLOOR keeps the scene dark-blue-legible (the
             * shader's AMBIENT already prevents pure black) so block-light and the
             * heat glow stay readable through the night.
             *
             * Gated on a NON-shot session: VOXEL_SHOT captures a fixed frame, and a
             * moving sun would make those captures non-reproducible, so the headless
             * screenshot path keeps the frozen sun = 1.0f initialiser above. */
            if (shot_path == NULL) {
                double t     = frame_start / 1000.0;
                float  phase = (float)(t * (2.0 * M_PI / DAY_LENGTH_SEC));
                sun = 0.5f + 0.5f * sinf(phase);
                if (sun < 0.15f)
                    sun = 0.15f;             /* night floor: dark, not black       */
            }
            render_begin(mvp, sun);
            n = world_resident_count(world);
            for (ri = 0; ri < n; ++ri) {
                Chunk *c = world_resident_at(world, ri);
                int slot;
                if (c == NULL)
                    continue;
                slot = world_render_slot(world, c);
                if (slot >= 0)
                    render_draw_chunk(slot);
            }
            render_end();

            /* Block-target wireframe overlay + centre crosshair. Drawn after
             * render_end so they sit on top of the opaque + liquid scene. hl_have
             * is set by the per-frame raycast (live only); the crosshair is also
             * live-only, so VOXEL_SHOT captures stay clean and reproducible. */
            if (hl_have)
                render_highlight_voxel(hl_x, hl_y, hl_z);
            if (shot_path == NULL)
                render_crosshair(aspect);
        }

        /* Headless one-shot capture: grab the back buffer GL just drew (before
         * the swap), a few frames in so the context is warm, then exit. */
        ++frame_no;
        if (shot_path != NULL && frame_no >= shot_frame) {
            if (render_screenshot_ppm(shot_path, win_w, win_h) == 0)
                fprintf(stderr, "main: wrote screenshot %s\n", shot_path);
            else
                fprintf(stderr, "main: screenshot failed\n");
            break;
        }

        /* --- Present the back buffer (platform owns vsync) --------------- */
        plat_swap_buffers();

        /* --- Cap to ~30 FPS: sleep the bulk, spin the tail --------------- *
         * The platform layer has no sleep primitive in platform.h, so the cap is
         * a busy-wait on the monotonic clock. On the single-core M170 this burns
         * the slack rather than yielding, but it holds the 33.33 ms wall and
         * keeps the loop timing legible. */
        frame_used = plat_time_ms() - frame_start;
        sleep_ms   = FRAME_TARGET_MS - frame_used;
        if (sleep_ms > 0.0) {
            double deadline = frame_start + FRAME_TARGET_MS;
            while (plat_time_ms() < deadline) {
                /* spin until the frame's 33.33 ms wall is reached */
            }
        }
    }

    /* ---- 10. Teardown ---------------------------------------------------- *
     * Detach the sim first (if bound), then tear down the world (which evicts
     * every resident - returning all slabs + render slots - and frees the pool),
     * then close persistence, then the scratch buffers and the renderer. The
     * WorldStore owns all chunk storage, so there is no per-chunk free loop here
     * (unlike the old static grid): world_shutdown returns every slab to the pool
     * and frees the pool.
     *
     * ORDERING (Section 8, load-bearing): world_shutdown runs FIRST and is what
     * persists the edits - it evicts every resident (each CHUNK_MODIFIED chunk is
     * written back through the evict hook -> persist_save_chunk) and then
     * persist_flush()es the region headers/indices to disk, so it MUST see the
     * still-open PersistStore handle. persist_close() therefore comes AFTER
     * world_shutdown: the store is BORROWED by the world (world.c never closes
     * it), so this driver - the owner via persist_open - closes it last, landing
     * any buffered header/index and releasing the region file handles. NULL-safe
     * (persist_close ignores a NULL store), so an open that failed is fine.
     *
     * The PROGRESSION journal (ARCHITECTURE Section 9 "Beat 2 - Understand") is
     * dumped FIRST, before anything is torn down: prog_journal_dump prints the
     * ordered discovery list, the per-material observed facts (the empirical
     * melt/freeze ranges + pools), and the current emergent capability tier with
     * the materials now workable. It reads ONLY the observer state and the const
     * MaterialDef table - never the sim or world - so it is safe at the top of
     * teardown (and harmless if no discoveries were made). Then detach the sim's
     * borrowed sink and free the ring + observer (no-ops when the alloc failed). */
    if (prog_state != NULL)
        prog_journal_dump(prog_state);   /* discoveries + facts + tier (stderr)   */
    if (sim != NULL)
        sim_set_progress_sink(sim, NULL);/* drop the borrowed sink before freeing */
    free(prog_ring);
    free(prog_state);

    if (sim != NULL) {
        if (sim->chunk != NULL)
            sim_shutdown(sim);   /* clears the front/source tables               */
        free(sim);
    }
    if (world != NULL) {
        world_shutdown(world);   /* evicts -> writes back modified -> flushes     */
        free(world);
    }
    persist_close(persist);      /* close region handles + free the store (last)  */
    /* Release mouse capture so the cursor reappears on exit (idempotent; a no-op
     * in VOXEL_SHOT mode where capture was never enabled, and the backends also
     * auto-release on window close - this is the belt-and-braces explicit off). */
    plat_set_mouse_capture(0);
    mesh_buffer_free(&scratch);
    mesh_buffer_free(&liq_scratch);
    render_shutdown();

    return 0;
}

/* ---- Win32 entry-point shim --------------------------------------------- *
 * On the XP target the linker expects WinMain for a GUI subsystem .exe; this
 * trivially forwards to main() so the one portable entry point above serves both
 * backends. We do NOT include <windows.h> here (the engine core never pulls a
 * system header); the parameter types are spelled with stdint-compatible
 * primitives the Win32 ABI uses, matching opengl32/gdi32 linkage in
 * platform_win32.c. */
#ifdef _WIN32

#ifndef WINAPI
#define WINAPI __stdcall
#endif

int WINAPI WinMain(void *hInstance, void *hPrevInstance,
                   char *lpCmdLine, int nShowCmd)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nShowCmd;
    return main();
}

#endif /* _WIN32 */
