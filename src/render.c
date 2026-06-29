/* render.c - GL2.1 / GLSL1.20 forward renderer (ARCHITECTURE.md Section 5).
 *
 * Implements render.h: compile+link the GLSL 1.20 opaque (and liquid) shaders,
 * build the placeholder 1024x1024 material atlas, manage per-chunk VBO/IBO
 * slots, and submit one glDrawElements per chunk. Reads the canonical 12-byte
 * MeshVert (mesher.h) back byte-for-byte; the atlas grid (16x16 of 64x64 tiles)
 * and the shader's u_atlas_cols=16 are the single shared grid constant.
 *
 * GL acquisition split (mirrors gl_loader.h's contract):
 *   - The post-1.1 entry points (VBOs, the shader pipeline, glActiveTexture)
 *     come through the manually-loaded function pointers in gl_loader.h.
 *   - The GL 1.1 entry points (glEnable/glClear/glGenTextures/glTexImage2D/
 *     glTexParameteri/glDrawElements/...) and the GL 1.1 enum constants
 *     (GL_TEXTURE_2D, GL_RGBA, GL_TRIANGLES, ...) come from the system
 *     <GL/gl.h>, which the platform statically links against real GL.
 *   - The post-1.1 enum TOKENS (GL_ARRAY_BUFFER, GL_STATIC_DRAW,
 *     GL_VERTEX_SHADER, ...) are NOT in a strict GL 1.1 <GL/gl.h>, so they are
 *     defined defensively below (only when the system header omits them).
 * The base GL typedefs in gl_loader.h are byte-identical to those in <GL/gl.h>
 * (GLenum=unsigned int, GLuint=unsigned int, GLfloat=float, ...), so including
 * both is safe under GCC -std=c99 (identical typedefs are accepted).
 *
 * This file is compiled against real GL/OS headers in the build; it is NOT
 * syntax-checkable on the authoring machine, so it is written carefully against
 * only the symbols the headers and the pinned GL function list declare.
 */

#include "render.h"
#include "mesher.h"
#include "material.h"
#include "gl_loader.h"

#include <GL/gl.h>      /* GL 1.1 prototypes + the GL enum constants */

#include <stddef.h>     /* offsetof, size_t                          */
#include <stdint.h>
#include <stdio.h>      /* fprintf for shader/link diagnostics       */
#include <stdlib.h>     /* malloc/free for the placeholder atlas     */
#include <math.h>       /* tanf/sqrt for the matrix helpers          */

/* The XP MinGW <GL/gl.h> is a strict GL 1.1 header: the post-1.1 enum TOKENS we
 * use (buffer objects, the shader pipeline, the multitexture unit, GL 1.2
 * clamp) are NOT in it. The matching FUNCTIONS arrive as the gl_loader.h
 * pointers; the TOKENS we must supply ourselves. We define each only when the
 * system header has not already (modern Mesa <GL/gl.h> defines them), so there
 * is never a redefinition clash. Same defensive pattern platform_linux.c uses
 * for the GLX tokens. Values are the fixed GL enum numbers (stable forever). */

/* Buffer objects (GL 1.5 / ARB_vertex_buffer_object) */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif

/* Shader objects + program pipeline (GL 2.0) */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

/* Multitexture unit 0 (GL 1.3 / ARB_multitexture) */
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

/* Texture clamp (GL 1.2) - the placeholder atlas clamps to its outer edge */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

/* ====================================================================== */
/* Atlas geometry - the one grid both this file and the mesher cite.       */
/* ====================================================================== */
#define ATLAS_COLS   16                          /* tiles per row/col       */
#define ATLAS_TILE   64                          /* texels per tile edge    */
#define ATLAS_SIZE   (ATLAS_COLS * ATLAS_TILE)   /* 1024 texels             */

/* Attribute locations bound by name before link (no VAOs -> we own them). */
enum {
    LOC_POS   = 0,   /* vec3  a_pos   : px,py,pz ushort->float            */
    LOC_MAT   = 1,   /* float a_mat   : material / atlas tile id          */
    LOC_FACE  = 2,   /* float a_face  : face direction 0..5               */
    LOC_LIGHT = 3,   /* float a_light : packed light byte, NON-normalized 0..255 (lo nibble sky, hi nibble heat) */
    LOC_AO    = 4,   /* float a_ao    : ambient occlusion, normalized 0..1 */
    LOC_UV    = 5    /* vec2  a_uv    : tile-local UV corners             */
};

/* ====================================================================== */
/* Module state                                                            */
/* ====================================================================== */

/* One resident chunk's GPU buffers + draw metadata. A chunk owns TWO geometry
 * streams sharing one world origin (ARCHITECTURE 5.6): the OPAQUE stream (drawn
 * front-to-back in render_draw_chunk) and the LIQUID stream (drawn back-to-front
 * with depth-write off + alpha blend in render_end). Both use the same 12-byte
 * MeshVert layout, so the liquid stream re-uses render_draw_chunk's attribute
 * pointer setup verbatim. */
typedef struct {
    GLuint   vbo;          /* interleaved 12-byte MeshVert buffer, or 0    */
    GLuint   ibo;          /* GL_UNSIGNED_SHORT index buffer, or 0         */
    GLsizei  index_count;  /* opaque indices to draw; 0 == drawable-as-nothing */
    GLuint   liq_vbo;      /* liquid-stream interleaved MeshVert buffer, or 0 */
    GLuint   liq_ibo;      /* liquid-stream u16 index buffer, or 0         */
    GLsizei  liq_index_count; /* liquid indices to draw; 0 == none         */
    GLfloat  ox, oy, oz;   /* world position of this chunk's (0,0,0) corner */
} ChunkSlot;

/* The two share one vertex layout; liquid is opaque + alpha + UV scroll
 * (the liquid program is built but the v1 opaque-only path is what
 * render_begin/render_draw_chunk drive; render_end is the liquid hook). */
typedef struct {
    GLuint program;
    GLint  u_mvp;
    GLint  u_chunk_origin;
    GLint  u_atlas_cols;     /* = 16.0                                     */
    GLint  u_sun;
    GLint  u_sun_tint;
    GLint  u_atlas;          /* sampler2D, texture unit 0                  */
    GLint  u_fog_start;      /* view-distance fog: depth where fade begins */
    GLint  u_fog_end;        /* ... and where it reaches full sky (adjustable) */
} Program;

static Program    g_opaque;
static Program    g_liquid;               /* FS_LIQUID alpha pass; cached locs   */
static GLuint     g_atlas_tex = 0;
static ChunkSlot  g_slots[MAX_RENDER_CHUNKS];
static int        g_inited = 0;

/* Block-targeting wireframe overlay (0.2 break/place): a position-only program
 * and a static unit-cube line buffer. render_highlight_voxel translates the cube
 * to a world voxel via u_origin and draws its 12 edges. Independent of the chunk
 * program (its own uniforms); if it fails to build, the highlight just no-ops. */
static GLuint     g_overlay_prog     = 0;
static GLint      g_overlay_u_mvp    = -1;
static GLint      g_overlay_u_origin = -1;
static GLint      g_overlay_u_color  = -1;
static GLint      g_overlay_u_alpha  = -1;
static GLuint     g_wire_vbo         = 0;   /* unit-cube edges (block highlight) */
static GLuint     g_cross_vbo        = 0;   /* unit crosshair (2 lines, NDC)     */
static GLuint     g_avatar_vbo       = 0;   /* filled cube (remote-player avatar) */
static GLuint     g_ui_vbo           = 0;   /* dynamic NDC tris (font + menu rects) */

/* Per-frame uniform state captured in render_begin so render_end's liquid pass
 * can re-bind the liquid program with the SAME mvp/sun without a second API
 * call. render_end takes no camera arg this milestone (single-chunk demo), so
 * these are the only frame inputs the transparent pass needs. */
static float      g_frame_mvp[16];
static int        g_frame_have_mvp = 0;
static float      g_frame_sun = 1.0f;

