/* Loads the built .vst3 like a host would and checks it end to end */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vst3_c_api.h>

#include "test_host.h"
#include "tw_dsp.h"
#include "tw_gui.h"
#include "tw_ui.h"

#if defined(_WIN32)
#define GUI_PLATFORM Steinberg_kPlatformTypeHWND
#define MODULE_ENTRY "InitDll"
#define MODULE_EXIT "ExitDll"
#elif defined(__APPLE__)
#define GUI_PLATFORM Steinberg_kPlatformTypeNSView
#define MODULE_ENTRY "bundleEntry"
#define MODULE_EXIT "bundleExit"
#else
#define GUI_PLATFORM Steinberg_kPlatformTypeX11EmbedWindowID
#define MODULE_ENTRY "ModuleEntry"
#define MODULE_EXIT "ModuleExit"
#endif

#define SR 48000
#define BLOCK 256

typedef Steinberg_tresult tresult;

static int failures;

#define CHECK(cond) check(__func__, #cond, cond)

static void check(const char *test, const char *what, int ok)
{
    if (!ok) {
        printf("FAIL %s: %s\n", test, what);
        failures++;
    }
}

/* Host objects in this file are static, so their reference counts do nothing */
static uint32_t SMTG_STDMETHODCALLTYPE no_ref(void *self)
{
    return 1;
}

static tresult SMTG_STDMETHODCALLTYPE no_query(void *self, const Steinberg_TUID iid, void **obj)
{
    *obj = NULL;
    return Steinberg_kNoInterface;
}

/* ---- parameter changes: one queue per parameter ---- */

typedef struct {
    Steinberg_Vst_IParamValueQueue iface;
    Steinberg_Vst_ParamID id;
    Steinberg_int32 offset[8];
    double value[8];
    int n;
} Queue;

typedef struct {
    Steinberg_Vst_IParameterChanges iface;
    Queue queue[8];
    int n;
} Changes;

