/* gl_loader.h - Manually-loaded OpenGL 2.1 function pointers.
 *
 * ZERO third-party libraries (no GLEW): every GL entry point past GL 1.1 is
 * loaded by hand through a platform getproc (wglGetProcAddress on Win32,
 * glXGetProcAddressARB on X11/GLX). This header declares the typedefs and
 * extern pointers for the GL2.1/GLSL1.20 subset the engine actually uses, plus
 * gl_load() which fills them.
 *
 * HARD CONSTRAINTS honoured: no VAOs, no UBOs, no instancing, no
 * geometry/compute shaders, no glGenerateMipmap (GL 3.0). VBOs via the core
 * GL 2.0 API. Mipmaps via the GL_GENERATE_MIPMAP texture parameter (core 1.4)
 * + GL_TEXTURE_MAX_LEVEL (core 1.2), set with glTexParameteri - so no separate
 * mipmap-generation entry point is loaded here.
 *
 * Functions that are part of GL 1.1 (present in opengl32.dll / libGL directly:
 * glEnable, glDisable, glClear, glClearColor, glViewport, glDrawArrays,
 * glGenTextures, glBindTexture, glTexImage2D, glTexParameteri, glDrawElements,
 * glDepthFunc, glDepthMask, glCullFace, glBlendFunc) are NOT loaded here; the
 * platform layer links them statically. This loader covers the post-1.1 entry
 * points (VBOs, the shader pipeline, multitexture) that MUST come via getproc.
 */
#ifndef GL_LOADER_H
#define GL_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* ---- Minimal GL types (we do not include <GL/gl.h> to stay self-contained;
 * the platform .c that links real GL must keep these compatible). ---------- */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef char           GLchar;
typedef ptrdiff_t      GLsizeiptr;
typedef ptrdiff_t      GLintptr;
typedef void           GLvoid;

/* Calling convention: GL entry points are __stdcall on Win32 (APIENTRY) and
 * plain cdecl elsewhere. The function-pointer typedefs MUST carry it, or calls
 * made through them corrupt the stack on Windows (the real ship target). */
#ifndef APIENTRY
# ifdef _WIN32
#  define APIENTRY __stdcall
# else
#  define APIENTRY
# endif
#endif

/* ---- Function-pointer typedefs ------------------------------------------ */

/* Buffer objects (ARB_vertex_buffer_object, core GL 1.5/2.0) */
typedef void (APIENTRY *PFN_glGenBuffers)(GLsizei n, GLuint *buffers);
typedef void (APIENTRY *PFN_glDeleteBuffers)(GLsizei n, const GLuint *buffers);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum target, GLuint buffer);
typedef void (APIENTRY *PFN_glBufferData)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void (APIENTRY *PFN_glBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);

/* Generic vertex attributes (GL 2.0) */
typedef void (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint index);
typedef void (APIENTRY *PFN_glDisableVertexAttribArray)(GLuint index);
typedef void (APIENTRY *PFN_glVertexAttribPointer)(GLuint index, GLint size, GLenum type,
                                          GLboolean normalized, GLsizei stride,
                                          const void *pointer);
typedef void (APIENTRY *PFN_glBindAttribLocation)(GLuint program, GLuint index, const GLchar *name);

/* Shader objects + program pipeline (GL 2.0) */
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum type);
typedef void   (APIENTRY *PFN_glDeleteShader)(GLuint shader);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint shader);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint shader, GLenum pname, GLint *params);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glDeleteProgram)(GLuint program);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint program, GLuint shader);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint program);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint program, GLenum pname, GLint *params);
typedef void   (APIENTRY *PFN_glGetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint program);

/* Uniforms (GL 2.0) */
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint program, const GLchar *name);
typedef void   (APIENTRY *PFN_glUniform1i)(GLint location, GLint v0);
typedef void   (APIENTRY *PFN_glUniform1f)(GLint location, GLfloat v0);
typedef void   (APIENTRY *PFN_glUniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void   (APIENTRY *PFN_glUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

/* Multitexture (core GL 1.3). Declared only on Win32: MinGW's <GL/gl.h> stops
 * at GL 1.1 so the engine loads it itself, whereas Mesa's <GL/gl.h> already
 * declares it (and libGL exports it) - declaring our own pointer there clashes. */
#ifdef _WIN32
typedef void (APIENTRY *PFN_glActiveTexture)(GLenum texture);
#endif

/* ---- Extern function pointers (defined in gl_loader.c) ------------------ */
extern PFN_glGenBuffers                 glGenBuffers;
extern PFN_glDeleteBuffers              glDeleteBuffers;
extern PFN_glBindBuffer                 glBindBuffer;
extern PFN_glBufferData                 glBufferData;
extern PFN_glBufferSubData              glBufferSubData;

extern PFN_glEnableVertexAttribArray    glEnableVertexAttribArray;
extern PFN_glDisableVertexAttribArray   glDisableVertexAttribArray;
extern PFN_glVertexAttribPointer        glVertexAttribPointer;
extern PFN_glBindAttribLocation         glBindAttribLocation;

extern PFN_glCreateShader               glCreateShader;
extern PFN_glDeleteShader               glDeleteShader;
extern PFN_glShaderSource               glShaderSource;
extern PFN_glCompileShader              glCompileShader;
extern PFN_glGetShaderiv                glGetShaderiv;
extern PFN_glGetShaderInfoLog           glGetShaderInfoLog;
extern PFN_glCreateProgram              glCreateProgram;
extern PFN_glDeleteProgram              glDeleteProgram;
extern PFN_glAttachShader               glAttachShader;
extern PFN_glLinkProgram                glLinkProgram;
extern PFN_glGetProgramiv               glGetProgramiv;
extern PFN_glGetProgramInfoLog          glGetProgramInfoLog;
extern PFN_glUseProgram                 glUseProgram;

extern PFN_glGetUniformLocation         glGetUniformLocation;
extern PFN_glUniform1i                  glUniform1i;
extern PFN_glUniform1f                  glUniform1f;
extern PFN_glUniform3f                  glUniform3f;
extern PFN_glUniformMatrix4fv           glUniformMatrix4fv;

#ifdef _WIN32
extern PFN_glActiveTexture              glActiveTexture;
#endif

/* Load every pointer above via the supplied getproc (platform-specific).
 * Returns 0 on success; non-zero = the index+1 of the first pointer that
 * failed to resolve, so the caller can report which entry point is missing. */
int gl_load(void *(*getproc)(const char *name));

/* The human-readable name of the entry point that failed to resolve in the most
 * recent gl_load() call (NULL on success). Lets the caller report e.g.
 * "glCreateShader missing" rather than just an index. Defined in gl_loader.c. */
extern const char *gl_load_failed_name;

#endif /* GL_LOADER_H */
