/* VST3 plugin through the Steinberg C API: one object is the component, processor and controller */
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vst3_c_api.h>

#include "tw_dsp.h"
#include "tw_ui.h"
#include "tw_gui.h"

#if defined(_WIN32)
#define GUI_PLATFORM Steinberg_kPlatformTypeHWND
#define EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define GUI_PLATFORM Steinberg_kPlatformTypeNSView
#define EXPORT __attribute__((visibility("default")))
#else
#define GUI_PLATFORM Steinberg_kPlatformTypeX11EmbedWindowID
#define EXPORT __attribute__((visibility("default")))
#endif

/* Param ids: 0..TW_P_COUNT-1 follow tw_param_info, then the EQ bands in tuning builds */
#ifdef TW_EQ_TUNING
#define N_PARAMS (TW_P_COUNT + TW_EQ_BANDS)
#else
#define N_PARAMS TW_P_COUNT
#endif

#define MAX_QUEUES 64
#define GUI_TIMER_MS 30
#define VENDOR "Neil Howie"

#define FROM(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

typedef Steinberg_tresult tresult;

static const Steinberg_TUID class_id = SMTG_INLINE_UID(0x5A1C6E3B, 0x9F2D4C71, 0xB3E8A04D, 0x7C6612F5);

typedef struct {
    Steinberg_Vst_IComponent component;
    Steinberg_Vst_IAudioProcessor processor;
    Steinberg_Vst_IEditController controller;
    atomic_uint refs;
    TwState *dsp;
    double sample_rate;
    /* Plain values, shared between the audio and UI threads */
    _Atomic float value[N_PARAMS];
    Steinberg_Vst_IComponentHandler *handler;
} Plugin;

static int is_switch(Steinberg_Vst_ParamID id)
{
    return id == TW_P_BLACKEN || id == TW_P_BYPASS;
}

static void param_range(Steinberg_Vst_ParamID id, double *min, double *max, double *def)
{
    if (id < TW_P_COUNT) {
        *min = tw_param_info[id].min;
        *max = tw_param_info[id].max;
        *def = tw_param_info[id].def;
        return;
    }
    *min = -24;
    *max = 24;
    *def = tw_blacken_default_db[id - TW_P_COUNT];
}

static double to_plain(Steinberg_Vst_ParamID id, double normalized)
{
    double min, max, def;
    param_range(id, &min, &max, &def);
    if (is_switch(id))
        return normalized >= 0.5;
    return min + normalized * (max - min);
}

static double to_normalized(Steinberg_Vst_ParamID id, double plain)
{
    double min, max, def;
    param_range(id, &min, &max, &def);
    return (plain - min) / (max - min);
}

