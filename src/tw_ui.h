#ifndef TW_UI_H_INCLUDED
#define TW_UI_H_INCLUDED

#include <stdint.h>

#include "tw_dsp.h"

#define TW_UI_W 280
#define TW_UI_H 470
#define TW_UI_DRAG_PIXELS 200

typedef struct {
    int x, y, w, h;
} TwUiRect;

/* Area of the pedal that controls a parameter (knob, toggle or footswitch) */
TwUiRect tw_ui_rect(int param);

/* Parameter whose control is under the point, or -1 */
int tw_ui_hit(int x, int y);

/* New value after dragging a knob up by dy pixels from start_value */
float tw_ui_drag_value(int param, float start_value, int dy);

/* Draws the pedal into a TW_UI_W x TW_UI_H buffer of 0xAARRGGBB pixels.
   The active control shows its value in place of its label. */
void tw_ui_draw(uint32_t *pixels, const float value[TW_P_COUNT], int active);

#endif
