/* platform_linux.c - X11 + GLX backend of platform.h (the dev/test box).
 *
 * _POSIX_C_SOURCE must precede every include: under -std=c99 the toolchain
 * hides POSIX-only clock_gettime/CLOCK_MONOTONIC/struct timespec otherwise. */
#define _POSIX_C_SOURCE 199309L
/*
 * This is the Linux development/testing backend. The real ship target is the
 * Win32+wgl backend (platform_win32.c); this file exists so the ONE portable
 * GL2.1/GLSL1.20 engine core builds and runs on a modern Linux box (Mesa 4.6
 * compatibility profile, a strict superset of GL 2.1) for day-to-day work.
 *
 * ZERO third-party libraries: we talk to Xlib + GLX directly and load GL entry
 * points by hand through glXGetProcAddressARB (handed to gl_load()). We request
 * a compatibility context that provides >= 2.1; on Mesa the legacy glXCreate-
 * Context path already yields a compatibility context, and we additionally try
 * GLX_ARB_create_context to pin a >=2.1 compatibility profile when available.
 *
 * Guarded with #ifndef _WIN32 so a single Makefile can compile both backends
 * and let the Win32 one win on Windows.
 */
#ifndef _WIN32

#include "platform.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <GL/glx.h>

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* GLX_ARB_create_context tokens. These may not be in older <GL/glx.h>, so we
 * define them defensively rather than depend on the system header version. */
#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB             0x2091
#endif
#ifndef GLX_CONTEXT_MINOR_VERSION_ARB
#define GLX_CONTEXT_MINOR_VERSION_ARB             0x2092
#endif
#ifndef GLX_CONTEXT_PROFILE_MASK_ARB
#define GLX_CONTEXT_PROFILE_MASK_ARB              0x9126
#endif
#ifndef GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
#define GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif

typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display *dpy, GLXFBConfig config,
                                                     GLXContext share_context, Bool direct,
                                                     const int *attrib_list);

/* ---- Backend state ------------------------------------------------------- */
static Display   *g_dpy        = NULL;
static Window     g_win        = 0;
static GLXContext g_ctx        = NULL;
static Atom       g_wm_delete  = 0;
static int        g_should_close = 0;

/* Fullscreen (EWMH): toggled via a _NET_WM_STATE client message to the root. */
static Atom       g_net_wm_state = 0;
static Atom       g_net_wm_state_fullscreen = 0;
static int        g_fullscreen   = 0;

/* ---- Mouse-look (relative pointer) state --------------------------------- *
 * Recenter-and-hide scheme, pure Xlib (no XFixes / XInput2): while captured we
 * hide the pointer with an invisible 1x1 cursor and warp it back to the window
 * centre after every MotionNotify, accumulating the displacement-from-centre as
 * the relative delta. The warp generates its own MotionNotify landing exactly on
 * the centre; we tag it with g_warp_pending and discard it so the warp never
 * feeds back into the accumulated delta. */
static int    g_mouse_captured = 0;
static int    g_warp_pending   = 0;   /* a recenter warp is in flight; eat its event */
static int    g_mdx = 0, g_mdy = 0;   /* accumulated relative motion, pixels */
static int    g_mb_left = 0, g_mb_right = 0; /* read-and-clear click counters  */
static Cursor g_blank_cursor   = 0;   /* invisible 1x1 cursor for the hidden pointer */
static int    g_win_w = 0, g_win_h = 0; /* cached client size so the centre is known */

/* Engine-portable key state, indexed by PLAT_KEY_* (0..PLAT_KEY_COUNT-1). */
static unsigned char g_keys[PLAT_KEY_COUNT];

/* Typed-text ring (UI text entry, e.g. a server IP). Filled in KeyPress, drained
 * read-and-clear by plat_text_poll. Printable ASCII + backspace (0x08). */
static char g_text[64];
static int  g_text_len = 0;

/* Monotonic clock origin, captured at window creation, so plat_time_ms() is
 * "milliseconds since startup" rather than an absolute epoch. */
static int          g_clock_inited = 0;
static struct timespec g_clock_origin;

