#include <stdlib.h>

#include "lv2/core/lv2.h"
#include "tw_dsp.h"

#define TW_URI "urn:tritoneworship:tritoneworship"

/* Control ports 0..TW_P_COUNT-1 follow the tw_param_info order */
enum {
    P_IN_L = TW_P_COUNT,
    P_IN_R,
    P_OUT_L,
    P_OUT_R,
    P_LATENCY,
    P_EQ0,
};

typedef struct {
    TwState *dsp;
    const float *ctl[TW_P_COUNT];
    const float *in_l, *in_r;
    float *out_l, *out_r;
    float *latency;
#ifdef TW_EQ_TUNING
    const float *eq[TW_EQ_BANDS];
#endif
} Plugin;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *path, const LV2_Feature *const *features)
{
    Plugin *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->dsp = tw_create((float)rate);
    if (!p->dsp) {
        free(p);
        return NULL;
    }
    return p;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    Plugin *p = h;
    switch (port) {
    case P_IN_L: p->in_l = data; break;
    case P_IN_R: p->in_r = data; break;
    case P_OUT_L: p->out_l = data; break;
    case P_OUT_R: p->out_r = data; break;
    case P_LATENCY: p->latency = data; break;
    default:
        if (port < TW_P_COUNT)
            p->ctl[port] = data;
#ifdef TW_EQ_TUNING
        else if (port < P_EQ0 + TW_EQ_BANDS)
            p->eq[port - P_EQ0] = data;
#endif
        break;
    }
}

static void run(LV2_Handle h, uint32_t n)
{
    Plugin *p = h;
    for (int i = 0; i < TW_P_COUNT; i++)
        tw_set_param(&p->dsp->p, i, *p->ctl[i]);
#ifdef TW_EQ_TUNING
    for (int b = 0; b < TW_EQ_BANDS; b++)
        p->dsp->p.eq_db[b] = *p->eq[b];
#endif
    if (p->latency)
        *p->latency = TW_LATENCY;
    tw_process(p->dsp, p->in_l, p->in_r, p->out_l, p->out_r, n);
}

static void cleanup(LV2_Handle h)
{
    Plugin *p = h;
    tw_destroy(p->dsp);
    free(p);
}

static const LV2_Descriptor descriptor = {
    TW_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