/* Current view-distance fog band (world-space depth). Defaults match the radius-6
 * window (~92 vox); render_set_fog updates them + pushes to both chunk programs. */
static float      g_fog_start = 55.0f;
static float      g_fog_end   = 92.0f;

/* Per-frame u_sun_tint, derived from the live sun in render_begin and shared by
 * both passes (opaque sets it; render_end's liquid pass reuses it, like
 * g_frame_sun/g_frame_mvp). Defaults to warm daylight so a pre-render_begin
 * draw is unchanged. Tying it to sun gives a free dawn/dusk warm-shift with zero
 * remeshes (the architecture's live-sun selling point). */
static float      g_frame_tint[3] = { 1.0f, 0.96f, 0.88f };

/* ====================================================================== */
/* Small column-major 4x4 math helper (the engine's float matrices live    */
/* SSE-side per the toolchain decision; main.c may build its own mvp and    */
/* hand it to render_begin, or use these helpers).                          */
/* ====================================================================== */

/* All matrices are column-major (OpenGL convention): element (row r, col c)
 * is m[c*4 + r], so glUniformMatrix4fv(loc,1,GL_FALSE,m) consumes them
 * directly with no transpose. */

void render_mat4_identity(float *m)
{
    int i;
    for (i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b  (column-major; applies b then a to a column vector). */
void render_mat4_mul(float *out, const float *a, const float *b)
{
    float r[16];
    int col, row, k;
    for (col = 0; col < 4; ++col) {
        for (row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = s;
        }
    }
    for (col = 0; col < 16; ++col) out[col] = r[col];
}

/* Right-handed perspective, mapping z to [-1,1] (classic GL clip space).
 * fov_y in radians. */
void render_mat4_perspective(float *m, float fov_y, float aspect,
                             float z_near, float z_far)
{
    float f  = 1.0f / tanf(fov_y * 0.5f);
    float dz = z_near - z_far;          /* negative */
    int i;
    for (i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (z_far + z_near) / dz;       /* = -(zf+zn)/(zf-zn) */
    m[11] = -1.0f;
    m[14] = (2.0f * z_far * z_near) / dz; /* = -(2 zf zn)/(zf-zn) */
}

static void vec3_sub(float *o, const float *a, const float *b)
{
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static void vec3_cross(float *o, const float *a, const float *b)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static void vec3_normalize(float *v)
{
    float len = (float)sqrt((double)(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]));
    if (len > 1e-8f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}
static float vec3_dot(const float *a, const float *b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

/* Right-handed look-at view matrix (column-major), GL convention. */
void render_mat4_lookat(float *m,
                        float ex, float ey, float ez,
                        float cx, float cy, float cz,
                        float ux, float uy, float uz)
{
    float eye[3] = { ex, ey, ez };
    float ctr[3] = { cx, cy, cz };
    float up[3]  = { ux, uy, uz };
    float f[3], s[3], u[3];

    vec3_sub(f, ctr, eye);   /* forward */
    vec3_normalize(f);
    vec3_cross(s, f, up);    /* right = forward x up */
    vec3_normalize(s);
    vec3_cross(u, s, f);     /* true up = right x forward */

    /* Column-major rows:
     *   row0 =  s, row1 = u, row2 = -f, last column = -dot(.,eye) */
    m[0] = s[0]; m[4] = s[1]; m[8]  = s[2];  m[12] = -vec3_dot(s, eye);
    m[1] = u[0]; m[5] = u[1]; m[9]  = u[2];  m[13] = -vec3_dot(u, eye);
    m[2] = -f[0];m[6] = -f[1];m[10] = -f[2]; m[14] =  vec3_dot(f, eye);
    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f;  m[15] = 1.0f;
}

/* ====================================================================== */
/* Shader sources - GLSL 1.20: #version 120, attribute/varying/gl_FragColor */
/* No in/out, no integer attributes, no flat. Every attribute maps onto a   */
/* real field of the 12-byte MeshVert.                                      */
/* ====================================================================== */

static const char *VS_OPAQUE =
    "#version 120\n"
    "uniform mat4  u_mvp;\n"
    "uniform vec3  u_chunk_origin;\n"
    "uniform float u_atlas_cols;\n"   /* = 16.0, shared grid constant */
    "uniform float u_sun;\n"
    "uniform float u_fog_start;\n"    /* view-distance fog start (depth), set by render_set_fog */
    "uniform float u_fog_end;\n"      /* ... and full-fog depth; both adjustable at runtime       */
    "attribute vec3  a_pos;\n"        /* px,py,pz : 0..16 chunk-local */
    "attribute float a_mat;\n"        /* atlas tile id 0..255         */
    "attribute float a_face;\n"       /* liquid fill-height TOP-DROP, 1/16 voxel; 0 for opaque */
    "attribute float a_light;\n"      /* packed byte 0..255: lo nibble sky, hi nibble heat (NOT normalized) */
    "attribute float a_ao;\n"         /* ambient occlusion, 0..1      */
    "attribute vec2  a_uv;\n"         /* tile-local UV corners        */
    "varying   vec2  v_uv;\n"
    "varying   float v_bright;\n"
    "varying   float v_heat;\n"       /* temperature glow 0..1 -> red-orange emissive in FS */
    "varying   float v_fog;\n"        /* 0 near .. 1 at FOG_END (cheap depth proxy) */
    "void main() {\n"
    /* a_face carries the liquid fill-height TOP-DROP in 1/16-voxel units (the
     * mesher writes it into the per-vertex 'face' byte): SUBTRACT it from world.y
     * so a partial-fill liquid surface (and the top edge of its side faces) sits
     * at the fill height. Opaque geometry uploads face=0, so its world.y is
     * unchanged - keeping all opaque output byte-identical. */
    "    vec3 world = a_pos + u_chunk_origin - vec3(0.0, a_face * (1.0/16.0), 0.0);\n"
    "    gl_Position = u_mvp * vec4(world, 1.0);\n"
    /* Distance fog factor from gl_Position.w (perspective depth ~ view distance):
     * 0 inside FOG_START, ramping to 1 by FOG_END. FOG_END is held well below the
     * 1000 far plane so distant chunks FADE into the sky colour rather than pop
     * at the clip plane. The central demo (a few tens of voxels out) stays
     * unfogged. The FS mixes the final colour toward the matching sky colour. */
    /* 0.5: fog start/end are UNIFORMS (render_set_fog) so the view distance is
     * adjustable at runtime - they track the streaming window radius (fog reaches
     * full a little inside the loaded edge so chunks fade rather than pop in). The
     * max() guards a degenerate start==end. */
    "    v_fog = clamp((gl_Position.w - u_fog_start) / max(u_fog_end - u_fog_start, 1.0), 0.0, 1.0);\n"
    /* Reconstruct atlas UV from tile id + corner using the shared grid const.
     * col = mod(mat,16); row = floor(mat/16); uv = (corner + (col,row))/cols. */
    "    float inv = 1.0 / u_atlas_cols;\n"
    "    float col = mod(a_mat, u_atlas_cols);\n"
    "    float row = floor(a_mat * inv);\n"
    "    v_uv = (a_uv + vec2(col, row)) * inv;\n"
    /* a_light packs TWO 4-bit channels (mesher, NON-normalized byte 0..255):
     * low nibble = baked sky/block light, high nibble = temperature glow. Split
     * them: sky drives the white, sun-scaled diffuse term (folded live here, so
     * a moving sun costs no remesh); heat goes to the fragment as a separate
     * red-orange emissive. The +0.5 guards integer-float rounding. Remap the lit
     * term into [AMBIENT,1] so a shadowed face shows AMBIENT*a_ao, not black. */
    "    float lo  = floor(mod(a_light + 0.5, 16.0));\n"   /* sky  0..15 */
    "    float hi  = floor((a_light + 0.5) / 16.0);\n"     /* heat 0..15 */
    "    float sky = lo / 15.0;\n"
    "    v_heat    = hi / 15.0;\n"
    "    const float AMBIENT = 0.70;\n"   /* 0.3 asteroid: high flat floor so the ball
                                           * isn't half-dark (skylight bakes only its +Y
                                           * cap; true radial skylight deferred). AO
                                           * still shapes faces. Was 0.15.            */
    "    float lit = clamp(sky * u_sun, 0.0, 1.0);\n"
    "    v_bright = (AMBIENT + (1.0 - AMBIENT) * lit) * a_ao;\n"
    "}\n";

static const char *FS_OPAQUE =
    "#version 120\n"
    "uniform sampler2D u_atlas;\n"
    "uniform vec3      u_sun_tint;\n"
    "varying vec2  v_uv;\n"
    "varying float v_bright;\n"
    "varying float v_heat;\n"
    "varying float v_fog;\n"           /* 0 near .. 1 at fog end (depth proxy) */
    "void main() {\n"
    "    vec4 tex = texture2D(u_atlas, v_uv);\n"
    "    vec3 base = tex.rgb * (v_bright * u_sun_tint);\n"
    /* Temperature emissive (blackbody-ish): dark red -> orange -> warm white as
     * the voxel heats. ADDED on top of the diffuse term, so hot metal glows even
     * where it sits in shadow or beside a bright block-light source. Scaled by
     * v_heat^2 for a soft onset (a barely-warm face does not light up). The hot
     * end is biased toward the voxel's OWN texel colour (tex.rgb, baked from
     * g_materials[id].color_rgba), so molten copper (orange) and lava (red) read
     * distinct even at white-heat - data-driven, no material-id switch. glow==0
     * voxels keep base byte-identical (cool terrain unchanged). */
    "    vec3 hotcol = mix(vec3(0.8,0.05,0.0), tex.rgb, 0.6);\n"
    "    vec3 hot = mix(hotcol, tex.rgb + vec3(0.25), v_heat) * (v_heat*v_heat);\n"
    "    gl_FragColor = vec4(base + hot, 1.0);\n"
    /* Distance fog: fade toward the sky clear-colour over the far band so chunks
     * dissolve into the horizon instead of popping at the far plane. SKY_COLOR
     * matches glClearColor (render_init) so the horizon is seamless. */
    "    const vec3 SKY_COLOR = vec3(0.45, 0.62, 0.86);\n"
    "    gl_FragColor.rgb = mix(gl_FragColor.rgb, SKY_COLOR, v_fog);\n"
    "}\n";

/* The liquid program is the opaque shader plus a MaterialDef-derived alpha and
 * a time-scrolled UV; v1 keeps the opaque path live and builds liquid for the
 * render_end hook. The pinned contract only mandates the opaque shader byte-
 * for-byte, so the liquid pair here mirrors it with alpha passthrough. */
static const char *FS_LIQUID =
    "#version 120\n"
    "uniform sampler2D u_atlas;\n"
    "uniform vec3      u_sun_tint;\n"
    "varying vec2  v_uv;\n"
    "varying float v_bright;\n"
    "varying float v_heat;\n"
    "varying float v_fog;\n"           /* lockstep with FS_OPAQUE */
    "void main() {\n"
    "    vec4 tex = texture2D(u_atlas, v_uv);\n"
    "    vec3 base = tex.rgb * (v_bright * u_sun_tint);\n"
    /* Same material-tinted heat emissive as FS_OPAQUE (lava id10 + molten copper
     * id11 are liquids, so the tint must read right here): bias the hot end
     * toward each voxel's own texel colour. Kept identical to FS_OPAQUE. */
    "    vec3 hotcol = mix(vec3(0.8,0.05,0.0), tex.rgb, 0.6);\n"
    "    vec3 hot = mix(hotcol, tex.rgb + vec3(0.25), v_heat) * (v_heat*v_heat);\n"
    "    gl_FragColor = vec4(base + hot, tex.a);\n"
    /* Distance fog on the RGB only; alpha (the liquid's transparency) is left
     * untouched so a far water surface still blends. SKY_COLOR matches the
     * opaque pass + glClearColor. */
    "    const vec3 SKY_COLOR = vec3(0.45, 0.62, 0.86);\n"
    "    gl_FragColor.rgb = mix(gl_FragColor.rgb, SKY_COLOR, v_fog);\n"
    "}\n";

/* Overlay program for the block-target wireframe: position only (reuses the
 * a_pos attribute slot), translated to a world voxel by u_origin, flat dark
 * colour. GLSL 1.20, same conservative subset as the chunk shaders. */
static const char *VS_OVERLAY =
    "#version 120\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec3 u_origin;\n"   /* world position of the target voxel's corner */
    "attribute vec3 a_pos;\n"    /* unit-cube edge vertex, 0..1 (+/- a hair)    */
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_pos + u_origin, 1.0);\n"
    "}\n";

static const char *FS_OVERLAY =
    "#version 120\n"
    "uniform vec3 u_color;\n"     /* set per draw: dark block outline / light crosshair */
    "uniform float u_alpha;\n"    /* 1.0 for opaque overlays; <1 for the menu dim       */
    "void main() {\n"
    "    gl_FragColor = vec4(u_color, u_alpha);\n"
    "}\n";

/* ====================================================================== */
/* Shader compile / link plumbing                                          */
/* ====================================================================== */

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh;
    GLint  ok = 0;

    sh = glCreateShader(type);
    if (sh == 0) {
        fprintf(stderr, "render: glCreateShader failed (type 0x%x)\n",
                (unsigned)type);
        return 0;
    }
    /* GLSL 1.20 sources are NUL-terminated C string literals -> length NULL. */
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), &n, log);
        if (n <= 0) log[0] = '\0';
        fprintf(stderr, "render: %s shader compile failed:\n%s\n",
                (type == GL_VERTEX_SHADER) ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Bind the six attribute locations by name BEFORE linking (no VAOs). The names
 * here MUST match the shader source and the glVertexAttribPointer slots. */
static void bind_attrib_locations(GLuint program)
{
    glBindAttribLocation(program, LOC_POS,   "a_pos");
    glBindAttribLocation(program, LOC_MAT,   "a_mat");
    glBindAttribLocation(program, LOC_FACE,  "a_face");
    glBindAttribLocation(program, LOC_LIGHT, "a_light");
    glBindAttribLocation(program, LOC_AO,    "a_ao");
    glBindAttribLocation(program, LOC_UV,    "a_uv");
}

/* Compile vs+fs, bind attribute locations, link. Returns program or 0. */
static GLuint build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs, fs, prog;
    GLint  ok = 0;

    vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    if (vs == 0) return 0;
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (fs == 0) { glDeleteShader(vs); return 0; }

    prog = glCreateProgram();
    if (prog == 0) {
        fprintf(stderr, "render: glCreateProgram failed\n");
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    bind_attrib_locations(prog);    /* MUST precede glLinkProgram */
    glLinkProgram(prog);

    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log), &n, log);
        if (n <= 0) log[0] = '\0';
        fprintf(stderr, "render: program link failed:\n%s\n", log);
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    /* Shaders may be flagged for deletion once attached+linked; the program
     * keeps them alive until it is itself deleted. */
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/* Fetch and cache the uniform locations for a built program. A missing uniform
 * (optimized out because unused) returns -1, which the GL spec lets us pass to
 * glUniform* as a silent no-op, so we keep whatever we get. */
static void cache_uniform_locations(Program *p)
{
    p->u_mvp          = glGetUniformLocation(p->program, "u_mvp");
    p->u_chunk_origin = glGetUniformLocation(p->program, "u_chunk_origin");
    p->u_atlas_cols   = glGetUniformLocation(p->program, "u_atlas_cols");
    p->u_fog_start    = glGetUniformLocation(p->program, "u_fog_start");
    p->u_fog_end      = glGetUniformLocation(p->program, "u_fog_end");
    p->u_sun          = glGetUniformLocation(p->program, "u_sun");
    p->u_sun_tint     = glGetUniformLocation(p->program, "u_sun_tint");
    p->u_atlas        = glGetUniformLocation(p->program, "u_atlas");
}

/* ====================================================================== */
/* Placeholder atlas: one 64x64 tile per material id, filled with that      */
/* material's color_rgba plus a faint checker/border so faces read clearly.  */
/* ====================================================================== */

static int build_placeholder_atlas(void)
{
    /* 1024 * 1024 * 4 = 4 MiB. Built once on the heap, uploaded, freed. */
    const size_t pixels = (size_t)ATLAS_SIZE * (size_t)ATLAS_SIZE;
    unsigned char *img;
    int id;

    img = (unsigned char *)malloc(pixels * 4u);
    if (img == NULL) {
        fprintf(stderr, "render: atlas allocation (%lu bytes) failed\n",
                (unsigned long)(pixels * 4u));
        return 1;
    }

    /* For each of the 256 material ids, fill its tile. Tile (col,row) =
     * (id % 16, id / 16); pixel rows run top-to-bottom in the buffer. */
    for (id = 0; id < MAX_MATERIALS; ++id) {
        const MaterialDef *md = &g_materials[id];
        int tcol = id % ATLAS_COLS;
        int trow = id / ATLAS_COLS;
        int x0   = tcol * ATLAS_TILE;
        int y0   = trow * ATLAS_TILE;
        unsigned r = md->color_rgba[0];
        unsigned g = md->color_rgba[1];
        unsigned b = md->color_rgba[2];
        unsigned a = md->color_rgba[3];
        int tx, ty;

        /* Air and any all-zero material would be invisible; give it a neutral
         * grey so the placeholder atlas is never a pure black tile. */
        if ((r | g | b) == 0) { r = g = b = 60; }
        if (a == 0)           { a = 255; }

        for (ty = 0; ty < ATLAS_TILE; ++ty) {
            for (tx = 0; tx < ATLAS_TILE; ++tx) {
                int px = x0 + tx;
                int py = y0 + ty;
                size_t idx = ((size_t)py * (size_t)ATLAS_SIZE + (size_t)px) * 4u;
                unsigned cr = r, cg = g, cb = b;

                /* Faint 8x8 checker so a flat-colored face still shows depth. */
                if (((tx >> 3) ^ (ty >> 3)) & 1) {
                    cr = (cr * 7u) / 8u;
                    cg = (cg * 7u) / 8u;
                    cb = (cb * 7u) / 8u;
                }
                /* Dark 1-texel border so tile edges (and quad seams) are
                 * visible during bring-up. */
                if (tx == 0 || ty == 0 ||
                    tx == ATLAS_TILE - 1 || ty == ATLAS_TILE - 1) {
                    cr = (cr * 5u) / 8u;
                    cg = (cg * 5u) / 8u;
                    cb = (cb * 5u) / 8u;
                }

                img[idx + 0] = (unsigned char)cr;
                img[idx + 1] = (unsigned char)cg;
                img[idx + 2] = (unsigned char)cb;
                img[idx + 3] = (unsigned char)a;
            }
        }
    }

    glGenTextures(1, &g_atlas_tex);
    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);

    /* M1: nearest filtering, no mip chain needed yet (gutters + capped mips
     * are the Section 5.4 path, deferred to a later milestone). Clamp so the
     * single placeholder atlas never wraps across the whole-texture edge. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 ATLAS_SIZE, ATLAS_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img);

    free(img);
    return 0;
}

/* ====================================================================== */
/* Public API                                                              */
/* ====================================================================== */

int render_init(void)
{
    int i;

    /* Build the opaque program (the v1 draw path). */
    g_opaque.program = build_program(VS_OPAQUE, FS_OPAQUE);
    if (g_opaque.program == 0)
        return 1;
    cache_uniform_locations(&g_opaque);

    /* Build the liquid program too (shares the vertex layout + VS_OPAQUE, so
     * the attribute slots and atlas-UV math are identical; only the fragment
     * stage differs by emitting tex.a as alpha). If it fails it is not fatal to
     * the opaque path, but the transparent pass in render_end will then no-op.
     * Cache its uniform locations like the opaque program (it is its own GL
     * program object, so its uniform locations are independent). */
    g_liquid.program = build_program(VS_OPAQUE, FS_LIQUID);
    if (g_liquid.program == 0) {
        fprintf(stderr, "render: liquid program build failed (liquids will not draw)\n");
    } else {
        cache_uniform_locations(&g_liquid);
    }

    /* Block-target wireframe overlay program + its unit-cube line buffer. Reuses
     * build_program (which binds a_pos at LOC_POS; the chunk attribute names just
     * don't exist in this shader and are ignored at link). Non-fatal on failure:
     * render_highlight_voxel then no-ops. */
    g_overlay_prog = build_program(VS_OVERLAY, FS_OVERLAY);
    if (g_overlay_prog == 0) {
        fprintf(stderr, "render: overlay program build failed (no block highlight)\n");
    } else {
        g_overlay_u_mvp    = glGetUniformLocation(g_overlay_prog, "u_mvp");
        g_overlay_u_origin = glGetUniformLocation(g_overlay_prog, "u_origin");
        g_overlay_u_color  = glGetUniformLocation(g_overlay_prog, "u_color");
        g_overlay_u_alpha  = glGetUniformLocation(g_overlay_prog, "u_alpha");
        {
            /* 12 edges (24 verts) of a unit cube, inflated a hair so the lines sit
             * just outside the block faces and don't z-fight with the block. */
            const GLfloat L = -0.003f, H = 1.003f;
            const GLfloat wire[24 * 3] = {
                L,L,L, H,L,L,  H,L,L, H,L,H,  H,L,H, L,L,H,  L,L,H, L,L,L, /* bottom */
                L,H,L, H,H,L,  H,H,L, H,H,H,  H,H,H, L,H,H,  L,H,H, L,H,L, /* top    */
                L,L,L, L,H,L,  H,L,L, H,H,L,  H,L,H, H,H,H,  L,L,H, L,H,H  /* posts  */
            };
            /* Unit crosshair: a + at the origin in a normalized space the draw
             * scales into clip coords (so it stays centred + square per aspect). */
            const GLfloat cross[4 * 3] = {
                -1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,   /* horizontal */
                 0.0f,-1.0f, 0.0f,  0.0f, 1.0f, 0.0f    /* vertical   */
            };
            glGenBuffers(1, &g_wire_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, g_wire_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(wire), wire, GL_STATIC_DRAW);
            glGenBuffers(1, &g_cross_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, g_cross_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(cross), cross, GL_STATIC_DRAW);

            /* Remote-player avatar: a filled 1.0-unit cube centred at the origin
             * (the draw translates it by u_origin = the player's world position).
             * SYMMETRIC so it reads the same regardless of the player's radial
             * orientation on the sphere (the overlay shader cannot rotate). 36
             * verts = 6 faces x 2 tris; winding is irrelevant (cull off on draw). */
            {
                const GLfloat e = 0.5f, n = -0.5f, p = 0.5f;
                const GLfloat cube[36 * 3] = {
                    n,n,n, n,p,n, n,p,p,  n,n,n, n,p,p, n,n,p,   /* -X */
                    p,n,n, p,p,p, p,p,n,  p,n,n, p,n,p, p,p,p,   /* +X */
                    n,n,n, p,n,n, p,n,p,  n,n,n, p,n,p, n,n,p,   /* -Y */
                    n,p,n, p,p,p, p,p,n,  n,p,n, n,p,p, p,p,p,   /* +Y */
                    n,n,n, p,p,n, p,n,n,  n,n,n, n,p,n, p,p,n,   /* -Z */
                    n,n,p, p,n,p, p,p,p,  n,n,p, p,p,p, n,p,p    /* +Z */
                };
                (void)e;
                glGenBuffers(1, &g_avatar_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, g_avatar_vbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(cube), cube, GL_STATIC_DRAW);
            }
            /* Dynamic buffer for 2D UI tris (text glyphs + menu rects), re-uploaded
             * per draw via glBufferData orphaning - never gen/delete in the loop. */
            glGenBuffers(1, &g_ui_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    /* Placeholder material atlas. */
    if (build_placeholder_atlas() != 0) {
        glDeleteProgram(g_opaque.program);
        g_opaque.program = 0;
        return 1;
    }

    /* Once-per-run GL state for the opaque pass (Section 5.1 scaffolding). */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);            /* greedy mesher emits CCW-outward quads */
    glDisable(GL_BLEND);
    glClearColor(0.45f, 0.62f, 0.86f, 1.0f);   /* sky-ish clear */

    /* Bind the program once and set the constant uniforms (atlas grid + the
     * sampler unit). u_atlas_cols and the sampler never change after this. */
    glUseProgram(g_opaque.program);
    if (g_opaque.u_atlas_cols >= 0)
        glUniform1f(g_opaque.u_atlas_cols, (GLfloat)ATLAS_COLS);
    if (g_opaque.u_atlas >= 0)
        glUniform1i(g_opaque.u_atlas, 0);       /* texture unit 0 */

    /* Same one-time constants for the liquid program (its own program object,
     * so its uniforms are set against ITS handle). */
    if (g_liquid.program != 0) {
        glUseProgram(g_liquid.program);
        if (g_liquid.u_atlas_cols >= 0)
            glUniform1f(g_liquid.u_atlas_cols, (GLfloat)ATLAS_COLS);
        if (g_liquid.u_atlas >= 0)
            glUniform1i(g_liquid.u_atlas, 0);   /* same atlas, texture unit 0 */
        glUseProgram(g_opaque.program);          /* leave opaque current */
    }

    /* Seed the fog uniforms to the default view distance (radius-6 window). MUST be
     * set explicitly: an unset uniform defaults to 0, and 0..0 fog would clamp every
     * fragment to full sky. render_set_fog is called again at runtime when the player
     * changes the view distance. */
    render_set_fog(g_fog_start, g_fog_end);

    /* All slots start empty (both streams). */
    for (i = 0; i < MAX_RENDER_CHUNKS; ++i) {
        g_slots[i].vbo             = 0;
        g_slots[i].ibo             = 0;
        g_slots[i].index_count     = 0;
        g_slots[i].liq_vbo         = 0;
        g_slots[i].liq_ibo         = 0;
        g_slots[i].liq_index_count = 0;
        g_slots[i].ox = g_slots[i].oy = g_slots[i].oz = 0.0f;
    }

    g_inited = 1;
    return 0;
}

void render_upload_chunk(int slot, const MeshBuffer *m, int cx, int cy, int cz)
{
    ChunkSlot *s;

    if (slot < 0 || slot >= MAX_RENDER_CHUNKS)
        return;
    s = &g_slots[slot];

    /* Record world origin: chunk coords -> world voxels (CHUNK_DIM per chunk).
     * a_pos is chunk-local 0..16, so world = a_pos + origin in the shader. */
    s->ox = (GLfloat)(cx * CHUNK_DIM);
    s->oy = (GLfloat)(cy * CHUNK_DIM);
    s->oz = (GLfloat)(cz * CHUNK_DIM);

    /* Empty mesh: keep the slot, mark it drawable-as-nothing. Existing buffers
     * (if any) are left orphaned-empty rather than churned via delete. */
    if (m == NULL || m->index_count == 0 || m->vert_count == 0) {
        s->index_count = 0;
        return;
    }

    /* Lazily create the pooled buffer handles for this slot exactly once;
     * re-uploads reuse them via glBufferData orphaning (never gen/delete). */
    if (s->vbo == 0)
        glGenBuffers(1, &s->vbo);
    if (s->ibo == 0)
        glGenBuffers(1, &s->ibo);

    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)m->vert_count * sizeof(MeshVert)),
                 m->verts, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)m->index_count * sizeof(uint16_t)),
                 m->indices, GL_STATIC_DRAW);

    s->index_count = (GLsizei)m->index_count;
}

