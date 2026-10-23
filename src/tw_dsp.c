#include "tw_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692f
#define EQ_Q 2.0f
#define BLACKEN_DRIVE 4.0f
#define BYPASS_FADE_S 0.01f
#define BASS_EQ_FLAT_MAX_HZ 250.0f
#define BASS_MIN_DB -28.0f
#define BASS_MAX_GAIN 1.2f
#define COMP_STEPS 24 /* about how many minor thirds fit below the cutoff */
#define COMP_SHARE 0.5f /* share of the stack's level rise, in dB, that Amount takes back */
#define BLACKEN_TRIM_DB -1.5f /* +3 dB vs Blacken off, A-weighted on a saw riff */

const TwParamInfo tw_param_info[TW_P_COUNT] = {
    [TW_P_AMOUNT] = {"amount", "Amount", 0, 100, 50},
    [TW_P_BASS] = {"bass", "Bass", 0, 100, 0},
    [TW_P_BLACKEN] = {"blacken", "Blacken", 0, 1, 0},
    [TW_P_TREM_AMOUNT] = {"trem_amount", "Tremolo Amount", 0, 100, 0},
    [TW_P_TREM_SPEED] = {"trem_speed", "Tremolo Speed", 666, 1666, 800},
    [TW_P_BYPASS] = {"bypass", "Bypass", 0, 1, 0},
};

const float tw_eq_freqs[TW_EQ_BANDS] = {
    31, 44, 63, 88, 125, 175, 250, 350, 500, 700,
    1000, 1400, 2000, 2800, 4000, 5600, 8000, 11000, 14000, 18000,
};

/* First guess at a bad black metal recording: no low end, honky mids,
   boosted 2.5-5k (where the ear is most sensitive/harsh), fizz around 8k,
   cassette-style top end roll-off. */
const float tw_blacken_default_db[TW_EQ_BANDS] = {
    -19, -19, -16, -13, -10, -7, -5, -4, -2, 2.4f,
    4.8f, 3.6f, 4.8f, 8.4f, 9.6f, 6, 4.8f, 1.2f, -7, -13,
};

typedef struct {
    int slot;
    float ratio, gain;
} Voice;

void tw_set_param(TwParams *p, int param, float value)
{
    switch (param) {
    case TW_P_AMOUNT: p->amount = value; break;
    case TW_P_BASS: p->bass = value; break;
    case TW_P_BLACKEN: p->blacken = value > 0.5f; break;
    case TW_P_TREM_AMOUNT: p->trem_amount = value; break;
    case TW_P_TREM_SPEED: p->trem_bpm = value; break;
    case TW_P_BYPASS: p->bypass = value > 0.5f; break;
    }
}

/* Gentle soft clip: unity gain for quiet signals, rounds off loud peaks */
float tw_blacken_distort(float x)
{
    return tanhf(BLACKEN_DRIVE * x) / BLACKEN_DRIVE;
}

float tw_amount_factor(float amount)
{
    /* Linear in dB: 70% cut per step at 0, no cut at 100 */
    return 0.3f * powf(1.0f / 0.3f, amount / 100.0f);
}

float tw_bass_gain(float bass)
{
    /* Linear in dB from -28 dB to +1.6 dB (x1.2), fully off at 0 */
    if (bass <= 0.0f)
        return 0.0f;
    float min = powf(10.0f, BASS_MIN_DB / 20.0f);
    return min * powf(BASS_MAX_GAIN / min, bass / 100.0f);
}

/* Power of the note plus COMP_STEPS minor thirds, each f times the level of the last */
static float stack_power(float f)
{
    float p = 0.0f, g = 1.0f;
    for (int i = 0; i <= COMP_STEPS; i++, g *= f * f)
        p += g;
    return p;
}

float tw_amount_compensation(float amount)
{
    float rise = stack_power(tw_amount_factor(amount)) / stack_power(tw_amount_factor(0));
    return powf(rise, -0.5f * COMP_SHARE);
}

static float rand01(TwState *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (float)(s->rng >> 8) / 16777216.0f;
}