/* Map an X11 KeySym to a PLAT_KEY_* code, or -1 if we don't track it. */
static int keysym_to_plat(KeySym ks)
{
    switch (ks) {
    case XK_Escape:      return PLAT_KEY_ESC;
    case XK_w: case XK_W: return PLAT_KEY_W;
    case XK_a: case XK_A: return PLAT_KEY_A;
    case XK_s: case XK_S: return PLAT_KEY_S;
    case XK_d: case XK_D: return PLAT_KEY_D;
    case XK_space:       return PLAT_KEY_SPACE;
    case XK_Shift_L:     return PLAT_KEY_LSHIFT;
    case XK_1:           return PLAT_KEY_1;
    case XK_2:           return PLAT_KEY_2;
    case XK_3:           return PLAT_KEY_3;
    case XK_4:           return PLAT_KEY_4;
    case XK_5:           return PLAT_KEY_5;
    case XK_f: case XK_F: return PLAT_KEY_F;
    case XK_Up:          return PLAT_KEY_UP;
    case XK_Down:        return PLAT_KEY_DOWN;
    case XK_Return: case XK_KP_Enter: return PLAT_KEY_ENTER;
    default:             return -1;
    }
}

int plat_create_window(int w, int h, const char *title)
{
    static int visual_attribs[] = {
        GLX_X_RENDERABLE,  True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_DOUBLEBUFFER,  True,
        GLX_RED_SIZE,      8,
        GLX_GREEN_SIZE,    8,
        GLX_BLUE_SIZE,     8,
        GLX_ALPHA_SIZE,    8,
        GLX_DEPTH_SIZE,    24,
        None
    };

    int          screen;
    Window       root;
    GLXFBConfig *fbcs       = NULL;
    GLXFBConfig  fbc;
    int          fbc_count  = 0;
    XVisualInfo *vi         = NULL;
    Colormap     cmap;
    XSetWindowAttributes swa;
    int          glx_major  = 0, glx_minor = 0;
    PFN_glXCreateContextAttribsARB createCtxARB = NULL;
    const char  *exts;

    memset(g_keys, 0, sizeof(g_keys));
    g_should_close = 0;

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "platform_linux: XOpenDisplay failed (no DISPLAY?)\n");
        return 1;
    }
    screen = DefaultScreen(g_dpy);
    root   = RootWindow(g_dpy, screen);

    /* GLX 1.3+ is required for the FBConfig path. */
    if (!glXQueryVersion(g_dpy, &glx_major, &glx_minor) ||
        glx_major < 1 || (glx_major == 1 && glx_minor < 3)) {
        fprintf(stderr, "platform_linux: GLX 1.3+ required (have %d.%d)\n",
                glx_major, glx_minor);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return 1;
    }

    fbcs = glXChooseFBConfig(g_dpy, screen, visual_attribs, &fbc_count);
    if (!fbcs || fbc_count <= 0) {
        fprintf(stderr, "platform_linux: glXChooseFBConfig found no double-buffered RGBA/depth config\n");
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return 1;
    }
    fbc = fbcs[0];
    XFree(fbcs);

    vi = glXGetVisualFromFBConfig(g_dpy, fbc);
    if (!vi) {
        fprintf(stderr, "platform_linux: glXGetVisualFromFBConfig failed\n");
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return 1;
    }

    cmap = XCreateColormap(g_dpy, root, vi->visual, AllocNone);

    memset(&swa, 0, sizeof(swa));
    swa.colormap          = cmap;
    swa.background_pixmap = None;
    swa.border_pixel      = 0;
    swa.event_mask        = ExposureMask | StructureNotifyMask |
                            KeyPressMask | KeyReleaseMask |
                            PointerMotionMask | ButtonPressMask;

    g_win = XCreateWindow(g_dpy, root,
                          0, 0, (unsigned)w, (unsigned)h, 0,
                          vi->depth, InputOutput, vi->visual,
                          CWBorderPixel | CWColormap | CWEventMask, &swa);
    if (!g_win) {
        fprintf(stderr, "platform_linux: XCreateWindow failed\n");
        XFreeColormap(g_dpy, cmap);
        XFree(vi);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return 1;
    }

    XStoreName(g_dpy, g_win, title ? title : "Voxel Engine");

    /* Cache the client size so plat_poll() knows the centre to warp/recenter to
     * for mouse-look without an XGetGeometry round-trip per event. */
    g_win_w = (int)w;
    g_win_h = (int)h;

    /* Build the invisible cursor once, with stock Xlib (no XFixes): a 1x1
     * all-zero depth-1 pixmap used as both the cursor source and mask makes the
     * pointer fully transparent while mouse-look capture is on. */
    {
        static const char zero_bits[1] = { 0 };
        XColor black;
        Pixmap blank_pix;
        memset(&black, 0, sizeof(black));
        blank_pix = XCreateBitmapFromData(g_dpy, g_win, zero_bits, 1, 1);
        if (blank_pix != None) {
            g_blank_cursor = XCreatePixmapCursor(g_dpy, blank_pix, blank_pix,
                                                 &black, &black, 0, 0);
            XFreePixmap(g_dpy, blank_pix);
        }
    }

    /* Subscribe to the window-manager close button. */
    g_wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wm_delete, 1);

    /* EWMH fullscreen atoms (for plat_set_fullscreen). */
    g_net_wm_state            = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    g_net_wm_state_fullscreen = XInternAtom(g_dpy, "_NET_WM_STATE_FULLSCREEN", False);

    /* Try the ARB path for an explicit >=2.1 compatibility context; fall back
     * to the legacy glXCreateContext (already a compatibility context). */
    exts = glXQueryExtensionsString(g_dpy, screen);
    if (exts && strstr(exts, "GLX_ARB_create_context")) {
        createCtxARB = (PFN_glXCreateContextAttribsARB)
            glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    }

    if (createCtxARB) {
        /* Request 2.1 compatibility explicitly. Drivers are free to hand back a
         * newer compatibility context (a strict superset of 2.1), which is what
         * Mesa does - exactly what the engine wants. */
        int ctx_attribs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
            GLX_CONTEXT_MINOR_VERSION_ARB, 1,
            GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
            None
        };
        g_ctx = createCtxARB(g_dpy, fbc, NULL, True, ctx_attribs);
        /* Older servers may not like the profile attribute; sync and retry
         * without it before giving up on the ARB path. */
        XSync(g_dpy, False);
        if (!g_ctx) {
            int ctx_attribs_noprofile[] = {
                GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
                GLX_CONTEXT_MINOR_VERSION_ARB, 1,
                None
            };
            g_ctx = createCtxARB(g_dpy, fbc, NULL, True, ctx_attribs_noprofile);
            XSync(g_dpy, False);
        }
    }

    if (!g_ctx) {
        /* Legacy fallback: a direct rendering context off the chosen FBConfig.
         * This yields a compatibility context on Mesa and classic drivers. */
        g_ctx = glXCreateNewContext(g_dpy, fbc, GLX_RGBA_TYPE, NULL, True);
    }

    XFree(vi);

    if (!g_ctx) {
        fprintf(stderr, "platform_linux: failed to create a GLX context\n");
        XDestroyWindow(g_dpy, g_win);
        g_win = 0;
        XFreeColormap(g_dpy, cmap);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return 1;
    }

    XMapWindow(g_dpy, g_win);

    if (!glXMakeCurrent(g_dpy, g_win, g_ctx)) {
        fprintf(stderr, "platform_linux: glXMakeCurrent failed\n");
        glXDestroyContext(g_dpy, g_ctx);
        g_ctx = NULL;
        XDestroyWindow(g_dpy, g_win);
        g_win = 0;
        XFreeColormap(g_dpy, cmap);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return 1;
    }

    /* Set the initial viewport explicitly (the context is current now). The
     * server will also send a ConfigureNotify with the real mapped size, which
     * re-applies glViewport if the WM gave us a different size. */
    glViewport(0, 0, g_win_w, g_win_h);

    /* Capture the monotonic clock origin now so plat_time_ms() starts near 0. */
    clock_gettime(CLOCK_MONOTONIC, &g_clock_origin);
    g_clock_inited = 1;

    return 0;
}

