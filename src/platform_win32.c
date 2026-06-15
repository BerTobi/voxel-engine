/* platform_win32.c - Win32 + wgl backend of platform.h (the real ship target).
 *
 * This is the backend that runs on the 2005 Dell XPS M170 under Windows XP SP3
 * (Pentium M, GeForce Go 7800 GTX / G70). The X11+GLX backend (platform_linux.c)
 * is the dev/test box; this one is what actually ships. The ONE portable
 * GL2.1/GLSL1.20 engine core never includes a windowing header - it only calls
 * the plat_* functions declared in platform.h.
 *
 * ZERO third-party libraries: we talk to Win32 + wgl directly (no SDL/GLFW) and
 * load GL entry points by hand through plat_gl_getproc (handed to gl_load()).
 * Everything here is XP-safe: classic RegisterClass/CreateWindow, a basic
 * ChoosePixelFormat/SetPixelFormat (PFD_DOUBLEBUFFER, 24-bit depth) pixel
 * format, the legacy wglCreateContext path (no WGL_ARB_create_context needed -
 * the XP G70 driver hands back a compatibility context that already provides
 * >= 2.1, a strict subset of which is all the engine uses), and
 * QueryPerformanceCounter for timing. No post-XP Win32 APIs are called.
 *
 * Guarded with #ifdef _WIN32 so a single Makefile can compile both backends and
 * let this one win on Windows.
 */
#ifdef _WIN32

#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* We pull GL 1.1 declarations (wglCreateContext, wglMakeCurrent, ...) from the
 * system header; opengl32.dll exports these statically so we link them, not
 * load them. The engine's own post-1.1 entry points come via plat_gl_getproc. */
#include <GL/gl.h>

#include <stddef.h>
#include <string.h>   /* memset - not reliably declared by <windows.h> */

/* ---- Backend state ------------------------------------------------------- */
static HINSTANCE g_hinst        = NULL;     /* module instance for the class    */
static HWND      g_hwnd         = NULL;     /* the engine window                */
static HDC       g_hdc          = NULL;     /* its device context (owns pixfmt) */
static HGLRC     g_hglrc        = NULL;     /* the GL render context            */
static HMODULE   g_opengl32     = NULL;     /* opengl32.dll, for GL1.1 getproc  */
static int       g_should_close = 0;

/* The window class is registered exactly once per process. */
static const char  *g_class_name   = "VoxelEngineWindow";
static int           g_class_inited = 0;

/* Engine-portable key state, indexed by PLAT_KEY_* (0..PLAT_KEY_COUNT-1). */
static unsigned char g_keys[PLAT_KEY_COUNT];

/* ---- Relative mouse-look state ------------------------------------------- *
 * Mirrors the X11 backend's recenter-and-hide scheme so both behave identically.
 * While captured we hide the cursor, pin it to the client centre every poll, and
 * report the per-poll displacement-from-centre as the relative delta. The warp we
 * issue to recenter must not feed back into that delta, so we ignore the sample
 * that lands exactly on the centre right after a warp (g_warp_pending). All of
 * this is plain XP-safe Win32: GetCursorPos/SetCursorPos/ShowCursor/ClipCursor -
 * no raw input (WM_INPUT) and no post-XP APIs. */
static int g_mouse_captured = 0;     /* capture ENGAGED right now (grab active)  */
static int g_capture_desired = 0;    /* engine INTENT (plat_set_mouse_capture);  *
                                      * re-armed on refocus, dropped on blur      */
static int g_warp_pending   = 0;     /* a recenter SetCursorPos is in flight     */
static int g_mdx = 0, g_mdy = 0;     /* accumulated relative motion (read+clear) */
static int g_cursor_hidden  = 0;     /* tracks our balanced ShowCursor(FALSE)    */
static int g_client_w = 0;           /* requested client size, for the centre    */
static int g_client_h = 0;

/* QueryPerformanceCounter timing. We capture frequency and an origin tick at
 * window creation so plat_time_ms() is "milliseconds since startup" rather than
 * an absolute count, and so the double math stays well-conditioned. */