static void to_string128(Steinberg_Vst_String128 dst, const char *src)
{
    int i = 0;
    for (; src[i] && i < 127; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void apply_params(Plugin *p)
{
    for (int i = 0; i < TW_P_COUNT; i++)
        tw_set_param(&p->dsp->p, i, atomic_load(&p->value[i]));
#ifdef TW_EQ_TUNING
    for (int b = 0; b < TW_EQ_BANDS; b++)
        p->dsp->p.eq_db[b] = atomic_load(&p->value[TW_P_COUNT + b]);
#endif
}

static uint32_t plugin_add_ref(Plugin *p)
{
    return atomic_fetch_add(&p->refs, 1) + 1;
}

static uint32_t plugin_release(Plugin *p)
{
    uint32_t left = atomic_fetch_sub(&p->refs, 1) - 1;
    if (left == 0) {
        if (p->handler)
            p->handler->lpVtbl->release(p->handler);
        tw_destroy(p->dsp);
        free(p);
    }
    return left;
}

static tresult plugin_query(Plugin *p, const Steinberg_TUID iid, void **obj)
{
    if (!memcmp(iid, Steinberg_FUnknown_iid, 16) || !memcmp(iid, Steinberg_IPluginBase_iid, 16) ||
        !memcmp(iid, Steinberg_Vst_IComponent_iid, 16))
        *obj = &p->component;
    else if (!memcmp(iid, Steinberg_Vst_IAudioProcessor_iid, 16))
        *obj = &p->processor;
    else if (!memcmp(iid, Steinberg_Vst_IEditController_iid, 16))
        *obj = &p->controller;
    else {
        *obj = NULL;
        return Steinberg_kNoInterface;
    }
    plugin_add_ref(p);
    return Steinberg_kResultOk;
}

/* ---- state: one "symbol=value" line per parameter ---- */

static void param_symbol(Steinberg_Vst_ParamID id, char *buf, size_t size)
{
    if (id < TW_P_COUNT)
        snprintf(buf, size, "%s", tw_param_info[id].symbol);
    else
        snprintf(buf, size, "eq_%g", tw_eq_freqs[id - TW_P_COUNT]);
}

static tresult state_save(Plugin *p, struct Steinberg_IBStream *s)
{
    char text[N_PARAMS * 48] = "";
    int len = 0;
    for (Steinberg_Vst_ParamID i = 0; i < N_PARAMS; i++) {
        char sym[32];
        param_symbol(i, sym, sizeof sym);
        len += snprintf(text + len, sizeof text - len, "%s=%.6g\n", sym, atomic_load(&p->value[i]));
    }
    for (int done = 0; done < len;) {
        Steinberg_int32 w = 0;
        if (s->lpVtbl->write(s, text + done, len - done, &w) != Steinberg_kResultOk || w <= 0)
            return Steinberg_kResultFalse;
        done += w;
    }
    return Steinberg_kResultOk;
}

static tresult state_load(Plugin *p, struct Steinberg_IBStream *s)
{
    char text[N_PARAMS * 48 * 2];
    int len = 0;
    for (Steinberg_int32 r = 1; len < (int)sizeof text - 1 && r > 0;) {
        if (s->lpVtbl->read(s, text + len, sizeof text - 1 - len, &r) != Steinberg_kResultOk)
            break;
        len += r;
    }
    text[len] = 0;

    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        for (Steinberg_Vst_ParamID i = 0; i < N_PARAMS; i++) {
            char sym[32];
            param_symbol(i, sym, sizeof sym);
            if (strcmp(sym, line) == 0) {
                double min, max, def, v = atof(eq + 1);
                param_range(i, &min, &max, &def);
                atomic_store(&p->value[i], (float)(v < min ? min : v > max ? max : v));
            }
        }
    }
    return Steinberg_kResultOk;
}

/* ---- IComponent ---- */

static tresult SMTG_STDMETHODCALLTYPE comp_query(void *self, const Steinberg_TUID iid, void **obj)
{
    return plugin_query(FROM(self, Plugin, component), iid, obj);
}

static uint32_t SMTG_STDMETHODCALLTYPE comp_add_ref(void *self)
{
    return plugin_add_ref(FROM(self, Plugin, component));
}

static uint32_t SMTG_STDMETHODCALLTYPE comp_release(void *self)
{
    return plugin_release(FROM(self, Plugin, component));
}

static tresult SMTG_STDMETHODCALLTYPE initialize(void *self, struct Steinberg_FUnknown *context)
{
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE terminate(void *self)
{
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE get_controller_class_id(void *self, Steinberg_TUID id)
{
    return Steinberg_kNotImplemented; /* the component is its own controller */
}

static tresult SMTG_STDMETHODCALLTYPE set_io_mode(void *self, Steinberg_Vst_IoMode mode)
{
    return Steinberg_kResultOk;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE get_bus_count(void *self, Steinberg_Vst_MediaType type,
                                                            Steinberg_Vst_BusDirection dir)
{
    return type == Steinberg_Vst_MediaTypes_kAudio ? 1 : 0;
}

static tresult SMTG_STDMETHODCALLTYPE get_bus_info(void *self, Steinberg_Vst_MediaType type,
                                                   Steinberg_Vst_BusDirection dir, Steinberg_int32 index,
                                                   struct Steinberg_Vst_BusInfo *bus)
{
    if (type != Steinberg_Vst_MediaTypes_kAudio || index != 0)
        return Steinberg_kInvalidArgument;
    bus->mediaType = type;
    bus->direction = dir;
    bus->channelCount = 2;
    to_string128(bus->name, dir == Steinberg_Vst_BusDirections_kInput ? "In" : "Out");
    bus->busType = Steinberg_Vst_BusTypes_kMain;
    bus->flags = Steinberg_Vst_BusInfo_BusFlags_kDefaultActive;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE get_routing_info(void *self, struct Steinberg_Vst_RoutingInfo *in,
                                                       struct Steinberg_Vst_RoutingInfo *out)
{
    return Steinberg_kNotImplemented;
}

static tresult SMTG_STDMETHODCALLTYPE activate_bus(void *self, Steinberg_Vst_MediaType type,
                                                   Steinberg_Vst_BusDirection dir, Steinberg_int32 index,
                                                   Steinberg_TBool state)
{
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE set_active(void *self, Steinberg_TBool state)
{
    Plugin *p = FROM(self, Plugin, component);
    tw_destroy(p->dsp);
    p->dsp = NULL;
    if (!state)
        return Steinberg_kResultOk;
    p->dsp = tw_create((float)p->sample_rate);
    if (!p->dsp)
        return Steinberg_kResultFalse;
    apply_params(p);
    tw_reset(p->dsp);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE comp_set_state(void *self, struct Steinberg_IBStream *s)
{
    return state_load(FROM(self, Plugin, component), s);
}

static tresult SMTG_STDMETHODCALLTYPE comp_get_state(void *self, struct Steinberg_IBStream *s)
{
    return state_save(FROM(self, Plugin, component), s);
}

static Steinberg_Vst_IComponentVtbl component_vtbl = {
    .queryInterface = comp_query,
    .addRef = comp_add_ref,
    .release = comp_release,
    .initialize = initialize,
    .terminate = terminate,
    .getControllerClassId = get_controller_class_id,
    .setIoMode = set_io_mode,
    .getBusCount = get_bus_count,
    .getBusInfo = get_bus_info,
    .getRoutingInfo = get_routing_info,
    .activateBus = activate_bus,
    .setActive = set_active,
    .setState = comp_set_state,
    .getState = comp_get_state,
};

/* ---- IAudioProcessor ---- */

static tresult SMTG_STDMETHODCALLTYPE proc_query(void *self, const Steinberg_TUID iid, void **obj)
{
    return plugin_query(FROM(self, Plugin, processor), iid, obj);
}

static uint32_t SMTG_STDMETHODCALLTYPE proc_add_ref(void *self)
{
    return plugin_add_ref(FROM(self, Plugin, processor));
}

static uint32_t SMTG_STDMETHODCALLTYPE proc_release(void *self)
{
    return plugin_release(FROM(self, Plugin, processor));
}

static tresult SMTG_STDMETHODCALLTYPE set_bus_arrangements(void *self, Steinberg_Vst_SpeakerArrangement *in,
                                                           Steinberg_int32 n_in,
                                                           Steinberg_Vst_SpeakerArrangement *out,
                                                           Steinberg_int32 n_out)
{
    if (n_in == 1 && n_out == 1 && in[0] == Steinberg_Vst_SpeakerArr_kStereo &&
        out[0] == Steinberg_Vst_SpeakerArr_kStereo)
        return Steinberg_kResultTrue;
    return Steinberg_kResultFalse;
}

static tresult SMTG_STDMETHODCALLTYPE get_bus_arrangement(void *self, Steinberg_Vst_BusDirection dir,
                                                          Steinberg_int32 index,
                                                          Steinberg_Vst_SpeakerArrangement *arr)
{
    if (index != 0)
        return Steinberg_kInvalidArgument;
    *arr = Steinberg_Vst_SpeakerArr_kStereo;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE can_process_sample_size(void *self, Steinberg_int32 size)
{
    return size == Steinberg_Vst_SymbolicSampleSizes_kSample32 ? Steinberg_kResultTrue : Steinberg_kResultFalse;
}

static uint32_t SMTG_STDMETHODCALLTYPE get_latency_samples(void *self)
{
    return TW_LATENCY;
}

static tresult SMTG_STDMETHODCALLTYPE setup_processing(void *self, struct Steinberg_Vst_ProcessSetup *setup)
{
    Plugin *p = FROM(self, Plugin, processor);
    if (setup->symbolicSampleSize != Steinberg_Vst_SymbolicSampleSizes_kSample32)
        return Steinberg_kResultFalse;
    p->sample_rate = setup->sampleRate;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE set_processing(void *self, Steinberg_TBool state)
{
    return Steinberg_kResultOk;
}

/* Applies every queued point at or before `upto`; returns the offset of the next one left */
static int apply_points(Plugin *p, struct Steinberg_Vst_IParamValueQueue **queue, int *pos, int n_queues,
                        int upto)
{
    int next = INT_MAX;
    for (int q = 0; q < n_queues; q++) {
        Steinberg_Vst_ParamID id = queue[q]->lpVtbl->getParameterId(queue[q]);
        int n_points = queue[q]->lpVtbl->getPointCount(queue[q]);
        for (; pos[q] < n_points; pos[q]++) {
            Steinberg_int32 offset;
            Steinberg_Vst_ParamValue v;
            queue[q]->lpVtbl->getPoint(queue[q], pos[q], &offset, &v);
            if (offset > upto) {
                if (offset < next)
                    next = offset;
                break;
            }
            if (id < N_PARAMS)
                atomic_store(&p->value[id], (float)to_plain(id, v));
        }
    }
    return next;
}

/* Splits the block at each parameter point so automation is sample accurate */
static tresult SMTG_STDMETHODCALLTYPE process(void *self, struct Steinberg_Vst_ProcessData *d)
{
    Plugin *p = FROM(self, Plugin, processor);
    struct Steinberg_Vst_IParameterChanges *changes = d->inputParameterChanges;
    struct Steinberg_Vst_IParamValueQueue *queue[MAX_QUEUES];
    int pos[MAX_QUEUES] = {0};
    int n_queues = changes ? changes->lpVtbl->getParameterCount(changes) : 0;
    if (n_queues > MAX_QUEUES)
        n_queues = MAX_QUEUES;
    for (int q = 0; q < n_queues; q++)
        queue[q] = changes->lpVtbl->getParameterData(changes, q);

    const int n = d->numSamples;
    float **in = NULL, **out = NULL;
    if (p->dsp && n > 0 && d->numInputs > 0 && d->numOutputs > 0) {
        in = d->inputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32;
        out = d->outputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32;
        d->outputs[0].silenceFlags = 0;
    }
    for (int done = 0;;) {
        int next = apply_points(p, queue, pos, n_queues, done);
        if (next > n)
            next = n;
        if (out && next > done) {
            apply_params(p);
            tw_process(p->dsp, in[0] + done, in[1] + done, out[0] + done, out[1] + done, next - done);
        }
        if (next >= n)
            break;
        done = next;
    }
    apply_points(p, queue, pos, n_queues, INT_MAX);
    return Steinberg_kResultOk;
}

static uint32_t SMTG_STDMETHODCALLTYPE get_tail_samples(void *self)
{
    return TW_LATENCY + TW_FFT_SIZE; /* last overlap-add frame */
}

static Steinberg_Vst_IAudioProcessorVtbl processor_vtbl = {
    .queryInterface = proc_query,
    .addRef = proc_add_ref,
    .release = proc_release,
    .setBusArrangements = set_bus_arrangements,
    .getBusArrangement = get_bus_arrangement,
    .canProcessSampleSize = can_process_sample_size,
    .getLatencySamples = get_latency_samples,
    .setupProcessing = setup_processing,
    .setProcessing = set_processing,
    .process = process,
    .getTailSamples = get_tail_samples,
};

/* ---- IPlugView: the X11 pedal, polled from the host's run loop timer ---- */

typedef struct {
    Steinberg_IPlugView view;
    Steinberg_Linux_ITimerHandler timer;
    atomic_uint refs;
    Plugin *plugin;
    struct Steinberg_IPlugFrame *frame;
    Steinberg_Linux_IRunLoop *run_loop;
    TwGui *gui;
} View;

static float gui_get(void *ctx, int param)
{
    Plugin *p = ctx;
    return atomic_load(&p->value[param]);
}

static void gui_begin(void *ctx, int param)
{
    Plugin *p = ctx;
    if (p->handler)
        p->handler->lpVtbl->beginEdit(p->handler, param);
}

static void gui_set(void *ctx, int param, float value)
{
    Plugin *p = ctx;
    atomic_store(&p->value[param], value);
    if (p->handler)
        p->handler->lpVtbl->performEdit(p->handler, param, to_normalized(param, value));
}

static void gui_end(void *ctx, int param)
{
    Plugin *p = ctx;
    if (p->handler)
        p->handler->lpVtbl->endEdit(p->handler, param);
}

static const TwGuiCallbacks gui_callbacks = {gui_get, gui_begin, gui_set, gui_end};

static uint32_t view_add_ref(View *v)
{
    return atomic_fetch_add(&v->refs, 1) + 1;
}

static uint32_t view_release(View *v)
{
    uint32_t left = atomic_fetch_sub(&v->refs, 1) - 1;
    if (left == 0) {
        if (v->gui)
            tw_gui_destroy(v->gui);
        plugin_release(v->plugin);
        free(v);
    }
    return left;
}

static tresult view_query(View *v, const Steinberg_TUID iid, void **obj)
{
    if (!memcmp(iid, Steinberg_FUnknown_iid, 16) || !memcmp(iid, Steinberg_IPlugView_iid, 16))
        *obj = &v->view;
    else if (!memcmp(iid, Steinberg_Linux_ITimerHandler_iid, 16))
        *obj = &v->timer;
    else {
        *obj = NULL;
        return Steinberg_kNoInterface;
    }
    view_add_ref(v);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE pv_query(void *self, const Steinberg_TUID iid, void **obj)
{
    return view_query(FROM(self, View, view), iid, obj);
}

static uint32_t SMTG_STDMETHODCALLTYPE pv_add_ref(void *self)
{
    return view_add_ref(FROM(self, View, view));
}

static uint32_t SMTG_STDMETHODCALLTYPE pv_release(void *self)
{
    return view_release(FROM(self, View, view));
}

static tresult SMTG_STDMETHODCALLTYPE is_platform_type_supported(void *self, Steinberg_FIDString type)
{
    return strcmp(type, GUI_PLATFORM) == 0 ? Steinberg_kResultTrue : Steinberg_kResultFalse;
}

static tresult SMTG_STDMETHODCALLTYPE attached(void *self, void *parent, Steinberg_FIDString type)
{
    View *v = FROM(self, View, view);
    if (is_platform_type_supported(self, type) != Steinberg_kResultTrue)
        return Steinberg_kResultFalse;
#ifdef TW_GUI_NEEDS_HOST_TIMER
    if (!v->frame ||
        v->frame->lpVtbl->queryInterface(v->frame, Steinberg_Linux_IRunLoop_iid, (void **)&v->run_loop) !=
            Steinberg_kResultOk)
        return Steinberg_kResultFalse;
#endif
    v->gui = tw_gui_create(parent, &gui_callbacks, v->plugin);
    if (!v->gui) {
        if (v->run_loop)
            v->run_loop->lpVtbl->release(v->run_loop);
        v->run_loop = NULL;
        return Steinberg_kResultFalse;
    }
    tw_gui_show(v->gui, 1);
    if (v->run_loop)
        v->run_loop->lpVtbl->registerTimer(v->run_loop, &v->timer, GUI_TIMER_MS);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE removed(void *self)
{
    View *v = FROM(self, View, view);
    if (v->run_loop) {
        v->run_loop->lpVtbl->unregisterTimer(v->run_loop, &v->timer);
        v->run_loop->lpVtbl->release(v->run_loop);
        v->run_loop = NULL;
    }
    if (v->gui)
        tw_gui_destroy(v->gui);
    v->gui = NULL;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE on_wheel(void *self, float distance)
{
    return Steinberg_kResultFalse;
}

static tresult SMTG_STDMETHODCALLTYPE on_key(void *self, Steinberg_char16 key, Steinberg_int16 code,
                                             Steinberg_int16 modifiers)
{
    return Steinberg_kResultFalse;
}

static tresult SMTG_STDMETHODCALLTYPE get_size(void *self, struct Steinberg_ViewRect *r)
{
    *r = (struct Steinberg_ViewRect){0, 0, TW_UI_W, TW_UI_H};
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE on_size(void *self, struct Steinberg_ViewRect *r)
{
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE on_focus(void *self, Steinberg_TBool state)
{
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE set_frame(void *self, struct Steinberg_IPlugFrame *frame)
{
    FROM(self, View, view)->frame = frame;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE can_resize(void *self)
{
    return Steinberg_kResultFalse;
}

static tresult SMTG_STDMETHODCALLTYPE check_size_constraint(void *self, struct Steinberg_ViewRect *r)
{
    return get_size(self, r);
}

static Steinberg_IPlugViewVtbl view_vtbl = {
    .queryInterface = pv_query,
    .addRef = pv_add_ref,
    .release = pv_release,
    .isPlatformTypeSupported = is_platform_type_supported,
    .attached = attached,
    .removed = removed,
    .onWheel = on_wheel,
    .onKeyDown = on_key,
    .onKeyUp = on_key,
    .getSize = get_size,
    .onSize = on_size,
    .onFocus = on_focus,
    .setFrame = set_frame,
    .canResize = can_resize,
    .checkSizeConstraint = check_size_constraint,
};

static tresult SMTG_STDMETHODCALLTYPE timer_query(void *self, const Steinberg_TUID iid, void **obj)
{
    return view_query(FROM(self, View, timer), iid, obj);
}

static uint32_t SMTG_STDMETHODCALLTYPE timer_add_ref(void *self)
{
    return view_add_ref(FROM(self, View, timer));
}

static uint32_t SMTG_STDMETHODCALLTYPE timer_release(void *self)
{
    return view_release(FROM(self, View, timer));
}

static void SMTG_STDMETHODCALLTYPE on_timer(void *self)
{
    View *v = FROM(self, View, timer);
    if (v->gui)
        tw_gui_idle(v->gui);
}

static Steinberg_Linux_ITimerHandlerVtbl timer_vtbl = {
    .queryInterface = timer_query,
    .addRef = timer_add_ref,
    .release = timer_release,
    .onTimer = on_timer,
};

/* ---- IEditController ---- */

static tresult SMTG_STDMETHODCALLTYPE ctrl_query(void *self, const Steinberg_TUID iid, void **obj)
{
    return plugin_query(FROM(self, Plugin, controller), iid, obj);
}

static uint32_t SMTG_STDMETHODCALLTYPE ctrl_add_ref(void *self)
{
    return plugin_add_ref(FROM(self, Plugin, controller));
}

static uint32_t SMTG_STDMETHODCALLTYPE ctrl_release(void *self)
{
    return plugin_release(FROM(self, Plugin, controller));
}

static tresult SMTG_STDMETHODCALLTYPE set_component_state(void *self, struct Steinberg_IBStream *s)
{
    return state_load(FROM(self, Plugin, controller), s);
}

/* The controller has no state of its own */
static tresult SMTG_STDMETHODCALLTYPE ctrl_state(void *self, struct Steinberg_IBStream *s)
{
    return Steinberg_kResultOk;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE get_parameter_count(void *self)
{
    return N_PARAMS;
}

static tresult SMTG_STDMETHODCALLTYPE get_parameter_info(void *self, Steinberg_int32 index,
                                                         struct Steinberg_Vst_ParameterInfo *info)
{
    if (index < 0 || index >= N_PARAMS)
        return Steinberg_kInvalidArgument;
    double min, max, def;
    param_range(index, &min, &max, &def);
    char name[32];
    if (index < TW_P_COUNT)
        snprintf(name, sizeof name, "%s", tw_param_info[index].name);
    else
        snprintf(name, sizeof name, "EQ %g Hz", tw_eq_freqs[index - TW_P_COUNT]);
    memset(info, 0, sizeof *info);
    info->id = index;
    to_string128(info->title, name);
    to_string128(info->shortTitle, name);
    to_string128(info->units, index == TW_P_TREM_SPEED ? "BPM" : index >= TW_P_COUNT ? "dB" : "");
    info->stepCount = is_switch(index) ? 1 : 0;
    info->defaultNormalizedValue = to_normalized(index, def);
    info->unitId = Steinberg_Vst_kRootUnitId;
    info->flags = Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate;
    if (index == TW_P_BYPASS)
        info->flags |= Steinberg_Vst_ParameterInfo_ParameterFlags_kIsBypass;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE get_param_string_by_value(void *self, Steinberg_Vst_ParamID id,
                                                                Steinberg_Vst_ParamValue normalized,
                                                                Steinberg_Vst_String128 string)
{
    if (id >= N_PARAMS)
        return Steinberg_kInvalidArgument;
    double v = to_plain(id, normalized);
    char text[32];
    if (is_switch(id))
        snprintf(text, sizeof text, "%s", v > 0.5 ? "On" : "Off");
    else if (id == TW_P_TREM_SPEED)
        snprintf(text, sizeof text, "%.0f BPM", v);
    else if (id >= TW_P_COUNT)
        snprintf(text, sizeof text, "%.1f dB", v);
    else
        snprintf(text, sizeof text, "%.1f", v);
    to_string128(string, text);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE get_param_value_by_string(void *self, Steinberg_Vst_ParamID id,
                                                                Steinberg_Vst_TChar *string,
                                                                Steinberg_Vst_ParamValue *normalized)
{
    if (id >= N_PARAMS)
        return Steinberg_kInvalidArgument;
    char text[32];
    int i = 0;
    for (; string[i] && i < 31; i++)
        text[i] = (char)string[i];
    text[i] = 0;
    double min, max, def, v;
    param_range(id, &min, &max, &def);
    if (is_switch(id))
        v = strcmp(text, "On") == 0 || atof(text) > 0.5;
    else
        v = atof(text);
    *normalized = to_normalized(id, v < min ? min : v > max ? max : v);
    return Steinberg_kResultOk;
}

static Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE normalized_to_plain(void *self, Steinberg_Vst_ParamID id,
                                                                           Steinberg_Vst_ParamValue v)
{
    return id < N_PARAMS ? to_plain(id, v) : 0;
}

static Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE plain_to_normalized(void *self, Steinberg_Vst_ParamID id,
                                                                           Steinberg_Vst_ParamValue v)
{
    return id < N_PARAMS ? to_normalized(id, v) : 0;
}

static Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE get_param_normalized(void *self, Steinberg_Vst_ParamID id)
{
    Plugin *p = FROM(self, Plugin, controller);
    return id < N_PARAMS ? to_normalized(id, atomic_load(&p->value[id])) : 0;
}

static tresult SMTG_STDMETHODCALLTYPE set_param_normalized(void *self, Steinberg_Vst_ParamID id,
                                                           Steinberg_Vst_ParamValue v)
{
    Plugin *p = FROM(self, Plugin, controller);
    if (id >= N_PARAMS)
        return Steinberg_kInvalidArgument;
    atomic_store(&p->value[id], (float)to_plain(id, v));
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE set_component_handler(void *self,
                                                            struct Steinberg_Vst_IComponentHandler *handler)
{
    Plugin *p = FROM(self, Plugin, controller);
    if (handler)
        handler->lpVtbl->addRef(handler);
    if (p->handler)
        p->handler->lpVtbl->release(p->handler);
    p->handler = handler;
    return Steinberg_kResultOk;
}

static struct Steinberg_IPlugView *SMTG_STDMETHODCALLTYPE create_view(void *self, Steinberg_FIDString name)
{
    Plugin *p = FROM(self, Plugin, controller);
    if (!name || strcmp(name, "editor") != 0)
        return NULL;
    View *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->view.lpVtbl = &view_vtbl;
    v->timer.lpVtbl = &timer_vtbl;
    atomic_init(&v->refs, 1);
    v->plugin = p;
    plugin_add_ref(p);
    return &v->view;
}

static Steinberg_Vst_IEditControllerVtbl controller_vtbl = {
    .queryInterface = ctrl_query,
    .addRef = ctrl_add_ref,
    .release = ctrl_release,
    .initialize = initialize,
    .terminate = terminate,
    .setComponentState = set_component_state,
    .setState = ctrl_state,
    .getState = ctrl_state,
    .getParameterCount = get_parameter_count,
    .getParameterInfo = get_parameter_info,
    .getParamStringByValue = get_param_string_by_value,
    .getParamValueByString = get_param_value_by_string,
    .normalizedParamToPlain = normalized_to_plain,
    .plainParamToNormalized = plain_to_normalized,
    .getParamNormalized = get_param_normalized,
    .setParamNormalized = set_param_normalized,
    .setComponentHandler = set_component_handler,
    .createView = create_view,
};

/* ---- factory and module entry ---- */

static void class_info(struct Steinberg_PClassInfo *info)
{
    memcpy(info->cid, class_id, sizeof class_id);
    info->cardinality = Steinberg_PClassInfo_ClassCardinality_kManyInstances;
    snprintf(info->category, sizeof info->category, "Audio Module Class");
    snprintf(info->name, sizeof info->name, "TritoneWorship");
}

static tresult SMTG_STDMETHODCALLTYPE factory_query(void *self, const Steinberg_TUID iid, void **obj)
{
    if (!memcmp(iid, Steinberg_FUnknown_iid, 16) || !memcmp(iid, Steinberg_IPluginFactory_iid, 16) ||
        !memcmp(iid, Steinberg_IPluginFactory2_iid, 16)) {
        *obj = self;
        return Steinberg_kResultOk;
    }
    *obj = NULL;
    return Steinberg_kNoInterface;
}

/* The factory is static, so reference counts do nothing */
static uint32_t SMTG_STDMETHODCALLTYPE factory_ref(void *self)
{
    return 1;
}

static tresult SMTG_STDMETHODCALLTYPE get_factory_info(void *self, struct Steinberg_PFactoryInfo *info)
{
    memset(info, 0, sizeof *info);
    snprintf(info->vendor, sizeof info->vendor, VENDOR);
    info->flags = Steinberg_PFactoryInfo_FactoryFlags_kUnicode;
    return Steinberg_kResultOk;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE count_classes(void *self)
{
    return 1;
}

static tresult SMTG_STDMETHODCALLTYPE get_class_info(void *self, Steinberg_int32 index,
                                                     struct Steinberg_PClassInfo *info)
{
    if (index != 0)
        return Steinberg_kInvalidArgument;
    class_info(info);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE get_class_info2(void *self, Steinberg_int32 index,
                                                      struct Steinberg_PClassInfo2 *info)
{
    if (index != 0)
        return Steinberg_kInvalidArgument;
    struct Steinberg_PClassInfo base;
    class_info(&base);
    memset(info, 0, sizeof *info);
    memcpy(info->cid, base.cid, sizeof base.cid);
    info->cardinality = base.cardinality;
    memcpy(info->category, base.category, sizeof base.category);
    memcpy(info->name, base.name, sizeof base.name);
    snprintf(info->subCategories, sizeof info->subCategories, "Fx|Distortion|Pitch Shift");
    snprintf(info->vendor, sizeof info->vendor, VENDOR);
    snprintf(info->version, sizeof info->version, "0.1.0");
    snprintf(info->sdkVersion, sizeof info->sdkVersion, "%s", Steinberg_Vst_SDKVersionString);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE create_instance(void *self, Steinberg_FIDString cid, Steinberg_FIDString iid,
                                                      void **obj)
{
    *obj = NULL;
    if (memcmp(cid, class_id, sizeof class_id) != 0)
        return Steinberg_kInvalidArgument;
    Plugin *p = calloc(1, sizeof *p);
    if (!p)
        return Steinberg_kOutOfMemory;
    p->component.lpVtbl = &component_vtbl;
    p->processor.lpVtbl = &processor_vtbl;
    p->controller.lpVtbl = &controller_vtbl;
    atomic_init(&p->refs, 1);
    p->sample_rate = 48000;
    for (Steinberg_Vst_ParamID i = 0; i < N_PARAMS; i++) {
        double min, max, def;
        param_range(i, &min, &max, &def);
        atomic_init(&p->value[i], (float)def);
    }
    tresult r = plugin_query(p, iid, obj);
    plugin_release(p);
    return r;
}

static Steinberg_IPluginFactory2Vtbl factory_vtbl = {
    .queryInterface = factory_query,
    .addRef = factory_ref,
    .release = factory_ref,
    .getFactoryInfo = get_factory_info,
    .countClasses = count_classes,
    .getClassInfo = get_class_info,
    .createInstance = create_instance,
    .getClassInfo2 = get_class_info2,
};

static Steinberg_IPluginFactory2 factory = {&factory_vtbl};

/* Each platform's module entry points; there is nothing to set up */
#if defined(_WIN32)
EXPORT bool InitDll(void)
{
    return true;
}

EXPORT bool ExitDll(void)
{
    return true;
}
#elif defined(__APPLE__)
EXPORT bool bundleEntry(void *bundle)
{
    return true;
}

EXPORT bool bundleExit(void)
{
    return true;
}
#else
EXPORT bool ModuleEntry(void *handle)
{
    return true;
}

EXPORT bool ModuleExit(void)
{
    return true;
}
#endif

EXPORT Steinberg_IPluginFactory *GetPluginFactory(void)
{
    return (Steinberg_IPluginFactory *)&factory;
}
