/* Loads the built .clap like a host would and checks it end to end */
#include <clap/clap.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "test_host.h"
#include "tw_dsp.h"
#include "tw_ui.h"

#if defined(_WIN32)
#define GUI_API CLAP_WINDOW_API_WIN32
#elif defined(__APPLE__)
#define GUI_API CLAP_WINDOW_API_COCOA
#else
#define GUI_API CLAP_WINDOW_API_X11
#endif

#define SR 48000
#define BLOCK 256

static int failures;

#define CHECK(cond) check(__func__, #cond, cond)

static void check(const char *test, const char *what, int ok)
{
    if (!ok) {
        printf("FAIL %s: %s\n", test, what);
        failures++;
    }
}

/* ---- a minimal host ---- */

static int flush_requested;

static bool host_register_timer(const clap_host_t *h, uint32_t ms, clap_id *id)
{
    *id = 7;
    return true;
}

static bool host_unregister_timer(const clap_host_t *h, clap_id id)
{
    return true;
}

static const clap_host_timer_support_t host_timer = {host_register_timer, host_unregister_timer};

static void host_rescan(const clap_host_t *h, clap_param_rescan_flags flags) {}
static void host_clear(const clap_host_t *h, clap_id id, clap_param_clear_flags flags) {}
static void host_request_flush(const clap_host_t *h) { flush_requested = 1; }

static const clap_host_params_t host_params = {host_rescan, host_clear, host_request_flush};

static const void *host_get_extension(const clap_host_t *h, const char *id)
{
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &host_timer;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &host_params;
    return NULL;
}

static void host_request(const clap_host_t *h) {}

static const clap_host_t host = {
    CLAP_VERSION_INIT, NULL, "test host", "", "", "1",
    host_get_extension, host_request, host_request, host_request,
};

/* ---- event lists ---- */

typedef struct {
    clap_event_param_value_t ev[16];
    uint32_t n;
} InEvents;

static uint32_t in_size(const clap_input_events_t *list)
{
    return ((InEvents *)list->ctx)->n;
}

static const clap_event_header_t *in_get(const clap_input_events_t *list, uint32_t i)
{
    return &((InEvents *)list->ctx)->ev[i].header;
}

static void add_param_event(InEvents *e, uint32_t time, clap_id id, double value)
{
    e->ev[e->n++] = (clap_event_param_value_t){
        .header = {sizeof(clap_event_param_value_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0},
        .param_id = id, .note_id = -1, .port_index = -1, .channel = -1, .key = -1, .value = value,
    };
}

typedef struct {
    uint16_t type[16];
    clap_id id[16];
    double value[16];
    uint32_t n;
} OutEvents;

static bool out_push(const clap_output_events_t *list, const clap_event_header_t *h)
{
    OutEvents *o = list->ctx;
    o->type[o->n] = h->type;
    if (h->type == CLAP_EVENT_PARAM_VALUE) {
        o->id[o->n] = ((const clap_event_param_value_t *)h)->param_id;
        o->value[o->n] = ((const clap_event_param_value_t *)h)->value;
    } else {
        o->id[o->n] = ((const clap_event_param_gesture_t *)h)->param_id;
    }
    o->n++;
    return true;
}

/* ---- plugin helpers ---- */

static const clap_plugin_factory_t *factory;

static const clap_plugin_t *create_plugin(void)
{
    const clap_plugin_t *p = factory->create_plugin(factory, &host, "org.tritoneworship.tritoneworship");
    p->init(p);
    return p;
}

static void flush(const clap_plugin_t *p, InEvents *in, OutEvents *out)
{
    const clap_plugin_params_t *params = p->get_extension(p, CLAP_EXT_PARAMS);
    clap_input_events_t in_list = {in, in_size, in_get};
    clap_output_events_t out_list = {out, out_push};
    params->flush(p, &in_list, &out_list);
}

static void set_param(const clap_plugin_t *p, clap_id id, double value)
{
    InEvents in = {0};
    OutEvents out = {0};
    add_param_event(&in, 0, id, value);
    flush(p, &in, &out);
}

static double get_param(const clap_plugin_t *p, clap_id id)
{
    const clap_plugin_params_t *params = p->get_extension(p, CLAP_EXT_PARAMS);
    double v = NAN;
    params->get_value(p, id, &v);
    return v;
}

static float sine(uint64_t i)
{
    return 0.1f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / SR);
}

