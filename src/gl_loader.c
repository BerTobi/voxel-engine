/* gl_loader.c - Definitions + manual resolution of the GL2.1/GLSL1.20 entry
 * points declared in gl_loader.h.
 *
 * ZERO third-party libraries (no GLEW): every pointer below is filled by
 * calling the platform-supplied getproc (wglGetProcAddress on Win32,
 * glXGetProcAddressARB on X11/GLX). The platform owns the GL context, so the
 * platform owns getproc; we just take it as an argument.
 *
 * This translation unit is intentionally self-contained: gl_loader.h already
 * declares every GL base type (GLenum, GLuint, GLsizeiptr, ...) and every
 * PFN_* function-pointer typedef the engine uses, so we do NOT include
 * <GL/gl.h>. Pulling in the system GL header here would risk clashing
 * typedefs/macros against the minimal set the header pins, and the whole point
 * of the header is to stay independent of whatever (often ancient) GL headers
 * a given build machine ships. The platform .c files that link real GL are the
 * ones responsible for keeping these typedefs ABI-compatible with the driver.
 *
 * NOTE: this file depends on no OS/GL headers and is pure C99; everything it
 * touches is declared in gl_loader.h.
 */
#include "gl_loader.h"

/* ---- Definitions of the extern pointers declared in gl_loader.h ---------- */
/* All start NULL so a partially-loaded set is obviously broken if some caller
 * ever uses one before gl_load() succeeds. */

PFN_glGenBuffers                 glGenBuffers                 = 0;
PFN_glDeleteBuffers              glDeleteBuffers              = 0;
PFN_glBindBuffer                 glBindBuffer                 = 0;
PFN_glBufferData                 glBufferData                 = 0;
PFN_glBufferSubData              glBufferSubData              = 0;

PFN_glEnableVertexAttribArray    glEnableVertexAttribArray    = 0;
PFN_glDisableVertexAttribArray   glDisableVertexAttribArray   = 0;
PFN_glVertexAttribPointer        glVertexAttribPointer        = 0;
PFN_glBindAttribLocation         glBindAttribLocation         = 0;

PFN_glCreateShader               glCreateShader               = 0;
PFN_glDeleteShader               glDeleteShader               = 0;
PFN_glShaderSource               glShaderSource               = 0;
PFN_glCompileShader              glCompileShader              = 0;
PFN_glGetShaderiv                glGetShaderiv                = 0;
PFN_glGetShaderInfoLog           glGetShaderInfoLog           = 0;
PFN_glCreateProgram              glCreateProgram              = 0;
PFN_glDeleteProgram              glDeleteProgram              = 0;
PFN_glAttachShader               glAttachShader               = 0;
PFN_glLinkProgram                glLinkProgram                = 0;
PFN_glGetProgramiv               glGetProgramiv               = 0;
PFN_glGetProgramInfoLog          glGetProgramInfoLog          = 0;
PFN_glUseProgram                 glUseProgram                 = 0;

PFN_glGetUniformLocation         glGetUniformLocation         = 0;
PFN_glUniform1i                  glUniform1i                  = 0;
PFN_glUniform1f                  glUniform1f                  = 0;
PFN_glUniform3f                  glUniform3f                  = 0;
PFN_glUniformMatrix4fv           glUniformMatrix4fv           = 0;

#ifdef _WIN32
/* GL 1.3 multitexture: loaded only on Windows. Mesa's <GL/gl.h> + libGL provide
 * glActiveTexture directly on Linux, so we must not also define it there. */
PFN_glActiveTexture              glActiveTexture              = 0;
#endif

/* ---- The load table ------------------------------------------------------ */
/* One row per pointer: the GL name to ask getproc for, and the address of the
 * slot to fill. Driving gl_load() off a table (instead of 30 hand-written
 * assignments) keeps the loader a single loop and makes "which symbol failed"
 * fall out for free as the row index. The name string and the slot must always
 * refer to the same entry point - that pairing is the only thing to get right.
 *
 * We store each slot as a `void **` and cast getproc's result through a
 * function-pointer-shaped union member to dodge the ISO C "object pointer <->
 * function pointer" conversion wrinkle while staying warning-clean; in
 * practice every platform getproc here (wgl/glX) returns a function address. */