/* Each pick: fast attack, decay, then a short damp as the pick hits the string
   again. Alternate strokes are accented and slightly humanised. */
float tw_trem_gain(TwState *s)
{
    if (s->trem_t >= s->trem_len) {
        s->trem_t = 0;
        s->trem_upstroke = !s->trem_upstroke;
        s->trem_len = (int)(s->sr * 60.0f / s->p.trem_bpm * (0.98f + 0.04f * rand01(s)));
        s->trem_vel = (s->trem_upstroke ? 0.85f : 1.0f) * (0.92f + 0.08f * rand01(s));
    }
    float t = (float)s->trem_t++;
    float attack = 0.0015f * s->sr;
    float mute = 0.001f * s->sr;
    float env = t < attack ? t / attack : expf(-(t - attack) / (float)s->trem_len);
    float tail = ((float)s->trem_len - t) / mute;
    if (tail < 1.0f)
        env *= tail;
    /* Below 30% depth the effect is inaudible, so 1-100 maps to 30-100% */
    float depth = s->p.trem_amount <= 0.0f ? 0.0f : 0.3f + 0.7f * s->p.trem_amount / 100.0f;
    return 1.0f - depth * (1.0f - env * s->trem_vel);
}

static float wrap_phase(float x)
{
    return x - TWO_PI * floorf((x + (float)M_PI) / TWO_PI);
}

static void fft(TwState *s, float *re, float *im, int inverse)
{
    const int n = TW_FFT_SIZE;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s->cos_tab[k * step];
                float wi = inverse ? s->sin_tab[k * step] : -s->sin_tab[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
            }
        }
    }
}

static int build_voices(const TwState *s, Voice *v)
{
    float f = tw_amount_factor(s->p.amount);
    float bin_hz = s->sr / TW_FFT_SIZE;
    int n = 0;

    float g = 1.0f;
    for (int i = 1; i <= TW_MAX_UP_STEPS; i++) {
        g *= f;
        float r = powf(2.0f, i / 4.0f);
        if (g < 1e-4f || r * bin_hz > TW_CUTOFF_HZ)
            break;
        v[n++] = (Voice){i - 1, r, g};
    }

    g = tw_bass_gain(s->p.bass);
    for (int j = 0; j < TW_BASS_STEPS && g >= 1e-4f; j++, g *= f)
        v[n++] = (Voice){TW_MAX_UP_STEPS + j, 0.5f * powf(2.0f, j / 4.0f), g};
    return n;
}

/* Spectral peaks, each owning the bins down to the minimum between it and
   its neighbours. Returns the number of peaks. */
static int find_peaks(TwState *s)
{
    int np = 0;
    for (int k = 1; k < TW_BINS - 1; k++)
        if (s->ana_mag[k] > s->ana_mag[k - 1] && s->ana_mag[k] >= s->ana_mag[k + 1])
            s->peak[np++] = k;
    for (int i = 0; i < np; i++) {
        s->peak_lo[i] = i == 0 ? 0 : s->peak_hi[i - 1] + 1;
        if (i == np - 1) {
            s->peak_hi[i] = TW_BINS - 1;
            continue;
        }
        int lowest = s->peak[i];
        for (int k = s->peak[i]; k < s->peak[i + 1]; k++)
            if (s->ana_mag[k] < s->ana_mag[lowest])
                lowest = k;
        s->peak_hi[i] = lowest;
    }
    return np;
}

/* Phase vocoder with identity phase locking: one analysis, each voice moves
   every peak's bins together to the shifted frequency, all voices summed in the
   frequency domain, one resynthesis. Shifted partials above the cutoff are
   dropped. */
