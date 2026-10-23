#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "tw_dsp.h"

#define SR 48000.0f
#define AMP 0.1f
#define QUIET 0.002f /* low enough that the Blacken clipper is transparent */
#define SETTLE (TW_LATENCY + 2 * TW_FFT_SIZE)
#define MEASURE 32768
#define BLOCK 256

static int failures;

#define CHECK_NEAR(actual, expected, tol) check_near(__func__, #actual, actual, expected, tol)
#define CHECK(cond) check_near(__func__, #cond, (cond) ? 1 : 0, 1, 0)

static void check_near(const char *test, const char *what, double actual, double expected, double tol)
{
    if (fabs(actual - expected) > tol) {
        printf("FAIL %s: %s = %g, expected %g +/- %g\n", test, what, actual, expected, tol);
        failures++;
    }
}

static float out_l[SETTLE + MEASURE], out_r[SETTLE + MEASURE];

/* Feed a stereo sine through the plugin, return the settled left output */
static const float *render_sine_at(TwState *s, float freq, float amp)
{
    static float in[SETTLE + MEASURE];
    for (int i = 0; i < SETTLE + MEASURE; i++)
        in[i] = amp * sinf(2.0f * (float)M_PI * freq * i / SR);
    for (int i = 0; i < SETTLE + MEASURE; i += BLOCK)
        tw_process(s, in + i, in + i, out_l + i, out_r + i, BLOCK);
    return out_l + SETTLE;
}

static const float *render_sine(TwState *s, float freq)
{
    return render_sine_at(s, freq, AMP);
}

/* Amplitude of the sinusoid at freq, via a Hann-windowed DFT */
static double level_at(const float *x, float freq)
{
    double re = 0, im = 0, wsum = 0;
    for (int i = 0; i < MEASURE; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / MEASURE);
        double ph = 2 * M_PI * freq * i / SR;
        re += w * x[i] * cos(ph);
        im -= w * x[i] * sin(ph);
        wsum += w;
    }
    return 2 * sqrt(re * re + im * im) / wsum;
}

static float minor_thirds_up(float freq, int steps)
{
    return freq * powf(2.0f, steps / 4.0f);
}

static void test_amount_curve(void)
{
    CHECK_NEAR(tw_amount_factor(0), 0.3, 1e-6);
    CHECK_NEAR(tw_amount_factor(50), sqrt(0.3), 1e-6);
    CHECK_NEAR(tw_amount_factor(100), 1.0, 1e-6);
}

static void test_death_curve(void)
{
    CHECK_NEAR(tw_death_gain(0), 0.0, 1e-9);
    CHECK_NEAR(tw_death_gain(1), pow(10, -28 * 0.99 / 20), 1e-6);
    CHECK_NEAR(tw_death_gain(50), pow(10, -14.0 / 20), 1e-6);
    CHECK_NEAR(tw_death_gain(100), 1.0, 1e-6);
}

static void test_amount_0_drops_each_minor_third_by_70_percent(void)
{
    TwState *s = tw_create(SR);
    s->p.amount = 0;
    const float *y = render_sine(s, 440);
    CHECK_NEAR(level_at(y, 440), AMP, AMP * 0.05);
    CHECK_NEAR(level_at(y, minor_thirds_up(440, 1)), AMP * 0.3, AMP * 0.3 * 0.2);
    CHECK_NEAR(level_at(y, minor_thirds_up(440, 2)), AMP * 0.09, AMP * 0.09 * 0.2);
    tw_destroy(s);
}

static void test_amount_100_keeps_every_minor_third_at_full_level(void)
{
    TwState *s = tw_create(SR);
    s->p.amount = 100;
    const float *y = render_sine(s, 440);
    CHECK_NEAR(level_at(y, minor_thirds_up(440, 1)), AMP, AMP * 0.2);
    CHECK_NEAR(level_at(y, minor_thirds_up(440, 4)), AMP, AMP * 0.2);
    CHECK_NEAR(level_at(y, minor_thirds_up(440, 9)), AMP, AMP * 0.2);
    tw_destroy(s);
}

static void test_replication_stops_above_18khz(void)
{
    TwState *s = tw_create(SR);
    s->p.amount = 100;
    const float *y = render_sine(s, 10000);
    CHECK_NEAR(level_at(y, minor_thirds_up(10000, 3)), AMP, AMP * 0.2); /* 16.8 kHz */
    CHECK_NEAR(level_at(y, minor_thirds_up(10000, 4)), 0, AMP * 0.01); /* 20 kHz */
    tw_destroy(s);
}