void *plat_gl_getproc(const char *name)
{
    /* glXGetProcAddressARB resolves both core-since-1.1 and extension entry
     * points regardless of whether the context is current, which is exactly
     * what gl_load() expects. */
    return (void *)glXGetProcAddressARB((const GLubyte *)name);
}

void plat_poll(void)
{
    XEvent ev;

    if (!g_dpy)
        return;

    while (XPending(g_dpy) > 0) {
        XNextEvent(g_dpy, &ev);
        switch (ev.type) {
        case KeyPress: {
            /* Use the unshifted keysym (group/level 0) so 'w' and shifted 'W'
             * both map to the same PLAT_KEY_*. */
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int    k  = keysym_to_plat(ks);
            char   tb[8];
            int    tn, i;
            if (k >= 0)
                g_keys[k] = 1;
            /* Also capture the TYPED character(s) for UI text fields (server IP):
             * XLookupString honours shift/layout. Keep printable ASCII + backspace
             * (0x08); Enter (0x0D) is dropped here (handled as PLAT_KEY_ENTER). */
            tn = XLookupString(&ev.xkey, tb, (int)sizeof tb, NULL, NULL);
            for (i = 0; i < tn; ++i) {
                unsigned char ch = (unsigned char)tb[i];
                if (((ch >= 0x20 && ch <= 0x7E) || ch == 0x08) && g_text_len < (int)sizeof g_text)
                    g_text[g_text_len++] = (char)ch;
            }
            break;
        }
        case KeyRelease: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int    k  = keysym_to_plat(ks);
            if (k >= 0)
                g_keys[k] = 0;
            break;
        }
        case MotionNotify: {
            /* Relative mouse-look: accumulate displacement from the window
             * centre, then warp the pointer back to centre so motion is
             * unbounded and the pointer never drifts to the window edge. The
             * warp itself emits a MotionNotify landing exactly on the centre;
             * g_warp_pending tags it so we eat that synthetic event instead of
             * feeding it back into the delta (the allocation-free alternative to
             * XFixes/XInput2 raw motion). */
            int cx, cy;
            if (!g_mouse_captured)
                break;
            cx = g_win_w / 2;
            cy = g_win_h / 2;
            if (g_warp_pending &&
                ev.xmotion.x == cx && ev.xmotion.y == cy) {
                /* This is the recenter warp's own event - discard it. */
                g_warp_pending = 0;
                break;
            }
            g_mdx += ev.xmotion.x - cx;
            g_mdy += ev.xmotion.y - cy;
            XWarpPointer(g_dpy, None, g_win, 0, 0, 0, 0, cx, cy);
            g_warp_pending = 1;
            break;
        }
        case ButtonPress:
            /* Edge-triggered mouse clicks for block break/place. Only while
             * captured (a live session); Button1 = left, Button3 = right. */
            if (g_mouse_captured) {
                if (ev.xbutton.button == Button1) g_mb_left++;
                else if (ev.xbutton.button == Button3) g_mb_right++;
            }
            break;
        case ConfigureNotify:
            /* The window was resized (or moved). Cache the new size so the
             * mouse-look recenter target and plat_get_size track it, and resize
             * the GL viewport so the scene fills the window rather than staying
             * clipped to the creation size. StructureNotifyMask (selected at
             * creation) delivers this. The context is current by now (made
             * current at the end of plat_create_window), so glViewport is safe. */
            if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
                int nw = ev.xconfigure.width;
                int nh = ev.xconfigure.height;
                if (nw != g_win_w || nh != g_win_h) {
                    g_win_w = nw;
                    g_win_h = nh;
                    glViewport(0, 0, nw, nh);
                }
            }
            break;
        case ClientMessage:
            /* Window-manager close button (WM_DELETE_WINDOW). */
            if ((Atom)ev.xclient.data.l[0] == g_wm_delete) {
                g_should_close = 1;
                plat_set_mouse_capture(0);  /* show the cursor again on exit */
            }
            break;
        case DestroyNotify:
            g_should_close = 1;
            plat_set_mouse_capture(0);
            break;
        default:
            break;
        }
    }
}