/* Processes one block of a stereo sine starting at sample `start`; returns the left output */
static const float *process_block(const clap_plugin_t *p, uint64_t start, InEvents *events)
{
    static float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
    for (int i = 0; i < BLOCK; i++)
        l[i] = r[i] = sine(start + i);
    float *in_ch[2] = {l, r}, *out_ch[2] = {ol, orr};
    clap_audio_buffer_t in = {.data32 = in_ch, .channel_count = 2};
    clap_audio_buffer_t out = {.data32 = out_ch, .channel_count = 2};
    InEvents none = {0};
    OutEvents ignored = {0};
    clap_input_events_t in_list = {events ? events : &none, in_size, in_get};
    clap_output_events_t out_list = {&ignored, out_push};
    clap_process_t pr = {
        .steady_time = (int64_t)start, .frames_count = BLOCK,
        .audio_inputs = &in, .audio_outputs = &out, .audio_inputs_count = 1, .audio_outputs_count = 1,
        .in_events = &in_list, .out_events = &out_list,
    };
    p->process(p, &pr);
    return ol;
}

/* ---- tests ---- */

static void test_factory_offers_one_plugin(void)
{
    CHECK(factory->get_plugin_count(factory) == 1);
    const clap_plugin_descriptor_t *d = factory->get_plugin_descriptor(factory, 0);
    CHECK(!strcmp(d->id, "org.tritoneworship.tritoneworship"));
    CHECK(!strcmp(d->name, "TritoneWorship"));
}

static void test_params_match_the_dsp_parameter_table(void)
{
    const clap_plugin_t *p = create_plugin();
    const clap_plugin_params_t *params = p->get_extension(p, CLAP_EXT_PARAMS);
#ifdef TW_EQ_TUNING
    CHECK(params->count(p) == TW_P_COUNT + TW_EQ_BANDS);
#else
    CHECK(params->count(p) == TW_P_COUNT);
#endif
    for (uint32_t i = 0; i < TW_P_COUNT; i++) {
        clap_param_info_t info;
        CHECK(params->get_info(p, i, &info));
        CHECK(!strcmp(info.name, tw_param_info[i].name));
        CHECK(info.min_value == tw_param_info[i].min && info.max_value == tw_param_info[i].max);
        CHECK(get_param(p, i) == tw_param_info[i].def);
        CHECK(!!(info.flags & CLAP_PARAM_IS_BYPASS) == (i == TW_P_BYPASS));
    }
    p->destroy(p);
}

static void test_reports_latency_for_delay_compensation(void)
{
    const clap_plugin_t *p = create_plugin();
    const clap_plugin_latency_t *latency = p->get_extension(p, CLAP_EXT_LATENCY);
    p->activate(p, SR, 1, BLOCK);
    CHECK(latency->get(p) == TW_LATENCY);
    p->deactivate(p);
    p->destroy(p);
}

static void test_automation_applies_at_its_exact_sample(void)
{
    const clap_plugin_t *p = create_plugin();
    set_param(p, TW_P_BYPASS, 1);
    p->activate(p, SR, 1, BLOCK);
    for (uint64_t t = 0; t < 8 * BLOCK; t += BLOCK)
        process_block(p, t, NULL);

    InEvents ev = {0};
    add_param_event(&ev, 128, TW_P_BYPASS, 0);
    const float *y = process_block(p, 8 * BLOCK, &ev);
    int untouched_before = 1;
    for (int i = 0; i < 128; i++)
        untouched_before &= y[i] == sine(8 * BLOCK + i);
    CHECK(untouched_before);                    /* still bypassed up to sample 127 */
    CHECK(y[129] != sine(8 * BLOCK + 129));     /* effect starts fading in at sample 128 */
    CHECK(get_param(p, TW_P_BYPASS) == 0);
    p->deactivate(p);
    p->destroy(p);
}

typedef struct {
    char data[4096];
    size_t len, pos;
} Stream;

static int64_t stream_write(const clap_ostream_t *s, const void *buf, uint64_t size)
{
    Stream *st = s->ctx;
    memcpy(st->data + st->len, buf, size);
    st->len += size;
    return (int64_t)size;
}

static int64_t stream_read(const clap_istream_t *s, void *buf, uint64_t size)
{
    Stream *st = s->ctx;
    uint64_t n = st->len - st->pos < size ? st->len - st->pos : size;
    memcpy(buf, st->data + st->pos, n);
    st->pos += n;
    return (int64_t)n;
}