static int           g_clock_inited = 0;
static LARGE_INTEGER g_clock_freq;
static LARGE_INTEGER g_clock_origin;

/* Map a Win32 virtual-key code to a PLAT_KEY_* code, or -1 if we don't track it.
 * Letter VKs on Windows are the ASCII uppercase codes ('W' == 0x57, etc.), so a
 * single case per key covers both shifted and unshifted - mirroring how the X11
 * backend folds 'w'/'W' onto one PLAT_KEY_*. */
static int vk_to_plat(WPARAM vk)
{
    switch (vk) {
    case VK_ESCAPE:  return PLAT_KEY_ESC;
    case 'W':        return PLAT_KEY_W;
    case 'A':        return PLAT_KEY_A;
    case 'S':        return PLAT_KEY_S;
    case 'D':        return PLAT_KEY_D;
    case VK_SPACE:   return PLAT_KEY_SPACE;
    case VK_LSHIFT:  return PLAT_KEY_LSHIFT;
    /* The generic VK_SHIFT also arrives for the left shift on some setups; fold
     * it onto the same engine key so movement input is reliable. */
    case VK_SHIFT:   return PLAT_KEY_LSHIFT;
    default:         return -1;
    }
}

/* Restore the cursor to its normal, visible, unconfined state. Called when
 * capture is turned off and on the window-close paths so the pointer reappears
 * on exit. Balances any ShowCursor(FALSE) we issued (the display count must be
 * driven back to >= 0) and drops the ClipCursor confinement. Idempotent. */
static void plat_mouse_release(void)
{
    if (g_cursor_hidden) {
        ShowCursor(TRUE);
        g_cursor_hidden = 0;
    }
    ClipCursor(NULL);
    g_mouse_captured = 0;
    g_warp_pending   = 0;
    g_mdx = g_mdy = 0;
}

/* Actually ENGAGE the grab: hide the cursor, recenter it to the client centre
 * (priming g_warp_pending to swallow that warp), and confine it to the client
 * rect. Factored out of plat_set_mouse_capture so the focus handler can re-arm
 * capture on refocus without duplicating the logic. Idempotent (no-op if already
 * engaged or there is no window). Does NOT touch g_capture_desired - that is the
 * engine's intent and is owned by plat_set_mouse_capture. */
static void mouse_engage(void)
{
    POINT pt;

    if (g_mouse_captured || !g_hwnd)
        return;
    g_mouse_captured = 1;

    if (!g_cursor_hidden) {
        ShowCursor(FALSE);
        g_cursor_hidden = 1;
    }

    pt.x = g_client_w / 2;
    pt.y = g_client_h / 2;
    if (ClientToScreen(g_hwnd, &pt)) {
        SetCursorPos(pt.x, pt.y);
        g_warp_pending = 1;
    }
    g_mdx = g_mdy = 0;

    {
        RECT  cr;
        POINT tl, br;
        if (GetClientRect(g_hwnd, &cr)) {
            tl.x = cr.left;  tl.y = cr.top;
            br.x = cr.right; br.y = cr.bottom;
            if (ClientToScreen(g_hwnd, &tl) && ClientToScreen(g_hwnd, &br)) {
                RECT clip;
                clip.left   = tl.x;
                clip.top    = tl.y;
                clip.right  = br.x;
                clip.bottom = br.y;
                ClipCursor(&clip);
            }
        }
    }
}

/* Window procedure: close requests, key state, focus changes, and resize.
 * Anything we don't handle goes to DefWindowProc so XP draws the frame etc. */
