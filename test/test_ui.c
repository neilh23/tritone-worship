#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tw_ui.h"

static int failures;
static uint32_t px[TW_UI_W * TW_UI_H];

#define CHECK(cond) check(__func__, #cond, cond)

static void check(const char *test, const char *what, int ok)
{
    if (!ok) {
        printf("FAIL %s: %s\n", test, what);
        failures++;
    }
}

static void draw_defaults(float *v, int active)
{
    for (int p = 0; p < TW_P_COUNT; p++)
        v[p] = tw_param_info[p].def;
    tw_ui_draw(px, v, active);
}

static int brightness(int x, int y)
{
    uint32_t c = px[y * TW_UI_W + x];
    return (((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255)) / 3;
}

/* Brightness on the knob face at the given pointer angle (0 = straight up) */
static int knob_brightness_at(int param, float degrees)
{
    TwUiRect r = tw_ui_rect(param);
    float radius = (r.w - 20) / 2.0f;
    float cx = r.x + r.w / 2.0f, cy = r.y + 10 + radius;
    float a = degrees * 3.14159265f / 180.0f;
    return brightness((int)(cx + sinf(a) * radius * 0.6f), (int)(cy - cosf(a) * radius * 0.6f));
}

static int count_red_pixels(TwUiRect r)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t c = px[y * TW_UI_W + x];
            if (((c >> 16) & 255) > 180 && ((c >> 8) & 255) < 80)
                n++;
        }
    return n;
}

static int region_differs(const uint32_t *a, const uint32_t *b, TwUiRect r)
{
    for (int y = r.y; y < r.y + r.h; y++)
        if (memcmp(&a[y * TW_UI_W + r.x], &b[y * TW_UI_W + r.x], r.w * sizeof *a))
            return 1;
    return 0;
}

static void test_dragging_full_height_covers_whole_range(void)
{
    CHECK(tw_ui_drag_value(TW_P_AMOUNT, 0, TW_UI_DRAG_PIXELS) == 100);
    CHECK(tw_ui_drag_value(TW_P_TREM_SPEED, 666, TW_UI_DRAG_PIXELS) == 1666);
    CHECK(tw_ui_drag_value(TW_P_DEATH, 100, -TW_UI_DRAG_PIXELS / 2) == 50);
}

static void test_dragging_past_the_ends_clamps(void)
{
    CHECK(tw_ui_drag_value(TW_P_AMOUNT, 10, -TW_UI_DRAG_PIXELS) == 0);
    CHECK(tw_ui_drag_value(TW_P_TREM_SPEED, 1300, TW_UI_DRAG_PIXELS) == 1666);
}

static void test_controls_are_inside_pedal_and_do_not_overlap(void)
{
    for (int a = 0; a < TW_P_COUNT; a++) {
        TwUiRect r = tw_ui_rect(a);
        CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= TW_UI_W && r.y + r.h <= TW_UI_H);
        for (int b = a + 1; b < TW_P_COUNT; b++) {
            TwUiRect s = tw_ui_rect(b);
            CHECK(r.x + r.w <= s.x || s.x + s.w <= r.x || r.y + r.h <= s.y || s.y + s.h <= r.y);
        }
    }
}

static void test_hit_finds_the_control_under_the_point(void)
{
    TwUiRect r = tw_ui_rect(TW_P_DEATH);
    CHECK(tw_ui_hit(r.x, r.y) == TW_P_DEATH);
    CHECK(tw_ui_hit(r.x + r.w - 1, r.y + r.h - 1) == TW_P_DEATH);
    CHECK(tw_ui_hit(r.x + r.w, r.y) != TW_P_DEATH);
    r = tw_ui_rect(TW_P_BYPASS);
    CHECK(tw_ui_hit(r.x + r.w / 2, r.y + r.h / 2) == TW_P_BYPASS);
    CHECK(tw_ui_hit(0, 0) == -1);
}

static void test_knob_pointer_follows_value(void)
{
    float v[TW_P_COUNT];
    draw_defaults(v, -1);

    v[TW_P_AMOUNT] = 0;
    tw_ui_draw(px, v, -1);
    CHECK(knob_brightness_at(TW_P_AMOUNT, -135) > 150); /* pointer at 7 o'clock */
    CHECK(knob_brightness_at(TW_P_AMOUNT, 135) < 80);

    v[TW_P_AMOUNT] = 100;
    tw_ui_draw(px, v, -1);
    CHECK(knob_brightness_at(TW_P_AMOUNT, -135) < 80);
    CHECK(knob_brightness_at(TW_P_AMOUNT, 135) > 150); /* pointer at 5 o'clock */
}

static void test_led_lights_when_blacken_is_on(void)
{
    float v[TW_P_COUNT];
    draw_defaults(v, -1);
    CHECK(count_red_pixels(tw_ui_rect(TW_P_BLACKEN)) == 0);

    v[TW_P_BLACKEN] = 1;
    tw_ui_draw(px, v, -1);
    CHECK(count_red_pixels(tw_ui_rect(TW_P_BLACKEN)) > 20);
}

static void test_status_led_at_top_lights_unless_bypassed(void)
{
    TwUiRect top = {0, 0, TW_UI_W, 60};
    float v[TW_P_COUNT];
    draw_defaults(v, -1);
    CHECK(count_red_pixels(top) > 20);

    v[TW_P_BYPASS] = 1;
    tw_ui_draw(px, v, -1);
    CHECK(count_red_pixels(top) == 0);
}

/* Brightness 14 px above and below the toggle, which sits at the bottom of the Blacken area */
static void toggle_brightness(int *above, int *below)
{
    TwUiRect r = tw_ui_rect(TW_P_BLACKEN);
    int cx = r.x + r.w / 2, cy = r.y + r.h - 17;
    *above = brightness(cx, cy - 14);
    *below = brightness(cx, cy + 14);
}

static void test_blacken_toggle_points_up_when_on(void)
{
    int above, below;
    float v[TW_P_COUNT];
    draw_defaults(v, -1);
    toggle_brightness(&above, &below);
    CHECK(below > 120 && above < 80);

    v[TW_P_BLACKEN] = 1;
    tw_ui_draw(px, v, -1);
    toggle_brightness(&above, &below);
    CHECK(above > 120 && below < 80);
}

static void test_dragged_knob_shows_value_instead_of_label(void)
{
    static uint32_t idle[TW_UI_W * TW_UI_H];
    float v[TW_P_COUNT];
    draw_defaults(v, -1);
    memcpy(idle, px, sizeof px);

    draw_defaults(v, TW_P_TREM_SPEED);
    CHECK(region_differs(idle, px, tw_ui_rect(TW_P_TREM_SPEED)));
    CHECK(!region_differs(idle, px, tw_ui_rect(TW_P_AMOUNT)));
}

int main(void)
{
    test_dragging_full_height_covers_whole_range();
    test_dragging_past_the_ends_clamps();
    test_controls_are_inside_pedal_and_do_not_overlap();
    test_hit_finds_the_control_under_the_point();
    test_knob_pointer_follows_value();
    test_led_lights_when_blacken_is_on();
    test_status_led_at_top_lights_unless_bypassed();
    test_blacken_toggle_points_up_when_on();
    test_dragged_knob_shows_value_instead_of_label();
    printf(failures ? "%d check(s) failed\n" : "all UI tests passed\n", failures);
    return failures != 0;
}
