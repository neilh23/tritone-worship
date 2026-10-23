#ifndef TW_GUI_H
#define TW_GUI_H

/* Pedal UI in a child of the host's window: an X11 Window, Win32 HWND or Cocoa NSView */

#if !defined(_WIN32) && !defined(__APPLE__)
/* X11 has no event loop of its own in a plugin, so the host's timer must call tw_gui_idle */
#define TW_GUI_NEEDS_HOST_TIMER 1
#endif

typedef struct TwGui TwGui;

typedef struct {
    float (*get)(void *ctx, int param);
    void (*begin)(void *ctx, int param); /* start of a drag or click */
    void (*set)(void *ctx, int param, float value);
    void (*end)(void *ctx, int param);
} TwGuiCallbacks;

TwGui *tw_gui_create(void *parent, const TwGuiCallbacks *cb, void *ctx);
void tw_gui_destroy(TwGui *g);
void tw_gui_show(TwGui *g, int visible);

/* Handles pending input and redraws if any value changed */
void tw_gui_idle(TwGui *g);

#endif