typedef struct {
    const char  *name;   /* GL entry-point name passed to getproc           */
    void       **slot;   /* address of the extern pointer above to fill in  */
} GlProcEntry;

/* Helper: a row for entry point X expands to its name + the address of the
 * matching global. The (void **)&NAME cast is safe: each global is itself a
 * pointer object, so &NAME is a pointer-to-pointer; we only ever read it back
 * through the same typed lvalue. */
#define GL_PROC_ROW(NAME) { #NAME, (void **)&NAME }

static const GlProcEntry gl_proc_table[] = {
    /* Buffer objects (core GL 1.5/2.0) */
    GL_PROC_ROW(glGenBuffers),
    GL_PROC_ROW(glDeleteBuffers),
    GL_PROC_ROW(glBindBuffer),
    GL_PROC_ROW(glBufferData),
    GL_PROC_ROW(glBufferSubData),

    /* Generic vertex attributes (GL 2.0) */
    GL_PROC_ROW(glEnableVertexAttribArray),
    GL_PROC_ROW(glDisableVertexAttribArray),
    GL_PROC_ROW(glVertexAttribPointer),
    GL_PROC_ROW(glBindAttribLocation),

    /* Shader objects + program pipeline (GL 2.0) */
    GL_PROC_ROW(glCreateShader),
    GL_PROC_ROW(glDeleteShader),
    GL_PROC_ROW(glShaderSource),
    GL_PROC_ROW(glCompileShader),
    GL_PROC_ROW(glGetShaderiv),
    GL_PROC_ROW(glGetShaderInfoLog),
    GL_PROC_ROW(glCreateProgram),
    GL_PROC_ROW(glDeleteProgram),
    GL_PROC_ROW(glAttachShader),
    GL_PROC_ROW(glLinkProgram),
    GL_PROC_ROW(glGetProgramiv),
    GL_PROC_ROW(glGetProgramInfoLog),
    GL_PROC_ROW(glUseProgram),

    /* Uniforms (GL 2.0) */
    GL_PROC_ROW(glGetUniformLocation),
    GL_PROC_ROW(glUniform1i),
    GL_PROC_ROW(glUniform1f),
    GL_PROC_ROW(glUniform3f),
    GL_PROC_ROW(glUniformMatrix4fv),

#ifdef _WIN32
    /* Multitexture (core GL 1.3) - Win32 only; Linux gets it from libGL. */
    GL_PROC_ROW(glActiveTexture),
#endif
};

#define GL_PROC_COUNT ((int)(sizeof(gl_proc_table) / sizeof(gl_proc_table[0])))

/* Name of the entry point that failed to resolve in the most recent gl_load()
 * call, or NULL on success. gl_load()'s int return is just the row index + 1
 * (per gl_loader.h), so this gives the caller the human-readable symbol to log
 * without changing the header's signature. */
const char *gl_load_failed_name = 0;

/* wglGetProcAddress (and, rarely, broken glX ICDs) can hand back small
 * non-NULL sentinel handles (1, 2, 3, or (void*)-1) for an *exported but
 * unsupported* entry point instead of a real address. Treat those as failures
 * too, otherwise we'd store a poison pointer and crash on first call. This is
 * the documented Win32/G70-era hazard and costs nothing on sane drivers. */
static int gl_proc_is_valid(void *p)
{
    if (p == 0)               return 0;
    if (p == (void *)-1)      return 0;
    if (p == (void *)1)       return 0;
    if (p == (void *)2)       return 0;
    if (p == (void *)3)       return 0;
    return 1;
}

int gl_load(void *(*getproc)(const char *name))
{
    int i;

    gl_load_failed_name = 0;

    if (getproc == 0) {
        /* No resolver at all: report the first entry as the failure so the
         * caller still gets a non-zero result and a name to print. */
        gl_load_failed_name = gl_proc_table[0].name;
        return 1;
    }

    for (i = 0; i < GL_PROC_COUNT; ++i) {
        void *p = getproc(gl_proc_table[i].name);
        if (!gl_proc_is_valid(p)) {
            gl_load_failed_name = gl_proc_table[i].name;
            *gl_proc_table[i].slot = 0;
            return i + 1;   /* non-zero; index+1 identifies the missing symbol */
        }
        *gl_proc_table[i].slot = p;
    }

    return 0;   /* every pointer resolved */
}
