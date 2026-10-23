#include <clap/clap.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tw_dsp.h"
#include "tw_ui.h"
#include "tw_gui.h"

#if defined(_WIN32)
#define GUI_API CLAP_WINDOW_API_WIN32
#define GUI_PARENT(w) ((w)->win32)
#elif defined(__APPLE__)
#define GUI_API CLAP_WINDOW_API_COCOA
#define GUI_PARENT(w) ((w)->cocoa)
#else
#define GUI_API CLAP_WINDOW_API_X11
#define GUI_PARENT(w) ((void *)(uintptr_t)(w)->x11)
#endif

/* Param ids: 0..TW_P_COUNT-1 follow tw_param_info, then the EQ bands in tuning builds */
#ifdef TW_EQ_TUNING
#define N_PARAMS (TW_P_COUNT + TW_EQ_BANDS)
#else
#define N_PARAMS TW_P_COUNT
#endif

#define GUI_BEGIN 1u
#define GUI_CHANGED 2u
#define GUI_END 4u
#define GUI_TIMER_MS 30

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *host_params;
    const clap_host_timer_support_t *host_timer;
    TwState *dsp;
    /* Shared between the audio and main threads */
    _Atomic float value[N_PARAMS];
    atomic_uint gui_flags[N_PARAMS];
    TwGui *gui;
    clap_id timer;
    bool timer_registered;
} Plugin;

static const char *const features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_PITCH_SHIFTER,
    CLAP_PLUGIN_FEATURE_DISTORTION,
    CLAP_PLUGIN_FEATURE_STEREO,
    NULL,
};

static const clap_plugin_descriptor_t descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "org.tritoneworship.tritoneworship",
    .name = "TritoneWorship",
    .vendor = "Neil Howie",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "Minor third stacking, death octave, black metal EQ and tremolo picking",
    .features = features,
};

static void param_range(clap_id id, double *min, double *max, double *def)
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

static void apply_params(Plugin *p)
{
    for (int i = 0; i < TW_P_COUNT; i++)
        tw_set_param(&p->dsp->p, i, atomic_load(&p->value[i]));
#ifdef TW_EQ_TUNING
    for (int b = 0; b < TW_EQ_BANDS; b++)
        p->dsp->p.eq_db[b] = atomic_load(&p->value[TW_P_COUNT + b]);
#endif
}

static void handle_event(Plugin *p, const clap_event_header_t *h)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
        return;
    const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
    if (e->param_id < N_PARAMS)
        atomic_store(&p->value[e->param_id], (float)e->value);
}

static void push_gesture(const clap_output_events_t *out, clap_id id, uint16_t type)
{
    clap_event_param_gesture_t e = {
        .header = {sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, type, 0},
        .param_id = id,
    };
    out->try_push(out, &e.header);
}

/* Tells the host about changes made in the GUI */
static void send_gui_changes(Plugin *p, const clap_output_events_t *out)
{
    for (clap_id i = 0; i < N_PARAMS; i++) {
        unsigned f = atomic_exchange(&p->gui_flags[i], 0);
        if (f & GUI_BEGIN)
            push_gesture(out, i, CLAP_EVENT_PARAM_GESTURE_BEGIN);
        if (f & GUI_CHANGED) {
            clap_event_param_value_t e = {
                .header = {sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0},
                .param_id = i,
                .note_id = -1,
                .port_index = -1,
                .channel = -1,
                .key = -1,
                .value = atomic_load(&p->value[i]),
            };
            out->try_push(out, &e.header);
        }
        if (f & GUI_END)
            push_gesture(out, i, CLAP_EVENT_PARAM_GESTURE_END);
    }
}

/* ---- plugin ---- */

static bool plugin_init(const clap_plugin_t *plugin)
{
    Plugin *p = plugin->plugin_data;
    p->host_params = p->host->get_extension(p->host, CLAP_EXT_PARAMS);
    p->host_timer = p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    for (clap_id i = 0; i < N_PARAMS; i++) {
        double min, max, def;
        param_range(i, &min, &max, &def);
        atomic_init(&p->value[i], (float)def);
        atomic_init(&p->gui_flags[i], 0);
    }
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    Plugin *p = plugin->plugin_data;
    tw_destroy(p->dsp);
    free(p);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames,
                            uint32_t max_frames)
{
    Plugin *p = plugin->plugin_data;
    p->dsp = tw_create((float)sample_rate);
    if (!p->dsp)
        return false;
    apply_params(p);
    tw_reset(p->dsp);
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    Plugin *p = plugin->plugin_data;
    tw_destroy(p->dsp);
    p->dsp = NULL;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    Plugin *p = plugin->plugin_data;
    tw_reset(p->dsp);
}