int plat_should_close(void)
{
    return g_should_close;
}

void plat_swap_buffers(void)
{
    if (g_dpy && g_win)
        glXSwapBuffers(g_dpy, g_win);
}

double plat_time_ms(void)
{
    struct timespec now;

    if (!g_clock_inited)
        return 0.0;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec  - g_clock_origin.tv_sec)  * 1000.0 +
           (double)(now.tv_nsec - g_clock_origin.tv_nsec) / 1000000.0;
}

int plat_key_down(int keycode)
{
    if (keycode < 0 || keycode >= PLAT_KEY_COUNT)
        return 0;
    return g_keys[keycode];
}

void plat_get_size(int *w, int *h)
{
    /* The live drawable size, kept current by ConfigureNotify. Before the window
     * exists this is the requested size (set in plat_create_window) or 0. */
    if (w) *w = g_win_w;
    if (h) *h = g_win_h;
}

void plat_mouse_buttons(int *left_clicks, int *right_clicks)
{
    /* Read-and-clear the per-button press counts accumulated in plat_poll. */
    if (left_clicks)  *left_clicks  = g_mb_left;
    if (right_clicks) *right_clicks = g_mb_right;
    g_mb_left = 0;
    g_mb_right = 0;
}

int plat_text_poll(char *out, int max)
{
    int n = (g_text_len < max) ? g_text_len : max;
    int i;
    for (i = 0; i < n; ++i) out[i] = g_text[i];
    g_text_len = 0;                  /* read-and-clear (caller buffer should be >= 64) */
    return n;
}