static Steinberg_Vst_ParamID SMTG_STDMETHODCALLTYPE queue_id(void *self)
{
    return ((Queue *)self)->id;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE queue_count(void *self)
{
    return ((Queue *)self)->n;
}

static tresult SMTG_STDMETHODCALLTYPE queue_get(void *self, Steinberg_int32 i, Steinberg_int32 *offset,
                                                Steinberg_Vst_ParamValue *value)
{
    Queue *q = self;
    *offset = q->offset[i];
    *value = q->value[i];
    return Steinberg_kResultOk;
}

static Steinberg_Vst_IParamValueQueueVtbl queue_vtbl = {no_query, no_ref, no_ref, queue_id, queue_count, queue_get};

static Steinberg_int32 SMTG_STDMETHODCALLTYPE changes_count(void *self)
{
    return ((Changes *)self)->n;
}

static struct Steinberg_Vst_IParamValueQueue *SMTG_STDMETHODCALLTYPE changes_get(void *self, Steinberg_int32 i)
{
    return &((Changes *)self)->queue[i].iface;
}

static Steinberg_Vst_IParameterChangesVtbl changes_vtbl = {no_query, no_ref, no_ref, changes_count, changes_get};

static void add_point(Changes *c, Steinberg_Vst_ParamID id, int offset, double normalized)
{
    c->iface.lpVtbl = &changes_vtbl;
    Queue *q = &c->queue[c->n++];
    *q = (Queue){{&queue_vtbl}, id, {offset}, {normalized}, 1};
}

/* ---- memory stream ---- */

typedef struct {
    Steinberg_IBStream iface;
    char data[4096];
    int len, pos;
} Stream;

static tresult SMTG_STDMETHODCALLTYPE stream_read(void *self, void *buf, Steinberg_int32 size, Steinberg_int32 *read)
{
    Stream *s = self;
    int n = s->len - s->pos < size ? s->len - s->pos : size;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    *read = n;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE stream_write(void *self, void *buf, Steinberg_int32 size,
                                                   Steinberg_int32 *written)
{
    Stream *s = self;
    memcpy(s->data + s->len, buf, size);
    s->len += size;
    *written = size;
    return Steinberg_kResultOk;
}

static Steinberg_IBStreamVtbl stream_vtbl = {no_query, no_ref, no_ref, stream_read, stream_write};

/* ---- component handler: records the GUI's edits ---- */

enum { BEGIN, PERFORM, END };

static struct {
    int type[16];
    Steinberg_Vst_ParamID id[16];
    double value[16];
    int n;
} edits;

static void record(int type, Steinberg_Vst_ParamID id, double value)
{
    edits.type[edits.n] = type;
    edits.id[edits.n] = id;
    edits.value[edits.n] = value;
    edits.n++;
}

static tresult SMTG_STDMETHODCALLTYPE begin_edit(void *self, Steinberg_Vst_ParamID id)
{
    record(BEGIN, id, 0);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE perform_edit(void *self, Steinberg_Vst_ParamID id, Steinberg_Vst_ParamValue v)
{
    record(PERFORM, id, v);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE end_edit(void *self, Steinberg_Vst_ParamID id)
{
    record(END, id, 0);
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE restart_component(void *self, Steinberg_int32 flags)
{
    return Steinberg_kResultOk;
}

static Steinberg_Vst_IComponentHandlerVtbl handler_vtbl = {no_query, no_ref, no_ref, begin_edit, perform_edit,
                                                           end_edit, restart_component};
static Steinberg_Vst_IComponentHandler handler = {&handler_vtbl};

/* ---- plug frame with a run loop that remembers the plugin's timer ---- */

static Steinberg_Linux_ITimerHandler *timer;

static tresult SMTG_STDMETHODCALLTYPE register_timer(void *self, struct Steinberg_Linux_ITimerHandler *h,
                                                     Steinberg_Linux_TimerInterval ms)
{
    timer = h;
    return Steinberg_kResultOk;
}

static tresult SMTG_STDMETHODCALLTYPE unregister_timer(void *self, struct Steinberg_Linux_ITimerHandler *h)
{
    timer = NULL;
    return Steinberg_kResultOk;
}

static Steinberg_Linux_IRunLoopVtbl run_loop_vtbl = {no_query, no_ref, no_ref, NULL, NULL, register_timer,
                                                     unregister_timer};
static Steinberg_Linux_IRunLoop run_loop = {&run_loop_vtbl};

static tresult SMTG_STDMETHODCALLTYPE frame_query(void *self, const Steinberg_TUID iid, void **obj)
{
    if (!memcmp(iid, Steinberg_Linux_IRunLoop_iid, 16)) {
        *obj = &run_loop;
        return Steinberg_kResultOk;
    }
    return no_query(self, iid, obj);
}

static tresult SMTG_STDMETHODCALLTYPE resize_view(void *self, struct Steinberg_IPlugView *v,
                                                  struct Steinberg_ViewRect *r)
{
    return Steinberg_kResultOk;
}

static Steinberg_IPlugFrameVtbl frame_vtbl = {frame_query, no_ref, no_ref, resize_view};
static Steinberg_IPlugFrame frame = {&frame_vtbl};

/* ---- plugin helpers ---- */

static Steinberg_IPluginFactory2 *factory;
static Steinberg_TUID class_id;

typedef struct {
    Steinberg_Vst_IComponent *comp;
    Steinberg_Vst_IAudioProcessor *proc;
    Steinberg_Vst_IEditController *ctrl;
} Plugin;

static Plugin create_plugin(void)
{
    Plugin p;
    factory->lpVtbl->createInstance(factory, class_id, Steinberg_Vst_IComponent_iid, (void **)&p.comp);
    p.comp->lpVtbl->initialize(p.comp, NULL);
    p.comp->lpVtbl->queryInterface(p.comp, Steinberg_Vst_IAudioProcessor_iid, (void **)&p.proc);
    p.comp->lpVtbl->queryInterface(p.comp, Steinberg_Vst_IEditController_iid, (void **)&p.ctrl);
    return p;
}

static void destroy_plugin(Plugin p)
{
    p.ctrl->lpVtbl->release(p.ctrl);
    p.proc->lpVtbl->release(p.proc);
    p.comp->lpVtbl->terminate(p.comp);
    p.comp->lpVtbl->release(p.comp);
}

static void activate(Plugin p)
{
    struct Steinberg_Vst_ProcessSetup setup = {Steinberg_Vst_ProcessModes_kRealtime,
                                               Steinberg_Vst_SymbolicSampleSizes_kSample32, BLOCK, SR};
    p.proc->lpVtbl->setupProcessing(p.proc, &setup);
    p.comp->lpVtbl->setActive(p.comp, 1);
    p.proc->lpVtbl->setProcessing(p.proc, 1);
}

static void deactivate(Plugin p)
{
    p.proc->lpVtbl->setProcessing(p.proc, 0);
    p.comp->lpVtbl->setActive(p.comp, 0);
}

static double plain(Plugin p, Steinberg_Vst_ParamID id)
{
    return p.ctrl->lpVtbl->normalizedParamToPlain(p.ctrl, id, p.ctrl->lpVtbl->getParamNormalized(p.ctrl, id));
}

static void set_plain(Plugin p, Steinberg_Vst_ParamID id, double v)
{
    p.ctrl->lpVtbl->setParamNormalized(p.ctrl, id, p.ctrl->lpVtbl->plainParamToNormalized(p.ctrl, id, v));
}

static float sine(uint64_t i)
{
    return 0.1f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / SR);
}

/* Processes one block of a stereo sine starting at sample `start`; returns the left output */
static const float *process_block(Plugin p, uint64_t start, Changes *changes)
{
    static float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
    for (int i = 0; i < BLOCK; i++)
        l[i] = r[i] = sine(start + i);
    float *in_ch[2] = {l, r}, *out_ch[2] = {ol, orr};
    struct Steinberg_Vst_AudioBusBuffers in = {2, 0, {in_ch}}, out = {2, 0, {out_ch}};
    struct Steinberg_Vst_ProcessData d = {
        .processMode = Steinberg_Vst_ProcessModes_kRealtime,
        .symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32,
        .numSamples = BLOCK, .numInputs = 1, .numOutputs = 1, .inputs = &in, .outputs = &out,
        .inputParameterChanges = changes ? &changes->iface : NULL,
    };
    p.proc->lpVtbl->process(p.proc, &d);
    return ol;
}

static void ascii(char *dst, const Steinberg_char16 *src)
{
    int i = 0;
    for (; src[i]; i++)
        dst[i] = (char)src[i];
    dst[i] = 0;
}

/* ---- tests ---- */

static void test_factory_offers_one_audio_effect(void)
{
    CHECK(factory->lpVtbl->countClasses(factory) == 1);
    struct Steinberg_PClassInfo2 info;
    CHECK(factory->lpVtbl->getClassInfo2(factory, 0, &info) == Steinberg_kResultOk);
    CHECK(!strcmp(info.category, "Audio Module Class"));
    CHECK(!strcmp(info.name, "TritoneWorship"));
    CHECK(!strncmp(info.subCategories, "Fx", 2));
}

static void test_component_is_its_own_controller(void)
{
    Plugin p = create_plugin();
    Steinberg_TUID unused;
    CHECK(p.comp->lpVtbl->getControllerClassId(p.comp, unused) != Steinberg_kResultOk);
    CHECK(p.ctrl != NULL);
    destroy_plugin(p);
}

static void test_params_match_the_dsp_parameter_table(void)
{
    Plugin p = create_plugin();
#ifdef TW_EQ_TUNING
    CHECK(p.ctrl->lpVtbl->getParameterCount(p.ctrl) == TW_P_COUNT + TW_EQ_BANDS);
#else
    CHECK(p.ctrl->lpVtbl->getParameterCount(p.ctrl) == TW_P_COUNT);
#endif
    for (int i = 0; i < TW_P_COUNT; i++) {
        struct Steinberg_Vst_ParameterInfo info;
        char title[128];
        CHECK(p.ctrl->lpVtbl->getParameterInfo(p.ctrl, i, &info) == Steinberg_kResultOk);
        ascii(title, info.title);
        CHECK(!strcmp(title, tw_param_info[i].name));
        CHECK(plain(p, i) == tw_param_info[i].def);
        CHECK(p.ctrl->lpVtbl->normalizedParamToPlain(p.ctrl, i, info.defaultNormalizedValue) == tw_param_info[i].def);
        CHECK(p.ctrl->lpVtbl->normalizedParamToPlain(p.ctrl, i, 0) == tw_param_info[i].min);
        CHECK(p.ctrl->lpVtbl->normalizedParamToPlain(p.ctrl, i, 1) == tw_param_info[i].max);
        CHECK(info.stepCount == (i == TW_P_BLACKEN || i == TW_P_BYPASS));
        CHECK(!!(info.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsBypass) == (i == TW_P_BYPASS));
    }
    destroy_plugin(p);
}

static void test_param_values_display_as_text(void)
{
    Plugin p = create_plugin();
    Steinberg_Vst_String128 s;
    char text[128];
    p.ctrl->lpVtbl->getParamStringByValue(p.ctrl, TW_P_TREM_SPEED, 0.5, s);
    ascii(text, s);
    CHECK(!strcmp(text, "1166 BPM")); /* halfway through 666-1666 */
    p.ctrl->lpVtbl->getParamStringByValue(p.ctrl, TW_P_BLACKEN, 1, s);
    ascii(text, s);
    CHECK(!strcmp(text, "On"));

    Steinberg_Vst_ParamValue v;
    Steinberg_Vst_TChar typed[] = {'7', '5', 0};
    p.ctrl->lpVtbl->getParamValueByString(p.ctrl, TW_P_AMOUNT, typed, &v);
    CHECK(v == 0.75);
    destroy_plugin(p);
}

static void test_only_stereo_32_bit_is_offered(void)
{
    Plugin p = create_plugin();
    Steinberg_Vst_SpeakerArrangement stereo = Steinberg_Vst_SpeakerArr_kStereo, mono = Steinberg_Vst_kSpeakerL;
    CHECK(p.proc->lpVtbl->setBusArrangements(p.proc, &stereo, 1, &stereo, 1) == Steinberg_kResultTrue);
    CHECK(p.proc->lpVtbl->setBusArrangements(p.proc, &mono, 1, &mono, 1) == Steinberg_kResultFalse);
    CHECK(p.proc->lpVtbl->canProcessSampleSize(p.proc, Steinberg_Vst_SymbolicSampleSizes_kSample32) ==
          Steinberg_kResultTrue);
    CHECK(p.proc->lpVtbl->canProcessSampleSize(p.proc, Steinberg_Vst_SymbolicSampleSizes_kSample64) ==
          Steinberg_kResultFalse);
    destroy_plugin(p);
}

static void test_reports_latency_for_delay_compensation(void)
{
    Plugin p = create_plugin();
    activate(p);
    CHECK(p.proc->lpVtbl->getLatencySamples(p.proc) == TW_LATENCY);
    deactivate(p);
    destroy_plugin(p);
}

static void test_automation_applies_at_its_exact_sample(void)
{
    Plugin p = create_plugin();
    set_plain(p, TW_P_BYPASS, 1);
    activate(p);
    for (uint64_t t = 0; t < 8 * BLOCK; t += BLOCK)
        process_block(p, t, NULL);

    Changes c = {0};
    add_point(&c, TW_P_BYPASS, 128, 0);
    const float *y = process_block(p, 8 * BLOCK, &c);
    int untouched_before = 1;
    for (int i = 0; i < 128; i++)
        untouched_before &= y[i] == sine(8 * BLOCK + i);
    CHECK(untouched_before);                /* still bypassed up to sample 127 */
    CHECK(y[129] != sine(8 * BLOCK + 129)); /* effect starts fading in at sample 128 */
    CHECK(plain(p, TW_P_BYPASS) == 0);
    deactivate(p);
    destroy_plugin(p);
}

static void test_state_round_trips_every_parameter(void)
{
    Plugin a = create_plugin();
    set_plain(a, TW_P_AMOUNT, 77);
    set_plain(a, TW_P_DEATH, 33);
    set_plain(a, TW_P_BLACKEN, 1);
    set_plain(a, TW_P_TREM_SPEED, 1234);
    Stream st = {{&stream_vtbl}};
    CHECK(a.comp->lpVtbl->getState(a.comp, &st.iface) == Steinberg_kResultOk);

    Plugin b = create_plugin();
    CHECK(b.comp->lpVtbl->setState(b.comp, &st.iface) == Steinberg_kResultOk);
    for (int i = 0; i < TW_P_COUNT; i++)
        CHECK(fabs(plain(b, i) - plain(a, i)) < 1e-3);
    CHECK(fabs(plain(b, TW_P_TREM_SPEED) - 1234) < 1e-3);
    destroy_plugin(a);
    destroy_plugin(b);
}

/* Fires the plugin's GUI timer until it has made n edits, or gives up after a second.
   Without a host timer the edits have already been made by the time the mouse event returns. */
static void pump_gui(int n)
{
    for (int tries = 0; tries < 100 && edits.n < n && timer; tries++) {
        timer->lpVtbl->onTimer(timer);
        sleep_ms(10);
    }
}

static void test_gui_knob_drag_and_footswitch_click_reach_the_host(void)
{
    if (!host_window_open()) {
        printf("SKIP %s: no window system to test with\n", __func__);
        return;
    }
    Plugin p = create_plugin();
    p.ctrl->lpVtbl->setComponentHandler(p.ctrl, &handler);
    Steinberg_IPlugView *view = p.ctrl->lpVtbl->createView(p.ctrl, "editor");
    CHECK(view->lpVtbl->isPlatformTypeSupported(view, GUI_PLATFORM) ==
          Steinberg_kResultTrue);
    struct Steinberg_ViewRect r;
    view->lpVtbl->getSize(view, &r);
    CHECK(r.right - r.left == TW_UI_W && r.bottom - r.top == TW_UI_H);
    view->lpVtbl->setFrame(view, &frame);
    CHECK(view->lpVtbl->attached(view, host_window_parent(), GUI_PLATFORM) == Steinberg_kResultOk);
#ifdef TW_GUI_NEEDS_HOST_TIMER
    CHECK(timer != NULL);
#endif
    int found = host_window_find_child();
    CHECK(found);
    if (!found)
        return;

    TwUiRect knob = tw_ui_rect(TW_P_AMOUNT);
    int kx = knob.x + knob.w / 2, ky = knob.y + knob.h / 2;
    edits.n = 0;
    host_mouse(MOUSE_DOWN, kx, ky);
    host_mouse(MOUSE_MOVE, kx, ky - TW_UI_DRAG_PIXELS / 4);
    host_mouse(MOUSE_UP, kx, ky - TW_UI_DRAG_PIXELS / 4);
    pump_gui(3);
    CHECK(edits.n == 3);
    CHECK(edits.type[0] == BEGIN && edits.id[0] == TW_P_AMOUNT);
    CHECK(edits.type[1] == PERFORM && edits.id[1] == TW_P_AMOUNT && edits.value[1] == 0.75);
    CHECK(edits.type[2] == END && edits.id[2] == TW_P_AMOUNT);
    CHECK(plain(p, TW_P_AMOUNT) == 75); /* dragged a quarter of the range up from 50 */

    TwUiRect sw = tw_ui_rect(TW_P_BYPASS);
    edits.n = 0;
    host_mouse(MOUSE_DOWN, sw.x + sw.w / 2, sw.y + sw.h / 2);
    pump_gui(3);
    CHECK(edits.n == 3 && edits.id[1] == TW_P_BYPASS && edits.value[1] == 1);
    CHECK(plain(p, TW_P_BYPASS) == 1);

    view->lpVtbl->removed(view);
    CHECK(timer == NULL);
    view->lpVtbl->release(view);
    p.ctrl->lpVtbl->setComponentHandler(p.ctrl, NULL);
    destroy_plugin(p);
    host_window_close();
}

int main(int argc, char **argv)
{
    void *lib = lib_open(argv[1]);
    if (!lib) {
        printf("FAIL cannot load %s\n", argv[1]);
        return 1;
    }
    /* InitDll takes no argument; the others take a module handle the plugin ignores */
    bool (*module_entry)(void *) = (bool (*)(void *))lib_symbol(lib, MODULE_ENTRY);
    bool (*module_exit)(void) = (bool (*)(void))lib_symbol(lib, MODULE_EXIT);
    Steinberg_IPluginFactory *(*get_factory)(void) = (Steinberg_IPluginFactory * (*)(void)) lib_symbol(lib, "GetPluginFactory");
    CHECK(module_entry && module_exit && get_factory);
    module_entry(lib);
    Steinberg_IPluginFactory *f = get_factory();
    f->lpVtbl->queryInterface(f, Steinberg_IPluginFactory2_iid, (void **)&factory);
    struct Steinberg_PClassInfo info;
    factory->lpVtbl->getClassInfo(factory, 0, &info);
    memcpy(class_id, info.cid, sizeof class_id);

    test_factory_offers_one_audio_effect();
    test_component_is_its_own_controller();
    test_params_match_the_dsp_parameter_table();
    test_param_values_display_as_text();
    test_only_stereo_32_bit_is_offered();
    test_reports_latency_for_delay_compensation();
    test_automation_applies_at_its_exact_sample();
    test_state_round_trips_every_parameter();
    test_gui_knob_drag_and_footswitch_click_reach_the_host();

    factory->lpVtbl->release(factory);
    f->lpVtbl->release(f);
    module_exit();
    printf(failures ? "%d check(s) failed\n" : "all VST3 tests passed\n", failures);
    return failures != 0;
}