/* Splits the block at each parameter event so automation is sample accurate */
static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *pr)
{
    Plugin *p = plugin->plugin_data;
    send_gui_changes(p, pr->out_events);

    const uint32_t n = pr->frames_count;
    const uint32_t n_events = pr->in_events->size(pr->in_events);
    float **in = pr->audio_inputs[0].data32, **out = pr->audio_outputs[0].data32;
    uint32_t ev = 0, done = 0;
    while (done < n) {
        uint32_t next = n;
        for (; ev < n_events; ev++) {
            const clap_event_header_t *h = pr->in_events->get(pr->in_events, ev);
            if (h->time > done) {
                if (h->time < n)
                    next = h->time;
                break;
            }
            handle_event(p, h);
        }
        apply_params(p);
        tw_process(p->dsp, in[0] + done, in[1] + done, out[0] + done, out[1] + done, next - done);
        done = next;
    }
    for (; ev < n_events; ev++)
        handle_event(p, pr->in_events->get(pr->in_events, ev));
    return CLAP_PROCESS_CONTINUE;
}

/* ---- audio ports ---- */

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input)
{
    return 1;
}

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info)
{
    if (index != 0)
        return false;
    info->id = 0;
    snprintf(info->name, sizeof info->name, "%s", is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = 0;
    return true;
}

static const clap_plugin_audio_ports_t ext_audio_ports = {ports_count, ports_get};

/* ---- params ---- */

static uint32_t params_count(const clap_plugin_t *plugin)
{
    return N_PARAMS;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    if (index >= N_PARAMS)
        return false;
    memset(info, 0, sizeof *info);
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (index == TW_P_BLACKEN)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (index == TW_P_BYPASS)
        info->flags |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_BYPASS;
    param_range(index, &info->min_value, &info->max_value, &info->default_value);
    if (index < TW_P_COUNT)
        snprintf(info->name, sizeof info->name, "%s", tw_param_info[index].name);
    else
        snprintf(info->name, sizeof info->name, "EQ %g Hz", tw_eq_freqs[index - TW_P_COUNT]);
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    Plugin *p = plugin->plugin_data;
    if (id >= N_PARAMS)
        return false;
    *value = atomic_load(&p->value[id]);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text,
                                 uint32_t size)
{
    if (id >= N_PARAMS)
        return false;
    if (id == TW_P_BLACKEN || id == TW_P_BYPASS)
        snprintf(text, size, "%s", value > 0.5 ? "On" : "Off");
    else if (id == TW_P_TREM_SPEED)
        snprintf(text, size, "%.0f BPM", value);
    else if (id >= TW_P_COUNT)
        snprintf(text, size, "%.1f dB", value);
    else
        snprintf(text, size, "%.1f", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text,
                                 double *value)
{
    if (id >= N_PARAMS)
        return false;
    if (id == TW_P_BLACKEN || id == TW_P_BYPASS)
        *value = strcmp(text, "On") == 0 || atof(text) > 0.5;
    else
        *value = atof(text);
    return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                         const clap_output_events_t *out)
{
    Plugin *p = plugin->plugin_data;
    for (uint32_t i = 0; i < in->size(in); i++)
        handle_event(p, in->get(in, i));
    send_gui_changes(p, out);
}

static const clap_plugin_params_t ext_params = {
    params_count, params_get_info, params_get_value, params_value_to_text,
    params_text_to_value, params_flush,
};

/* ---- state: one "symbol=value" line per parameter ---- */

static void param_symbol(clap_id id, char *buf, size_t size)
{
    if (id < TW_P_COUNT)
        snprintf(buf, size, "%s", tw_param_info[id].symbol);
    else
        snprintf(buf, size, "eq_%g", tw_eq_freqs[id - TW_P_COUNT]);
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    Plugin *p = plugin->plugin_data;
    char text[N_PARAMS * 48] = "";
    size_t len = 0;
    for (clap_id i = 0; i < N_PARAMS; i++) {
        char sym[32];
        param_symbol(i, sym, sizeof sym);
        len += snprintf(text + len, sizeof text - len, "%s=%.6g\n", sym, atomic_load(&p->value[i]));
    }
    for (size_t done = 0; done < len;) {
        int64_t w = stream->write(stream, text + done, len - done);
        if (w <= 0)
            return false;
        done += w;
    }
    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    Plugin *p = plugin->plugin_data;
    char text[N_PARAMS * 48 * 2];
    size_t len = 0;
    for (int64_t r; len < sizeof text - 1 && (r = stream->read(stream, text + len, sizeof text - 1 - len)) > 0;)
        len += r;
    text[len] = 0;

    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        for (clap_id i = 0; i < N_PARAMS; i++) {
            char sym[32];
            param_symbol(i, sym, sizeof sym);
            if (strcmp(sym, line) == 0) {
                double min, max, def, v = atof(eq + 1);
                param_range(i, &min, &max, &def);
                atomic_store(&p->value[i], (float)(v < min ? min : v > max ? max : v));
            }
        }
    }
    if (p->host_params)
        p->host_params->rescan(p->host, CLAP_PARAM_RESCAN_VALUES);
    return true;
}

static const clap_plugin_state_t ext_state = {state_save, state_load};

/* ---- latency ---- */

static uint32_t latency_get(const clap_plugin_t *plugin)
{
    return TW_LATENCY;
}

static const clap_plugin_latency_t ext_latency = {latency_get};

/* ---- GUI ---- */

static float gui_get(void *ctx, int param)
{
    Plugin *p = ctx;
    return atomic_load(&p->value[param]);
}

static void gui_flag(Plugin *p, int param, unsigned flag)
{
    atomic_fetch_or(&p->gui_flags[param], flag);
    if (p->host_params)
        p->host_params->request_flush(p->host);
}

static void gui_begin(void *ctx, int param)
{
    gui_flag(ctx, param, GUI_BEGIN);
}

static void gui_set(void *ctx, int param, float value)
{
    Plugin *p = ctx;
    atomic_store(&p->value[param], value);
    gui_flag(p, param, GUI_CHANGED);
}

static void gui_end(void *ctx, int param)
{
    gui_flag(ctx, param, GUI_END);
}

static const TwGuiCallbacks gui_callbacks = {gui_get, gui_begin, gui_set, gui_end};

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    return strcmp(api, GUI_API) == 0 && !is_floating;
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating)
{
    *api = GUI_API;
    *is_floating = false;
    return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    if (!gui_is_api_supported(plugin, api, is_floating))
        return false;
#ifdef TW_GUI_NEEDS_HOST_TIMER
    Plugin *p = plugin->plugin_data;
    if (!p->host_timer || !p->host_timer->register_timer(p->host, GUI_TIMER_MS, &p->timer))
        return false;
    p->timer_registered = true;
#endif
    return true;
}

