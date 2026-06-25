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
#include <string.h>   /* world-management UI: name/seed field editing */
#include <time.h>     /* world-create: a varied default seed */
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
#include "player.h"
#include "version.h"
#include "net.h"
#include "chunksync.h"

/* ---- Frame scheduling constants (binding, ARCHITECTURE.md Section 6) ------ */
#define FRAME_TARGET_MS   33.333    /* 30 FPS, binding TARGET_FRAMERATE        */

/* ---- World seed ---------------------------------------------------------- *
 * The whole world is a deterministic function of this one seed (Section 7):
 * same seed -> byte-identical terrain on every machine and every re-entry.
 * A fixed default keeps the headless screenshot reproducible; VOXEL_SEED lets a
 * developer roll a different landscape without a recompile. */
#define WORLD_SEED_DEFAULT  0x5EED1234ABCD0001ull

/* ---- The fixed HOME forge chunk (heat sim binds here) --------------------- *
 * The single-chunk heat SimState binds to ONE fixed world chunk so the forge
 * demo always lands at a known, REACHABLE spot the spawn camera frames. 0.4 M1
 * picks the CRUST chunk directly under the spawn pole: the player spawns above
 * the planet's north pole (world (8, 134, 8), main.c camera setup); the pole
 * SURFACE is world-Y 128 (= chunk cy 8), and HOME is the solid crust chunk just
 * beneath it - chunk (0,7,0), world-Y 112..127 - so demo_decorate can carve a
 * stone CAVITY into the crust and the player, standing on the surface, looks
 * straight down into it. (The (0,1,0) used through 0.3 was ~40 voxels inside the
 * solid core - a forge there was invisible/unreachable; this is the 0.4 M1 fix.)
 * HOME is in the resident band [WORLD_BAND_Y0..Y1] (0..8), so it streams in like
 * any other chunk. With persistence live, HOME is flagged CHUNK_MODIFIED after
 * decoration (and whenever the sim materially changes it), so flying away EVICTS
 * it to disk and flying back LOADS the evolved (carved + melted) state rather
 * than a fresh chunk; the edits survive a process restart. (If HOME re-loads
 * already decorated, home_is_decorated detects the held-lava marker and we skip
 * re-decoration, preserving the player/sim's evolved state.) */
#define HOME_CX           0
#define HOME_CY           7
#define HOME_CZ           0

/* Local-Y the camera aims at (the forge's lava level), offset by the HOME chunk's
 * world origin to give DEMO_WORLD_Y. The carved cavity + lava pool sit around
 * this row (local y 10..12 -> world-Y 122..124), so the default look point lands
 * on the glow. */
#define DEMO_FLOOR_Y      12

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

static Vec3 vec3_add(Vec3 a, Vec3 b)
{
    Vec3 r;
    r.x = a.x + b.x;
    r.y = a.y + b.y;
    r.z = a.z + b.z;
    return r;
}

