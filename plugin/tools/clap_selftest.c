// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    A CLAP host, cut down to the one question worth asking automatically: does anything
    the DSP pushes ever reach the UI?

    It exists because the answer was NO and nothing noticed.  DPF's CLAP wrapper carried
    an updateState() that threw the value away and returned true, so the plugin loaded,
    drew its panel, and then sat there showing whatever state the host happened to hand
    over at the moment the window opened -- a dead LCD and a patch menu stuck on "waiting
    for the machine", in a build that was otherwise perfectly healthy.  Every other format
    was fine, which is exactly why it went unseen: the formats we run day to day are LV2
    and the JACK standalone.

    So: load the .clap, open its GUI, run some audio through it, pump the timer the plugin
    asked for, and let the UI say how many panel updates it decoded.  One is enough to
    prove the channel is open, because the open-time snapshot does not carry a panel.

    Deliberately not a general host.  It implements the extensions DPF asks for and
    nothing else, and it trusts the plugin, because the plugin is ours.
*/

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clap/entry.h"
#include "clap/plugin-factory.h"
#include "clap/ext/gui.h"
#include "clap/ext/latency.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/thread-check.h"
#include "clap/ext/timer-support.h"

#define FRAMES 256

static clap_id  g_timer_id = CLAP_INVALID_ID;
static bool     g_callback_pending = false;

// ---- the host's side of the extensions DPF looks for --------------------------------

static bool CLAP_ABI timer_register(const clap_host_t *h, uint32_t period_ms, clap_id *id)
{
    (void) h; (void) period_ms;
    *id = g_timer_id = 1;
    return true;
}

static bool CLAP_ABI timer_unregister(const clap_host_t *h, clap_id id)
{
    (void) h; (void) id;
    g_timer_id = CLAP_INVALID_ID;
    return true;
}

static const clap_host_timer_support_t g_timer = { timer_register, timer_unregister };

static void CLAP_ABI gui_resize_hints_changed(const clap_host_t *h) { (void) h; }
static bool CLAP_ABI gui_request_resize(const clap_host_t *h, uint32_t w, uint32_t g)
{ (void) h; (void) w; (void) g; return true; }
static bool CLAP_ABI gui_request_show(const clap_host_t *h) { (void) h; return true; }
static bool CLAP_ABI gui_request_hide(const clap_host_t *h) { (void) h; return true; }
static void CLAP_ABI gui_closed(const clap_host_t *h, bool destroyed)
{ (void) h; (void) destroyed; }

static const clap_host_gui_t g_gui = {
    gui_resize_hints_changed, gui_request_resize,
    gui_request_show, gui_request_hide, gui_closed,
};

static void CLAP_ABI params_rescan(const clap_host_t *h, clap_param_rescan_flags f)
{ (void) h; (void) f; }
static void CLAP_ABI params_clear(const clap_host_t *h, clap_id p, clap_param_clear_flags f)
{ (void) h; (void) p; (void) f; }
static void CLAP_ABI params_request_flush(const clap_host_t *h) { (void) h; }

static const clap_host_params_t g_params = {
    params_rescan, params_clear, params_request_flush,
};

static void CLAP_ABI state_mark_dirty(const clap_host_t *h) { (void) h; }
static const clap_host_state_t g_state = { state_mark_dirty };

static void CLAP_ABI latency_changed(const clap_host_t *h) { (void) h; }
static const clap_host_latency_t g_latency = { latency_changed };

// Single threaded on purpose: everything below runs on this one thread, so the honest
// answer to both questions is "yes".  A real host would not be able to say that.
static bool CLAP_ABI is_main_thread(const clap_host_t *h) { (void) h; return true; }
static bool CLAP_ABI is_audio_thread(const clap_host_t *h) { (void) h; return false; }
static const clap_host_thread_check_t g_thread = { is_main_thread, is_audio_thread };

static const void *CLAP_ABI host_get_extension(const clap_host_t *h, const char *id)
{
    (void) h;
    if (strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &g_timer;
    if (strcmp(id, CLAP_EXT_GUI) == 0)           return &g_gui;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0)        return &g_params;
    if (strcmp(id, CLAP_EXT_STATE) == 0)         return &g_state;
    if (strcmp(id, CLAP_EXT_LATENCY) == 0)       return &g_latency;
    if (strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)  return &g_thread;
    return NULL;
}

static void CLAP_ABI host_request_restart(const clap_host_t *h) { (void) h; }
static void CLAP_ABI host_request_process(const clap_host_t *h) { (void) h; }
static void CLAP_ABI host_request_callback(const clap_host_t *h)
{ (void) h; g_callback_pending = true; }

