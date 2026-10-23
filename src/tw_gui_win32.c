#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <string.h>

#include "tw_gui.h"
#include "tw_ui.h"

#define CLASS_NAME "TritoneWorshipPedal"
#define TIMER_ID 1
#define TIMER_MS 30

struct TwGui {
    HWND hwnd;
    uint32_t px[TW_UI_W * TW_UI_H];
    TwGuiCallbacks cb;
    void *ctx;
    float drawn[TW_P_COUNT];
    int drawn_active;
    int active; /* knob being dragged, -1 for none */
    int drag_y;
    float drag_start;
};

static HINSTANCE this_module(void)
{
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)this_module, &m);
    return m;
}

static void read_values(TwGui *g, float v[TW_P_COUNT])
{
    for (int p = 0; p < TW_P_COUNT; p++)
        v[p] = g->cb.get(g->ctx, p);
}

static void paint(TwGui *g)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g->hwnd, &ps);
    read_values(g, g->drawn);
    g->drawn_active = g->active;
    tw_ui_draw(g->px, g->drawn, g->active);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = TW_UI_W;
    bmi.bmiHeader.biHeight = -TW_UI_H; /* top-down rows */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, TW_UI_W, TW_UI_H, 0, 0, 0, TW_UI_H, g->px, &bmi, DIB_RGB_COLORS);
    EndPaint(g->hwnd, &ps);
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
    SetCapture(g->hwnd);
}

static void release(TwGui *g)
{
    if (g->active < 0)
        return;
    g->cb.end(g->ctx, g->active);
    g->active = -1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTA *)lp)->lpCreateParams);
    TwGui *g = (TwGui *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    if (!g)
        return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint(g);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        press(g, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_MOUSEMOVE:
        if (g->active >= 0)
            g->cb.set(g->ctx, g->active,
                      tw_ui_drag_value(g->active, g->drag_start, g->drag_y - GET_Y_LPARAM(lp)));
        break;
    case WM_LBUTTONUP:
        release(g);
        ReleaseCapture();
        break;
    case WM_CAPTURECHANGED:
        release(g);
        break;
    case WM_TIMER:
        break;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    tw_gui_idle(g);
    return 0;
}

TwGui *tw_gui_create(void *parent, const TwGuiCallbacks *cb, void *ctx)
{
    TwGui *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->cb = *cb;
    g->ctx = ctx;
    g->active = -1;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = this_module();
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassA(&wc); /* fails harmlessly if another pedal already registered it */
    g->hwnd = CreateWindowExA(0, CLASS_NAME, "TritoneWorship", WS_CHILD, 0, 0, TW_UI_W, TW_UI_H, (HWND)parent,
                              NULL, wc.hInstance, g);
    if (!g->hwnd) {
        free(g);
        return NULL;
    }
    SetTimer(g->hwnd, TIMER_ID, TIMER_MS, NULL);
    return g;
}

void tw_gui_destroy(TwGui *g)
{
    KillTimer(g->hwnd, TIMER_ID);
    DestroyWindow(g->hwnd);
    UnregisterClassA(CLASS_NAME, this_module()); /* fails harmlessly while another pedal is open */
    free(g);
}

void tw_gui_show(TwGui *g, int visible)
{
    ShowWindow(g->hwnd, visible ? SW_SHOW : SW_HIDE);
}

/* Window messages arrive through wnd_proc; this only repaints when a value has changed */
void tw_gui_idle(TwGui *g)
{
    float v[TW_P_COUNT];
    read_values(g, v);
    if (g->active != g->drawn_active || memcmp(v, g->drawn, sizeof v))
        InvalidateRect(g->hwnd, NULL, FALSE);
}