static Vec3 vec3_scale(Vec3 a, float s)
{
    Vec3 r;
    r.x = a.x * s;
    r.y = a.y * s;
    r.z = a.z * s;
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

static float vec3_dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
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
#define NET_REQ_RING 2048   /* pending chunk-sync requests (holds a prime burst)  */

typedef struct {
    MeshBuffer *opaque;   /* greedy_mesh scratch (non-liquid stream)            */
    MeshBuffer *liquid;   /* greedy_mesh_liquid scratch (PHASE_LIQUID stream)   */
    /* 0.3 chunk sync: on a CLIENT, cb_gen enqueues each freshly-generated chunk
     * here; the main loop drains the ring into net_request_chunk so the host's
     * edits to that chunk (if any) are fetched + patched in. NULL net = no-op. */
    NetState   *net;
    int         req_cx[NET_REQ_RING], req_cy[NET_REQ_RING], req_cz[NET_REQ_RING];
    int         req_head, req_count;
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
    StreamCtx *ctx = (StreamCtx *)user;
    worldgen_fill_chunk(c, cx, cy, cz, seed);
    /* CLIENT: this chunk was just regenerated from the (shared) seed - ask the
     * host whether it has been edited, so any delta gets patched in. Enqueue;
     * the main loop sends a bounded number per frame. Drop if the ring is full
     * (re-requested when the chunk is next regenerated). */
    if (ctx != NULL && ctx->net != NULL && net_mode(ctx->net) == NET_CLIENT &&
        ctx->req_count < NET_REQ_RING) {
        int t = (ctx->req_head + ctx->req_count) % NET_REQ_RING;
        ctx->req_cx[t] = cx; ctx->req_cy[t] = cy; ctx->req_cz[t] = cz;
        ctx->req_count++;
    }
}

/* 0.5 M1 sparse-air: tells the WorldStore which chunks generate as WHOLLY AIR so
 * it skips the 16 KiB slab + the fill for them (the ~72%-air resident window).
 * Mirrors worldgen exactly. world_insert still runs cb_gen FIRST (so a CLIENT's
 * host-delta request above still fires), then collapses to uniform-air; an edited
 * sky chunk syncs in later via world_edit_voxel, which realizes a real block. */
static int cb_is_air(int cx, int cy, int cz, uint64_t seed, void *user)
{
    (void)seed; (void)user;
    return worldgen_chunk_all_air(cx, cy, cz);
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

/* 0.4: the placement HOTBAR - the 5 PLACE_MATS as colour swatches along the
 * bottom, the SELECTED slot ringed white with its material NAME above it (number
 * keys 1..5 switch). Tells the player which block a right-click will place. Drawn
 * over the scene via the overlay program (NDC, y up), live sessions only. */
static void draw_hotbar(float aspect, int sel_mat)
{
    const float slot = 0.12f, gap = 0.012f;
    const float total = 5.0f * slot + 4.0f * gap;
    const float x0 = -total * 0.5f;             /* centred along the bottom */
    const float yb = -0.97f, yt = -0.85f;
    const MaterialDef *selm = material_get(PLACE_MATS[sel_mat]);
    char nm[24];
    int i, k;
    const char *p;

    render_ui_rect(x0 - 0.02f, yb - 0.02f, x0 + total + 0.02f, yt + 0.02f,
                   0.0f, 0.0f, 0.0f, 0.55f);     /* backing panel */
    for (i = 0; i < 5; ++i) {
        const MaterialDef *m = material_get(PLACE_MATS[i]);
        float sx = x0 + (float)i * (slot + gap);
        char num[2];
        if (i == sel_mat)                        /* selected: white ring behind the swatch */
            render_ui_rect(sx - 0.013f, yb - 0.013f, sx + slot + 0.013f, yt + 0.013f,
                           1.0f, 1.0f, 1.0f, 0.95f);
        render_ui_rect(sx, yb, sx + slot, yt,
                       m->color_rgba[0] / 255.0f, m->color_rgba[1] / 255.0f,
                       m->color_rgba[2] / 255.0f, 1.0f);
        num[0] = (char)('1' + i); num[1] = '\0';
        render_text(sx + 0.007f, yt - 0.016f, 0.028f, aspect, 1, 1, 1, 1, num);
    }
    /* The selected block's NAME above its slot (the answer to "which block?"). */
    k = 0;
    for (p = selm->name; *p && k < (int)sizeof nm - 1; ++p) {
        char ch = *p;
        if (k == 0 && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');  /* Capitalise */
        nm[k++] = ch;
    }
    nm[k] = '\0';
    render_text(x0 + (float)sel_mat * (slot + gap), yt + 0.05f, 0.045f, aspect,
                1.0f, 0.95f, 0.55f, 1.0f, nm);
}

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

/* Player collision/buoyancy solidity (PlySolidFn): 1 = opaque (blocks), 2 =
 * liquid (passable, buoyant), 0 = passable. The OPAQUE test matches ray_solid,
 * so physics and break/place agree on what is solid (you never stand on water). */
static int ply_solid(void *ctx, int x, int y, int z)
{
    Voxel v = world_get_voxel((const WorldStore *)ctx, x, y, z);
    const MaterialDef *m = material_get(vox_mat(v));
    if (m->flags & MAT_OPAQUE)            return 1;
    if (m->phase == (uint8_t)PHASE_LIQUID) return 2;
    return 0;
}

/* Build the voxel to PLACE for material id m (liquids carry VF_LIQUID + full
 * fill; solids a full opaque cube), reusing the demo voxel makers. */
static Voxel place_voxel(uint8_t m)
{
    return (material_get(m)->phase == (uint8_t)PHASE_LIQUID) ? make_liquid(m)
                                                             : make_voxel(m);
}

/* ---- 0.4 M3a: the global logical clock (WorldClock) ---------------------- *
 * One monotone logical tick the heat sim reads as its tick_index, OWNED by the
 * frame loop rather than self-counted inside each SimState. Single-chunk this
 * milestone (byte-identical to 0.3: feeding the sim's own counter back in is a
 * no-op while one chunk self-counts), but it is the SHARED clock M3b feeds to
 * EVERY resident chunk so they tick on one logical time, and the clock player
 * EDITS are stamped against (the 0.5-lockstep hook - gap 2: an edit must apply at
 * the same logical tick on every peer). NOT persisted (session-local); a fresh
 * sim bind adopts the sim's reset value, so eviction/rebind rewinds exactly as
 * 0.3 did. */
static uint64_t g_world_tick     = 0;   /* logical tick fed to the sim each tick   */
static uint64_t g_last_edit_tick = 0;   /* world_tick of the most recent edit (hook) */

/* ---- 0.4 M3b: the world-wide CA active set ------------------------------- *
 * The heat CA is no longer a single bound chunk. The forge `sim` (main, below)
 * is the PRIMARY chunk; g_xsim[] holds ADDITIONAL chunks woken by cross-chunk
 * heat (M4 populates it - faces are CLOSED this milestone, so for the one-chunk
 * forge it stays EMPTY and the tick is byte-identical to 0.3). The per-frame tick
 * walks {forge} ∪ g_xsim in CANONICAL (cy,cz,cx) order so multi-chunk evolution
 * is reproducible (the determinism the host stream + future lockstep rely on).
 * Capped for memory (~80 KiB/SimState) and per-frame cost; over-cap wakes are
 * dropped (counted). Session-local: reset + freed per session. */
#define WORLDCA_MAX_XSIMS 48
static SimState *g_xsim[WORLDCA_MAX_XSIMS];
static int       g_xsim_cx[WORLDCA_MAX_XSIMS];  /* bind-time coords (eviction check) */
static int       g_xsim_cy[WORLDCA_MAX_XSIMS];
static int       g_xsim_cz[WORLDCA_MAX_XSIMS];
static int       g_nxsim   = 0;
static unsigned char g_push_buf[NET_CHUNK_MAX]; /* 0.4 M5: host chunk-stream scratch */

/* Canonical (cy,cz,cx) ordering predicate: 1 iff sim a sorts AFTER sim b. */
static int casim_after(const SimState *a, const SimState *b)
{
    const Chunk *ca = a->chunk, *cb = b->chunk;
    if (ca->cy != cb->cy) return ca->cy > cb->cy;
    if (ca->cz != cb->cz) return ca->cz > cb->cz;
    return ca->cx > cb->cx;
}

/* Free every woken extra sim and empty the set (per-session reset / teardown). */
static void worldca_reset_xsims(void)
{
    int i;
    for (i = 0; i < g_nxsim; ++i) {
        if (g_xsim[i] != NULL) {
            if (g_xsim[i]->chunk != NULL)
                sim_shutdown(g_xsim[i]);
            free(g_xsim[i]);
            g_xsim[i] = NULL;
        }
    }
    g_nxsim = 0;
}

/* The SimState simulating chunk c (the forge, or a woken neighbour), or NULL. */
static SimState *worldca_find(SimState *forge, const Chunk *c)
{
    int i;
    if (forge != NULL && forge->chunk == c)
        return forge;
    for (i = 0; i < g_nxsim; ++i)
        if (g_xsim[i]->chunk == c)
            return g_xsim[i];
    return NULL;
}

/* Bind a fresh SimState to chunk c (woken by cross-chunk heat) and add it to the
 * active set; returns the already-bound sim if any, NULL if the cap is hit or
 * alloc/init fails. The sink is installed so a woken chunk's discoveries surface. */
static SimState *worldca_wake_chunk(SimState *forge, Chunk *c, ProgressSink *sink)
{
    SimState *ns = worldca_find(forge, c);
    if (ns != NULL) return ns;
    if (g_nxsim >= WORLDCA_MAX_XSIMS) return NULL;
    ns = (SimState *)malloc(sizeof(SimState));
    if (ns == NULL) return NULL;
    ns->chunk = NULL;
    if (sim_init(ns, c) != 0) { free(ns); return NULL; }
    sim_set_progress_sink(ns, sink);
    if (getenv("VOXEL_DEBUG_EDITS"))
        fprintf(stderr, "CA: woke chunk (%d,%d,%d) [cross-chunk heat]\n",
                c->cx, c->cy, c->cz);
    g_xsim_cx[g_nxsim] = c->cx;
    g_xsim_cy[g_nxsim] = c->cy;
    g_xsim_cz[g_nxsim] = c->cz;
    g_xsim[g_nxsim++]  = ns;
    return ns;
}

/* Per-world-tick context for the cross-chunk callbacks (passed as `user`). */
#define WORLDCA_WAKE_MAX 256u
typedef struct {
    WorldStore *world;
    SimState   *forge;
    Chunk      *wake_chunk[WORLDCA_WAKE_MAX];
    uint16_t    wake_li[WORLDCA_WAKE_MAX];
    int         n_wake;
} WorldCATickCtx;

/* SimNeighFn: a cross-chunk neighbour's START-OF-TICK heat + material. An active
 * neighbour's live heat[] IS its start-of-tick value (all chunks READ before any
 * COMMIT); an inactive resident neighbour decodes its (static) voxel temp. A
 * non-resident neighbour returns 0 = closed wall. */
static int worldca_nfn(void *user, const SimState *s, int face,
                       int nlx, int nly, int nlz, int32_t *out_heat, uint8_t *out_mat)
{
    WorldCATickCtx *cx = (WorldCATickCtx *)user;
    Chunk    *nc = s->chunk->neigh[face];
    SimState *ns;
    int nli;
    if (nc == NULL) return 0;
    nli = vox_index(nlx, nly, nlz);
    ns  = worldca_find(cx->forge, nc);
    /* 0.5 M1: the neighbour chunk may be uniform-air (NULL voxels) - read via
     * chunk_vox, which returns its uniform_word (the worldgen air word, ambient
     * temp), so an inactive air neighbour decodes the same heat as a dense one. */
    *out_mat  = vox_mat(chunk_vox(nc, nli));
    *out_heat = (ns != NULL) ? ns->heat[nli]
                             : temp_to_heat(vox_temp_code(chunk_vox(nc, nli)));
    return 1;
}

/* SimWakeFn: record (deferred) a request to wake the neighbour chunk's boundary
 * voxel - processed after the commit pass, so it first ticks NEXT world tick
 * (enqueue-only => order-independent). Bounded; excess requests just retry. */
static void worldca_wfn(void *user, const SimState *s, int face,
                        int nlx, int nly, int nlz)
{
    WorldCATickCtx *cx = (WorldCATickCtx *)user;
    Chunk *nc = s->chunk->neigh[face];
    if (nc == NULL || (unsigned)cx->n_wake >= WORLDCA_WAKE_MAX) return;
    cx->wake_chunk[cx->n_wake] = nc;
    cx->wake_li[cx->n_wake]    = (uint16_t)vox_index(nlx, nly, nlz);
    ++cx->n_wake;
}

/* Apply a player edit at world voxel (wx,wy,wz); if that voxel lives in the
 * chunk the sim is bound to, wake the sim around it so heat/fluid react. */
static void edit_and_notify(WorldStore *w, SimState *s, int wx, int wy, int wz, Voxel v)
{
    if (!world_edit_voxel(w, wx, wy, wz, v))
        return;
    /* 0.4 M3a: stamp the edit with the current logical tick. A LOCAL field in
     * 0.4 (the on-wire tick lands with the M5 protocol bump); it is the
     * 0.5-lockstep hook so edits can later be applied at the same tick on every
     * peer. VOXEL_DEBUG_EDITS surfaces it; default-off, so behaviour is unchanged. */
    g_last_edit_tick = g_world_tick;
    if (getenv("VOXEL_DEBUG_EDITS"))
        fprintf(stderr, "edit (%d,%d,%d) @ world_tick %llu\n",
                wx, wy, wz, (unsigned long long)g_last_edit_tick);
    if (s != NULL && s->chunk != NULL) {
        int cx = floordiv16(wx), cy = floordiv16(wy), cz = floordiv16(wz);
        if (s->chunk->cx == cx && s->chunk->cy == cy && s->chunk->cz == cz)
            sim_notify_edit(s, vox_index(wx - cx * 16, wy - cy * 16, wz - cz * 16));
    }
}

/* net_drain_edits callback: apply a REMOTE edit to the local world exactly like a
 * local one. Threaded a {world, sim} context so it reaches edit_and_notify. */
typedef struct { WorldStore *w; SimState *s; } NetApplyCtx;
static void apply_net_edit(int wx, int wy, int wz, uint32_t voxel, void *user)
{
    NetApplyCtx *c = (NetApplyCtx *)user;
    edit_and_notify(c->w, c->s, wx, wy, wz, (Voxel)voxel);
}
/* The chunk-delta serialize/apply live in chunksync.c (world-coupled, unit-tested);
 * main.c just wires chunksync_serve/chunksync_apply into the net callbacks. */

static void demo_decorate(Chunk *c)
{
    int x, y, z;
    const Voxel air    = make_voxel(MAT_AIR);
    const Voxel lava   = make_voxel(MAT_LAVA);    /* MAT_EMISSIVE: sim auto-holds  */
    const Voxel copper = make_voxel(MAT_COPPER);

    if (c->cx != HOME_CX || c->cy != HOME_CY || c->cz != HOME_CZ)
        return;

    /* 0.4 M1 - a REACHABLE SURFACE FORGE. HOME is the crust chunk directly under
     * the spawn pole (chunk (0,7,0), world-Y 112..127); the planet's north-pole
     * surface is world-Y 128, one voxel above the chunk top. The chunk arrives
     * from worldgen as solid stone, so we CARVE a stone CAVITY into it and stand a
     * held lava pool inside with a copper charge SUBMERGED: the surrounding stone
     * is a poor conductor (emergent insulation), so the trapped heat drives the
     * charge past copper's 1085 C melt point and it MELTS into molten copper - no
     * recipe, just heat crossing a MaterialDef threshold (the forge thesis). The
     * cavity opens at the chunk top so the glow reads from the surface above.
     * Everything is INTERIOR in x/z (the single-chunk sim treats out-of-chunk
     * faces as a closed wall; the top opening at y=15 is a closed wall too, which
     * only helps here - it traps heat). Player-lit COMBUSTION (fuel as a real
     * progression axis) is the 0.5 headline; 0.4 hands the player a NATURAL lava
     * source to trap and learn from. */

    /* The lava POOL: a 3x3x3 block (x7..9, z7..9, y10..12 -> world-Y 122..124),
     * walled and floored by the surrounding un-carved STONE (the insulating
     * cavity) so the finite lava cannot drain. MAT_LAVA is MAT_EMISSIVE, so
     * sim_init auto-registers every lava voxel as a held ~1150 C Dirichlet source. */
    for (x = 7; x <= 9; ++x)
        for (z = 7; z <= 9; ++z)
            for (y = 10; y <= 12; ++y)
                chunk_set(c, x, y, z, lava);

    /* The copper CHARGE, fully submerged at the pool centre (8,11,8): lava on all
     * six faces (the proven melt rig from test_sim/test_progress). It banks its
     * latent heat of fusion over many ticks, plateaus at the melt point, then
     * flips to molten copper - the emergent smelter. */
    chunk_set(c, 8, 11, 8, copper);

    /* Open a 3x3 viewing shaft above the pool (x7..9, z7..9, y13..15 -> world-Y
     * 125..127) up to the chunk top, so the glow + the melt read from the surface
     * (the chunk above, cy 8, is open air over the pole). The lava's top face
     * (y12) vents into this shaft, but lava is a HELD source (re-stamped each
     * tick), so venting never cools it - the submerged charge still melts. */
    for (x = 7; x <= 9; ++x)
        for (z = 7; z <= 9; ++z)
            for (y = 13; y <= 15; ++y)
                chunk_set(c, x, y, z, air);

    /* 0.4 M4: a lava CHIMNEY up the shaft centre to the chunk top (y15 = world-Y
     * 127, one below the pole surface). Its top voxel borders the chunk ABOVE, so
     * the forge's heat CROSSES the chunk seam and warms the crust there - waking
     * that chunk's CA (the world-wide cross-chunk diffusion, made visible). Held
     * (MAT_EMISSIVE), so it never drains; continuous with the pool below. */
    for (y = 13; y <= 15; ++y)
        chunk_set(c, 8, y, 8, lava);

    /* Two reachable copper-ORE voxels set into the shaft wall (x6/x10, y14): far
     * enough from the lava that they only warm, never melt - mineable ore the
     * player carries to the heat, vs the submerged charge that melts. (When the
     * player can MAKE heat, in 0.5, this is the loop's other half.) */
    chunk_set(c, 6, 14, 8, copper);
    chunk_set(c, 10, 14, 8, copper);
}

/* True iff HOME already carries the forge (decorated this run, or LOADED from a
 * previous run's save). Probes the lava-pool floor centre (8,10,8): a fresh
 * worldgen HOME is solid STONE there (sub-surface crust), while a decorated/
 * persisted HOME has the held lava source there - and lava is a held heat source
 * the sim NEVER drains or melts, so the marker survives any amount of sim
 * evolution. This is the guard that makes the cross-restart demo correct: on a
 * re-run HOME loads its EVOLVED (carved + melted) voxels from disk and we must
 * NOT re-stamp the pristine authored decoration over them - that would CLOBBER
 * the melted copper / banked heat the player (sim) produced and we just reloaded. */
static int home_is_decorated(const Chunk *c)
{
    if (c == NULL)
        return 0;
    /* Probe the forge pool's floor centre (8,10,8): a held lava source the sim
     * NEVER drains or melts, so the marker survives any amount of sim evolution.
     * A fresh-gen HOME is solid stone there; a decorated/persisted one has lava. */
    return vox_mat(chunk_get(c, 8, 10, 8)) == MAT_LAVA;
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

/* ---- 0.3 pause menu (screen-space, drawn over the frozen frame) ---------- *
 * NDC: x,y in [-1,1], centre (0,0), y up. `aspect` keeps the font square. The
 * coordinates are tuned by eye against the headless VOXEL_MENU screenshot. */
/* 0.4 M2: the in-world progression JOURNAL HUD. Surfaces the read-only observer
 * (progress.c, console-only since 0.1) with the 0.3 text/UI primitives: a
 * transient discovery TOAST when a new (kind,material) is first observed, and a
 * toggleable JOURNAL panel (J) listing the most recent discoveries + the current
 * capability tier. `ps` is `const ProgressState *` - the read-only contract is
 * COMPILER-ENFORCED here, not just documented; the HUD only READS the observer
 * and never touches sim state (the byte-identical "remove the observer" invariant
 * is preserved). Drawn after render_end via the overlay program (NDC, y up). */
static void draw_journal_hud(const ProgressState *ps, float aspect,
                             int journal_open, int toast_active,
                             const char *toast_line)
{
    if (ps == NULL)
        return;

    /* Discovery TOAST - a banner near the top. The caller owns the lifetime
     * (toast_active); reads as something the player just SAW. */
    if (toast_active && toast_line != NULL && toast_line[0] != '\0') {
        render_ui_rect(-0.62f, 0.78f, 0.62f, 0.93f, 0.10f, 0.11f, 0.15f, 0.85f);
        render_text(-0.58f, 0.905f, 0.040f, aspect, 1.0f, 0.82f, 0.25f, 1.0f,
                    "DISCOVERY");
        render_text(-0.58f, 0.850f, 0.050f, aspect, 1.0f, 1.0f, 1.0f, 1.0f,
                    toast_line);
    }

    /* JOURNAL panel (J): recent discoveries (newest first) + the current tier. */
    if (journal_open) {
        char line[64];
        int  n     = prog_discovery_count(ps);
        int  shown = (n > 8) ? 8 : n;
        int  i;
        float y = 0.50f;
        render_ui_rect(-0.92f, -0.62f, 0.06f, 0.66f, 0.06f, 0.07f, 0.10f, 0.92f);
        render_text(-0.88f, 0.60f, 0.060f, aspect, 1.0f, 0.85f, 0.20f, 1.0f,
                    "JOURNAL");
        if (n == 0)
            render_text(-0.88f, 0.44f, 0.040f, aspect, 0.70f, 0.70f, 0.75f, 1.0f,
                        "nothing observed yet");
        for (i = 0; i < shown; ++i) {
            prog_discovery_text(ps, n - 1 - i, line, (int)sizeof line);
            render_text(-0.88f, y, 0.038f, aspect, 0.92f, 0.92f, 0.96f, 1.0f, line);
            y -= 0.075f;
        }
        {
            char tier[40], out[64];
            prog_tier_text(ps, tier, (int)sizeof tier);
            snprintf(out, sizeof out, "tier: %s", tier);
            render_text(-0.88f, -0.50f, 0.045f, aspect, 0.55f, 0.85f, 1.0f, 1.0f, out);
        }
        render_text(-0.88f, -0.58f, 0.030f, aspect, 0.50f, 0.50f, 0.55f, 1.0f,
                    "[J] close");
    }
}

static void draw_pause_menu(float aspect, int sel, int fullscreen_on)
{
    const char *labels[4];
    const float iy[4] = { 0.26f, 0.10f, -0.06f, -0.22f };   /* item baselines (text tops) */
    int i;

    render_ui_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.55f);      /* dim scene  */
    render_ui_rect(-0.46f, -0.52f, 0.46f, 0.56f, 0.10f, 0.11f, 0.15f, 0.92f); /* panel    */
    render_text(-0.20f, 0.50f, 0.11f, aspect, 1.0f, 0.85f, 0.20f, 1.0f, "PAUSED");

    labels[0] = "Resume";
    labels[1] = fullscreen_on ? "Fullscreen: On" : "Fullscreen: Off";
    labels[2] = "Main Menu";
    labels[3] = "Quit";
    for (i = 0; i < 4; ++i) {
        if (i == sel)                                                       /* selection bar */
            render_ui_rect(-0.40f, iy[i] - 0.075f, 0.40f, iy[i] + 0.035f, 0.22f, 0.42f, 0.80f, 0.85f);
        render_text(-0.34f, iy[i], 0.07f, aspect, 1.0f, 1.0f, 1.0f, 1.0f, labels[i]);
    }
    render_text(-0.40f, -0.36f, 0.04f, aspect, 0.65f, 0.65f, 0.70f, 1.0f,
                "Up/Down + Enter    Esc resumes");
}

/* ---- 0.3 connect screen (startup + quit-to-menu): Single / Host / Join / Quit *
 * Reuses the UI primitives; runs before any world exists. */
typedef enum { CHOICE_SINGLE = 0, CHOICE_HOST, CHOICE_JOIN, CHOICE_QUIT, CHOICE_ENV } ChoiceMode;
typedef struct {
    ChoiceMode mode;
    char       ip[64];
    uint64_t   seed;                /* 0.4: chosen world's seed (SINGLE/HOST via menu) */
    char       dir[WORLD_DIR_MAX];  /* 0.4: chosen world's save dir ('' => env/default) */
} ConnectChoice;

/* 0.4 world management: worlds live under this root as saves/<slug>/ with a
 * world.meta (persist.c). The headless VOXEL_SAVE path stays separate. */
#define WORLD_SAVE_ROOT "saves"
#define MENU_MAX_WORLDS 64

static unsigned g_create_salt = 0;   /* varies the default seed across worlds this run */

/* Resolve a create-screen seed field: empty => a varied random seed; all-digits =>
 * that number; otherwise FNV-hash the string (Minecraft-style typed seeds). */
static uint64_t menu_parse_seed(const char *s)
{
    const char *p;
    if (s == NULL || s[0] == '\0')
        return ((uint64_t)time(NULL) * 2654435761ULL)
             ^ ((uint64_t)(g_create_salt++) * 0x9E3779B97F4A7C15ULL)
             ^ 0xABCDEF0123456789ULL;
    for (p = s; *p; ++p)
        if (*p < '0' || *p > '9') break;
    if (*p == '\0')                              /* all digits => numeric */
        return (uint64_t)strtoull(s, NULL, 10);
    {                                            /* otherwise FNV-1a of the string */
        uint64_t h = 1469598103934665603ULL;
        for (p = s; *p; ++p) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
        return h;
    }
}

static void draw_connect_screen(float aspect, int sel, const char *ip)
{
    const float iy[4] = { 0.28f, 0.12f, -0.04f, -0.30f };   /* Single, Host, Join, Quit */
    char joinline[80];
    const char *jp;
    int i;

    render_ui_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.04f, 0.05f, 0.08f, 1.0f);     /* solid bg  */
    render_text(-0.57f, 0.62f, 0.085f, aspect, 1.0f, 0.85f, 0.20f, 1.0f, VOXEL_TITLE);

    /* Single Player + Host Game: plain items. */
    if (sel == 0) render_ui_rect(-0.42f, iy[0]-0.075f, 0.42f, iy[0]+0.035f, 0.22f,0.42f,0.80f,0.85f);
    render_text(-0.36f, iy[0], 0.07f, aspect, 1,1,1,1, "Single Player");
    if (sel == 1) render_ui_rect(-0.42f, iy[1]-0.075f, 0.42f, iy[1]+0.035f, 0.22f,0.42f,0.80f,0.85f);
    render_text(-0.36f, iy[1], 0.07f, aspect, 1,1,1,1, "Host Game");

    /* Join: shows the editable IP field (a cursor '_' when Join is selected). */
    if (sel == 2) render_ui_rect(-0.42f, iy[2]-0.075f, 0.42f, iy[2]+0.035f, 0.22f,0.42f,0.80f,0.85f);
    {
        int n = 0; const char *src = "Join: "; const char *p;
        for (p = src; *p && n < (int)sizeof joinline - 1; ++p) joinline[n++] = *p;
        jp = (ip && ip[0]) ? ip : "";
        for (p = jp; *p && n < (int)sizeof joinline - 1; ++p) joinline[n++] = *p;
        if (sel == 2 && n < (int)sizeof joinline - 1) joinline[n++] = '_';   /* cursor */
        joinline[n] = '\0';
    }
    render_text(-0.36f, iy[2], 0.07f, aspect, 1,1,1,1, joinline);

    if (sel == 3) render_ui_rect(-0.42f, iy[3]-0.075f, 0.42f, iy[3]+0.035f, 0.22f,0.42f,0.80f,0.85f);
    render_text(-0.36f, iy[3], 0.07f, aspect, 1,1,1,1, "Quit");

    render_text(-0.48f, -0.46f, 0.034f, aspect, 0.6f,0.6f,0.65f,1.0f,
                "Up/Down + Enter    type an IP to Join");
    (void)i;
}

/* 0.4 world-select screen: the saved worlds + a "Create New World" entry. */
static void draw_world_list(float aspect, const WorldInfo *worlds, int n, int sel, int want_host)
{
    int i;
    float y = 0.46f;
    render_ui_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.04f, 0.05f, 0.08f, 1.0f);
    render_text(-0.80f, 0.66f, 0.075f, aspect, 1.0f, 0.85f, 0.20f, 1.0f,
                want_host ? "Host: Select World" : "Select World");
    if (n == 0)
        render_text(-0.58f, 0.30f, 0.04f, aspect, 0.6f, 0.6f, 0.65f, 1.0f,
                    "(no worlds yet - create one below)");
    {   /* Show a 7-row window that keeps the selected world visible (scrolls). */
        int first = 0, last;
        if (n > 7) {
            int s = (sel < n) ? sel : n - 1;        /* "Create" (sel==n) -> pin to the tail */
            first = s - 3;
            if (first < 0) first = 0;
            if (first > n - 7) first = n - 7;
        }
        last = (first + 7 < n) ? first + 7 : n;
        if (first > 0)                              /* more-above indicator */
            render_text(-0.60f, 0.55f, 0.03f, aspect, 0.5f, 0.5f, 0.55f, 1.0f, "^ more");
        for (i = first; i < last; ++i) {
            if (i == sel)
                render_ui_rect(-0.66f, y - 0.06f, 0.66f, y + 0.035f, 0.22f, 0.42f, 0.80f, 0.85f);
            render_text(-0.60f, y, 0.052f, aspect, 1, 1, 1, 1, worlds[i].name);
            y -= 0.105f;
        }
        if (last < n)                               /* more-below indicator */
            render_text(-0.60f, y, 0.03f, aspect, 0.5f, 0.5f, 0.55f, 1.0f, "v more");
    }
    {                                                  /* "Create New World" = item index n */
        float cy = -0.46f;
        if (sel == n)
            render_ui_rect(-0.66f, cy - 0.06f, 0.66f, cy + 0.035f, 0.25f, 0.55f, 0.30f, 0.85f);
        render_text(-0.60f, cy, 0.052f, aspect, 0.7f, 1.0f, 0.75f, 1.0f, "+ Create New World");
    }
    render_text(-0.80f, -0.64f, 0.03f, aspect, 0.6f, 0.6f, 0.65f, 1.0f,
                "Up/Down + Enter    [D] delete    [Esc] back");
}

/* 0.4 create-world screen: a Name field + an optional Seed field (focus 0/1). */
static void draw_create_world(float aspect, const char *name, const char *seed, int focus)
{
    char line[96];
    int k;
    const char *p;
    render_ui_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.04f, 0.05f, 0.08f, 1.0f);
    render_text(-0.62f, 0.62f, 0.075f, aspect, 1.0f, 0.85f, 0.20f, 1.0f, "Create New World");

    if (focus == 0) render_ui_rect(-0.80f, 0.18f, 0.80f, 0.31f, 0.22f, 0.42f, 0.80f, 0.85f);
    k = 0;
    for (p = "Name: ";  *p && k < (int)sizeof line - 2; ++p) line[k++] = *p;
    for (p = name;      *p && k < (int)sizeof line - 2; ++p) line[k++] = *p;
    if (focus == 0 && k < (int)sizeof line - 1) line[k++] = '_';
    line[k] = '\0';
    render_text(-0.76f, 0.255f, 0.055f, aspect, 1, 1, 1, 1, line);

    if (focus == 1) render_ui_rect(-0.80f, -0.02f, 0.80f, 0.11f, 0.22f, 0.42f, 0.80f, 0.85f);
    k = 0;
    for (p = "Seed (optional): "; *p && k < (int)sizeof line - 2; ++p) line[k++] = *p;
    for (p = seed;                *p && k < (int)sizeof line - 2; ++p) line[k++] = *p;
    if (focus == 1 && k < (int)sizeof line - 1) line[k++] = '_';
    line[k] = '\0';
    render_text(-0.76f, 0.055f, 0.05f, aspect, 1, 1, 1, 1, line);

    render_text(-0.78f, -0.42f, 0.032f, aspect, 0.6f, 0.6f, 0.65f, 1.0f,
                "type to edit   Up/Down switch field   Enter create   Esc back");
}