/* Upload (or re-upload) a chunk's LIQUID-stream mesh into the slot's separate
 * liquid VBO/IBO (ARCHITECTURE 5.6: liquids are meshed into their own buffer).
 * Mirrors render_upload_chunk exactly but on the liq_* handles; the world
 * origin is shared with the opaque stream (set by render_upload_chunk), so this
 * does NOT touch ox/oy/oz - call it after / alongside the opaque upload. An
 * empty liquid mesh marks the liquid stream drawable-as-nothing (a chunk with
 * no liquid simply never draws in render_end's transparent pass). */
void render_upload_chunk_liquid(int slot, const MeshBuffer *m,
                                int cx, int cy, int cz)
{
    ChunkSlot *s;

    (void)cx; (void)cy; (void)cz;   /* origin is owned by the opaque upload */

    if (slot < 0 || slot >= MAX_RENDER_CHUNKS)
        return;
    s = &g_slots[slot];

    /* Empty liquid mesh: keep the slot, mark the liquid stream as nothing.
     * Pooled buffers (if any) are left orphaned-empty rather than churned. */
    if (m == NULL || m->index_count == 0 || m->vert_count == 0) {
        s->liq_index_count = 0;
        return;
    }

    /* Lazily create the pooled liquid buffer handles once; re-uploads reuse
     * them via glBufferData orphaning (never gen/delete), like the opaque path. */
    if (s->liq_vbo == 0)
        glGenBuffers(1, &s->liq_vbo);
    if (s->liq_ibo == 0)
        glGenBuffers(1, &s->liq_ibo);

    glBindBuffer(GL_ARRAY_BUFFER, s->liq_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)m->vert_count * sizeof(MeshVert)),
                 m->verts, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->liq_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)m->index_count * sizeof(uint16_t)),
                 m->indices, GL_STATIC_DRAW);

    s->liq_index_count = (GLsizei)m->index_count;
}