static const clap_host_t g_host = {
    CLAP_VERSION_INIT, NULL,
    "Voltaire 110 CLAP self test", "emu110", "", "0",
    host_get_extension, host_request_restart, host_request_process, host_request_callback,
};

// ---- empty event lists ---------------------------------------------------------------

static uint32_t CLAP_ABI in_size(const clap_input_events_t *l) { (void) l; return 0; }
static const clap_event_header_t *CLAP_ABI in_get(const clap_input_events_t *l, uint32_t i)
{ (void) l; (void) i; return NULL; }
static const clap_input_events_t g_in = { NULL, in_size, in_get };

static bool CLAP_ABI out_push(const clap_output_events_t *l, const clap_event_header_t *e)
{ (void) l; (void) e; return true; }
static const clap_output_events_t g_out = { NULL, out_push };

// --------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <plugin.clap>\n", argv[0]);
        return 2;
    }
    const char *const path = argv[1];

    void *const lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL)
    {
        fprintf(stderr, "clap: cannot load %s: %s\n", path, dlerror());
        return 1;
    }

    const clap_plugin_entry_t *const entry =
            (const clap_plugin_entry_t *) dlsym(lib, "clap_entry");
    if (entry == NULL)
    {
        fprintf(stderr, "clap: %s has no clap_entry\n", path);
        return 1;
    }
    if (!entry->init(path))
    {
        fprintf(stderr, "clap: entry->init failed\n");
        return 1;
    }

    const clap_plugin_factory_t *const factory =
            (const clap_plugin_factory_t *) entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (factory == NULL || factory->get_plugin_count(factory) == 0)
    {
        fprintf(stderr, "clap: no plugin factory\n");
        return 1;
    }

    const clap_plugin_descriptor_t *const desc = factory->get_plugin_descriptor(factory, 0);
    printf("clap: %s -- %s\n", desc->id, desc->name);

    const clap_plugin_t *const plugin = factory->create_plugin(factory, &g_host, desc->id);
    if (plugin == NULL || !plugin->init(plugin))
    {
        fprintf(stderr, "clap: create/init failed\n");
        return 1;
    }

    // The GUI, floating, so no X window has to be built here for it to be parented into.
    const clap_plugin_gui_t *const gui =
            (const clap_plugin_gui_t *) plugin->get_extension(plugin, CLAP_EXT_GUI);
    if (gui == NULL)
    {
        fprintf(stderr, "clap: plugin has no gui extension\n");
        return 1;
    }
    if (!gui->create(plugin, CLAP_WINDOW_API_X11, true))
    {
        fprintf(stderr, "clap: gui->create failed (is there a DISPLAY?)\n");
        return 1;
    }
    gui->show(plugin);
    printf("clap: gui created\n");

    if (!plugin->activate(plugin, 48000.0, FRAMES, FRAMES))
    {
        fprintf(stderr, "clap: activate failed\n");
        return 1;
    }
    plugin->start_processing(plugin);

    float bufL[FRAMES], bufR[FRAMES];
    float *chans[2] = { bufL, bufR };
    clap_audio_buffer_t out;
    memset(&out, 0, sizeof(out));
    out.data32 = chans;
    out.channel_count = 2;

    clap_process_t proc;
    memset(&proc, 0, sizeof(proc));
    proc.steady_time = -1;
    proc.frames_count = FRAMES;
    proc.audio_outputs = &out;
    proc.audio_outputs_count = 1;
    proc.in_events = &g_in;
    proc.out_events = &g_out;

    const clap_plugin_timer_support_t *const timer =
            (const clap_plugin_timer_support_t *)
                    plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT);

    // Enough blocks for the firmware to boot and put something on the display, with the
    // timer pumped alongside so the UI gets its idle.  The machine takes a few seconds of
    // EMULATED time, which is a few hundred blocks, not a few hundred milliseconds.
    for (int i = 0; i < 4000; i ++)
    {
        plugin->process(plugin, &proc);
        if (timer != NULL && g_timer_id != CLAP_INVALID_ID)
            timer->on_timer(plugin, g_timer_id);
        if (g_callback_pending)
        {
            g_callback_pending = false;
            plugin->on_main_thread(plugin);
        }
    }

    plugin->stop_processing(plugin);
    gui->destroy(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    printf("clap: ran %d blocks and shut down cleanly\n", 4000);
    return 0;
}