static void test_death_0_adds_nothing_below(void)
{
    TwState *s = tw_create(SR);
    s->p.death = 0;
    const float *y = render_sine(s, 440);
    CHECK_NEAR(level_at(y, 220), 0, AMP * 0.01);
    tw_destroy(s);
}

static void test_death_100_adds_octave_below_with_thirds_reduced_by_amount(void)
{
    TwState *s = tw_create(SR);
    s->p.amount = 0;
    s->p.death = 100;
    const float *y = render_sine(s, 440);
    CHECK_NEAR(level_at(y, 220), AMP, AMP * 0.2);
    CHECK_NEAR(level_at(y, minor_thirds_up(220, 1)), AMP * 0.3, AMP * 0.3 * 0.2);
    CHECK_NEAR(level_at(y, minor_thirds_up(220, 2)), AMP * 0.09, AMP * 0.09 * 0.2);
    tw_destroy(s);
}

static void test_tremolo_amount_0_is_unity_gain(void)
{
    TwState *s = tw_create(SR);
    s->p.trem_amount = 0;
    for (int i = 0; i < SR; i++)
        CHECK_NEAR(tw_trem_gain(s), 1.0, 1e-6);
    tw_destroy(s);
}

static int picks_in_one_second(float bpm)
{
    TwState *s = tw_create(SR);
    s->p.trem_amount = 100;
    s->p.trem_bpm = bpm;
    int picks = 0;
    for (int i = 0; i < SR; i++) {
        float g = tw_trem_gain(s);
        if (s->trem_t == 1) {
            picks++;
            CHECK_NEAR(g, 0.0, 1e-6); /* string damped by the pick at each stroke */
        }
    }
    tw_destroy(s);
    return picks;
}

static void test_tremolo_picks_at_bpm_limits(void)
{
    CHECK_NEAR(picks_in_one_second(666), 11, 1);  /* 666 / 60 = 11.1 */
    CHECK_NEAR(picks_in_one_second(1666), 28, 1); /* 1666 / 60 = 27.8 */
}

static void test_tremolo_lowest_setting_starts_at_30_percent_depth(void)
{
    TwState *s = tw_create(SR);
    s->p.trem_amount = 1;
    float g = tw_trem_gain(s); /* first sample of a pick: string fully damped */
    CHECK_NEAR(g, 1.0 - (0.3 + 0.7 * 0.01), 1e-6);
    tw_destroy(s);
}

static void test_tremolo_downstrokes_louder_than_upstrokes(void)
{
    TwState *s = tw_create(SR);
    s->p.trem_amount = 100;
    s->p.trem_bpm = 1200;
    float peak[40] = {0};
    int pick = -1;
    while (pick < 40) {
        float g = tw_trem_gain(s);
        if (s->trem_t == 1)
            pick++;
        if (pick < 40 && g > peak[pick])
            peak[pick] = g;
    }
    for (int i = 0; i < 40; i += 2)
        if (peak[i] <= peak[i + 1]) {
            printf("FAIL %s: pick %d peak %g <= pick %d peak %g\n", __func__, i, peak[i], i + 1, peak[i + 1]);
            failures++;
        }
    tw_destroy(s);
}

static void test_blacken_off_bypasses_eq(void)
{
    TwState *s = tw_create(SR);
    s->p.amount = 0;
    s->p.blacken = 0;
    s->p.eq_db[14] = 12; /* 4 kHz */
    const float *y = render_sine(s, 4000);
    CHECK_NEAR(level_at(y, 4000), AMP, AMP * 0.05);
    tw_destroy(s);
}

static void test_blacken_distortion_barely_touches_quiet_signals_and_clips_peaks(void)
{
    CHECK_NEAR(tw_blacken_distort(0.01f), 0.01, 0.0001);
    CHECK_NEAR(tw_blacken_distort(-0.01f), -0.01, 0.0001);
    CHECK_NEAR(tw_blacken_distort(0.5f) / 0.5, 0.48, 0.02); /* about 6.3 dB softer */
    CHECK(tw_blacken_distort(10.0f) <= 0.25f);              /* never exceeds 1 / drive */
}

static void test_blacken_distortion_adds_odd_harmonics(void)
{
    TwState *s = tw_create(SR);
    s->p.blacken = 1;
    for (int b = 0; b < TW_EQ_BANDS; b++)
        s->p.eq_db[b] = 0;
    const float *y = render_sine(s, 1000);
    double third = level_at(y, 3000) / level_at(y, 1000);
    tw_destroy(s);

    s = tw_create(SR);
    y = render_sine(s, 1000);
    double third_clean = level_at(y, 3000) / level_at(y, 1000);
    tw_destroy(s);

    CHECK(third > 10 * third_clean);
}