/* Free (clear) a render slot on chunk eviction (the WorldStore slot_free
 * callback). Zero BOTH stream index counts so render_draw_chunk's
 * `s->index_count == 0` guard and render_end's `s->liq_index_count == 0` guard
 * make the slot draw nothing, while leaving the pooled vbo/ibo/liq_* handles
 * allocated for the next chunk that takes this slot (re-upload via glBufferData
 * orphaning, never gen/delete - same pooling rule as render_upload_chunk). The
 * stale ox/oy/oz are harmless once index_count is 0 (never read by a skipped
 * draw); the next render_upload_chunk on this slot overwrites them. */
void render_free_slot(int slot)
{
    ChunkSlot *s;

    if (!g_inited || slot < 0 || slot >= MAX_RENDER_CHUNKS)
        return;
    s = &g_slots[slot];
    s->index_count     = 0;
    s->liq_index_count = 0;
}

void render_set_fog(float start, float end)
{
    g_fog_start = start;
    g_fog_end   = end;
    /* The program!=0 guards below make this safe before render_init builds them
     * (it just records the values); render_init calls us once they exist. */
    if (g_opaque.program != 0) {
        glUseProgram(g_opaque.program);
        if (g_opaque.u_fog_start >= 0) glUniform1f(g_opaque.u_fog_start, start);
        if (g_opaque.u_fog_end   >= 0) glUniform1f(g_opaque.u_fog_end,   end);
    }
    if (g_liquid.program != 0) {
        glUseProgram(g_liquid.program);
        if (g_liquid.u_fog_start >= 0) glUniform1f(g_liquid.u_fog_start, start);
        if (g_liquid.u_fog_end   >= 0) glUniform1f(g_liquid.u_fog_end,   end);
    }
    if (g_opaque.program != 0)
        glUseProgram(g_opaque.program);  /* leave opaque current */
}