static void process_frame(TwState *s, TwChannel *c)
{
    const float expct = TWO_PI * TW_HOP / TW_FFT_SIZE;
    const float osamp = (float)TW_FFT_SIZE / TW_HOP;
    const float bin_hz = s->sr / TW_FFT_SIZE;
    Voice v[TW_MAX_VOICES];
    int nv = build_voices(s, v);

    for (int k = 0; k < TW_FFT_SIZE; k++) {
        s->re[k] = c->in_fifo[k] * s->window[k];
        s->im[k] = 0.0f;
    }
    fft(s, s->re, s->im, 0);

    for (int k = 0; k < TW_BINS; k++) {
        float ph = atan2f(s->im[k], s->re[k]);
        float d = wrap_phase(ph - c->last_phase[k] - k * expct);
        c->last_phase[k] = ph;
        s->ana_re[k] = s->re[k];
        s->ana_im[k] = s->im[k];
        s->ana_mag[k] = hypotf(s->re[k], s->im[k]);
        s->ana_phase[k] = ph;
        s->ana_freq[k] = k + d * osamp / TWO_PI;
    }
    int np = find_peaks(s);

    memset(s->acc_re, 0, sizeof s->acc_re);
    memset(s->acc_im, 0, sizeof s->acc_im);
    for (int i = 0; i < nv; i++) {
        float *sp = c->sum_phase[v[i].slot];
        for (int j = 0; j < np; j++) {
            int p = s->peak[j];
            float target = s->ana_freq[p] * v[i].ratio;
            if (target * bin_hz > TW_CUTOFF_HZ)
                break;
            int shift = (int)lroundf(target - s->ana_freq[p]);
            int q = p + shift;
            if (q < 0 || q >= TW_BINS)
                continue;
            sp[q] = wrap_phase(sp[q] + target * expct);
            float rot = sp[q] - s->ana_phase[p];
            float rr = v[i].gain * cosf(rot), ri = v[i].gain * sinf(rot);
            for (int k = s->peak_lo[j]; k <= s->peak_hi[j]; k++) {
                int t = k + shift;
                if (t < 0 || t >= TW_BINS)
                    continue;
                s->acc_re[t] += s->ana_re[k] * rr - s->ana_im[k] * ri;
                s->acc_im[t] += s->ana_re[k] * ri + s->ana_im[k] * rr;
            }
        }
    }

    for (int k = 0; k < TW_BINS; k++) {
        s->re[k] = s->acc_re[k];
        s->im[k] = s->acc_im[k];
    }
    s->im[0] = 0.0f;
    s->im[TW_FFT_SIZE / 2] = 0.0f;
    for (int k = 1; k < TW_FFT_SIZE / 2; k++) {
        s->re[TW_FFT_SIZE - k] = s->re[k];
        s->im[TW_FFT_SIZE - k] = -s->im[k];
    }
    fft(s, s->re, s->im, 1);

    for (int k = 0; k < TW_FFT_SIZE; k++)
        c->out_accum[k] += s->window[k] * s->re[k] * s->out_scale;
    memcpy(c->out_fifo, c->out_accum, TW_HOP * sizeof(float));
    memmove(c->out_accum, c->out_accum + TW_HOP, TW_FFT_SIZE * sizeof(float));
    memmove(c->in_fifo, c->in_fifo + TW_HOP, TW_LATENCY * sizeof(float));
}

