#include "tw_ui.h"

#include <math.h>
#include <stdio.h>

#define BONE 0xffe8e2d0u
#define DEG (3.14159265f / 180.0f)

typedef struct {
    int cx, cy, r;
    const char *label;
} Knob;

static const Knob knobs[TW_P_COUNT] = {
    [TW_P_AMOUNT] = {80, 74, 30, "AMOUNT"},
    [TW_P_DEATH] = {200, 74, 30, "DEATH"},
    [TW_P_TREM_AMOUNT] = {80, 180, 24, "TREMOLO"},
    [TW_P_TREM_SPEED] = {200, 180, 24, "SPEED"},
};

#define SWITCH_X 140
#define STATUS_LED_Y 32
#define BLACKEN_LED_Y 250
#define TOGGLE_Y 285
#define FOOTSWITCH_Y 432

/* 5x7 font, one byte per row, bit 4 is the leftmost pixel */
static const uint8_t font_digits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static const uint8_t font_letters[26][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
};

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static float norm(int param, float value)
{
    const TwParamInfo *p = &tw_param_info[param];
    return clampf((value - p->min) / (p->max - p->min), 0.0f, 1.0f);
}

TwUiRect tw_ui_rect(int param)
{
    if (param == TW_P_BLACKEN)
        return (TwUiRect){SWITCH_X - 25, BLACKEN_LED_Y - 10, 50, 62};
    if (param == TW_P_BYPASS)
        return (TwUiRect){SWITCH_X - 30, FOOTSWITCH_Y - 30, 60, 60};
    const Knob *k = &knobs[param];
    return (TwUiRect){k->cx - k->r - 10, k->cy - k->r - 10, 2 * k->r + 20, 2 * k->r + 46};
}

int tw_ui_hit(int x, int y)
{
    for (int p = 0; p < TW_P_COUNT; p++) {
        TwUiRect r = tw_ui_rect(p);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return p;
    }
    return -1;
}

float tw_ui_drag_value(int param, float start_value, int dy)
{
    const TwParamInfo *p = &tw_param_info[param];
    float v = start_value + dy * (p->max - p->min) / TW_UI_DRAG_PIXELS;
    return clampf(v, p->min, p->max);
}

/* Colours are 0xAARRGGBB; alpha scales the shape's coverage */
static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float ca = (a >> shift) & 255, cb = (b >> shift) & 255;
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

static void blend(uint32_t *px, int x, int y, uint32_t c, float coverage)
{
    if (x < 0 || y < 0 || x >= TW_UI_W || y >= TW_UI_H)
        return;
    float a = clampf(coverage, 0.0f, 1.0f) * (float)(c >> 24) / 255.0f;
    if (a <= 0.0f)
        return;
    uint32_t *d = &px[y * TW_UI_W + x];
    *d = mix(*d, c | 0xff000000u, a) | 0xff000000u;
}

static void fill_circle(uint32_t *px, float cx, float cy, float r, uint32_t top, uint32_t bottom)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float d = hypotf(x + 0.5f - cx, y + 0.5f - cy) - r;
            blend(px, x, y, mix(top, bottom, clampf((y - cy + r) / (2 * r), 0, 1)), 0.5f - d);
        }
}

static void fill_rrect(uint32_t *px, float x0, float y0, float x1, float y1, float rad,
                       uint32_t top, uint32_t bottom)
{
    float hx = (x1 - x0) / 2 - rad, hy = (y1 - y0) / 2 - rad;
    float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++)
        for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
            float qx = fmaxf(fabsf(x + 0.5f - mx) - hx, 0), qy = fmaxf(fabsf(y + 0.5f - my) - hy, 0);
            float d = hypotf(qx, qy) - rad;
            blend(px, x, y, mix(top, bottom, clampf((y - y0) / (y1 - y0), 0, 1)), 0.5f - d);
        }
}

static void fill_line(uint32_t *px, float ax, float ay, float bx, float by, float width, uint32_t c)
{
    float w = width / 2, dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int y = (int)(fminf(ay, by) - w - 1); y <= (int)(fmaxf(ay, by) + w + 1); y++)
        for (int x = (int)(fminf(ax, bx) - w - 1); x <= (int)(fmaxf(ax, bx) + w + 1); x++) {
            float px_ = x + 0.5f - ax, py_ = y + 0.5f - ay;
            float t = clampf((px_ * dx + py_ * dy) / len2, 0, 1);
            float d = hypotf(px_ - t * dx, py_ - t * dy) - w;
            blend(px, x, y, c, 0.5f - d);
        }
}

static void glow(uint32_t *px, float cx, float cy, float r, uint32_t c)
{
    for (int y = (int)(cy - r); y <= (int)(cy + r); y++)
        for (int x = (int)(cx - r); x <= (int)(cx + r); x++) {
            float f = 1.0f - hypotf(x + 0.5f - cx, y + 0.5f - cy) / r;
            if (f > 0)
                blend(px, x, y, c, f * f);
        }
}

static const uint8_t *glyph(char ch)
{
    if (ch >= '0' && ch <= '9')
        return font_digits[ch - '0'];
    if (ch >= 'A' && ch <= 'Z')
        return font_letters[ch - 'A'];
    return NULL;
}

static void draw_text(uint32_t *px, const char *s, int cx, int y, int scale, uint32_t c)
{
    int n = 0;
    while (s[n])
        n++;
    int x = cx - (n * 6 * scale - scale) / 2;
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = glyph(*s);
        if (!g)
            continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            blend(px, x + col * scale + sx, y + row * scale + sy, c, 1);
    }
}

static void draw_screw(uint32_t *px, float cx, float cy)
{
    fill_circle(px, cx, cy, 4.5f, 0xff9a9a9a, 0xff3a3a3a);
    fill_line(px, cx - 3, cy + 1.5f, cx + 3, cy - 1.5f, 1.2f, 0xff202020);
}

