/* player.h - player embodiment: AABB collision + gravity + walk/jump/swim.
 *
 * A world-agnostic, pure-math, allocation-free physics step modelled on
 * raycast.c: the caller supplies a `solid` predicate, so the engine drives it
 * from the streamed WorldStore and the unit tests drive it from a synthetic
 * grid. Float (render-side) only - it NEVER feeds the deterministic integer
 * simulation. The body is an axis-aligned box; collision resolves per-axis with
 * sub-steps so a fast move cannot tunnel through a thin wall.
 */
#ifndef PLAYER_H
#define PLAYER_H

/* Voxel solidity for the player. Returns:
 *   0 = passable (air / empty)
 *   1 = opaque-collidable (MAT_OPAQUE: the player stands on / is blocked by it)
 *   2 = liquid (passable but buoyant - the player wades/swims, never stands on)
 * Opaque-only solidity matches ray_solid, so physics and break/place agree on
 * what is solid. */
typedef int (*PlySolidFn)(void *ctx, int x, int y, int z);

typedef struct { float x, y, z; } PlyVec;

/* The player body. pos is the FEET reference: x,z are the AABB centre, y is the
 * bottom (feet) plane; the eye sits at pos.y + PlyParams.eye. */
typedef struct {
    PlyVec pos;
    PlyVec vel;
    int    on_ground;   /* resolved last step: standing on a solid     */
    int    in_water;    /* AABB currently overlaps a liquid cell        */
} Player;

typedef struct {
    float half_xz;        /* half AABB width/depth (0.3 -> 0.6 wide)     */
    float height;         /* AABB height (1.8)                           */
    float eye;            /* eye height above feet (1.62)                */
    float gravity;        /* downward accel, voxels/s^2 (28)             */
    float term_fall;      /* terminal fall speed in air (56)             */
    float walk_speed;     /* ground horizontal speed (4.3)               */
    float jump_vel;       /* initial jump up-velocity (8.4)              */
    float air_control;    /* 0..1 lerp of horizontal vel toward target in air */
    float water_xy_scale; /* horizontal speed scale in water (0.5)       */
    float water_term;     /* terminal sink speed in water (3.0)          */
    float swim_vel;       /* swim up/down speed in water (4.0)           */
    float max_substep;    /* max move per collision sub-step, voxels (0.45) */
} PlyParams;

/* The tuned default parameters (Minecraft-ish feel). */
PlyParams player_defaults(void);

/* Advance the player one frame. wish_dir is the already-yaw-rotated desired
 * horizontal direction in world XZ (unit length, or zero for no input);
 * want_jump = jump/swim-up held (Space), want_down = crouch/swim-down (Shift).
 * dt is seconds (clamped internally to keep collision sub-stepping bounded).
 * solid(ctx,x,y,z) reports voxel solidity. Updates p->pos/vel/on_ground/in_water. */
void player_step(Player *p, const PlyParams *pp, PlyVec wish_dir,
                 int want_jump, int want_down, float dt,
                 PlySolidFn solid, void *ctx);

#endif /* PLAYER_H */