void plat_mouse_delta(int *dx, int *dy)
{
    /* Read-and-clear the accumulated relative motion. When capture is off
     * nothing is ever accumulated in plat_poll, so this naturally returns 0. */
    if (dx) *dx = g_mdx;
    if (dy) *dy = g_mdy;
    g_mdx = 0;
    g_mdy = 0;
}

void plat_set_mouse_capture(int on)
{
    /* No-op before the window exists; idempotent on->on / off->off. */
    if (!g_dpy || !g_win)
        return;

    if (on) {
        if (g_mouse_captured)
            return;
        g_mouse_captured = 1;
        /* Hide the pointer and confine it to the window so motion is unbounded
         * and clicks never escape; warp once to the centre to start clean. */
        if (g_blank_cursor)
            XDefineCursor(g_dpy, g_win, g_blank_cursor);
        XGrabPointer(g_dpy, g_win, True,
                     PointerMotionMask | ButtonPressMask,
                     GrabModeAsync, GrabModeAsync,
                     g_win, g_blank_cursor, CurrentTime);
        XWarpPointer(g_dpy, None, g_win, 0, 0, 0, 0,
                     g_win_w / 2, g_win_h / 2);
        g_warp_pending = 1;
        g_mdx = 0;
        g_mdy = 0;
        XFlush(g_dpy);
    } else {
        if (!g_mouse_captured)
            return;
        g_mouse_captured = 0;
        g_warp_pending = 0;
        g_mdx = 0;
        g_mdy = 0;
        XUndefineCursor(g_dpy, g_win);
        XUngrabPointer(g_dpy, CurrentTime);
        XFlush(g_dpy);
    }
}

void plat_set_fullscreen(int on)
{
    XEvent ev;
    on = on ? 1 : 0;
    if (!g_dpy || !g_win || !g_net_wm_state || !g_net_wm_state_fullscreen) return;
    if (on == g_fullscreen) return;                 /* idempotent */
    g_fullscreen = on;

    /* Ask the window manager to add/remove _NET_WM_STATE_FULLSCREEN (EWMH). The
     * WM resizes the drawable; our ConfigureNotify handler re-points glViewport. */
    memset(&ev, 0, sizeof ev);
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = g_win;
    ev.xclient.message_type = g_net_wm_state;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = on ? 1 : 0;           /* 1 = _NET_WM_STATE_ADD, 0 = REMOVE */
    ev.xclient.data.l[1]    = (long)g_net_wm_state_fullscreen;
    ev.xclient.data.l[2]    = 0;
    ev.xclient.data.l[3]    = 1;                     /* source: normal application */
    XSendEvent(g_dpy, RootWindow(g_dpy, DefaultScreen(g_dpy)), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(g_dpy);
}

#endif /* _WIN32 */