static void draw_knob(uint32_t *px, const Knob *k, float n)
{
    float r = (float)k->r;
    for (int i = 0; i <= 10; i++) {
        float a = (-135 + 27 * i) * DEG;
        fill_circle(px, k->cx + sinf(a) * (r + 7), k->cy - cosf(a) * (r + 7), 1.3f, BONE, BONE);
    }
    fill_circle(px, k->cx, k->cy + 3, r + 2, 0x90000000, 0x90000000);
    fill_circle(px, k->cx, k->cy, r, 0xff3a3a3c, 0xff0e0e0f);
    fill_circle(px, k->cx, k->cy, r - 2, 0xff1a1a1b, 0xff060606);
    fill_circle(px, k->cx, k->cy, r * 0.72f, 0xff4a4a4d, 0xff161617);
    fill_circle(px, k->cx, k->cy, r * 0.72f - 1.5f, 0xff2c2c2e, 0xff101011);
    float a = (-135 + 270 * n) * DEG;
    fill_line(px, k->cx + sinf(a) * r * 0.2f, k->cy - cosf(a) * r * 0.2f,
              k->cx + sinf(a) * r * 0.9f, k->cy - cosf(a) * r * 0.9f, 3.0f, BONE);
}

static void draw_led(uint32_t *px, float x, float y, int on)
{
    fill_circle(px, x, y, 7, 0xff5a5a5a, 0xff1a1a1a);
    if (on) {
        glow(px, x, y, 18, 0xa0ff2010);
        fill_circle(px, x, y, 5.5f, 0xffff6a50, 0xffc00000);
        fill_circle(px, x - 1.5f, y - 1.5f, 1.8f, 0xb0ffffff, 0xb0ffffff);
    } else {
        fill_circle(px, x, y, 5.5f, 0xff4a0d0d, 0xff1f0404);
    }
}

/* Mini toggle: chrome nut with the bat pointing up when on */
static void draw_toggle(uint32_t *px, int on)
{
    float tip = on ? TOGGLE_Y - 17 : TOGGLE_Y + 17;
    fill_circle(px, SWITCH_X, TOGGLE_Y + 2, 11, 0x90000000, 0x90000000);
    fill_circle(px, SWITCH_X, TOGGLE_Y, 10, 0xffd0d0d0, 0xff4a4a4a);
    fill_circle(px, SWITCH_X, TOGGLE_Y, 6.5f, 0xff3a3a3a, 0xffa8a8a8);
    fill_line(px, SWITCH_X, TOGGLE_Y, SWITCH_X, tip, 5.0f, 0xffb8b8b8);
    fill_line(px, SWITCH_X - 1, TOGGLE_Y, SWITCH_X - 1, tip, 1.5f, 0x90ffffff);
    fill_circle(px, SWITCH_X, tip, 3.8f, 0xfff2f2f2, 0xff8a8a8a);
}

static void draw_footswitch(uint32_t *px)
{
    fill_circle(px, SWITCH_X, FOOTSWITCH_Y + 3, 24, 0x90000000, 0x90000000);
    fill_circle(px, SWITCH_X, FOOTSWITCH_Y, 22, 0xffd0d0d0, 0xff505050);
    fill_circle(px, SWITCH_X, FOOTSWITCH_Y, 19, 0xff6a6a6a, 0xffbdbdbd);
    fill_circle(px, SWITCH_X, FOOTSWITCH_Y, 14, 0xffe6e6e6, 0xff8c8c8c);
}

void tw_ui_draw(uint32_t *px, const float value[TW_P_COUNT], int active)
{
    for (int i = 0; i < TW_UI_W * TW_UI_H; i++)
        px[i] = 0xff26282bu;

    fill_rrect(px, 6, 9, TW_UI_W - 6, TW_UI_H - 3, 18, 0x90000000, 0x90000000);
    fill_rrect(px, 6, 6, TW_UI_W - 6, TW_UI_H - 6, 18, 0xff3c3c40, 0xff1a1a1c);
    fill_rrect(px, 7.5f, 7.5f, TW_UI_W - 7.5f, TW_UI_H - 7.5f, 16.5f, 0xff1d1d20, 0xff09090a);
    draw_screw(px, 22, 22);
    draw_screw(px, TW_UI_W - 22, 22);
    draw_screw(px, 22, TW_UI_H - 22);
    draw_screw(px, TW_UI_W - 22, TW_UI_H - 22);

    for (int p = 0; p < TW_P_COUNT; p++) {
        const Knob *k = &knobs[p];
        if (!k->label)
            continue;
        draw_knob(px, k, norm(p, value[p]));
        char text[16];
        if (p != active)
            snprintf(text, sizeof text, "%s", k->label);
        else if (p == TW_P_TREM_SPEED)
            snprintf(text, sizeof text, "%d BPM", (int)lroundf(value[p]));
        else
            snprintf(text, sizeof text, "%d", (int)lroundf(value[p]));
        draw_text(px, text, k->cx, k->cy + k->r + 12, 2, BONE);
    }

    draw_led(px, SWITCH_X, STATUS_LED_Y, value[TW_P_BYPASS] < 0.5f);
    draw_led(px, SWITCH_X, BLACKEN_LED_Y, value[TW_P_BLACKEN] > 0.5f);
    draw_toggle(px, value[TW_P_BLACKEN] > 0.5f);
    draw_text(px, "BLACKEN", SWITCH_X, 312, 2, BONE);
    draw_text(px, "TRITONE", TW_UI_W / 2, 346, 3, BONE);
    draw_text(px, "WORSHIP", TW_UI_W / 2, 374, 3, BONE);
    draw_footswitch(px);
}
