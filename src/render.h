/* render.h - The renderer API: forward, two-pass, GL2.1/GLSL1.20.
 *
 * Binding source: ARCHITECTURE.md Section 5. The renderer owns the GL context
 * and every VBO/IBO handle. It reads MeshBuffers (mesher.h, the canonical
 * 12-byte vertex) and uploads them to per-chunk VBOs; one glDrawElements per
 * chunk; the atlas + shader + GL state are bound once per pass.
 *
 * The shader contract the render.c author must match exactly is documented at
 * the bottom of this header. light arrives as one NON-normalized byte (lo
 * nibble baked sky, hi nibble temp glow) and ao as one NORMALIZED byte; the
 * sun term is folded LIVE against u_sun, never baked.
 */
#ifndef RENDER_H
#define RENDER_H

#include "mesher.h"

/* A small fixed slot table; one VBO per resident chunk. Sized for the visible
 * working set, not the whole 10,000-chunk window (most chunks are culled,
 * underground, or empty). The shipping engine keys slots off chunk residency. */
#define MAX_RENDER_CHUNKS 65536  /* covers the radius-32 x band-13 CEILING window (54925)
                                  * + churn curtain. A render slot is a few handles +
                                  * counts, so this fixed array is ~2-3 MiB - cheap; the
                                  * big runtime cost is the voxel slab pool, sized to the
                                  * actual view distance at world_init, not this ceiling. */

/* Initialise the renderer: compile + link the opaque (and liquid) shaders,
 * build the placeholder material atlas (1024x1024 RGBA8, 16x16 grid of 64x64
 * tiles) from g_materials colours, set the once-per-run GL state, fetch all
 * uniform/attribute locations. Requires a current GL context and gl_load()
 * already done. Returns 0 on success, non-zero on failure. */
int  render_init(void);

/* Set the view-distance fog band (world-space depth): geometry fades from `start`
 * to full sky at `end`. The caller ties `end` to the streaming window radius so the
 * fog reaches full just inside the loaded edge (chunks fade rather than pop). Cheap
 * (two uniform writes on each chunk program); call whenever the view distance
 * changes. Safe before render_init (records the values; applied once programs exist). */
void render_set_fog(float start, float end);

/* Upload (or re-upload) a chunk's mesh into the given slot's VBO/IBO, recording
 * its world origin (cx,cy,cz in chunk coords -> the shader's u_chunk_origin).
 * Uses pooled buffer handles + glBufferData orphaning, never per-remesh
 * glGenBuffers/glDeleteBuffers. An empty mesh (index_count 0) marks the slot
 * drawable-as-nothing. slot in [0, MAX_RENDER_CHUNKS). */
void render_upload_chunk(int slot, const MeshBuffer *m, int cx, int cy, int cz);

/* Upload (or re-upload) a chunk's LIQUID mesh into the given slot's separate
 * liquid VBO/IBO for the blended pass (render_end). Mirrors render_upload_chunk
 * but on the liq_* handles; the world origin is shared with the opaque stream,
 * so this must be called alongside/after render_upload_chunk for the same slot
 * and does NOT touch ox/oy/oz. An empty liquid mesh draws as nothing. */
void render_upload_chunk_liquid(int slot, const MeshBuffer *m, int cx, int cy, int cz);

/* Free (clear) a render slot on chunk eviction: zero its opaque and liquid
 * index counts so render_draw_chunk (and render_end's liquid pass) skip it -
 * drawable-as-nothing - while KEEPING its pooled VBO/IBO handles for the next
 * chunk that reuses this slot (no glDeleteBuffers churn as the streaming window
 * slides). This is the WorldStore's slot_free callback (world.h WorldSlotFreeFn):
 * the slab and render slot return to their free stacks, but the GPU buffers stay
 * pooled. Equivalent to render_upload_chunk(slot, NULL, ...) for both streams,
 * named for eviction clarity. slot in [0, MAX_RENDER_CHUNKS); out-of-range or a
 * pre-init call is a silent no-op. */
void render_free_slot(int slot);

/* Begin a frame's opaque pass: clear, set depth/cull/blend state, bind the
 * atlas + opaque program once, and upload the per-frame uniforms.
 *   mvp4x4 - column-major 4x4 model-view-projection matrix (16 floats)
 *   sun    - live sun intensity for u_sun (day/night; zero remeshes). */
void render_begin(const float *mvp4x4, float sun);

/* Draw one chunk slot: bind its VBO, set the 12-byte attribute pointers,
 * set u_chunk_origin, one glDrawElements. No-op for empty/unset slots. */
void render_draw_chunk(int slot);

/* Finish the frame (any liquid/transparent pass + state teardown). Does NOT
 * swap buffers - that is the platform's plat_swap_buffers(). */
void render_end(void);

/* Draw a wireframe outline around world voxel (wx,wy,wz) - the block-target
 * highlight for break/place. Uses the MVP captured in render_begin, depth-tested
 * but not depth-writing. Call AFTER render_end (so it overlays the scene) and
 * before plat_swap_buffers; a no-op if the overlay program failed to build. */
void render_highlight_voxel(int wx, int wy, int wz);

/* Draw a remote player's avatar: a filled 1.0-unit cube of `color` (RGB 0..1)
 * centred at world position `pos` (3 floats). A solid scene object - depth-tested
 * AND depth-written (occludes / is occluded by terrain). Uses the MVP captured in
 * render_begin; call AFTER render_end, before plat_swap_buffers. No-op if the
 * overlay program failed to build or pos is NULL. (0.3 multiplayer.) */
