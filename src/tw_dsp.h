#ifndef TW_DSP_H
#define TW_DSP_H

#include <stdint.h>

#define TW_EQ_BANDS 20
#define TW_FFT_SIZE 2048
#define TW_HOP 256
#define TW_LATENCY (TW_FFT_SIZE - TW_HOP)
#define TW_BINS (TW_FFT_SIZE / 2 + 1)
#define TW_MAX_UP_STEPS 40
#define TW_BASS_STEPS 4
#define TW_MAX_VOICES (TW_MAX_UP_STEPS + TW_BASS_STEPS)
#define TW_CUTOFF_HZ 18000.0f

enum { TW_P_AMOUNT, TW_P_BASS, TW_P_BLACKEN, TW_P_TREM_AMOUNT, TW_P_TREM_SPEED, TW_P_BYPASS, TW_P_COUNT };

typedef struct {
    const char *symbol, *name;
    float min, max, def;
} TwParamInfo;

extern const TwParamInfo tw_param_info[TW_P_COUNT];
extern const float tw_eq_freqs[TW_EQ_BANDS];
extern const float tw_blacken_default_db[TW_EQ_BANDS];

typedef struct {
    float amount;      /* 0-100 */
    float bass;        /* 0-100 */
    int blacken;       /* 0/1 */
    float trem_amount; /* 0-100 */
    float trem_bpm;    /* 666-1666 */
    int bypass;        /* 0/1 */
    float eq_db[TW_EQ_BANDS];
} TwParams;

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} TwBiquad;

typedef struct {
    float in_fifo[TW_FFT_SIZE];
    float out_fifo[TW_FFT_SIZE];
    float out_accum[2 * TW_FFT_SIZE];
    float last_phase[TW_BINS];
    float sum_phase[TW_MAX_VOICES][TW_BINS];
    float dry[TW_FFT_SIZE];
    int dry_pos;
    int rover;
    TwBiquad eq[TW_EQ_BANDS];
} TwChannel;

typedef struct {
    float sr;
    TwParams p;
    float eq_applied_db[TW_EQ_BANDS];
    float blacken_trim;
    float bypass_mix; /* 0 = effect, 1 = dry input */
    TwChannel ch[2];

    float window[TW_FFT_SIZE];
    float cos_tab[TW_FFT_SIZE / 2];
    float sin_tab[TW_FFT_SIZE / 2];
    float out_scale;

    float re[TW_FFT_SIZE], im[TW_FFT_SIZE];
    float ana_re[TW_BINS], ana_im[TW_BINS];
    float ana_mag[TW_BINS], ana_phase[TW_BINS], ana_freq[TW_BINS];
    int peak[TW_BINS], peak_lo[TW_BINS], peak_hi[TW_BINS];
    float acc_re[TW_BINS], acc_im[TW_BINS];

    int trem_t;
    int trem_len;
    float trem_vel;
    int trem_upstroke;
    uint32_t rng;
} TwState;

void tw_set_param(TwParams *p, int param, float value);
float tw_amount_factor(float amount);
float tw_blacken_distort(float x);
float tw_bass_gain(float bass);
/* Output gain that takes back part of the level the stack adds as Amount rises; 1 at Amount 0 */
float tw_amount_compensation(float amount);
float tw_trem_gain(TwState *s);

TwState *tw_create(float sample_rate);
void tw_reset(TwState *s);
void tw_destroy(TwState *s);
void tw_process(TwState *s, const float *in_l, const float *in_r,
                float *out_l, float *out_r, uint32_t n);

#endif