static void gui_destroy(const clap_plugin_t *plugin)
{
    Plugin *p = plugin->plugin_data;
    if (p->timer_registered)
        p->host_timer->unregister_timer(p->host, p->timer);
    p->timer_registered = false;
    if (p->gui)
        tw_gui_destroy(p->gui);
    p->gui = NULL;
}

static bool gui_set_scale(const clap_plugin_t *plugin, double scale)
{
    return false;
}

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    *width = TW_UI_W;
    *height = TW_UI_H;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin)
{
    return false;
}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints)
{
    return false;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    return gui_get_size(plugin, width, height);
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    return width == TW_UI_W && height == TW_UI_H;
}

static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    Plugin *p = plugin->plugin_data;
    p->gui = tw_gui_create(GUI_PARENT(window), &gui_callbacks, p);
    return p->gui != NULL;
}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
    return false;
}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
}

static bool gui_show(const clap_plugin_t *plugin)
{
    Plugin *p = plugin->plugin_data;
    if (!p->gui)
        return false;
    tw_gui_show(p->gui, 1);
    return true;
}

static bool gui_hide(const clap_plugin_t *plugin)
{
    Plugin *p = plugin->plugin_data;
    if (!p->gui)
        return false;
    tw_gui_show(p->gui, 0);
    return true;
}

static const clap_plugin_gui_t ext_gui = {
    gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale,
    gui_get_size, gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size,
    gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide,
};

static void timer_on_timer(const clap_plugin_t *plugin, clap_id id)
{
    Plugin *p = plugin->plugin_data;
    if (p->gui && id == p->timer)
        tw_gui_idle(p->gui);
}

static const clap_plugin_timer_support_t ext_timer = {timer_on_timer};

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &ext_audio_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &ext_params;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &ext_state;
    if (!strcmp(id, CLAP_EXT_LATENCY))
        return &ext_latency;
    if (!strcmp(id, CLAP_EXT_GUI))
        return &ext_gui;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &ext_timer;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
}

/* ---- factory and entry ---- */

static uint32_t factory_count(const clap_plugin_factory_t *factory)
{
    return 1;
}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *factory,
                                                          uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory,
                                           const clap_host_t *host, const char *id)
{
    if (strcmp(id, descriptor.id) != 0 || !clap_version_is_compatible(host->clap_version))
        return NULL;
    Plugin *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->host = host;
    p->plugin = (clap_plugin_t){
        .desc = &descriptor,
        .plugin_data = p,
        .init = plugin_init,
        .destroy = plugin_destroy,
        .activate = plugin_activate,
        .deactivate = plugin_deactivate,
        .start_processing = plugin_start_processing,
        .stop_processing = plugin_stop_processing,
        .reset = plugin_reset,
        .process = plugin_process,
        .get_extension = plugin_get_extension,
        .on_main_thread = plugin_on_main_thread,
    };
    return &p->plugin;
}

static const clap_plugin_factory_t factory = {factory_count, factory_descriptor, factory_create};

static bool entry_init(const char *path)
{
    return true;
}

static void entry_deinit(void)
{
}

static const void *entry_get_factory(const char *id)
{
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory,
};
