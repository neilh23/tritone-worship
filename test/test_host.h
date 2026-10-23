/* Per-platform parts of the plugin test hosts: loading the plugin library, and a hidden parent
   window that receives the plugin's GUI and can be sent fake mouse events */
#ifndef TEST_HOST_H
#define TEST_HOST_H

enum { MOUSE_DOWN, MOUSE_MOVE, MOUSE_UP };

#if defined(_WIN32)
#include <stdio.h>
#include <windows.h>

/* LoadLibrary wants backslashes */
static void *lib_open(const char *path)
{
    char win_path[MAX_PATH];
    snprintf(win_path, sizeof win_path, "%s", path);
    for (char *c = win_path; *c; c++)
        if (*c == '/')
            *c = '\\';
    return LoadLibraryA(win_path);
}

static void *lib_symbol(void *lib, const char *name) { return (void *)GetProcAddress(lib, name); }
static void sleep_ms(int ms) { Sleep(ms); }

static HWND host_parent, host_child;

static int host_window_open(void)
{
    host_parent = CreateWindowExA(0, "STATIC", "", WS_OVERLAPPEDWINDOW, 0, 0, 300, 500, NULL, NULL, NULL, NULL);
    return host_parent != NULL;
}

static void *host_window_parent(void) { return host_parent; }

/* Finds the window the plugin added to the parent; returns 0 if there isn't exactly one */
static int host_window_find_child(void)
{
    host_child = GetWindow(host_parent, GW_CHILD);
    return host_child && !GetWindow(host_child, GW_HWNDNEXT);
}

static void host_mouse(int type, int x, int y)
{
    UINT msg = type == MOUSE_DOWN ? WM_LBUTTONDOWN : type == MOUSE_UP ? WM_LBUTTONUP : WM_MOUSEMOVE;
    SendMessageA(host_child, msg, type == MOUSE_UP ? 0 : MK_LBUTTON, MAKELPARAM(x, y));
}

static void host_window_close(void) { DestroyWindow(host_parent); }

#elif defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>

static void *lib_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *lib_symbol(void *lib, const char *name) { return dlsym(lib, name); }
static void sleep_ms(int ms) { usleep(ms * 1000); }

/* No Cocoa test harness: the GUI tests are skipped on macOS */
static int host_window_open(void) { return 0; }
static void *host_window_parent(void) { return NULL; }
static int host_window_find_child(void) { return 0; }
static void host_mouse(int type, int x, int y) {}
static void host_window_close(void) {}

#else
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

static void *lib_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *lib_symbol(void *lib, const char *name) { return dlsym(lib, name); }
static void sleep_ms(int ms) { usleep(ms * 1000); }

static Display *host_dpy;
static Window host_parent, host_child;

/* Returns 0 if there is no X display. The parent is never mapped, so nothing appears on screen. */
static int host_window_open(void)
{
    host_dpy = XOpenDisplay(NULL);
    if (!host_dpy)
        return 0;
    host_parent = XCreateSimpleWindow(host_dpy, DefaultRootWindow(host_dpy), 0, 0, 300, 500, 0, 0, 0);
    XSync(host_dpy, False);
    return 1;
}

static void *host_window_parent(void) { return (void *)(uintptr_t)host_parent; }

static int host_window_find_child(void)
{
    Window root, parent, *children;
    unsigned int n = 0;
    XQueryTree(host_dpy, host_parent, &root, &parent, &children, &n);
    if (n == 1)
        host_child = children[0];
    if (children)
        XFree(children);
    return n == 1;
}

static void host_mouse(int type, int x, int y)
{
    XEvent e = {0};
    e.type = type == MOUSE_DOWN ? ButtonPress : type == MOUSE_UP ? ButtonRelease : MotionNotify;
    e.xbutton.display = host_dpy;
    e.xbutton.window = host_child;
    e.xbutton.x = x;
    e.xbutton.y = y;
    e.xbutton.button = Button1;
    e.xbutton.state = type == MOUSE_DOWN ? 0 : Button1Mask;
    long mask = type == MOUSE_DOWN ? ButtonPressMask : type == MOUSE_UP ? ButtonReleaseMask : ButtonMotionMask;
    XSendEvent(host_dpy, host_child, False, mask, &e);
    XSync(host_dpy, False);
}

static void host_window_close(void)
{
    XDestroyWindow(host_dpy, host_parent);
    XCloseDisplay(host_dpy);
}
#endif

#endif