void render_avatar(const float pos[3], const float color[3]);

/* Draw a small crosshair (+) at the screen centre. `aspect` (width/height) keeps
 * it square on non-square windows. Screen-space, always on top (no depth test).
 * Call after render_end and before plat_swap_buffers; a no-op if the overlay
 * program failed to build. */
void render_crosshair(float aspect);

/* ---- 2D UI primitives (0.3 pause menu; screen-space, NDC coords) --------- *
 * Drawn AFTER render_end, before plat_swap_buffers (over the scene), via the
 * overlay program. Coordinates are NDC: x,y in [-1,1] (y up), the screen centre
 * is (0,0). Colour is RGBA 0..1 (a<1 blends over the scene). No-ops if the
 * overlay program failed to build. */

/* A filled rectangle from (x0,y0) to (x1,y1). Use a full-screen (-1,-1)..(1,1)
 * low-alpha rect to dim the scene behind a menu. */
void render_ui_rect(float x0, float y0, float x1, float y1,
                    float r, float g, float b, float a);

/* Draw `s` (printable ASCII) with an 8x8 bitmap font, top-left at NDC (x,y).
 * `cell` is the glyph HEIGHT in NDC; `aspect` (width/height) keeps glyphs square.
 * Advances left-to-right; '\n' starts a new line. */
void render_text(float x, float y, float cell, float aspect,
                 float r, float g, float b, float a, const char *s);

/* Release GL resources (programs, atlas texture, all pooled VBOs/IBOs). */
void render_shutdown(void);

/* Capture the current framebuffer (back/draw buffer) to a binary PPM (P6) file.
 * glReadPixels-based, so it records exactly what GL rendered - independent of
 * the desktop/compositor/screensaver. Call after render_end() and before the
 * buffer swap. Returns 0 on success, non-zero on failure. Used for headless
 * visual verification on the dev box; trivially portable to the XP target. */
int  render_screenshot_ppm(const char *path, int width, int height);

/* =====================================================================
 * SHADER CONTRACT - the render.c author MUST match these names/semantics.
 * GLSL 1.20 (#version 120; attribute/varying/gl_FragColor; no in/out, no
 * integer attributes, no flat).
 *
 * Vertex attributes (bound by name via glBindAttribLocation BEFORE link, so we
 * control the locations without VAOs; glVertexAttribPointer uses these slots):
 *   attribute vec3  a_pos;    // px,py,pz : ushort->float 0..16, GL_FALSE
 *   attribute float a_mat;    // mat      : ubyte->float 0..255, GL_FALSE (tile id)
 *   attribute float a_face;   // face     : ubyte->float 0..5,   GL_FALSE
 *   attribute float a_light;  // light    : ubyte NON-normalized 0..255 (lo nibble = baked sky/block, hi nibble = temp glow)
 *   attribute float a_ao;     // ao       : ubyte NORMALIZED 0..1
 *   attribute vec2  a_uv;     // u,v      : ubyte->float (tiling corners), GL_FALSE
 *
 * Suggested attribute locations (the loc the render.c author binds):
 *   LOC_POS=0, LOC_MAT=1, LOC_FACE=2, LOC_LIGHT=3, LOC_AO=4, LOC_UV=5
 *
 * Uniforms:
 *   uniform mat4  u_mvp;          // model-view-projection (render_begin's mvp4x4)
 *   uniform vec3  u_chunk_origin; // world pos of chunk (0,0,0) corner (per chunk)
 *   uniform float u_atlas_cols;   // tiles per atlas row = 16.0 (shared grid const)
 *   uniform float u_sun;          // live sun intensity (render_begin's sun)
 *   uniform vec3  u_sun_tint;     // warm daylight tint
 *   uniform sampler2D u_atlas;    // the one material atlas, texture unit 0
 *   // fog is folded in-shader from gl_Position.w against FOG_START/FOG_END
 *   // constants (no fog uniforms in v1); the listed u_fog_* are reserved.
 *
 * Varyings:
 *   varying vec2  v_uv;     // atlas UV: (vec2(col,row)+a_uv)/u_atlas_cols
 *   varying float v_bright; // sun-scaled diffuse: (AMBIENT+(1-AMBIENT)*
 *                           //   clamp(sky*u_sun,0,1)) * a_ao  -- folded LIVE,
 *                           //   sky = a_light lo nibble / 15
 *   varying float v_heat;   // temp glow 0..1 (a_light hi nibble / 15);
 *                           //   FS additive, material-base-tinted emissive
 *   varying float v_fog;    // 0 near .. 1 at FOG_END (perspective-w depth proxy)
 *
 * Fragment: gl_FragColor = vec4(base + hot, alpha), where
 *     base = texture2D(u_atlas, v_uv).rgb * (v_bright * u_sun_tint)
 *     hot  = v_heat-driven emissive whose hot end is biased toward the voxel's
 *            OWN texel colour (so molten copper id11 reads orange and lava id10
 *            red even at white-heat); zero when v_heat==0, data-driven, no
 *            material-id switch. alpha = 1.0 opaque / MaterialDef alpha liquid.
 * The final rgb is then mixed toward the sky colour by v_fog (matches
 * glClearColor) so distant chunks dissolve into the horizon.
 * ===================================================================== */

#endif /* RENDER_H */