void render_begin(const float *mvp4x4, float sun)
{
    if (!g_inited)
        return;

    /* Snapshot this frame's MVP + sun first so the uniform uploads below (and
     * render_end's liquid pass) all see the same values. */
    if (mvp4x4 != NULL) {
        int k;
        for (k = 0; k < 16; ++k) g_frame_mvp[k] = mvp4x4[k];
        g_frame_have_mvp = 1;
    } else {
        g_frame_have_mvp = 0;
    }
    g_frame_sun = (float)sun;

    /* Derive u_sun_tint from the live sun: a cool blue night, a warm daylight,
     * and a faint orange push through the low-sun horizon band so dawn/dusk read
     * warm. Folded entirely in the frame loop -> the day/night cycle costs ZERO
     * remeshes (sun is live in the shader). The colour is reused unchanged by
     * render_end's liquid pass via g_frame_tint. */
    {
        float s = g_frame_sun;
        float t;
        float orange;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        /* night {0.55,0.65,0.95} -> day {1.0,0.96,0.88} */
        g_frame_tint[0] = 0.55f + (1.00f - 0.55f) * s;
        g_frame_tint[1] = 0.65f + (0.96f - 0.65f) * s;
        g_frame_tint[2] = 0.95f + (0.88f - 0.95f) * s;
        /* Horizon warm push: peaks near s~0.5, zero at full night/noon. */
        t = s * (1.0f - s) * 4.0f;          /* 0 at s=0/1, 1 at s=0.5            */
        orange = t * 0.18f;
        g_frame_tint[0] += orange;          /* nudge red up                     */
        g_frame_tint[2] -= orange;          /* and blue down for a warm cast     */
        if (g_frame_tint[0] > 1.0f) g_frame_tint[0] = 1.0f;
        if (g_frame_tint[2] < 0.0f) g_frame_tint[2] = 0.0f;
    }

    /* Clear color + depth for the new frame. */
    glClear((GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

    /* Opaque-pass state (set once per frame; cheap and keeps state coherent
     * even if the liquid pass changed it last frame). */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);

    /* Atlas bound ONCE for the whole opaque pass (Section 5.1). */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);

    glUseProgram(g_opaque.program);

    /* Per-frame uniforms: the MVP and the live sun term + tint. */
    if (g_opaque.u_mvp >= 0 && mvp4x4 != NULL)
        glUniformMatrix4fv(g_opaque.u_mvp, 1, GL_FALSE, mvp4x4);
    if (g_opaque.u_sun >= 0)
        glUniform1f(g_opaque.u_sun, (GLfloat)sun);
    if (g_opaque.u_sun_tint >= 0)
        glUniform3f(g_opaque.u_sun_tint,            /* live, sun-derived */
                    g_frame_tint[0], g_frame_tint[1], g_frame_tint[2]);
}