static LRESULT CALLBACK plat_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CLOSE:
        /* User hit the X / Alt-F4: flag close. We do NOT DestroyWindow here;
         * the engine loop sees plat_should_close() and shuts down in order.
         * Release the pointer now so the cursor reappears even if the engine
         * tears down without an explicit plat_set_mouse_capture(0). */
        plat_mouse_release();
        g_should_close = 1;
        return 0;

    case WM_DESTROY:
        plat_mouse_release();
        g_should_close = 1;
        return 0;

    case WM_ACTIVATE:
        /* Focus change (Alt-Tab, click-away, click-back). CRITICAL on the XP
         * target: while captured, plat_poll recenters the SYSTEM cursor to the
         * client centre 30-60x/sec via SetCursorPos. If we kept capturing after
         * losing the foreground we would yank the desktop's cursor back into a
         * backgrounded window every frame - unusable until the engine is killed.
         * So DROP the grab on deactivate (without forgetting the engine's intent)
         * and RE-ARM it on activate if the engine still wants it. */
        if (LOWORD(wparam) == WA_INACTIVE) {
            if (g_mouse_captured)
                plat_mouse_release();    /* stop the recenter; keep g_capture_desired */
        } else {
            if (g_capture_desired)
                mouse_engage();          /* refocused: re-grab if still wanted */
        }
        break;                           /* let DefWindowProc do default activation */

    case WM_SIZE:
        /* The client area was resized (drag-border / maximize / restore). Cache
         * the new client size (LOWORD/HIWORD of lparam) so the mouse-look centre
         * and plat_get_size track it, and resize the GL viewport so the scene
         * fills the window instead of staying clipped to the creation size.
         * Skip a zero size (minimize) and skip glViewport until the GL context
         * exists (WM_SIZE also fires during CreateWindow, before wglMakeCurrent). */
        {
            int nw = (int)LOWORD(lparam);
            int nh = (int)HIWORD(lparam);
            if (nw > 0 && nh > 0) {
                g_client_w = nw;
                g_client_h = nh;
                if (g_hglrc)
                    glViewport(0, 0, nw, nh);
                /* If captured, re-confine to the new client rect. */
                if (g_mouse_captured) {
                    RECT  cr;
                    POINT tl, br;
                    if (GetClientRect(hwnd, &cr)) {
                        tl.x = cr.left;  tl.y = cr.top;
                        br.x = cr.right; br.y = cr.bottom;
                        if (ClientToScreen(hwnd, &tl) && ClientToScreen(hwnd, &br)) {
                            RECT clip;
                            clip.left = tl.x; clip.top = tl.y;
                            clip.right = br.x; clip.bottom = br.y;
                            ClipCursor(&clip);
                        }
                    }
                }
            }
        }
        break;                           /* DefWindowProc handles the rest */

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        /* Ignore auto-repeat for state tracking (bit 30 of lparam is the
         * previous key state); it does not change "is the key down". */
        int k = vk_to_plat(wparam);
        if (k >= 0)
            g_keys[k] = 1;
        /* Let DefWindowProc still process system keys (e.g. Alt-F4) below for
         * the SYS variants by falling through to the default for those; for the
         * plain key path we've consumed it. */
        if (msg == WM_KEYDOWN)
            return 0;
        break;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int k = vk_to_plat(wparam);
        if (k >= 0)
            g_keys[k] = 0;
        if (msg == WM_KEYUP)
            return 0;
        break;
    }

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