/* 0.4 confirm-delete overlay. */
static void draw_confirm_delete(float aspect, const char *name)
{
    char line[96];
    int k;
    const char *p;
    render_ui_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.04f, 0.05f, 0.08f, 1.0f);
    render_text(-0.42f, 0.30f, 0.07f, aspect, 1.0f, 0.5f, 0.3f, 1.0f, "Delete World?");
    k = 0;
    line[k++] = '"';
    for (p = name; *p && k < (int)sizeof line - 2; ++p) line[k++] = *p;
    line[k++] = '"';
    line[k] = '\0';
    render_text(-0.52f, 0.08f, 0.05f, aspect, 1, 1, 1, 1, line);
    render_text(-0.54f, -0.20f, 0.04f, aspect, 0.7f, 0.7f, 0.75f, 1.0f,
                "[Enter] delete forever    [Esc] cancel");
}

/* Run the menu until the user picks a world to play/host or an IP to join. A
 * state machine: MENU (Single/Host/Join/Quit) -> WORLDS (select/create/delete)
 * -> CREATE (name + optional seed), Minecraft-style. win_w/win_h are fallbacks. */
static ConnectChoice connect_screen(int win_w, int win_h)
{
    enum { SCR_MENU, SCR_WORLDS, SCR_CREATE, SCR_CONFIRM_DEL };
    ConnectChoice c;
    int screen = SCR_MENU, sel = 0, want_host = 0;
    int ip_len = 0;
    char ip[64];
    WorldInfo worlds[MENU_MAX_WORLDS];
    int n_worlds = 0, wsel = 0;
    char cname[WORLD_NAME_MAX], cseed[24];
    int cname_len = 0, cseed_len = 0, cfocus = 0;
    int up_prev = 0, dn_prev = 0, en_prev = 0, esc_prev = 0, del_prev = 0;
    float mvp[16];

    ip[0] = cname[0] = cseed[0] = '\0';
    c.mode = CHOICE_QUIT; c.ip[0] = '\0'; c.dir[0] = '\0'; c.seed = 0;
    mat4_identity(mvp);

    /* Prime edge-detect prevs from the CURRENT key state (a still-held Enter from
     * the pause-menu "Main Menu" must not fire on frame 1). Drain stale text. */
    plat_poll();
    up_prev  = plat_key_down(PLAT_KEY_UP);
    dn_prev  = plat_key_down(PLAT_KEY_DOWN);
    en_prev  = plat_key_down(PLAT_KEY_ENTER);
    esc_prev = plat_key_down(PLAT_KEY_ESC);
    del_prev = plat_key_down(PLAT_KEY_D);
    { char drain[64]; plat_text_poll(drain, (int)sizeof drain); }

    for (;;) {
        int cw = win_w, ch = win_h, up_now, dn_now, en_now, esc_now, del_now, tn, i;
        char tb[64];
        float aspect;
        double t0 = plat_time_ms();

        plat_poll();
        if (plat_should_close()) { c.mode = CHOICE_QUIT; return c; }
        plat_get_size(&cw, &ch);
        if (cw <= 0 || ch <= 0) { cw = win_w; ch = win_h; }
        aspect = (float)cw / (float)ch;

        up_now  = plat_key_down(PLAT_KEY_UP);
        dn_now  = plat_key_down(PLAT_KEY_DOWN);
        en_now  = plat_key_down(PLAT_KEY_ENTER);
        esc_now = plat_key_down(PLAT_KEY_ESC);
        del_now = plat_key_down(PLAT_KEY_D);
        tn = plat_text_poll(tb, (int)sizeof tb);

        if (screen == SCR_MENU) {
            if (up_now && !up_prev) sel = (sel + 3) % 4;
            if (dn_now && !dn_prev) sel = (sel + 1) % 4;
            if (sel == 2)                            /* edit the IP only on Join */
                for (i = 0; i < tn; ++i) {
                    char k = tb[i];
                    if (k == 0x08) { if (ip_len > 0) ip[--ip_len] = '\0'; }
                    else if (ip_len < (int)sizeof ip - 1) { ip[ip_len++] = k; ip[ip_len] = '\0'; }
                }
            if (en_now && !en_prev) {
                if (sel == 0 || sel == 1) {          /* Single / Host -> world list */
                    want_host = (sel == 1);
                    n_worlds = persist_list_worlds(WORLD_SAVE_ROOT, worlds, MENU_MAX_WORLDS);
                    wsel = 0; screen = SCR_WORLDS;
                } else if (sel == 2 && ip_len > 0) {
                    int j; c.mode = CHOICE_JOIN;
                    for (j = 0; j <= ip_len; ++j) c.ip[j] = ip[j];
                    return c;
                } else if (sel == 3) { c.mode = CHOICE_QUIT; return c; }
            }
        } else if (screen == SCR_WORLDS) {
            int nitems = n_worlds + 1;               /* + "Create New World" */
            if (up_now && !up_prev) wsel = (wsel + nitems - 1) % nitems;
            if (dn_now && !dn_prev) wsel = (wsel + 1) % nitems;
            if (del_now && !del_prev && wsel < n_worlds) screen = SCR_CONFIRM_DEL;
            if (en_now && !en_prev) {
                if (wsel < n_worlds) {               /* play / host this world */
                    int j;
                    c.mode = want_host ? CHOICE_HOST : CHOICE_SINGLE;
                    c.seed = worlds[wsel].seed;
                    for (j = 0; j < WORLD_DIR_MAX - 1 && worlds[wsel].dir[j]; ++j) c.dir[j] = worlds[wsel].dir[j];
                    c.dir[j] = '\0';
                    return c;
                }
                cname[0] = cseed[0] = '\0';          /* -> Create New World */
                cname_len = cseed_len = cfocus = 0;
                screen = SCR_CREATE;
            }
            if (esc_now && !esc_prev) screen = SCR_MENU;
        } else if (screen == SCR_CREATE) {
            if ((up_now && !up_prev) || (dn_now && !dn_prev)) cfocus ^= 1;
            for (i = 0; i < tn; ++i) {
                char k = tb[i];
                if (cfocus == 0) {
                    if (k == 0x08) { if (cname_len > 0) cname[--cname_len] = '\0'; }
                    else if (cname_len < (int)sizeof cname - 1) { cname[cname_len++] = k; cname[cname_len] = '\0'; }
                } else {
                    if (k == 0x08) { if (cseed_len > 0) cseed[--cseed_len] = '\0'; }
                    else if (cseed_len < (int)sizeof cseed - 1) { cseed[cseed_len++] = k; cseed[cseed_len] = '\0'; }
                }
            }
            if (en_now && !en_prev && cname_len > 0) {
                uint64_t seed = menu_parse_seed(cseed);
                char dir[WORLD_DIR_MAX];
                if (persist_world_create(WORLD_SAVE_ROOT, cname, seed, dir, (int)sizeof dir) == 0) {
                    int j;
                    c.mode = want_host ? CHOICE_HOST : CHOICE_SINGLE;
                    c.seed = seed;
                    for (j = 0; j < WORLD_DIR_MAX - 1 && dir[j]; ++j) c.dir[j] = dir[j];
                    c.dir[j] = '\0';
                    return c;
                }                                    /* create failed: stay on the screen */
            }
            if (esc_now && !esc_prev) screen = SCR_WORLDS;
        } else {                                     /* SCR_CONFIRM_DEL */
            if (en_now && !en_prev) {
                if (wsel < n_worlds) persist_world_delete(worlds[wsel].dir);
                n_worlds = persist_list_worlds(WORLD_SAVE_ROOT, worlds, MENU_MAX_WORLDS);
                if (wsel > n_worlds) wsel = n_worlds;
                screen = SCR_WORLDS;
            }
            if (esc_now && !esc_prev) screen = SCR_WORLDS;
        }
        up_prev = up_now; dn_prev = dn_now; en_prev = en_now;
        esc_prev = esc_now; del_prev = del_now;

        render_begin(mvp, 1.0f);
        render_end();
        if      (screen == SCR_MENU)    draw_connect_screen(aspect, sel, ip);
        else if (screen == SCR_WORLDS)  draw_world_list(aspect, worlds, n_worlds, wsel, want_host);
        else if (screen == SCR_CREATE)  draw_create_world(aspect, cname, cseed, cfocus);
        else                            draw_confirm_delete(aspect, (wsel < n_worlds) ? worlds[wsel].name : "");
        plat_swap_buffers();

        { double used = plat_time_ms() - t0;         /* ~30 fps cap (no busy spin) */
          if (used < FRAME_TARGET_MS) { double dl = t0 + FRAME_TARGET_MS; while (plat_time_ms() < dl) {} } }
    }
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

    /* 0.3 multiplayer (optional). NULL = single-player (VOXEL_HOST/VOXEL_CONNECT
     * unset), and every net_* call below is then a no-op, so the single-player
     * path is byte-for-byte the pre-0.3 behaviour. A CLIENT adopts the host's
     * seed (below) and does NOT persist (the host's save is canonical). */
    NetState     *net = NULL;
    ChunkSyncCtx  csctx;          /* 0.3: context for the net chunk-sync callbacks */

    /* 0.3 session/connect-menu loop control (see the outer for(;;) below). */
    int           quit_program  = 0;   /* set by window-close / pause-Quit / screenshot */
    int           session_test  = 0;   /* VOXEL_SESSIONS leak-test mode                  */
    long          sessions_left = 0;
    int           fullscreen_on = 0;   /* persists across sessions (tracks the window)   */

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
    Vec3   world_up;       /* 0.3: per-frame RADIAL up (was fixed +Y)            */
    Vec3   planet_center;  /* 0.3: asteroid center of mass (gravity + camera up) */
    float  yaw;     /* degrees; seeds the initial heading from VOXEL_YAW        */
    float  pitch;   /* degrees; <0 looks down, clamped to +/-CAM_PITCH_LIMIT    */
    Vec3   cam_heading;  /* 0.2.1: persistent UNIT tangent heading (forward on the
                          * surface). Parallel-transported onto each frame's radial
                          * tangent plane instead of rebuilt from a global yaw +
                          * reference vector - the old ref-switch at |up.y|=0.99
                          * snapped the basis ~180 deg and flickered near the poles.
                          * Mouse yaw rotates it about the radial up; pitch tilts
                          * the look toward up. Drifts slightly over a full lap
                          * (sphere holonomy) - accepted (quaternion fix deferred). */

    /* Player embodiment (0.2): a WALKING physics body (player.c) is the default
     * live mode; FLY is the old free-fly translation, toggled with F, and FORCED
     * under VOXEL_SHOT so headless captures stay byte-identical. In walk mode the
     * eye (cam_pos) is DERIVED from the body: cam_pos = feet + eye-offset. */
    Player     player;
    PlyParams  pp = player_defaults();
    int        fly_mode = 0;     /* set at init: forced on under VOXEL_SHOT */

    /* 0.3 radial gravity: the player falls toward the asteroid center of mass
     * (same WG_PLANET_* the worldgen + camera use). */
    pp.center_x = (float)WG_PLANET_CX;
    pp.center_y = (float)WG_PLANET_CY;
    pp.center_z = (float)WG_PLANET_CZ;

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
    /* Headless connect-screen capture (verification): VOXEL_CONNECT_SHOT=1 with
     * VOXEL_SHOT renders one connect-screen frame (Join selected, sample IP) to the
     * PPM and exits, so the menu layout/font is eyeballable without an interactive run. */
    if (shot_path != NULL && getenv("VOXEL_CONNECT_SHOT")) {
        float mvp[16];
        int cw = win_w, ch = win_h;
        mat4_identity(mvp);
        plat_poll();
        plat_get_size(&cw, &ch);
        if (cw <= 0 || ch <= 0) { cw = win_w; ch = win_h; }
        render_begin(mvp, 1.0f);
        render_end();
        {   /* VOXEL_MENU_SCREEN selects which screen to capture (default: the menu). */
            const char *which = getenv("VOXEL_MENU_SCREEN");
            float a = (float)cw / (float)ch;
            if (which && strcmp(which, "worlds") == 0) {
                WorldInfo w[MENU_MAX_WORLDS];
                int n = persist_list_worlds(WORLD_SAVE_ROOT, w, MENU_MAX_WORLDS);
                draw_world_list(a, w, n, 0, 0);
            } else if (which && strcmp(which, "create") == 0) {
                draw_create_world(a, "My World", "", 0);
            } else if (which && strcmp(which, "confirm") == 0) {
                draw_confirm_delete(a, "My World");
            } else {
                draw_connect_screen(a, 2, "127.0.0.1");
            }
        }
        if (render_screenshot_ppm(shot_path, cw, ch) == 0)
            fprintf(stderr, "main: wrote connect-screen screenshot %s\n", shot_path);
        mesh_buffer_free(&scratch);
        mesh_buffer_free(&liq_scratch);
        render_shutdown();
        return 0;
    }

    /* ===== 0.3: outer SESSION / connect-menu loop ===================== *
     * Each iteration sets up a session (world/sim/persist/net), runs the frame
     * loop, then tears it down; "Main Menu" returns here. Headless / scripted runs
     * (VOXEL_SHOT, or VOXEL_HOST/VOXEL_CONNECT) bypass the menu and run exactly ONE
     * session from the env (pre-0.3 behaviour). VOXEL_SESSIONS=N runs N single-
     * player sessions back-to-back (a leak/double-free check for the loop). */
    {
        const char *sess_env = getenv("VOXEL_SESSIONS");
        session_test  = (sess_env && sess_env[0]) ? 1 : 0;
        sessions_left = session_test ? strtol(sess_env, NULL, 10) : 0;
        if (session_test && sessions_left < 1) sessions_left = 1;
    }
    quit_program = 0;
    for (;;) {
        ConnectChoice choice;
        int env_single = (shot_path != NULL)
                       || (getenv("VOXEL_HOST")    && getenv("VOXEL_HOST")[0])
                       || (getenv("VOXEL_CONNECT") && getenv("VOXEL_CONNECT")[0]);
        int sframes = 0;
        if (session_test)    { choice.mode = CHOICE_SINGLE; choice.ip[0] = '\0'; choice.dir[0] = '\0'; }
        else if (env_single) { choice.mode = CHOICE_ENV;    choice.ip[0] = '\0'; choice.dir[0] = '\0'; }
        else {
            choice = connect_screen(win_w, win_h);
            if (choice.mode == CHOICE_QUIT) break;        /* exit program from the menu */
        }

    /* 0.4 world management: when the menu chose a named world (choice.dir set), its
     * seed + save dir drive this session. Otherwise (headless/scripted: ENV,
     * session_test) fall back to the default/VOXEL_SEED seed + resolve_save_dir. */
    {
        int menu_world = (choice.dir[0] != '\0')
                      && (choice.mode == CHOICE_SINGLE || choice.mode == CHOICE_HOST);
        if (menu_world) {
            seed = choice.seed;
        } else {
            seed = WORLD_SEED_DEFAULT;
            seed_env = getenv("VOXEL_SEED");
            if (seed_env != NULL && seed_env[0] != '\0')
                seed = (uint64_t)strtoull(seed_env, NULL, 0);
        }
    }

    /* 0.3 multiplayer: open the network BEFORE world_init + persist. A CLIENT
     * completes the handshake here and ADOPTS the host's seed, so it generates the
     * byte-identical deterministic planet (no map transfer). The source is the
     * connect-screen choice (Host/Join) or, on the headless/scripted path, the env
     * (net_init_from_env). A failed Join returns to the menu (continue). */
    if (choice.mode == CHOICE_ENV) {
        net = net_init_from_env(seed, (uint32_t)VOXEL_VERSION_PACKED, (uint32_t)WG_GEN_VERSION);
    } else if (choice.mode == CHOICE_HOST) {
        net = net_host((unsigned short)NET_DEFAULT_PORT, seed,
                       (uint32_t)VOXEL_VERSION_PACKED, (uint32_t)WG_GEN_VERSION);
        if (net) fprintf(stderr, "net: hosting on port %d (seed %016llx)\n",
                         NET_DEFAULT_PORT, (unsigned long long)seed);
    } else if (choice.mode == CHOICE_JOIN) {
        net = net_join_str(choice.ip, (uint32_t)VOXEL_VERSION_PACKED, (uint32_t)WG_GEN_VERSION);
        if (net == NULL) continue;                        /* connect failed -> back to menu */
    } else {
        net = NULL;                                       /* CHOICE_SINGLE */
    }
    if (net != NULL && net_mode(net) == NET_CLIENT)
        seed = net_seed(net);
    /* cb_gen (on a client) enqueues chunk-sync requests here; init before world_prime. */
    stream_ctx.net       = net;
    stream_ctx.req_head  = 0;
    stream_ctx.req_count = 0;

    cb.gen         = cb_gen;
    cb.mesh_upload = cb_mesh_upload;
    cb.slot_free   = cb_slot_free;
    cb.is_air      = cb_is_air;     /* 0.5 M1: enable sparse-air (skip empty chunks) */
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
    if (net != NULL && net_mode(net) == NET_CLIENT) {
        /* CLIENT: the world is ephemeral on this machine - the HOST owns the
         * canonical save. We regenerate from the (shared) seed and apply the
         * host's relayed edits live; we do not open a local save. */
        persist = NULL;
    } else {
        /* 0.4: a menu-chosen world saves into ITS directory (saves/<slug>); the
         * headless/scripted path keeps the seed-derived saves/<hex> dir. */
        if (choice.dir[0] != '\0'
            && (choice.mode == CHOICE_SINGLE || choice.mode == CHOICE_HOST))
            save_dir = choice.dir;
        else
            save_dir = resolve_save_dir(save_dir_buf, sizeof(save_dir_buf), seed);
        persist = persist_open(save_dir, seed, WG_GEN_VERSION);
        if (persist == NULL)
            fprintf(stderr, "main: persist_open(%s) failed - edits will be "
                            "ephemeral this run\n", save_dir);
        else
            world_set_persist(world, persist);
    }

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
        net_shutdown(net);       /* close sockets (NULL-safe)                      */
        return 1;
    }
    sim->chunk = NULL;   /* unbound: nothing to tick until HOME is resident      */
    worldca_reset_xsims();  /* 0.4 M3b: empty the woken-neighbour set per session */

    /* 0.3 chunk-delta sync: install the net callbacks now that world/sim/persist
     * all exist. Host SERVES chunk deltas (current-vs-seed); client APPLIES them
     * to its seed-regenerated chunks. csctx must outlive the loop (it does). */
    csctx.world = world; csctx.persist = persist; csctx.seed = seed;
    if (net != NULL) {
        if (net_mode(net) == NET_HOST)   net_set_chunk_server(net, chunksync_serve, &csctx);
        if (net_mode(net) == NET_CLIENT) net_set_chunk_apply(net, chunksync_apply, &csctx);
    }

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
    /* 0.3 asteroid: start the eye just ABOVE the planet's north pole (outside the
     * ball) so a live WALK session falls radially onto the surface and a headless
     * fly capture frames the ball. XZ defaults to the planet axis (= cam_look). */
    cam_pos.x    = cam_look_x;
    cam_pos.y    = (float)(WG_PLANET_CY + WG_PLANET_R) + 6.0f;
    cam_pos.z    = cam_look_z;

    /* Player body: FLY is forced under VOXEL_SHOT (headless captures must not get
     * gravity/collision); a live session walks. Seed the feet so the EYE lands
     * exactly where cam_pos starts (feet = eye - eye-offset), so fly and walk
     * begin from the same viewpoint and the first WALK frame just falls onto the
     * pedestal below. */
    fly_mode = (shot_path != NULL);
    player.pos.x   = cam_pos.x;
    player.pos.y   = cam_pos.y - pp.eye;
    player.pos.z   = cam_pos.z;
    player.vel.x   = player.vel.y = player.vel.z = 0.0f;
    player.on_ground = 0;
    player.in_water  = 0;

    world_up.x = 0.0f;
    world_up.y = 1.0f;
    world_up.z = 0.0f;

    /* 0.3 asteroid: gravity + the camera's "up" point at this center of mass. */
    planet_center.x = (float)WG_PLANET_CX;
    planet_center.y = (float)WG_PLANET_CY;
    planet_center.z = (float)WG_PLANET_CZ;

    yaw   = init_yaw;
    pitch = init_pitch;
    if (pitch >  CAM_PITCH_LIMIT) pitch =  CAM_PITCH_LIMIT;
    if (pitch < -CAM_PITCH_LIMIT) pitch = -CAM_PITCH_LIMIT;

    /* Seed the persistent tangent heading from init_yaw at the spawn-pose up,
     * using the same construction the per-frame basis used to use, so the initial
     * framing is byte-identical to before. From here it is parallel-transported
     * (no global reference vector), so it never snaps at a pole. */
    {
        Vec3 up0 = vec3_normalize(vec3_sub(cam_pos, planet_center));
        Vec3 ref, east, north;
        float ry = yaw * (float)(M_PI / 180.0);
        if (fabsf(up0.y) < 0.99f) { ref.x = 0.0f; ref.y = 1.0f; ref.z = 0.0f; }
        else                      { ref.x = 1.0f; ref.y = 0.0f; ref.z = 0.0f; }
        east  = vec3_normalize(vec3_cross(ref, up0));
        north = vec3_cross(up0, east);
        cam_heading = vec3_normalize(vec3_add(vec3_scale(east, -sinf(ry)),
                                              vec3_scale(north, -cosf(ry))));
    }

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
            if (sim->chunk != NULL) {
                sim_set_progress_sink(sim, prog_ring);
                g_world_tick = sim->tick_index;  /* M3a: WorldClock adopts the fresh sim tick (0) */
            }

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
                    sim->tick_index = g_world_tick;     /* M3a: feed the shared clock */
                    sim_tick(sim);
                    g_world_tick = sim->tick_index;     /* read the advanced clock back */
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

    /* 0.3 pause menu. ESC toggles `paused`: the scene freezes + the cursor is
     * freed + the menu draws over the frozen frame. VOXEL_MENU=1 starts paused so
     * a headless VOXEL_SHOT can capture the menu. menu_sel: 0=Resume 1=Fullscreen
     * 2=Main Menu 3=Quit. The *_prev flags edge-trigger ESC + the nav keys.
     * (fullscreen_on is hoisted to main scope so it tracks the persistent window
     * state across a Main-Menu round-trip.) */
    int paused        = (getenv("VOXEL_MENU") != NULL);
    int menu_sel      = 0;
    int esc_prev = 0, up_prev = 0, down_prev = 0, enter_prev = 0;
    /* 0.4 M2: the in-world journal. J toggles it live; VOXEL_JOURNAL starts it
     * open so a headless VOXEL_SHOT can capture the panel (mirrors VOXEL_MENU).
     * The discovery TOAST is latched at the per-frame observer drain below and
     * shown until toast_until (wall-ms). All frame-local - never sim state. */
    int    journal_open = (getenv("VOXEL_JOURNAL") != NULL);
    int    j_prev = 0;
    double toast_until = 0.0;
    char   toast_line[48];
    toast_line[0] = '\0';
    sim_accum_ms = 0.0;            /* per-session: don't inherit the prior session's accumulator */

    /* The live-session capture-enable above ran unconditionally; if we start
     * paused (VOXEL_MENU), free the cursor to honour the menu's contract. */
    if (shot_path == NULL && paused)
        plat_set_mouse_capture(0);

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
        if (plat_should_close()) {
            quit_program = 1;            /* the window close button quits the program */
            break;
        }
        if (session_test && ++sframes >= 8)
            break;                       /* VOXEL_SESSIONS leak test: end this session */

        /* --- 0.3: pump the network (accept, flush, parse) once per frame --- *
         * ALWAYS (even while paused) so the connection stays alive + peers' edits
         * keep applying; a silent client would otherwise be dropped by the host.
         * No-op (NULL) in single-player. A client that lost the host drops to local. */
        net_poll(net);

        /* --- 0.3 pause menu: ESC toggles it (no longer quits); arrow keys + Enter
         * navigate while paused. Live only (VOXEL_SHOT has no input). ESC opening
         * the menu frees the cursor so you can drag-resize / use the WM; resuming
         * re-grabs it (which also zeroes the mouse delta -> no camera snap). */
        if (shot_path == NULL) {
            int esc_now   = plat_key_down(PLAT_KEY_ESC);
            int up_now    = plat_key_down(PLAT_KEY_UP);
            int down_now  = plat_key_down(PLAT_KEY_DOWN);
            int enter_now = plat_key_down(PLAT_KEY_ENTER);
            if (esc_now && !esc_prev) {
                paused = !paused;
                menu_sel = 0;
                plat_set_mouse_capture(paused ? 0 : 1);
            }
            if (paused) {
                if (up_now   && !up_prev)    menu_sel = (menu_sel + 3) % 4;  /* up   */
                if (down_now && !down_prev)  menu_sel = (menu_sel + 1) % 4;  /* down */
                if (enter_now && !enter_prev) {
                    if (menu_sel == 0) {                 /* Resume */
                        paused = 0;
                        plat_set_mouse_capture(1);
                    } else if (menu_sel == 1) {          /* Fullscreen */
                        fullscreen_on = !fullscreen_on;
                        plat_set_fullscreen(fullscreen_on);
                    } else if (menu_sel == 2) {          /* Main Menu: leave session -> connect screen */
                        break;                           /* quit_program stays 0 */
                    } else {                             /* Quit: exit the program */
                        quit_program = 1;
                        break;
                    }
                }
            }
            esc_prev = esc_now; up_prev = up_now; down_prev = down_now; enter_prev = enter_now;

            /* 0.4 M2: J toggles the in-world journal (live, not while paused). */
            {
                int j_now = plat_key_down(PLAT_KEY_J);
                if (j_now && !j_prev && !paused)
                    journal_open = !journal_open;
                j_prev = j_now;
            }
        }

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
         * streaming capture (the player roaming so leading/trailing edges show).
         * 0.3: the ENTIRE input/camera/movement/edit block is gated on !paused, so
         * the menu freezes the view (cam_pos/target/world_up keep last frame's
         * values) and ignores look/move/break/place while open. */
        if (!paused) {
            Vec3 fwd;        /* full 3D look direction (heading tilted by pitch) */
            Vec3 fwd_flat;   /* tangent forward on the surface (the heading)     */
            Vec3 right;
            float move = CAM_MOVE_PER_SEC * dt_s;   /* fly-mode translate speed */
            float rp, cp;
            float yaw_delta = 0.0f;  /* this frame's mouse yaw, applied to heading */

            if (shot_path == NULL) {
                int mdx = 0, mdy = 0;
                plat_mouse_delta(&mdx, &mdy);
                yaw_delta = (float)mdx * CAM_MOUSE_SENS; /* rotate heading below  */
                pitch -= (float)mdy * CAM_MOUSE_SENS;   /* +down -> look down  */
                if (pitch >  CAM_PITCH_LIMIT) pitch =  CAM_PITCH_LIMIT;
                if (pitch < -CAM_PITCH_LIMIT) pitch = -CAM_PITCH_LIMIT;
            }

            /* --- 0.2.1 RADIAL camera basis (parallel-transported heading) ----
             * "up" is radial (away from the asteroid center) and rotates as the
             * eye moves. The heading (tangent forward) is PERSISTENT, not rebuilt
             * from a global yaw + a reference vector: each frame we transport it
             * onto the new up's tangent plane (drop its radial component,
             * renormalize), then apply this frame's mouse yaw as a rotation about
             * up. The old code picked a reference axis (world +Y, or +X near a
             * pole) and a hard switch at |up.y|=0.99 snapped the whole frame ~180
             * deg whenever up.y jittered across it - the pole flicker. With no
             * global reference there is NO such singularity (only looking straight
             * along the radius, handled by the degenerate-heading rebuild + the
             * pitch clamp).
             *   fwd_flat = tangent forward on the surface (the heading)
             *   fwd      = look direction (fwd_flat tilted toward up by pitch)
             *   right    = tangent right (for strafing) */
            {
                Vec3 up_l = vec3_normalize(vec3_sub(cam_pos, planet_center));
                float d   = vec3_dot(cam_heading, up_l);
                Vec3  h   = vec3_sub(cam_heading, vec3_scale(up_l, d)); /* -> tangent */
                float hl  = (float)sqrt((double)vec3_dot(h, h));
                world_up  = up_l;

                if (hl > 1e-4f) {
                    h = vec3_scale(h, 1.0f / hl);
                } else {
                    /* heading became (anti)parallel to up (looking along the
                     * radius): rebuild ANY tangent direction deterministically. */
                    Vec3 ref;
                    if (fabsf(up_l.y) < 0.99f) { ref.x = 0.0f; ref.y = 1.0f; ref.z = 0.0f; }
                    else                       { ref.x = 1.0f; ref.y = 0.0f; ref.z = 0.0f; }
                    h = vec3_normalize(vec3_cross(ref, up_l));
                }

                /* Mouse yaw: rotate h about up_l. h is already perpendicular to
                 * up_l, so Rodrigues reduces to h*cos + right*sin, where
                 * right = cross(h, up_l). +yaw_delta turns toward camera-right
                 * (validated to match the old +yaw sense - no inversion). */
                if (yaw_delta != 0.0f) {
                    float a = yaw_delta * (float)(M_PI / 180.0);
                    Vec3  rr = vec3_cross(h, up_l);
                    h = vec3_normalize(vec3_add(vec3_scale(h, cosf(a)),
                                                vec3_scale(rr, sinf(a))));
                }
                cam_heading = h;          /* persist for the next frame */

                rp = pitch * (float)(M_PI / 180.0);
                cp = cosf(rp);
                fwd_flat = h;
                fwd = vec3_normalize(vec3_add(vec3_scale(fwd_flat, cp),
                                              vec3_scale(up_l, sinf(rp))));
                right = vec3_normalize(vec3_cross(fwd_flat, up_l));
            }

            /* F toggles FLY <-> WALK (live only, edge-triggered). Forbidden under
             * VOXEL_SHOT (fly is forced there). Entering WALK, seed the body from
             * the current eye so the view does not jump. */
            if (shot_path == NULL) {
                static int f_prev = 0;
                int f_now = plat_key_down(PLAT_KEY_F);
                if (f_now && !f_prev) {
                    fly_mode = !fly_mode;
                    if (!fly_mode) {                 /* entering WALK */
                        player.pos.x = cam_pos.x - world_up.x * pp.eye;
                        player.pos.y = cam_pos.y - world_up.y * pp.eye;
                        player.pos.z = cam_pos.z - world_up.z * pp.eye;
                        player.vel.x = player.vel.y = player.vel.z = 0.0f;
                        player.on_ground = 0;
                    }
                }
                f_prev = f_now;
            }

            if (fly_mode) {
                /* FLY: free-fly in the tangent frame; SPACE/LSHIFT move along the
                 * RADIAL up. VOXEL_FLY still auto-advances world +X (capture sweep). */
                Vec3 delta;
                delta.x = fly_per_frame; delta.y = 0.0f; delta.z = 0.0f;
                if (plat_key_down(PLAT_KEY_W)) delta = vec3_add(delta, vec3_scale(fwd_flat, move));
                if (plat_key_down(PLAT_KEY_S)) delta = vec3_sub(delta, vec3_scale(fwd_flat, move));
                if (plat_key_down(PLAT_KEY_D)) delta = vec3_add(delta, vec3_scale(right,    move));
                if (plat_key_down(PLAT_KEY_A)) delta = vec3_sub(delta, vec3_scale(right,    move));
                if (plat_key_down(PLAT_KEY_SPACE))  delta = vec3_add(delta, vec3_scale(world_up, move));
                if (plat_key_down(PLAT_KEY_LSHIFT)) delta = vec3_sub(delta, vec3_scale(world_up, move));
                cam_pos = vec3_add(cam_pos, delta);
            } else {
                /* WALK: yaw-relative wish direction from WASD -> AABB physics ->
                 * derive the eye from the body. Space jumps/swims up, Shift
                 * crouches/swims down. Diagonal is normalized (not faster). */
                PlyVec wish;
                float wl;
                wish.x = 0.0f; wish.y = 0.0f; wish.z = 0.0f;
                if (plat_key_down(PLAT_KEY_W)) { wish.x += fwd_flat.x; wish.y += fwd_flat.y; wish.z += fwd_flat.z; }
                if (plat_key_down(PLAT_KEY_S)) { wish.x -= fwd_flat.x; wish.y -= fwd_flat.y; wish.z -= fwd_flat.z; }
                if (plat_key_down(PLAT_KEY_D)) { wish.x += right.x;    wish.y += right.y;    wish.z += right.z; }
                if (plat_key_down(PLAT_KEY_A)) { wish.x -= right.x;    wish.y -= right.y;    wish.z -= right.z; }
                wl = sqrtf(wish.x*wish.x + wish.y*wish.y + wish.z*wish.z);
                if (wl > 1e-4f) { wish.x /= wl; wish.y /= wl; wish.z /= wl; }
                player_step(&player, &pp, wish,
                            plat_key_down(PLAT_KEY_SPACE), plat_key_down(PLAT_KEY_LSHIFT),
                            dt_s, ply_solid, world);
                /* eye sits along the RADIAL up from the body (not world +Y) */
                cam_pos.x = player.pos.x + world_up.x * pp.eye;
                cam_pos.y = player.pos.y + world_up.y * pp.eye;
                cam_pos.z = player.pos.z + world_up.z * pp.eye;
            }

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
                if (ray.hit && lc > 0) {
                    Voxel v = air_voxel();
                    edit_and_notify(world, sim, ray.hx, ray.hy, ray.hz, v);
                    net_send_edit(net, ray.hx, ray.hy, ray.hz, (uint32_t)v); /* tell peers */
                }
                if (ray.hit && rc > 0) {
                    /* Don't place a block INTO yourself: reject a place cell the
                     * body overlaps. In WALK the body is the collision SPHERE
                     * (center player.pos, radius pp.r_p) — an orientation-free test
                     * that works anywhere on the planet; the old world-Y AABB built
                     * from pos-as-feet wrongly blocked the GROUND cell in front of
                     * you once "up" stopped being +Y (e.g. on the asteroid's side),
                     * so right-click placement silently did nothing. FLY rejects
                     * only the eye cell. */
                    int blocked;
                    if (fly_mode) {
                        blocked = (ray.px == (int)floorf(cam_pos.x) &&
                                   ray.py == (int)floorf(cam_pos.y) &&
                                   ray.pz == (int)floorf(cam_pos.z));
                    } else {
                        /* closest point on the place cell [px,px+1]^3 to the sphere
                         * center, then sphere-overlap (squared distance < r_p^2). */
                        float qx = player.pos.x, qy = player.pos.y, qz = player.pos.z;
                        float dxp, dyp, dzp;
                        if (qx < (float)ray.px)        qx = (float)ray.px;
                        else if (qx > (float)ray.px+1) qx = (float)ray.px + 1.0f;
                        if (qy < (float)ray.py)        qy = (float)ray.py;
                        else if (qy > (float)ray.py+1) qy = (float)ray.py + 1.0f;
                        if (qz < (float)ray.pz)        qz = (float)ray.pz;
                        else if (qz > (float)ray.pz+1) qz = (float)ray.pz + 1.0f;
                        dxp = player.pos.x - qx; dyp = player.pos.y - qy; dzp = player.pos.z - qz;
                        blocked = (dxp*dxp + dyp*dyp + dzp*dzp) < pp.r_p * pp.r_p;
                    }
                    if (!blocked) {
                        Voxel v = place_voxel(PLACE_MATS[sel_mat]);
                        edit_and_notify(world, sim, ray.px, ray.py, ray.pz, v);
                        net_send_edit(net, ray.px, ray.py, ray.pz, (uint32_t)v); /* tell peers */
                    }
                }
            }
        }

        /* --- 0.3: apply remote edits + broadcast my pose ------------------- *
         * Apply edits received this frame (host's own + relayed peers') to the
         * local deterministic world, exactly like a local edit, BEFORE streaming
         * so a freshly-edited resident chunk re-meshes this same frame. Then send
         * my eye pose + tangent heading for the others' avatars. Both no-ops in
         * single-player. */
        {
            NetApplyCtx nctx;
            nctx.w = world; nctx.s = sim;
            net_drain_edits(net, apply_net_edit, &nctx);
        }
        if (net != NULL && !paused) {        /* don't broadcast a pose while in the menu */
            float npos[3] = { cam_pos.x, cam_pos.y, cam_pos.z };
            float nhd[3]  = { cam_heading.x, cam_heading.y, cam_heading.z };
            net_send_pose(net, npos, nhd);
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

        /* 0.3: send a bounded batch of chunk-sync requests (client only). The ring
         * was just filled by cb_gen for any chunks this stream step generated; the
         * host replies and net_poll patches them in over the next frames. Bounded
         * so the join-time prime burst (~window size) paces out smoothly. */
        if (net != NULL && net_mode(net) == NET_CLIENT && !paused) {
            int sent = 0;
            while (stream_ctx.req_count > 0 && sent < 64) {
                net_request_chunk(net, stream_ctx.req_cx[stream_ctx.req_head],
                                       stream_ctx.req_cy[stream_ctx.req_head],
                                       stream_ctx.req_cz[stream_ctx.req_head]);
                stream_ctx.req_head = (stream_ctx.req_head + 1) % NET_REQ_RING;
                stream_ctx.req_count--;
                ++sent;
            }
        }

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
                if (sim->chunk != NULL) {
                    sim_set_progress_sink(sim, prog_ring);
                    g_world_tick = sim->tick_index;  /* M3a: WorldClock adopts the rebound sim tick (0) */
                }
            }
        }

        /* --- Heat sim: world-wide CA tick (0.4 M4 - faces OPEN) ---------- *
         * Each world tick: rebuild the ACTIVE SET (forge, while HOME is the live
         * resident chunk, ∪ woken neighbours), sort canonical, READ every member
         * (cross-chunk boundaries -> the NEIGHBOUR'S START-OF-TICK heat via
         * worldca_nfn) before COMMITTING any, so a seam diffuses exactly like an
         * interior face and the tick is order-independent (testdeterminism proves
         * it). Cross-chunk wake requests collected during READ (worldca_wfn) are
         * processed AFTER commit, so a chunk that heat first reaches starts ticking
         * the NEXT world tick. Fluid faces stay CLOSED (inside sim.c).
         * 0.4 M5: CLIENTS run NO CA - host-authoritative streaming drives their
         * world (they apply the host's pushed deltas and render them). Single-
         * player (net==NULL) and the host run the CA. */
        if (!paused && (net == NULL || net_mode(net) != NET_CLIENT)) {
            WorldCATickCtx wctx;
            int ticks = 0, i;

            /* Retire woken neighbours whose chunk was evicted (compare the LIVE
             * chunk at the bind-time coords vs the bound pointer; never tick a
             * recycled slab). */
            {
                int w = 0;
                for (i = 0; i < g_nxsim; ++i) {
                    if (world_get(world, g_xsim_cx[i], g_xsim_cy[i], g_xsim_cz[i])
                            == g_xsim[i]->chunk) {
                        g_xsim[w] = g_xsim[i];
                        g_xsim_cx[w] = g_xsim_cx[i];
                        g_xsim_cy[w] = g_xsim_cy[i];
                        g_xsim_cz[w] = g_xsim_cz[i];
                        ++w;
                    } else {
                        if (g_xsim[i]->chunk != NULL) sim_shutdown(g_xsim[i]);
                        free(g_xsim[i]);
                    }
                }
                g_nxsim = w;
            }

            wctx.world = world;
            wctx.forge = sim;
            sim_accum_ms += real_dt_ms;
            while (sim_accum_ms >= SIM_TICK_MS && ticks < SIM_MAX_TICKS_PER_FRAME) {
                SimState *act[1 + WORLDCA_MAX_XSIMS];
                int na = 0, a, b;
                if (sim->chunk != NULL
                    && world_get(world, HOME_CX, HOME_CY, HOME_CZ) == sim->chunk)
                    act[na++] = sim;
                for (i = 0; i < g_nxsim; ++i)
                    act[na++] = g_xsim[i];
                if (na == 0)
                    break;                           /* nothing active to drain */
                for (a = 1; a < na; ++a) {           /* canonical (cy,cz,cx) order */
                    SimState *k = act[a];
                    for (b = a - 1; b >= 0 && casim_after(act[b], k); --b)
                        act[b + 1] = act[b];
                    act[b + 1] = k;
                }
                sim_accum_ms -= SIM_TICK_MS;
                wctx.n_wake = 0;
                for (i = 0; i < na; ++i) act[i]->tick_index = g_world_tick;
                for (i = 0; i < na; ++i)
                    sim_tick_ex(act[i], SIM_PHASE_READ, worldca_nfn, worldca_wfn, &wctx);
                for (i = 0; i < na; ++i)
                    sim_tick_ex(act[i], SIM_PHASE_COMMIT, NULL, NULL, NULL);
                ++g_world_tick;
                ++ticks;
                for (i = 0; i < wctx.n_wake; ++i) {  /* deferred cross-chunk wakes */
                    Chunk *nc = wctx.wake_chunk[i];
                    if (world_get(world, nc->cx, nc->cy, nc->cz) != nc)
                        continue;
                    /* 0.5 M1: the woken neighbour may be uniform-air - the CA reads
                     * AND writes its voxels, so give it a real block first (realize
                     * expands the air word so unheated voxels stay correct). */
                    if (world_realize(world, nc) != 0)
                        continue;
                    {
                        SimState *ns = worldca_wake_chunk(sim, nc, prog_ring);
                        if (ns != NULL)
                            sim_notify_edit(ns, (int)wctx.wake_li[i]);
                    }
                }
            }
            if (ticks == SIM_MAX_TICKS_PER_FRAME && sim_accum_ms > SIM_TICK_MS)
                sim_accum_ms = SIM_TICK_MS;

            /* Budgeted sim-remesh over the forge + all woken neighbours; over-
             * budget chunks remesh next frame. A CA-mutated chunk is
             * CHUNK_MODIFIED (persist on eviction) | CHUNK_MODIFIED_BY_SIM (the
             * host streams it in M5). */
            {
                SimState *all[1 + WORLDCA_MAX_XSIMS];
                int nall = 0, budget = WORLD_REMESH_BUDGET;
                int is_host = (net != NULL && net_mode(net) == NET_HOST);
                int push_budget = 4;
                if (sim->chunk != NULL) all[nall++] = sim;
                for (i = 0; i < g_nxsim; ++i) all[nall++] = g_xsim[i];
                for (i = 0; i < nall; ++i) {
                    Chunk *sc = all[i]->chunk;
                    if (sc == NULL || !all[i]->dirty_mesh)
                        continue;
                    sc->flags |= CHUNK_MODIFIED | CHUNK_MODIFIED_BY_SIM;
                    if (budget <= 0)
                        continue;
                    --budget;
                    sc->flags |= CHUNK_DIRTY_MESH;
                    cb_mesh_upload(sc, world_render_slot(world, sc), &stream_ctx);
                    sc->flags &= (uint8_t)~CHUNK_DIRTY_MESH;
                    all[i]->dirty_mesh = 0;
                }
                /* 0.4 M5: the HOST streams CA-changed chunks to clients. Push each
                 * CHUNK_MODIFIED_BY_SIM chunk's delta-from-seed (the same payload a
                 * CREQ reply builds, via chunksync_serve) and clear the flag; the
                 * CA re-flags it on the next change, so a backpressure-skipped
                 * client catches up. Clients run NO CA and just render these. */
                if (is_host) {
                    for (i = 0; i < nall && push_budget > 0; ++i) {
                        Chunk *sc = all[i]->chunk;
                        int plen;
                        if (sc == NULL || !(sc->flags & CHUNK_MODIFIED_BY_SIM))
                            continue;
                        plen = chunksync_serve(sc->cx, sc->cy, sc->cz,
                                               g_push_buf, (int)NET_CHUNK_MAX, &csctx);
                        if (plen > 0)
                            net_host_push_chunk(net, g_push_buf, plen);
                        sc->flags &= (uint8_t)~CHUNK_MODIFIED_BY_SIM;
                        --push_budget;
                    }
                }
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
        if (prog_state != NULL) {
            int newd = prog_observe_drain(prog_state, prog_ring);
            if (newd > 0) {        /* 0.4 M2: latch a toast for the newest discovery */
                prog_discovery_text(prog_state,
                                    prog_discovery_count(prog_state) - 1,
                                    toast_line, (int)sizeof toast_line);
                toast_until = frame_start + 4500.0;   /* ~4.5 s on screen */
            }
        }

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

            /* 0.3: remote-player avatars - drawn after render_end (so they sit in
             * the frame's depth + reuse its MVP), before the overlays. No-op in
             * single-player (count 0). */
            {
                int ai, acount = net_avatar_count(net);
                for (ai = 0; ai < acount; ++ai) {
                    float apos[3], ahd[3], acol[3];
                    int   aid;
                    if (net_get_avatar(net, ai, apos, ahd, &aid, acol))
                        render_avatar(apos, acol);
                }
            }

            /* Block-target wireframe overlay + centre crosshair. Drawn after
             * render_end so they sit on top of the opaque + liquid scene. hl_have
             * is set by the per-frame raycast (live only); the crosshair is also
             * live-only, so VOXEL_SHOT captures stay clean and reproducible. */
            if (hl_have)
                render_highlight_voxel(hl_x, hl_y, hl_z);
            if (shot_path == NULL)
                render_crosshair(aspect);
            /* 0.4 M2: the in-world journal HUD (discovery toast + the J panel),
             * beneath the pause menu. Drawn live AND in shot mode (so a headless
             * VOXEL_JOURNAL capture works); suppressed while paused so the menu
             * owns the screen. */
            if (!paused)
                draw_journal_hud(prog_state, aspect, journal_open,
                                 frame_start < toast_until, toast_line);
            /* 0.4: the placement hotbar - which block keys 1..5 will place.
             * Live + shot mode (so headless captures verify it), hidden while
             * paused. */
            if (!paused)
                draw_hotbar(aspect, sel_mat);
            if (paused)                          /* 0.3: pause menu over the frozen scene */
                draw_pause_menu(aspect, menu_sel, fullscreen_on);
        }

        /* Headless one-shot capture: grab the back buffer GL just drew (before
         * the swap), a few frames in so the context is warm, then exit. */
        ++frame_no;
        if (shot_path != NULL && frame_no >= shot_frame) {
            /* Capture at the LIVE drawable size, not the creation size: a WM that
             * does not honor the requested 1024x768 (tiling / forced-maximize)
             * resizes the window on map, and the viewport followed it; reading the
             * fixed creation size would then grab only a corner crop (or undefined
             * pixels). Mirror the MVP-aspect path's plat_get_size + fallback. */
            int cap_w = win_w, cap_h = win_h;
            plat_get_size(&cap_w, &cap_h);
            if (cap_w <= 0 || cap_h <= 0) { cap_w = win_w; cap_h = win_h; }
            if (render_screenshot_ppm(shot_path, cap_w, cap_h) == 0)
                fprintf(stderr, "main: wrote screenshot %s\n", shot_path);
            else
                fprintf(stderr, "main: screenshot failed\n");
            quit_program = 1;            /* headless one-shot: exit after the capture */
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

    worldca_reset_xsims();   /* 0.4 M3b: free any woken-neighbour sims first      */
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
    net_shutdown(net);           /* 0.3: close sockets + free NetState (NULL-safe) */

    /* ===== end of one session: NULL the per-session pointers so the next loop
     * iteration starts clean (no stale / double-free), then decide whether to go
     * back to the connect menu or exit the program. ================================ */
    world = NULL; sim = NULL; persist = NULL; net = NULL;
    prog_ring = NULL; prog_state = NULL;
    stream_ctx.net = NULL;
    if (quit_program || env_single)
        break;                                  /* program quit, or one-shot env run */
    if (session_test && --sessions_left <= 0)
        break;                                  /* leak test: N sessions done */
    }   /* ===== outer session / connect-menu loop ===== */

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