void render_draw_chunk(int slot)
{
    ChunkSlot *s;
    const GLsizei stride = (GLsizei)sizeof(MeshVert);   /* 12 */

    if (!g_inited || slot < 0 || slot >= MAX_RENDER_CHUNKS)
        return;
    s = &g_slots[slot];
    if (s->vbo == 0 || s->ibo == 0 || s->index_count == 0)
        return;   /* empty / unset slot -> no-op */

    /* Per-chunk origin: the only per-chunk uniform (Section 5.3). */
    if (g_opaque.u_chunk_origin >= 0)
        glUniform3f(g_opaque.u_chunk_origin, s->ox, s->oy, s->oz);

    /* Bind this chunk's buffers and re-specify the 12-byte attribute pointers
     * (no VAOs -> pointers are part of per-chunk bind). Offsets/types/norms
     * exactly match the pinned MeshVert layout. */
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibo);

    glEnableVertexAttribArray(LOC_POS);
    glEnableVertexAttribArray(LOC_MAT);
    glEnableVertexAttribArray(LOC_FACE);
    glEnableVertexAttribArray(LOC_LIGHT);
    glEnableVertexAttribArray(LOC_AO);
    glEnableVertexAttribArray(LOC_UV);

    /* a_pos   : 3x u16, NOT normalized, offset 0  (raw 0..16 -> float)        */
    glVertexAttribPointer(LOC_POS,   3, GL_UNSIGNED_SHORT, GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, px));
    /* a_mat   : 1x u8,  NOT normalized, offset 6  (raw tile id 0..255)        */
    glVertexAttribPointer(LOC_MAT,   1, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, mat));
    /* a_face  : 1x u8,  NOT normalized, offset 7  (raw face dir 0..5)         */
    glVertexAttribPointer(LOC_FACE,  1, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, face));
    /* a_light : 1x u8, NON-normalized (raw 0..255: lo nibble sky, hi nibble heat) */
    glVertexAttribPointer(LOC_LIGHT, 1, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, light));
    /* a_ao    : 1x u8,  NORMALIZED 0..1, offset 9                              */
    glVertexAttribPointer(LOC_AO,    1, GL_UNSIGNED_BYTE,  GL_TRUE,  stride,
                          (const void *)(size_t)offsetof(MeshVert, ao));
    /* a_uv    : 2x u8,  NOT normalized, offset 10 (tile-local tiling corners) */
    glVertexAttribPointer(LOC_UV,    2, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, u));

    /* One draw call per chunk. 16-bit indices (Section 5.3). */
    glDrawElements(GL_TRIANGLES, s->index_count, GL_UNSIGNED_SHORT,
                   (const void *)0);
}

/* Set the 12-byte MeshVert attribute pointers for the currently-bound
 * GL_ARRAY_BUFFER. The liquid stream uses the SAME MeshVert layout as the
 * opaque stream, so this mirrors render_draw_chunk's inline pointer setup
 * byte-for-byte (offsets/types/normalize flags); keep the two in lockstep if
 * the vertex format ever changes. */
static void set_meshvert_attrib_pointers(void)
{
    const GLsizei stride = (GLsizei)sizeof(MeshVert);   /* 12 */

    glEnableVertexAttribArray(LOC_POS);
    glEnableVertexAttribArray(LOC_MAT);
    glEnableVertexAttribArray(LOC_FACE);
    glEnableVertexAttribArray(LOC_LIGHT);
    glEnableVertexAttribArray(LOC_AO);
    glEnableVertexAttribArray(LOC_UV);

    glVertexAttribPointer(LOC_POS,   3, GL_UNSIGNED_SHORT, GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, px));
    glVertexAttribPointer(LOC_MAT,   1, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, mat));
    glVertexAttribPointer(LOC_FACE,  1, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, face));
    glVertexAttribPointer(LOC_LIGHT, 1, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, light));
    glVertexAttribPointer(LOC_AO,    1, GL_UNSIGNED_BYTE,  GL_TRUE,  stride,
                          (const void *)(size_t)offsetof(MeshVert, ao));
    glVertexAttribPointer(LOC_UV,    2, GL_UNSIGNED_BYTE,  GL_FALSE, stride,
                          (const void *)(size_t)offsetof(MeshVert, u));
}