int plat_create_window(int w, int h, const char *title)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    int   pixfmt;
    DWORD style;
    RECT  rect;
    int   win_w, win_h;

    memset(g_keys, 0, sizeof(g_keys));
    g_should_close = 0;

    /* Cache the requested client size so the mouse-look recenter knows the client
     * centre (g_client_w/2, g_client_h/2) without a GetClientRect per poll. */
    g_client_w = w;
    g_client_h = h;

    g_hinst = GetModuleHandleA(NULL);

    /* Hold a handle to opengl32.dll for the GL1.1 getproc fallback (see
     * plat_gl_getproc). wglGetProcAddress returns NULL for entry points that
     * live directly in opengl32.dll, so we resolve those with GetProcAddress. */
    g_opengl32 = LoadLibraryA("opengl32.dll");
    if (!g_opengl32) {
        /* It is almost certainly already mapped (we link it), but if the named
         * load fails we can still try GetModuleHandle so the fallback works. */
        g_opengl32 = GetModuleHandleA("opengl32.dll");
    }

    /* Register the window class once. CS_OWNDC gives the window its own private
     * device context, which is required to keep one pixel format / GL context
     * stable for the window's lifetime. */
    if (!g_class_inited) {
        memset(&wc, 0, sizeof(wc));
        wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = plat_wndproc;
        wc.hInstance     = g_hinst;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;   /* GL clears the window; no GDI background. */
        wc.lpszClassName = g_class_name;
        if (!RegisterClassA(&wc)) {
            return 1;
        }
        g_class_inited = 1;
    }

    /* Compute the outer window size so the client area is exactly w x h. */
    style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    rect.left   = 0;
    rect.top    = 0;
    rect.right  = w;
    rect.bottom = h;
    if (AdjustWindowRect(&rect, style, FALSE)) {
        win_w = rect.right - rect.left;
        win_h = rect.bottom - rect.top;
    } else {
        win_w = w;
        win_h = h;
    }

    g_hwnd = CreateWindowExA(
        0,
        g_class_name,
        title ? title : "Voxel Engine",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        win_w, win_h,
        NULL, NULL, g_hinst, NULL);
    if (!g_hwnd) {
        return 1;
    }

    g_hdc = GetDC(g_hwnd);
    if (!g_hdc) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
        return 1;
    }

    /* Classic XP-safe pixel format: double-buffered RGBA with a 24-bit depth
     * buffer drawn to a window via OpenGL. This is the lowest-common-denominator
     * path the G70 driver supports without any WGL extensions. */
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 24;   /* 8/8/8 RGB; alpha requested separately below     */
    pfd.cAlphaBits   = 8;
    pfd.cDepthBits   = 24;   /* matches the GLX_DEPTH_SIZE 24 on the dev backend */
    pfd.cStencilBits = 0;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    pixfmt = ChoosePixelFormat(g_hdc, &pfd);
    if (pixfmt == 0) {
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
        return 1;
    }

    if (!SetPixelFormat(g_hdc, pixfmt, &pfd)) {
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
        return 1;
    }

    /* Legacy context creation. On the XP G70 driver this yields a compatibility
     * context whose reported version (typically 2.1) is a superset of the GL2.1
     * the engine targets - exactly what we want, and identical in spirit to the
     * legacy glXCreateNewContext fallback on the Linux backend. */
    g_hglrc = wglCreateContext(g_hdc);
    if (!g_hglrc) {
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
        return 1;
    }

    if (!wglMakeCurrent(g_hdc, g_hglrc)) {
        wglDeleteContext(g_hglrc);
        g_hglrc = NULL;
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);

    /* Set the initial viewport explicitly: WM_SIZE fired during CreateWindow
     * before the context was current (so it skipped glViewport), and we want the
     * first frame to fill the client area regardless of the GL default. */
    glViewport(0, 0, g_client_w, g_client_h);

    /* Capture the QueryPerformanceCounter origin now so plat_time_ms() starts
     * near 0. If the platform has no high-res counter (vanishingly rare, but
     * possible on very old hardware), leave the clock uninited -> time stays 0. */
    if (QueryPerformanceFrequency(&g_clock_freq) && g_clock_freq.QuadPart > 0) {
        QueryPerformanceCounter(&g_clock_origin);
        g_clock_inited = 1;
    }

    return 0;
}

void *plat_gl_getproc(const char *name)
{
    void *p;

    /* wglGetProcAddress resolves the post-1.1 entry points (VBOs, the shader
     * pipeline, multitexture) that the GL loader actually asks for. It requires
     * a current context, which plat_create_window has already made current. */
    p = (void *)wglGetProcAddress(name);

    /* wglGetProcAddress returns NULL (and historically a few small sentinel
     * handles) for GL 1.1 entry points, because those are exported directly by
     * opengl32.dll rather than by the ICD. Fall back to GetProcAddress on the
     * opengl32 module for those. gl_loader.c independently rejects the sentinel
     * values, but we also screen them here so a sentinel triggers the fallback
     * instead of being returned as a "resolved" pointer. */
    if (p == NULL ||
        p == (void *)1 || p == (void *)2 ||
        p == (void *)3 || p == (void *)-1) {
        if (g_opengl32) {
            p = (void *)GetProcAddress(g_opengl32, name);
        } else {
            p = NULL;
        }
    }

    return p;
}

