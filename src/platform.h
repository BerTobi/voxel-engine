/* platform.h - The thin opaque platform layer the engine talks to.
 *
 * ONE engine core; two backends behind this identical API: a Win32+wgl backend
 * (the real XP target) and an X11+GLX backend (the Linux dev/test box). ZERO
 * third-party libraries - no SDL/GLFW. The engine never includes a windowing
 * header; it only ever calls these functions. Keep this tiny.
 *
 * GL function pointers are loaded by passing plat_gl_getproc to gl_load()
 * (see gl_loader.h): the platform owns the GL context, so it owns getproc.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

/* Create the window + a GL 2.1 context and make it current.
 * Returns 0 on success, non-zero on failure. */
int    plat_create_window(int w, int h, const char *title);

/* The platform's GL entry-point resolver (wglGetProcAddress / glXGetProcAddressARB).
 * Pass this straight to gl_load(). */
void  *plat_gl_getproc(const char *name);

/* Pump the OS event queue (input, resize, close). Call once per frame. */
void   plat_poll(void);

/* Non-zero once the user has requested the window close. */
int    plat_should_close(void);

/* Present the back buffer (SwapBuffers / glXSwapBuffers; vsync if enabled). */
void   plat_swap_buffers(void);

/* Current drawable size in pixels (the live client/window size, which tracks
 * user resizes; the backend resizes the GL viewport to match). Either pointer
 * may be NULL. Before the window exists, reports the requested size or 0. The
 * engine reads this each frame to keep the projection aspect ratio correct. */
void   plat_get_size(int *w, int *h);

/* Monotonic high-resolution clock in milliseconds since startup, as a double.
 * QueryPerformanceCounter on XP/M170, clock_gettime(CLOCK_MONOTONIC) on Linux. */
double plat_time_ms(void);

/* Non-zero while the given key is held down. Use the PLAT_KEY_* codes below. */
int    plat_key_down(int keycode);

/* ---- Relative mouse look (FPS free-look) --------------------------------- *
 * The engine drives an FPS camera from RELATIVE pointer motion, never absolute
 * cursor coordinates: while capture is on the cursor is hidden and pinned to the
 * window centre, so there is no on-screen pointer to read - only the per-frame
 * displacement matters. These two calls keep the backend contract tiny and
 * windowing-agnostic; both backends implement the same recenter-and-hide scheme
 * (X11: XWarpPointer + an invisible XCreatePixmapCursor; Win32: SetCursorPos +
 * ShowCursor(FALSE)). No raw-input / no post-XP APIs are required. */

/* Accumulated relative pointer motion since the LAST call, in pixels, then ZERO
 * the accumulator (read-and-clear). *dx is +right, *dy is +down (screen-natural;
 * main.c flips dy into pitch). Returns (0,0) when capture is off or no motion
 * occurred. Either pointer may be NULL (that axis is then discarded). Call once
 * per LIVE frame, right after plat_poll(), to feed yaw/pitch. The recenter warp
 * the backend performs to keep the pointer inside the window does NOT leak into
 * this delta (the backends discard the warp-induced motion - see each .c). */
void   plat_mouse_delta(int *dx, int *dy);

/* Turn mouse capture on (non-zero) or off (0). While ON: hide the cursor and
 * grab/recenter it to the window centre each poll so motion is unbounded and the
 * pointer never escapes or clicks through; the next plat_mouse_delta reflects
 * motion relative to that centre. While OFF: show the cursor and release it, and
 * plat_mouse_delta returns (0,0). Idempotent (calling on->on or off->off is a
 * no-op). Safe to call before the window exists (no-op) and is implicitly OFF at
 * startup. main.c enables it for a live session and leaves it OFF in VOXEL_SHOT
 * mode so headless captures use a fixed, env-driven orientation. The backend
 * also releases capture automatically on window close so the cursor reappears. */
void   plat_set_mouse_capture(int on);

/* ---- Keycodes (engine-portable; the backend maps OS scancodes to these) -- */
#define PLAT_KEY_ESC     0
#define PLAT_KEY_W       1
#define PLAT_KEY_A       2
#define PLAT_KEY_S       3
#define PLAT_KEY_D       4
#define PLAT_KEY_SPACE   5
#define PLAT_KEY_LSHIFT  6
#define PLAT_KEY_COUNT   7   /* size of the backend's key-state array */

#endif /* PLATFORM_H */