static void test_bypass_passes_input_through_unchanged(void)
{
    TwState *s = tw_create(SR);
    s->p.bypass = 1;
    s->p.blacken = 1;
    s->p.trem_amount = 100;
    const float *y = render_sine(s, 440);
    double worst = 0;
    for (int i = 0; i < MEASURE; i++) {
        float in = AMP * sinf(2.0f * (float)M_PI * 440 * (SETTLE + i) / SR); /* as render_sine makes it */
        worst = fmax(worst, fabs(y[i] - in));
    }
    CHECK_NEAR(worst, 0, 0); /* bit-exact */
    tw_destroy(s);
}

static void test_reset_clears_the_audio_tail(void)
{
    TwState *s = tw_create(SR);
    s->p.death = 100;
    render_sine(s, 440);
    tw_reset(s);
    static float silence[BLOCK], l[BLOCK], r[BLOCK];
    double loudest = 0;
    for (int i = 0; i < TW_FFT_SIZE * 2; i += BLOCK) {
        tw_process(s, silence, silence, l, r, BLOCK);
        for (int j = 0; j < BLOCK; j++)
            loudest = fmax(loudest, fabs(l[j]));
    }
    CHECK_NEAR(loudest, 0, 1e-9);
    tw_destroy(s);
}

static double blacken_level_at_100hz_db(float death)
{
    TwState *s = tw_create(SR);
    s->p.blacken = 1;
    s->p.death = death;
    const float *y = render_sine_at(s, 100, QUIET);
    double db = 20 * log10(level_at(y, 100) / QUIET / s->blacken_trim);
    tw_destroy(s);
    return db;
}

static void test_blacken_cuts_bass_when_death_is_off(void)
{
    double db = blacken_level_at_100hz_db(0);
    if (db > -9.0) {
        printf("FAIL %s: 100 Hz is %g dB, expected a cut below -9 dB\n", __func__, db);
        failures++;
    }
}

static void test_blacken_leaves_bass_untouched_when_death_is_on(void)
{
    CHECK_NEAR(blacken_level_at_100hz_db(1), 0.0, 0.5);
    CHECK_NEAR(blacken_level_at_100hz_db(100), 0.0, 0.5);
}

static void test_blacken_trim_is_minus_0_6_db(void)
{
    TwState *s = tw_create(SR);
    CHECK_NEAR(20 * log10(s->blacken_trim), -0.6, 1e-4);
    tw_destroy(s);
}

static void test_blacken_on_applies_eq_band_gain_and_trim(void)
{
    TwState *s = tw_create(SR);
    s->p.amount = 0;
    s->p.blacken = 1;
    for (int b = 0; b < TW_EQ_BANDS; b++)
        s->p.eq_db[b] = 0;
    s->p.eq_db[14] = 12; /* 4 kHz */
    const float *y = render_sine_at(s, 4000, QUIET);
    double trim_db = 20 * log10(s->blacken_trim);
    CHECK_NEAR(20 * log10(level_at(y, 4000) / QUIET), 12.0 + trim_db, 0.5);
    tw_destroy(s);
}

int main(void)
{
    test_amount_curve();
    test_death_curve();
    test_amount_0_drops_each_minor_third_by_70_percent();
    test_amount_100_keeps_every_minor_third_at_full_level();
    test_replication_stops_above_18khz();
    test_death_0_adds_nothing_below();
    test_death_100_adds_octave_below_with_thirds_reduced_by_amount();
    test_tremolo_amount_0_is_unity_gain();
    test_tremolo_picks_at_bpm_limits();
    test_tremolo_lowest_setting_starts_at_30_percent_depth();
    test_tremolo_downstrokes_louder_than_upstrokes();
    test_blacken_off_bypasses_eq();
    test_blacken_distortion_barely_touches_quiet_signals_and_clips_peaks();
    test_blacken_distortion_adds_odd_harmonics();
    test_bypass_passes_input_through_unchanged();
    test_reset_clears_the_audio_tail();
    test_blacken_cuts_bass_when_death_is_off();
    test_blacken_leaves_bass_untouched_when_death_is_on();
    test_blacken_trim_is_minus_0_6_db();
    test_blacken_on_applies_eq_band_gain_and_trim();
    printf(failures ? "%d check(s) failed\n" : "all tests passed\n", failures);
    return failures != 0;
}
