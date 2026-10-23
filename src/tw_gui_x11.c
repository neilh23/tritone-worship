#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tw_gui.h"
#include "tw_ui.h"

struct TwGui {
    Display *dpy;
    Window win;
    XImage *img;
    uint32_t *px;
    TwGuiCallbacks cb;
    void *ctx;
    float drawn[TW_P_COUNT];
    int drawn_active;
    int dirty;
    int active; /* knob being dragged, -1 for none */
    int drag_y;
    float drag_start;
};

TwGui *tw_gui_create(void *parent, const TwGuiCallbacks *cb, void *ctx)
{
    TwGui *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->dpy = XOpenDisplay(NULL);
    if (!g->dpy) {
        free(g);
        return NULL;
    }
    g->cb = *cb;
    g->ctx = ctx;
    g->active = -1;
    g->dirty = 1;
    g->win = XCreateSimpleWindow(g->dpy, (Window)(uintptr_t)parent, 0, 0, TW_UI_W, TW_UI_H, 0, 0, 0);
    XSelectInput(g->dpy, g->win, ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask);
    int screen = DefaultScreen(g->dpy);
    g->px = malloc(TW_UI_W * TW_UI_H * sizeof *g->px);
    g->img = XCreateImage(g->dpy, DefaultVisual(g->dpy, screen), DefaultDepth(g->dpy, screen), ZPixmap,
                          0, (char *)g->px, TW_UI_W, TW_UI_H, 32, 0);
    XSync(g->dpy, False);
    return g;
}

void tw_gui_destroy(TwGui *g)
{
    XDestroyImage(g->img); /* also frees g->px */
    XDestroyWindow(g->dpy, g->win);
    XCloseDisplay(g->dpy);
    free(g);
}

void tw_gui_show(TwGui *g, int visible)
{
    if (visible)
        XMapRaised(g->dpy, g->win);
    else
        XUnmapWindow(g->dpy, g->win);
    XSync(g->dpy, False);
}

static void press(TwGui *g, int mx, int my)
{
    int p = tw_ui_hit(mx, my);
    if (p < 0)
        return;
    g->cb.begin(g->ctx, p);
    if (p == TW_P_BLACKEN || p == TW_P_BYPASS) {
        g->cb.set(g->ctx, p, g->cb.get(g->ctx, p) > 0.5f ? 0.0f : 1.0f);
        g->cb.end(g->ctx, p);
        return;
    }
    g->active = p;
    g->drag_y = my;
    g->drag_start = g->cb.get(g->ctx, p);
}

void tw_gui_idle(TwGui *g)
{
    while (XPending(g->dpy)) {
        XEvent ev;
        XNextEvent(g->dpy, &ev);
        switch (ev.type) {
        case Expose:
            g->dirty = 1;
            break;
        case ButtonPress:
            if (ev.xbutton.button == Button1)
                press(g, ev.xbutton.x, ev.xbutton.y);
            break;
        case MotionNotify:
            if (g->active >= 0)
                g->cb.set(g->ctx, g->active,
                          tw_ui_drag_value(g->active, g->drag_start, g->drag_y - ev.xmotion.y));
            break;
        case ButtonRelease:
            if (ev.xbutton.button == Button1 && g->active >= 0) {
                g->cb.end(g->ctx, g->active);
                g->active = -1;
            }
            break;
        }
    }

    float v[TW_P_COUNT];
    for (int p = 0; p < TW_P_COUNT; p++)
        v[p] = g->cb.get(g->ctx, p);
    if (!g->dirty && g->active == g->drawn_active && !memcmp(v, g->drawn, sizeof v))
        return;
    memcpy(g->drawn, v, sizeof v);
    g->drawn_active = g->active;
    g->dirty = 0;
    tw_ui_draw(g->px, v, g->active);
    XPutImage(g->dpy, g->win, DefaultGC(g->dpy, DefaultScreen(g->dpy)), g->img, 0, 0, 0, 0,
              TW_UI_W, TW_UI_H);
    XFlush(g->dpy);
}