void plat_poll(void)
{
    MSG msg;

    /* Drain every queued message this frame. PeekMessage with PM_REMOVE is the
     * non-blocking pump; WM_QUIT (e.g. from a PostQuitMessage) also stops us and
     * is treated as a close request. */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_should_close = 1;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Relative mouse-look sampling. Done here (not in a WM_MOUSEMOVE handler) so
     * the delta is read exactly once per frame and we never have to register for
     * mouse messages. This is the exact counterpart of the X11 backend's warp
     * scheme: sample the cursor, take the displacement from the client centre,
     * then warp the cursor back to centre - except we ignore the one sample that
     * lands precisely on the centre right after our own warp, so the synthetic
     * recenter motion never leaks into the accumulated delta. */
    if (g_mouse_captured && g_hwnd && GetForegroundWindow() == g_hwnd) {
        POINT p;
        int   cx = g_client_w / 2;
        int   cy = g_client_h / 2;

        if (GetCursorPos(&p) && ScreenToClient(g_hwnd, &p)) {
            if (g_warp_pending && p.x == cx && p.y == cy) {
                /* This sample IS our recenter warp landing on the centre - it
                 * carries no real motion, so discard it. */
                g_warp_pending = 0;
            } else {
                POINT c;
                g_mdx += p.x - cx;
                g_mdy += p.y - cy;
                /* Warp the cursor back to the client centre so motion stays
                 * unbounded and the pointer never escapes the window. */
                c.x = cx;
                c.y = cy;
                if (ClientToScreen(g_hwnd, &c)) {
                    SetCursorPos(c.x, c.y);
                    g_warp_pending = 1;
                }
            }
        }
    }
}

int plat_should_close(void)
{
    return g_should_close;
}

void plat_swap_buffers(void)
{
    if (g_hdc)
        SwapBuffers(g_hdc);
}

double plat_time_ms(void)
{
    LARGE_INTEGER now;

    if (!g_clock_inited)
        return 0.0;

    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_clock_origin.QuadPart) * 1000.0 /
           (double)g_clock_freq.QuadPart;
}

int plat_key_down(int keycode)
{
    if (keycode < 0 || keycode >= PLAT_KEY_COUNT)
        return 0;
    return g_keys[keycode];
}

void plat_mouse_delta(int *dx, int *dy)
{
    /* Read-and-clear the accumulated relative motion. When capture is off nothing
     * is accumulated in plat_poll, so this naturally returns (0,0). */
    if (dx) *dx = g_mdx;
    if (dy) *dy = g_mdy;
    g_mdx = g_mdy = 0;
}

void plat_set_mouse_capture(int on)
{
    /* No-op before the window exists (implicitly OFF at startup). */
    if (!g_hwnd)
        return;

    if (on) {
        g_capture_desired = 1;
        /* Engage now only if we actually hold the foreground; if the engine asks
         * for capture while backgrounded, the WM_ACTIVATE handler engages it on
         * refocus. mouse_engage is idempotent (on -> on). */
        if (GetForegroundWindow() == g_hwnd)
            mouse_engage();
    } else {
        g_capture_desired = 0;
        plat_mouse_release();       /* idempotent: off -> off */
    }
}

void plat_get_size(int *w, int *h)
{
    /* The live CLIENT size, kept current by WM_SIZE. Before the window exists
     * this is the requested size (set in plat_create_window) or 0. */
    if (w) *w = g_client_w;
    if (h) *h = g_client_h;
}

#endif /* _WIN32 */