void render_end(void)
{
    int i;
    int drew_liquid = 0;

    if (!g_inited)
        return;

    /* ---- Liquid / transparent pass (ARCHITECTURE 5.6) -------------------- *
     * Runs after the opaque pass has filled the depth buffer. Depth TEST stays
     * on (liquids hide behind opaque terrain), depth WRITE goes OFF (a near
     * water surface must not occlude farther water), alpha blending on with the
     * standard over-blend. Alpha is data-driven: it comes from the atlas tile's
     * alpha (baked from MaterialDef.color_rgba[3]; water a=200), which FS_LIQUID
     * passes through as gl_FragColor.a - no shader edit per new material.
     *
     * Ordering is back-to-front at CHUNK granularity. This milestone is
     * single-chunk (the demo has one liquid chunk), so we simply draw every
     * non-empty liquid slot in slot order; a camera-distance sort is the
     * follow-up when the world has multiple liquid chunks (render_end would
     * then take a camera position). Drawing all slots is correct for one chunk
     * and harmless (just unsorted) for several near-opaque ones. */
    if (g_liquid.program != 0) {
        glDepthMask(GL_FALSE);          /* test against opaque depth, do NOT write */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(g_liquid.program);

        /* Atlas stays bound from the opaque pass (same texture unit 0); re-bind
         * defensively so the liquid pass is self-contained. */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_atlas_tex);

        /* Per-frame uniforms on the liquid program (its own program object, so
         * its uniform locations are distinct from the opaque program's). */
        if (g_liquid.u_mvp >= 0 && g_frame_have_mvp)
            glUniformMatrix4fv(g_liquid.u_mvp, 1, GL_FALSE, g_frame_mvp);
        if (g_liquid.u_sun >= 0)
            glUniform1f(g_liquid.u_sun, (GLfloat)g_frame_sun);
        if (g_liquid.u_sun_tint >= 0)
            glUniform3f(g_liquid.u_sun_tint,        /* same as opaque pass */
                        g_frame_tint[0], g_frame_tint[1], g_frame_tint[2]);

        for (i = 0; i < MAX_RENDER_CHUNKS; ++i) {
            ChunkSlot *s = &g_slots[i];
            if (s->liq_vbo == 0 || s->liq_ibo == 0 || s->liq_index_count == 0)
                continue;   /* no liquid geometry in this slot */

            if (g_liquid.u_chunk_origin >= 0)
                glUniform3f(g_liquid.u_chunk_origin, s->ox, s->oy, s->oz);

            glBindBuffer(GL_ARRAY_BUFFER, s->liq_vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->liq_ibo);
            set_meshvert_attrib_pointers();

            glDrawElements(GL_TRIANGLES, s->liq_index_count,
                           GL_UNSIGNED_SHORT, (const void *)0);
            drew_liquid = 1;
        }

        /* Restore the opaque-pass GL state so the NEXT frame's opaque pass and
         * any other module's draw start clean (depth write on, no blend). */
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glUseProgram(g_opaque.program);
    }
    (void)drew_liquid;   /* kept for future culling diagnostics */

    /* Tidy the attribute-array enables so a stale enabled array cannot leak
     * into another module's draw. Done once at the end of the frame, covering
     * both the opaque and (if run) the liquid pass. */
    glDisableVertexAttribArray(LOC_POS);
    glDisableVertexAttribArray(LOC_MAT);
    glDisableVertexAttribArray(LOC_FACE);
    glDisableVertexAttribArray(LOC_LIGHT);
    glDisableVertexAttribArray(LOC_AO);
    glDisableVertexAttribArray(LOC_UV);

    /* Buffer swap is the platform's job (plat_swap_buffers). */
}

void render_highlight_voxel(int wx, int wy, int wz)
{
    if (!g_inited || g_overlay_prog == 0 || g_wire_vbo == 0 || !g_frame_have_mvp)
        return;

    glUseProgram(g_overlay_prog);
    if (g_overlay_u_mvp >= 0)
        glUniformMatrix4fv(g_overlay_u_mvp, 1, GL_FALSE, g_frame_mvp);
    if (g_overlay_u_origin >= 0)
        glUniform3f(g_overlay_u_origin, (GLfloat)wx, (GLfloat)wy, (GLfloat)wz);
    if (g_overlay_u_color >= 0)
        glUniform3f(g_overlay_u_color, 0.05f, 0.05f, 0.05f);   /* near-black outline */
    if (g_overlay_u_alpha >= 0) glUniform1f(g_overlay_u_alpha, 1.0f);

    /* Depth-TESTED (a highlight behind a wall stays hidden) but no depth WRITE,
     * no blend, no cull (lines have no winding). Drawn after render_end so it
     * overlays both the opaque and liquid passes. */
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glLineWidth(2.0f);

    glBindBuffer(GL_ARRAY_BUFFER, g_wire_vbo);
    glEnableVertexAttribArray(LOC_POS);
    glVertexAttribPointer(LOC_POS, 3, GL_FLOAT, GL_FALSE,
                          (GLsizei)(3 * sizeof(GLfloat)), (const void *)0);
    glDrawArrays(GL_LINES, 0, 24);
    glDisableVertexAttribArray(LOC_POS);

    /* Restore the state the next frame's opaque pass expects. */
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(g_opaque.program);
}

void render_avatar(const float pos[3], const float color[3])
{
    if (!g_inited || g_overlay_prog == 0 || g_avatar_vbo == 0 || !g_frame_have_mvp || pos == NULL)
        return;

    glUseProgram(g_overlay_prog);
    if (g_overlay_u_mvp >= 0)
        glUniformMatrix4fv(g_overlay_u_mvp, 1, GL_FALSE, g_frame_mvp);
    if (g_overlay_u_origin >= 0)
        glUniform3f(g_overlay_u_origin, pos[0], pos[1], pos[2]);
    if (g_overlay_u_color >= 0)
        glUniform3f(g_overlay_u_color,
                    color ? color[0] : 1.0f, color ? color[1] : 1.0f, color ? color[2] : 1.0f);
    if (g_overlay_u_alpha >= 0) glUniform1f(g_overlay_u_alpha, 1.0f);

    /* A solid scene object (unlike the highlight): depth-tested AND depth-written
     * so terrain occludes it and it occludes terrain. Cull off (winding-agnostic
     * cube). Drawn after render_end, so it sits in the already-rendered depth. */
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glBindBuffer(GL_ARRAY_BUFFER, g_avatar_vbo);
    glEnableVertexAttribArray(LOC_POS);
    glVertexAttribPointer(LOC_POS, 3, GL_FLOAT, GL_FALSE,
                          (GLsizei)(3 * sizeof(GLfloat)), (const void *)0);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glDisableVertexAttribArray(LOC_POS);

    glEnable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(g_opaque.program);
}

void render_crosshair(float aspect)
{
    /* A small + at the screen centre, drawn directly in clip space: the overlay
     * VS computes u_mvp * vec4(a_pos,1), so a diagonal scale matrix maps the unit
     * cross to NDC. The x scale is divided by aspect so the cross stays SQUARE
     * (equal pixel length on both axes) on a non-square window. Always on top
     * (depth test off), light colour so it reads over terrain. */
    const float SIZE = 0.018f;             /* half-length in NDC */
    float m[16];
    int i;
    if (!g_inited || g_overlay_prog == 0 || g_cross_vbo == 0)
        return;
    for (i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0]  = (aspect > 0.0f) ? SIZE / aspect : SIZE;
    m[5]  = SIZE;
    m[10] = 1.0f;
    m[15] = 1.0f;

    glUseProgram(g_overlay_prog);
    if (g_overlay_u_mvp >= 0)    glUniformMatrix4fv(g_overlay_u_mvp, 1, GL_FALSE, m);
    if (g_overlay_u_origin >= 0) glUniform3f(g_overlay_u_origin, 0.0f, 0.0f, 0.0f);
    if (g_overlay_u_color >= 0)  glUniform3f(g_overlay_u_color, 0.90f, 0.90f, 0.90f);
    if (g_overlay_u_alpha >= 0)  glUniform1f(g_overlay_u_alpha, 1.0f);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glLineWidth(2.0f);

    glBindBuffer(GL_ARRAY_BUFFER, g_cross_vbo);
    glEnableVertexAttribArray(LOC_POS);
    glVertexAttribPointer(LOC_POS, 3, GL_FLOAT, GL_FALSE,
                          (GLsizei)(3 * sizeof(GLfloat)), (const void *)0);
    glDrawArrays(GL_LINES, 0, 4);
    glDisableVertexAttribArray(LOC_POS);

    /* Restore the defaults the next frame's opaque pass expects. */
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(g_opaque.program);
}