static void test_state_round_trips_every_parameter(void)
{
    const clap_plugin_t *a = create_plugin();
    set_param(a, TW_P_AMOUNT, 77);
    set_param(a, TW_P_DEATH, 33);
    set_param(a, TW_P_BLACKEN, 1);
    set_param(a, TW_P_TREM_SPEED, 1234);
    Stream st = {0};
    clap_ostream_t os = {&st, stream_write};
    const clap_plugin_state_t *state = a->get_extension(a, CLAP_EXT_STATE);
    CHECK(state->save(a, &os));

    const clap_plugin_t *b = create_plugin();
    clap_istream_t is = {&st, stream_read};
    CHECK(state->load(b, &is));
    for (clap_id i = 0; i < TW_P_COUNT; i++)
        CHECK(get_param(b, i) == get_param(a, i));
    CHECK(get_param(b, TW_P_TREM_SPEED) == 1234);
    a->destroy(a);
    b->destroy(b);
}

/* Runs the plugin's GUI timer until it has sent n events, or gives up after a second */
static void pump_gui(const clap_plugin_t *p, OutEvents *out, uint32_t n)
{
    const clap_plugin_timer_support_t *timer = p->get_extension(p, CLAP_EXT_TIMER_SUPPORT);
    InEvents none = {0};
    for (int tries = 0; tries < 100 && out->n < n; tries++) {
        timer->on_timer(p, 7);
        flush(p, &none, out);
        sleep_ms(10);
    }
}

static void test_gui_knob_drag_and_footswitch_click_reach_the_host(void)
{
    if (!host_window_open()) {
        printf("SKIP %s: no window system to test with\n", __func__);
        return;
    }
    const clap_plugin_t *p = create_plugin();
    const clap_plugin_gui_t *gui = p->get_extension(p, CLAP_EXT_GUI);
    clap_window_t win = {.api = GUI_API};
#if defined(_WIN32)
    win.win32 = host_window_parent();
#else
    win.x11 = (clap_xwnd)(uintptr_t)host_window_parent();
#endif
    CHECK(gui->create(p, GUI_API, false));
    CHECK(gui->set_parent(p, &win));
    CHECK(gui->show(p));
    int found = host_window_find_child();
    CHECK(found);
    if (!found)
        return;

    TwUiRect knob = tw_ui_rect(TW_P_AMOUNT);
    int kx = knob.x + knob.w / 2, ky = knob.y + knob.h / 2;
    flush_requested = 0;
    host_mouse(MOUSE_DOWN, kx, ky);
    host_mouse(MOUSE_MOVE, kx, ky - TW_UI_DRAG_PIXELS / 4);
    host_mouse(MOUSE_UP, kx, ky - TW_UI_DRAG_PIXELS / 4);
    OutEvents out = {0};
    pump_gui(p, &out, 3);
    CHECK(flush_requested);
    CHECK(out.n == 3);
    CHECK(out.type[0] == CLAP_EVENT_PARAM_GESTURE_BEGIN && out.id[0] == TW_P_AMOUNT);
    CHECK(out.type[1] == CLAP_EVENT_PARAM_VALUE && out.id[1] == TW_P_AMOUNT && out.value[1] == 75);
    CHECK(out.type[2] == CLAP_EVENT_PARAM_GESTURE_END && out.id[2] == TW_P_AMOUNT);
    CHECK(get_param(p, TW_P_AMOUNT) == 75); /* dragged a quarter of the range up from 50 */

    TwUiRect sw = tw_ui_rect(TW_P_BYPASS);
    host_mouse(MOUSE_DOWN, sw.x + sw.w / 2, sw.y + sw.h / 2);
    memset(&out, 0, sizeof out);
    pump_gui(p, &out, 3);
    CHECK(get_param(p, TW_P_BYPASS) == 1);

    gui->destroy(p);
    p->destroy(p);
    host_window_close();
}

int main(int argc, char **argv)
{
    void *lib = lib_open(argv[1]);
    if (!lib) {
        printf("FAIL cannot load %s\n", argv[1]);
        return 1;
    }
    const clap_plugin_entry_t *entry = lib_symbol(lib, "clap_entry");
    entry->init(argv[1]);
    factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);

    test_factory_offers_one_plugin();
    test_params_match_the_dsp_parameter_table();
    test_reports_latency_for_delay_compensation();
    test_automation_applies_at_its_exact_sample();
    test_state_round_trips_every_parameter();
    test_gui_knob_drag_and_footswitch_click_reach_the_host();

    entry->deinit();
    printf(failures ? "%d check(s) failed\n" : "all CLAP tests passed\n", failures);
    return failures != 0;
}