static float biquad(TwBiquad *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static void set_peaking(TwBiquad *f, float sr, float freq, float db)
{
    if (freq >= 0.49f * sr) {
        f->b0 = 1.0f;
        f->b1 = f->b2 = f->a1 = f->a2 = 0.0f;
        return;
    }
    float a = powf(10.0f, db / 40.0f);
    float w0 = TWO_PI * freq / sr;
    float alpha = sinf(w0) / (2.0f * EQ_Q);
    float a0 = 1.0f + alpha / a;
    f->b0 = (1.0f + alpha * a) / a0;
    f->b1 = -2.0f * cosf(w0) / a0;
    f->b2 = (1.0f - alpha * a) / a0;
    f->a1 = f->b1;
    f->a2 = (1.0f - alpha / a) / a0;
}

/* While Bass is on, the bass bands are left flat so the octave below survives */
static void update_eq(TwState *s)
{
    float db[TW_EQ_BANDS];
    for (int b = 0; b < TW_EQ_BANDS; b++)
        db[b] = s->p.bass > 0.0f && tw_eq_freqs[b] <= BASS_EQ_FLAT_MAX_HZ ? 0.0f : s->p.eq_db[b];
    if (memcmp(db, s->eq_applied_db, sizeof db) == 0)
        return;
    memcpy(s->eq_applied_db, db, sizeof db);
    for (int c = 0; c < 2; c++)
        for (int b = 0; b < TW_EQ_BANDS; b++)
            set_peaking(&s->ch[c].eq[b], s->sr, tw_eq_freqs[b], db[b]);
}

static float channel_tick(TwState *s, TwChannel *c, float x, float trem)
{
    c->in_fifo[c->rover] = x;
    float wet = c->out_fifo[c->rover - TW_LATENCY];
    if (++c->rover >= TW_FFT_SIZE) {
        c->rover = TW_LATENCY;
        process_frame(s, c);
    }

    float dry = c->dry[c->dry_pos];
    c->dry[c->dry_pos] = x;
    c->dry_pos = (c->dry_pos + 1) % TW_LATENCY;

    float y = (dry + wet) * trem;
    /* EQ always runs so its state is warm when Blacken is switched on */
    float e = tw_blacken_distort(y);
    for (int b = 0; b < TW_EQ_BANDS; b++)
        e = biquad(&c->eq[b], e);
    return s->p.blacken ? e * s->blacken_trim : y;
}

TwState *tw_create(float sample_rate)
{
    TwState *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->sr = sample_rate;
    for (int i = 0; i < TW_P_COUNT; i++)
        tw_set_param(&s->p, i, tw_param_info[i].def);
    memcpy(s->p.eq_db, tw_blacken_default_db, sizeof s->p.eq_db);
    s->blacken_trim = powf(10.0f, BLACKEN_TRIM_DB / 20.0f);

    float wss = 0.0f;
    for (int k = 0; k < TW_FFT_SIZE; k++) {
        s->window[k] = 0.5f - 0.5f * cosf(TWO_PI * k / TW_FFT_SIZE);
        wss += s->window[k] * s->window[k];
    }
    s->out_scale = TW_HOP / (TW_FFT_SIZE * wss);
    for (int k = 0; k < TW_FFT_SIZE / 2; k++) {
        s->cos_tab[k] = cosf(TWO_PI * k / TW_FFT_SIZE);
        s->sin_tab[k] = sinf(TWO_PI * k / TW_FFT_SIZE);
    }
    tw_reset(s);
    return s;
}

void tw_reset(TwState *s)
{
    memset(s->ch, 0, sizeof s->ch);
    for (int c = 0; c < 2; c++)
        s->ch[c].rover = TW_LATENCY;
    s->eq_applied_db[0] = NAN;
    update_eq(s);
    s->trem_t = s->trem_len = 0;
    s->trem_upstroke = 1; /* so the first pick is a downstroke */
    s->rng = 0x12345678u;
    s->bypass_mix = s->p.bypass;
}

void tw_destroy(TwState *s)
{
    free(s);
}

void tw_process(TwState *s, const float *in_l, const float *in_r,
                float *out_l, float *out_r, uint32_t n)
{
    update_eq(s);
    const float fade = 1.0f / (BYPASS_FADE_S * s->sr);
    const float comp = tw_amount_compensation(s->p.amount);
    for (uint32_t i = 0; i < n; i++) {
        float xl = in_l[i], xr = in_r[i];
        float g = tw_trem_gain(s) * comp;
        float yl = channel_tick(s, &s->ch[0], xl, g);
        float yr = channel_tick(s, &s->ch[1], xr, g);
        /* Keeps processing while bypassed so switching back in is seamless */
        float m = s->bypass_mix = fminf(fmaxf(s->bypass_mix + (s->p.bypass ? fade : -fade), 0.0f), 1.0f);
        out_l[i] = xl * m + yl * (1.0f - m);
        out_r[i] = xr * m + yr * (1.0f - m);
    }
}