/* ======================================================================== *
 *  2D UI: bitmap text + filled rects (0.3 pause menu, future HUD)           *
 * ------------------------------------------------------------------------ *
 * Reuses the overlay program (position-only + u_color + u_alpha) drawn in
 * SCREEN space: positions are fed straight in NDC (u_mvp = identity,
 * u_origin = 0), as filled GL_TRIANGLES. The font is the public-domain 8x8
 * "font8x8_basic" (printable ASCII 32..126); each glyph row is a byte, bit 0 =
 * leftmost column. Glyphs are drawn as one filled quad per set pixel - plenty
 * fast for menu/HUD text. NDC is [-1,1] on both axes over a wider viewport, so a
 * pixel's NDC width is its height / aspect to stay square on screen.            */
static const unsigned char FONT8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* (space) */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}  /* ~ */
};

#define UI_MAX_QUADS 4096
#define UI_IDENTITY { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }

/* Append a screen-space quad [x0,y0]-[x1,y1] (two tris) to a float builder. */
static void ui_emit_quad(float *buf, int *n, float x0, float y0, float x1, float y1)
{
    float v[18];
    int i;
    v[0]=x0; v[1]=y0; v[2]=0;  v[3]=x1; v[4]=y0; v[5]=0;  v[6]=x1; v[7]=y1; v[8]=0;
    v[9]=x0; v[10]=y0;v[11]=0; v[12]=x1;v[13]=y1;v[14]=0; v[15]=x0;v[16]=y1;v[17]=0;
    for (i = 0; i < 18; ++i) buf[(*n)++] = v[i];
}

/* Draw `nfloats/3` NDC vertices as triangles in colour (r,g,b,a) via the overlay
 * program; manages its own GL state (blend on, depth off) + restores. */
static void ui_draw(const float *verts, int nfloats, float r, float g, float b, float a)
{
    static const float I[16] = UI_IDENTITY;
    if (!g_inited || g_overlay_prog == 0 || g_ui_vbo == 0 || nfloats <= 0) return;

    glUseProgram(g_overlay_prog);
    if (g_overlay_u_mvp    >= 0) glUniformMatrix4fv(g_overlay_u_mvp, 1, GL_FALSE, I);
    if (g_overlay_u_origin >= 0) glUniform3f(g_overlay_u_origin, 0.0f, 0.0f, 0.0f);
    if (g_overlay_u_color  >= 0) glUniform3f(g_overlay_u_color, r, g, b);
    if (g_overlay_u_alpha  >= 0) glUniform1f(g_overlay_u_alpha, a);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindBuffer(GL_ARRAY_BUFFER, g_ui_vbo);
    /* GL_STATIC_DRAW (not DYNAMIC): the engine's GL loader only exposes the
     * constants it uses, and the chunk VBOs already re-upload via glBufferData
     * orphaning with STATIC_DRAW - the usage hint is advisory and portable. */
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nfloats * sizeof(float)), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(LOC_POS);
    glVertexAttribPointer(LOC_POS, 3, GL_FLOAT, GL_FALSE, (GLsizei)(3 * sizeof(float)), (const void *)0);
    glDrawArrays(GL_TRIANGLES, 0, nfloats / 3);
    glDisableVertexAttribArray(LOC_POS);

    /* Restore the defaults the next frame's opaque pass expects. */
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(g_opaque.program);
}

void render_ui_rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
    float v[18];
    int n = 0;
    ui_emit_quad(v, &n, x0, y0, x1, y1);
    ui_draw(v, n, r, g, b, a);
}

void render_text(float x, float y, float cell, float aspect,
                 float r, float g, float b, float a, const char *s)
{
    static float buf[UI_MAX_QUADS * 18];
    int n = 0;
    float px = cell / 8.0f;                       /* per-pixel height (NDC-Y)        */
    float pw = (aspect > 0.0f) ? px / aspect : px;/* per-pixel width (square on screen) */
    float gx = x;
    const unsigned char *p;
    if (s == NULL) return;
    for (p = (const unsigned char *)s; *p; ++p) {
        int c = *p, row, col;
        if (c == '\n') { gx = x; y -= cell * 1.5f; continue; }
        if (c >= 32 && c <= 126) {
            const unsigned char *gl = FONT8X8[c - 32];
            for (row = 0; row < 8; ++row) {
                unsigned char bits = gl[row];
                if (!bits) continue;
                for (col = 0; col < 8; ++col) {
                    if (bits & (1u << col)) {
                        float qx0 = gx + (float)col * pw;
                        float qy1 = y - (float)row * px;   /* row 0 = glyph top (highest y) */
                        if (n + 18 > UI_MAX_QUADS * 18) { ui_draw(buf, n, r, g, b, a); return; }
                        ui_emit_quad(buf, &n, qx0, qy1 - px, qx0 + pw, qy1);
                    }
                }
            }
        }
        gx += 9.0f * pw;                           /* 8-px glyph + 1-px gap           */
    }
    ui_draw(buf, n, r, g, b, a);
}

void render_shutdown(void)
{
    int i;

    for (i = 0; i < MAX_RENDER_CHUNKS; ++i) {
        if (g_slots[i].vbo != 0) {
            glDeleteBuffers(1, &g_slots[i].vbo);
            g_slots[i].vbo = 0;
        }
        if (g_slots[i].ibo != 0) {
            glDeleteBuffers(1, &g_slots[i].ibo);
            g_slots[i].ibo = 0;
        }
        g_slots[i].index_count = 0;
        if (g_slots[i].liq_vbo != 0) {
            glDeleteBuffers(1, &g_slots[i].liq_vbo);
            g_slots[i].liq_vbo = 0;
        }
        if (g_slots[i].liq_ibo != 0) {
            glDeleteBuffers(1, &g_slots[i].liq_ibo);
            g_slots[i].liq_ibo = 0;
        }
        g_slots[i].liq_index_count = 0;
    }

    if (g_atlas_tex != 0) {
        glDeleteTextures(1, &g_atlas_tex);
        g_atlas_tex = 0;
    }

    if (g_wire_vbo != 0) {
        glDeleteBuffers(1, &g_wire_vbo);
        g_wire_vbo = 0;
    }
    if (g_cross_vbo != 0) {
        glDeleteBuffers(1, &g_cross_vbo);
        g_cross_vbo = 0;
    }
    if (g_avatar_vbo != 0) {
        glDeleteBuffers(1, &g_avatar_vbo);
        g_avatar_vbo = 0;
    }
    if (g_ui_vbo != 0) {
        glDeleteBuffers(1, &g_ui_vbo);
        g_ui_vbo = 0;
    }
    if (g_overlay_prog != 0) {
        glDeleteProgram(g_overlay_prog);
        g_overlay_prog = 0;
    }
    if (g_liquid.program != 0) {
        glDeleteProgram(g_liquid.program);
        g_liquid.program = 0;
    }
    if (g_opaque.program != 0) {
        glDeleteProgram(g_opaque.program);
        g_opaque.program = 0;
    }

    g_inited = 0;
}

/* ---- Headless framebuffer capture (PPM P6) ------------------------------- *
 * glReadPixels the rendered buffer and write a binary PPM. Records exactly what
 * GL drew, independent of the desktop/compositor/screensaver - the reliable way
 * to verify a frame on a dev box. Call after render_end(), before the swap. */
int render_screenshot_ppm(const char *path, int width, int height)
{
    size_t         row_bytes;
    unsigned char *px;
    FILE          *f;
    int            row;

    if (path == NULL || width <= 0 || height <= 0)
        return 1;

    row_bytes = (size_t)width * 3;
    px = (unsigned char *)malloc(row_bytes * (size_t)height);
    if (px == NULL)
        return 1;

    /* Default read buffer is GL_BACK on a double-buffered context; tightly pack
     * rows so the stride is exactly width*3 with no row padding. */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, px);

    f = fopen(path, "wb");
    if (f == NULL) {
        free(px);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    /* glReadPixels returns bottom-to-top; PPM is top-to-bottom, so flip rows. */
    for (row = height - 1; row >= 0; --row)
        fwrite(px + (size_t)row * row_bytes, 1, row_bytes, f);
    fclose(f);
    free(px);
    return 0;
}
