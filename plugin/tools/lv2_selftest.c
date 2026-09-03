/* Copyright (c) 2026 Elliott H. Liggett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A minimal LV2 host, so the thing that gets TESTED is the thing a user loads.
 *
 * Building the core and null-testing it proves the emulation.  It does not prove the
 * plugin: the DPF layer, the resampler running at the host's rate, MIDI arriving as LV2
 * atoms, the port layout in the generated TTL.  This loads the built bundle exactly as
 * Ardour would, plays a note, and writes a wav.
 */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/parameters/parameters.h>
#include <lv2/worker/worker.h>
#include <lv2/state/state.h>

#include <dlfcn.h>

/* Supplied by rt_audit.so when it is LD_PRELOADed; weak so the selftest links without it. */
extern void rt_audit_set_active(int on) __attribute__((weak));
extern void rt_audit_report(void)       __attribute__((weak));
extern void rt_audit_reset(void)        __attribute__((weak));
static void rt_arm(int on)   { if (rt_audit_set_active) rt_audit_set_active(on); }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE     48000.0
#define BLOCK    256
#define SECONDS  20.0
#define ATOM_CAP 4096

/* Optional: put the machine into a chorus patch before the note, so the effects path is
 * exercised.  Enabled with a fifth argument of "fx". */
static const struct { int len; unsigned char b[16]; } kFxSetup[] = {
    { 2, { 0xcf, 0x07 } },  /* program change P-08 Double E.P */
    { 11, { 0xf0, 0x41, 0x0f, 0x23, 0x12, 0x00, 0x01, 0x19, 0x02, 0x64, 0xf7 } },  /* chorus rate 2 */
    { 11, { 0xf0, 0x41, 0x0f, 0x23, 0x12, 0x00, 0x01, 0x1a, 0x01, 0x64, 0xf7 } },  /* chorus depth 1 */
    { 11, { 0xf0, 0x41, 0x0f, 0x23, 0x12, 0x00, 0x01, 0x1b, 0x00, 0x64, 0xf7 } },  /* tremolo rate 0 */
    { 11, { 0xf0, 0x41, 0x0f, 0x23, 0x12, 0x00, 0x01, 0x1c, 0x00, 0x63, 0xf7 } },  /* tremolo depth 0 */
    { 11, { 0xf0, 0x41, 0x0f, 0x23, 0x12, 0x00, 0x01, 0x18, 0x14, 0x53, 0xf7 } },  /* output mode 21 */
};

/* A pending worker job, run between blocks the way a host would. */
static unsigned char g_work[8192];
static uint32_t g_work_size = 0;
static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle h, uint32_t size, const void *data)
{
    (void)h;
    if (size > sizeof(g_work)) return LV2_WORKER_ERR_NO_SPACE;
    memcpy(g_work, data, size);
    g_work_size = size;
    return LV2_WORKER_SUCCESS;
}
/* Capture whatever the plugin stores.
 *
 * A DICTIONARY, not a slot.  The plugin has more than one state key -- the machine's
 * battery-backed memory and the settings-and-cards text -- and a host that keeps only the
 * last one it was handed silently loses the other.  This host also round-trips the key,
 * type and flags it was given: DPF checks the type coming back in, and returning 0 there
 * makes the restore vanish with no error anywhere. */
#define MAX_STATES 8
static struct StateSlot {
    uint32_t urid, type, flags;
    size_t   len;
    char    *val;
} g_states[MAX_STATES];
static int g_nstates = 0;

static const char *urid_name(uint32_t urid);         /* defined below, with the map */

static LV2_State_Status store_cb(LV2_State_Handle h, uint32_t key, const void *value,
                                 size_t size, uint32_t type, uint32_t flags)
{
    int i;
    (void)h;
    for (i = 0; i < g_nstates; i++) if (g_states[i].urid == key) break;
    if (i == g_nstates)
    {
        if (g_nstates == MAX_STATES) return LV2_STATE_ERR_NO_SPACE;
        g_nstates++;
        g_states[i].val = NULL;
    }
    free(g_states[i].val);
    g_states[i].val = malloc(size + 1);
    memcpy(g_states[i].val, value, size);
    g_states[i].val[size] = 0;
    g_states[i].len = size;
    g_states[i].urid = key; g_states[i].type = type; g_states[i].flags = flags;
    return LV2_STATE_SUCCESS;
}
static const void *retrieve_cb(LV2_State_Handle h, uint32_t key, size_t *size,
                               uint32_t *type, uint32_t *flags)
{
    int i;
    (void)h;
    for (i = 0; i < g_nstates; i++) if (g_states[i].urid == key)
    {
        if (size)  *size  = g_states[i].len;
        if (type)  *type  = g_states[i].type;
        if (flags) *flags = g_states[i].flags;
        return g_states[i].val;
    }
    if (size) *size = 0;
    return NULL;
}
static void states_clear(void)
{
    int i;
    for (i = 0; i < g_nstates; i++) { free(g_states[i].val); g_states[i].val = NULL; }
    g_nstates = 0;
}
/* Find a stored key by the tail of its URI, e.g. "nvram". */
static struct StateSlot *state_by_suffix(const char *suffix)
{
    int i;
    const size_t n = strlen(suffix);
    for (i = 0; i < g_nstates; i++)
    {
        const char *u = urid_name(g_states[i].urid);
        const size_t l = strlen(u);
        if (l >= n && strcmp(u + l - n, suffix) == 0) return &g_states[i];
    }
    return NULL;
}
/* The whole dictionary, copied aside so it outlives the next save.  Keeping the real
 * urid/type/flags is the point: a host hands back exactly what it was given, and guessing
 * the key URI here would test the guess rather than the plugin. */
static struct StateSlot g_snapshot[MAX_STATES];
static int g_nsnapshot = 0;
static void snapshot_take(void)
{
    int i;
    for (i = 0; i < g_nsnapshot; i++) free(g_snapshot[i].val);
    g_nsnapshot = g_nstates;
    for (i = 0; i < g_nstates; i++)
    {
        g_snapshot[i] = g_states[i];
        g_snapshot[i].val = malloc(g_states[i].len + 1);
        memcpy(g_snapshot[i].val, g_states[i].val, g_states[i].len + 1);
    }
}
/* Hand the snapshot back, in the given order of keys. */
static void snapshot_put(int reverse)
{
    int i;
    states_clear();
    for (i = 0; i < g_nsnapshot; i++)
    {
        const struct StateSlot *s = &g_snapshot[reverse ? g_nsnapshot - 1 - i : i];
        store_cb(NULL, s->urid, s->val, s->len, s->type, s->flags);
    }
}
static const struct StateSlot *snapshot_by_suffix(const char *suffix)
{
    int i;
    const size_t n = strlen(suffix);
    for (i = 0; i < g_nsnapshot; i++)
    {
        const char *u = urid_name(g_snapshot[i].urid);
        const size_t l = strlen(u);
        if (l >= n && strcmp(u + l - n, suffix) == 0) return &g_snapshot[i];
    }
    return NULL;
}

/* A copy that outlives the next save. */
static char *state_dup(const char *suffix, size_t *len)
{
    struct StateSlot *s = state_by_suffix(suffix);
    char *out;
    if (s == NULL) { if (len) *len = 0; return NULL; }
    out = malloc(s->len + 1);
    memcpy(out, s->val, s->len + 1);
    if (len) *len = s->len;
    return out;
}

static LV2_Worker_Status work_respond(LV2_Worker_Respond_Handle h, uint32_t size, const void *data)
{ (void)h; (void)size; (void)data; return LV2_WORKER_SUCCESS; }

/* A URID map just big enough for the handful of URIs DPF asks about. */
static char *g_uris[128];
static uint32_t g_nuris = 1;                      /* 0 is reserved */
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    for (uint32_t i = 1; i < g_nuris; i++)
        if (!strcmp(g_uris[i], uri)) return i;
    if (g_nuris >= 128) return 0;
    g_uris[g_nuris] = strdup(uri);
    return g_nuris++;
}
static const char *urid_name(uint32_t urid)
{
    return (urid > 0 && urid < g_nuris && g_uris[urid]) ? g_uris[urid] : "<unmapped>";
}

int main(int argc, char **argv)
{
    const char *so   = (argc > 1) ? argv[1] : "plugin/bin/Voltaire110.lv2/Voltaire110_dsp.so";
    const char *uri  = (argc > 2) ? argv[2] : "https://github.com/eliggett/emu110/voltaire110";
    const char *wav  = (argc > 3) ? argv[3] : "lv2_selftest.wav";

    void *lib = dlopen(so, RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
    if (!df) { fprintf(stderr, "no lv2_descriptor\n"); return 1; }

    const LV2_Descriptor *d = NULL;
    for (uint32_t i = 0; (d = df(i)); i++)
        if (!strcmp(d->URI, uri)) break;
    if (!d) { fprintf(stderr, "plugin %s not in %s\n", uri, so); return 1; }
    printf("loaded %s\n", d->URI);

    LV2_URID_Map map = { NULL, map_uri };
    LV2_Feature  f_map = { LV2_URID__map, &map };

    /* DPF refuses to instantiate without the Options feature -- it needs the block length
     * up front so it can size its buffers once and never allocate on the audio thread. */
    const uint32_t urid_int    = map_uri(NULL, LV2_ATOM__Int);
    const uint32_t urid_float  = map_uri(NULL, LV2_ATOM__Float);
    static int32_t block_max   = BLOCK;
    static int32_t block_nom   = BLOCK;
    static float   rate_val    = (float)RATE;
    const LV2_Options_Option opts[] = {
        { LV2_OPTIONS_INSTANCE, 0, map_uri(NULL, LV2_BUF_SIZE__maxBlockLength),
          sizeof(int32_t), urid_int, &block_max },
        { LV2_OPTIONS_INSTANCE, 0, map_uri(NULL, LV2_BUF_SIZE__nominalBlockLength),
          sizeof(int32_t), urid_int, &block_nom },
        { LV2_OPTIONS_INSTANCE, 0, map_uri(NULL, LV2_PARAMETERS__sampleRate),
          sizeof(float), urid_float, &rate_val },
        { LV2_OPTIONS_BLANK, 0, 0, 0, 0, NULL }
    };
    /* The Worker.  DPF requires it once state is enabled, because that is how it keeps
     * state handling OFF the audio thread -- schedule_work() is called from run() and the
     * host runs work() elsewhere.  Doing that faithfully is also what makes the real-time
     * audit below mean anything: work() is deliberately run OUTSIDE the audited window. */
    LV2_Worker_Schedule sched = { NULL, schedule_work };
    LV2_Feature f_worker = { LV2_WORKER__schedule, &sched };

    LV2_Feature f_opts = { LV2_OPTIONS__options, (void *)opts };
    LV2_Feature f_bounded = { LV2_BUF_SIZE__boundedBlockLength, NULL };
    const LV2_Feature *features[] = { &f_map, &f_opts, &f_bounded, &f_worker, NULL };

    LV2_Handle h = d->instantiate(d, RATE, "./", features);
    if (!h) { fprintf(stderr, "instantiate failed\n"); return 1; }

    static float outL[BLOCK], outR[BLOCK];
    static unsigned char ev_in[ATOM_CAP], ev_out[ATOM_CAP];
    float latency = 0, volume = (argc > 4) ? (float)atof(argv[4]) : 0.0f, hf = 1.0f;
    float btn[6] = { 0, 0, 0, 0, 0, 0 };
    const int want_fx = (argc > 5) && strcmp(argv[5], "fx") == 0;
    const int want_stream = (argc > 5) && strcmp(argv[5], "stream") == 0;
    const int want_state_test = (argc > 5) && strcmp(argv[5], "state") == 0;


    d->connect_port(h, 0, outL);
    d->connect_port(h, 1, outR);
    d->connect_port(h, 2, ev_in);
    d->connect_port(h, 3, ev_out);
    d->connect_port(h, 4, &latency);
    d->connect_port(h, 5, &volume);
    d->connect_port(h, 6, &hf);
    for (int i = 0; i < 6; i++) d->connect_port(h, 7 + i, &btn[i]);
    /* No output control ports any more: the panel arrives as a blob on events-out. */
    if (d->activate) d->activate(h);

    const uint32_t urid_seq   = map_uri(NULL, LV2_ATOM__Sequence);
    const uint32_t urid_frame = map_uri(NULL, LV2_ATOM__frameTime);
    const uint32_t urid_midi  = map_uri(NULL, LV2_MIDI__MidiEvent);

    const long total = (long)(SECONDS * RATE);
    short *pcm = malloc(sizeof(short) * 2 * total);
    long written = 0;
    int peak = 0;
    unsigned seen_leds = 0;
    long blocks_total = 0, lit_blocks[4] = { 0, 0, 0, 0 };
    long slow_total = 0, slow_midi_lit = 0;
    double next_slow = 0.0;
    unsigned last_midi = 0; long midi_edges = 0;
    long state_atoms = 0, midi_out_atoms = 0; unsigned blob_leds = 0;

    for (long done = 0; done < total; done += BLOCK)
    {
        /* Empty input sequence, except where a note goes. */
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)ev_in;
        seq->atom.type = urid_seq;
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->body.unit = 0;
        seq->body.pad  = 0;

        /* Note on at 12 s, off at 15 s -- after the machine has finished booting. */
        const long t12 = (long)(12.0 * RATE), t15 = (long)(16.0 * RATE);
        const unsigned char *msg = NULL;
        static const unsigned char on[3]  = { 0x90, 60, 64 };
        static const unsigned char off[3] = { 0x80, 60, 0 };
        /* Press EDIT/EXIT at 7 s and release at 8 s: the buttons are what drive the
         * machine's own menus, and this proves the whole loop from a host control port
         * through to the LCD. */
        btn[1] = (!want_fx && done >= (long)(7.0 * RATE)
                  && done < (long)(8.0 * RATE)) ? 1.0f : 0.0f;

        if (want_fx) {
            const long step = (done - (long)(7.0 * RATE)) / BLOCK;
            if (step >= 0 && step < (long)(sizeof(kFxSetup)/sizeof(kFxSetup[0])) &&
                ((done - (long)(7.0 * RATE)) % BLOCK) == 0) {
                LV2_Atom_Event *e = (LV2_Atom_Event *)((char *)
                        LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq));
                e->time.frames = 0;
                e->body.type = urid_midi;
                e->body.size = (uint32_t)kFxSetup[step].len;
                memcpy(LV2_ATOM_BODY(&e->body), kFxSetup[step].b, kFxSetup[step].len);
                seq->atom.size += (uint32_t)lv2_atom_pad_size(
                        sizeof(LV2_Atom_Event) + kFxSetup[step].len);
            }
        }
        if (want_stream) {
            /* A note every 400 ms from 10 s, which is what playing looks like -- the MIDI
             * lamp is driven by activity, so a single held note says little about it. */
            const long period = (long)(0.4 * RATE);
            const long since = done - (long)(10.0 * RATE);
            if (since >= 0 && (since % period) == 0)
                msg = ((since / period) & 1) ? off : on;
        } else {
            if (done <= t12 && t12 < done + BLOCK) msg = on;
            if (done <= t15 && t15 < done + BLOCK) msg = off;
        }
        if (msg)
        {
            LV2_Atom_Event *e = (LV2_Atom_Event *)((char *)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq));
            e->time.frames = 0;
            e->body.type = urid_midi;
            e->body.size = 3;
            memcpy(LV2_ATOM_BODY(&e->body), msg, 3);
            seq->atom.size += (uint32_t)lv2_atom_pad_size(sizeof(LV2_Atom_Event) + 3);
        }

        LV2_Atom_Sequence *oseq = (LV2_Atom_Sequence *)ev_out;
        oseq->atom.type = urid_seq;
        oseq->atom.size = ATOM_CAP - sizeof(LV2_Atom);

        /* Only audit once the machine is up: instantiate() and the first blocks legally
         * allocate, and counting those would bury the interesting result. */
        if (done == (long)(10.0 * RATE) && rt_audit_reset) rt_audit_reset();
        rt_arm(done >= (long)(10.0 * RATE));
        d->run(h, BLOCK);
        rt_arm(0);

        /* Count what the plugin put on its events-out port.  MIDI goes to the host; the
         * state blobs are what the UI would receive. */
        {
            LV2_Atom_Sequence *os = (LV2_Atom_Sequence *)ev_out;
            LV2_ATOM_SEQUENCE_FOREACH(os, e)
            {
                if (e->body.type == urid_midi) { midi_out_atoms++; continue; }
                state_atoms++;
                /* The panel blob is hex inside the atom; find it and take the lamps,
                 * which is the last field of the struct the plugin sends. */
                {
                    const char *p = (const char *)LV2_ATOM_BODY_CONST(&e->body);
                    const uint32_t n = e->body.size;
                    for (uint32_t k = 0; k + 198 <= n; k++) {
                        int ok = 1;
                        for (uint32_t j = 0; j < 198 && ok; j++) {
                            const char c = p[k + j];
                            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) ok = 0;
                        }
                        if (!ok) continue;
                        /* byte 96 of the struct is `leds` -> hex chars 192..193 */
                        const char hi = p[k + 192], lo = p[k + 193];
                        const int v = ((hi <= '9' ? hi - '0' : hi - 'a' + 10) << 4)
                                    | (lo <= '9' ? lo - '0' : lo - 'a' + 10);
                        blob_leds = (unsigned)v & 0x0f;
                        break;
                    }
                }
            }
        }

        /* Run any scheduled work here -- off the audio thread, as a host would. */
        if (g_work_size != 0)
        {
            const LV2_Worker_Interface *wi =
                    d->extension_data ? d->extension_data(LV2_WORKER__interface) : NULL;
            if (wi && wi->work)
                wi->work(h, work_respond, NULL, g_work_size, g_work);
            g_work_size = 0;
        }
        {   /* Lamp bits, and how much of the run each was lit -- "ever seen" cannot show
             * a polarity mistake, and every lamp on this machine is active low. */
            const unsigned l = blob_leds;
            seen_leds |= l;
            blocks_total++;
            for (int b = 0; b < 4; b++) if (l & (1u << b)) lit_blocks[b]++;
            /* What a host that polls the output ports SLOWLY would see.  Ardour and Carla
             * poll on their own schedules, so a lamp driven by a firmware pulse has to
             * survive being looked at rarely. */
            {
                const double t = (double)done / RATE;
                if (t >= next_slow) {
                    next_slow += 0.1;                  /* 10 Hz observer */
                    slow_total++;
                    if (l & 4u) slow_midi_lit++;
                }
                if (((l >> 2) & 1u) != last_midi) { midi_edges++; last_midi = (l >> 2) & 1u; }
            }
        }

        for (int i = 0; i < BLOCK && written < total; i++, written++)
        {
            int l = (int)(outL[i] * 32768.0f), r = (int)(outR[i] * 32768.0f);
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            pcm[written * 2 + 0] = (short)l;
            pcm[written * 2 + 1] = (short)r;
            if (abs(l) > peak) peak = abs(l);
            if (abs(r) > peak) peak = abs(r);
        }
    }

    /* Save and restore the way a DAW does: ask the plugin for its state through the LV2
     * state extension, then hand it back to a FRESH instance and check it came through.
     * This is what "reopen the session" actually exercises. */
    if (want_state_test)
    {
        const LV2_State_Interface *si = d->extension_data
                ? (const LV2_State_Interface *)d->extension_data(LV2_STATE__interface) : NULL;
        if (si == NULL) { printf("state: plugin exposes no LV2 state interface\n"); }
        else
        {
            struct StateSlot *slot;
            char *nvram_before, *settings_before;
            size_t nvram_len = 0, settings_len = 0;
            int i;

            /* Move the front-panel controls off their defaults first, so the settings
             * key has something to prove.  Ports are read inside run(), so it takes a
             * block for the plugin to notice. */
            volume = 6.5f;
            hf = 0.0f;
            d->run(h, BLOCK);

            states_clear();
            si->save(h, store_cb, NULL, 0, NULL);
            printf("state: the plugin stored %d key%s\n",
                   g_nstates, g_nstates == 1 ? "" : "s");
            for (i = 0; i < g_nstates; i++)
                printf("state:   %-56s %6zu bytes\n",
                       urid_name(g_states[i].urid), g_states[i].len);

            /* The settings key is text on purpose, so print it: if a project comes back
             * wrong this is the line that says which images it expected. */
            slot = state_by_suffix("settings");
            if (slot != NULL)
            {
                const char *p = slot->val;
                printf("state: settings key reads --\n");
                while (*p)
                {
                    const char *nl = strchr(p, '\n');
                    printf("state:   | %.*s\n", nl ? (int)(nl - p) : (int)strlen(p), p);
                    if (!nl) break;
                    p = nl + 1;
                }
            }

            slot = state_by_suffix("settings");
            printf("state: volume and HF correction %s\n",
                   (slot && strstr(slot->val, "volume 6.5000")
                         && strstr(slot->val, "hfcorrection 0"))
                   ? "were saved as set"
                   : "WERE NOT saved as set");

            snapshot_take();
            nvram_before    = state_dup("nvram", &nvram_len);
            settings_before = state_dup("settings", &settings_len);
            if (nvram_before == NULL)
                printf("state: NO nvram key -- the machine's memory is not being saved\n");

            /* A fresh instance, as reopening the session gives you. */
            LV2_Handle h2 = d->instantiate(d, RATE, "./", features);
            if (h2 == NULL) { printf("state: second instantiate FAILED\n"); }
            else
            {
                char *virgin;
                size_t virgin_len = 0;
                const size_t hdr = 16 * 2;            /* header, in hex characters */
                const size_t wr  = 0x1f00 * 2;
                const size_t pr  = 0x2000 * 2;

                d->connect_port(h2, 0, outL); d->connect_port(h2, 1, outR);
                d->connect_port(h2, 2, ev_in); d->connect_port(h2, 3, ev_out);
                d->connect_port(h2, 4, &latency);
                d->connect_port(h2, 5, &volume); d->connect_port(h2, 6, &hf);
                for (i = 0; i < 6; i++) d->connect_port(h2, 7 + i, &btn[i]);

                /* What the fresh instance holds WITHOUT a restore.  If this already
                 * matched, the comparison below would prove nothing -- both instances boot
                 * the same firmware and would reach the same factory patches. */
                states_clear();
                si->save(h2, store_cb, NULL, 0, NULL);
                virgin = state_dup("nvram", &virgin_len);
                printf("state: before restoring, fresh vs saved: %s\n",
                       (virgin && nvram_before && virgin_len == nvram_len
                        && memcmp(nvram_before + hdr + wr, virgin + hdr + wr, pr) == 0)
                       ? "ALREADY IDENTICAL -- the check below would prove nothing"
                       : "different, so the check below means something");
                free(virgin);

                /* Put the saved data back where retrieve_cb will find it.  The order the
                 * host STORED them in is reversed here to make the point that it does not
                 * matter -- the plugin is asked for its keys in DPF's own order regardless,
                 * which puts the machine's memory before the cards.  That is the awkward
                 * way round, since the cards have to be in their slots before the machine
                 * boots into the patches that name them, and it is the order that actually
                 * happens; the plugin handles it by booting again once the cards arrive. */
                snapshot_put(1);
                si->restore(h2, retrieve_cb, NULL, 0, NULL);

                states_clear();
                si->save(h2, store_cb, NULL, 0, NULL);
                slot = state_by_suffix("nvram");
                printf("state: restored into a fresh instance, it now reports %zu bytes\n",
                       slot ? slot->len : (size_t)0);

                /* The patch store is what must survive; work RAM is touched by booting. */
                {
                    const int same = slot && nvram_before && slot->len == nvram_len
                            && memcmp(nvram_before + hdr + wr, slot->val + hdr + wr, pr) == 0;
                    printf("state: user patch store %s across save/restore\n",
                           same ? "SURVIVED byte for byte" : "DID NOT survive");
                }
                slot = state_by_suffix("settings");
                {
                    const int same = slot && settings_before
                            && strcmp(slot->val, settings_before) == 0;
                    printf("state: settings and cards %s across save/restore\n",
                           same ? "came back identical" : "DID NOT come back identical");
                    if (!same && slot && settings_before)
                        printf("state:   wanted:\n%s\nstate:   got:\n%s\n",
                               settings_before, slot->val);
                }
                d->cleanup(h2);
            }

            /* A project that has MOVED.  Sessions travel between machines and people
             * reorganise their ROM directories, so the recorded path is a first guess.
             * Point it somewhere that does not exist and the plugin should still find the
             * image by name on its search path -- and say so by reporting the real path
             * back. */
            if (settings_before != NULL && strstr(settings_before, "\ncard ") != NULL)
            {
                char *moved = malloc(strlen(settings_before) * 2 + 64);
                const char *p = settings_before;
                char *o = moved;
                int rewrote = 0;
                while (*p)
                {
                    const char *nl = strchr(p, '\n');
                    const size_t n = nl ? (size_t)(nl - p) : strlen(p);
                    if (strncmp(p, "card ", 5) == 0)
                    {
                        /* keep everything up to the path, then repoint the path */
                        const char *slash = memchr(p, '/', n);
                        if (slash != NULL)
                        {
                            const char *base = slash;
                            const char *q;
                            for (q = p; q < p + n; q++) if (*q == '/') base = q + 1;
                            memcpy(o, p, (size_t)(slash - p)); o += slash - p;
                            o += sprintf(o, "/nonexistent/moved/%.*s",
                                         (int)(p + n - base), base);
                            rewrote = 1;
                        }
                        else { memcpy(o, p, n); o += n; }
                    }
                    else { memcpy(o, p, n); o += n; }
                    *o++ = '\n';
                    if (!nl) break;
                    p = nl + 1;
                }
                *o = 0;

                if (rewrote)
                {
                    LV2_Handle h3 = d->instantiate(d, RATE, "./", features);
                    if (h3 == NULL) printf("state: third instantiate FAILED\n");
                    else
                    {
                        const struct StateSlot *sl = snapshot_by_suffix("settings");
                        struct StateSlot *got;
                        int i2;
                        d->connect_port(h3, 0, outL); d->connect_port(h3, 1, outR);
                        d->connect_port(h3, 2, ev_in); d->connect_port(h3, 3, ev_out);
                        d->connect_port(h3, 4, &latency);
                        d->connect_port(h3, 5, &volume); d->connect_port(h3, 6, &hf);
                        for (i2 = 0; i2 < 6; i2++) d->connect_port(h3, 7 + i2, &btn[i2]);

                        states_clear();
                        if (sl) store_cb(NULL, sl->urid, moved, strlen(moved),
                                         sl->type, sl->flags);
                        d->connect_port(h3, 4, &latency);
                        si->restore(h3, retrieve_cb, NULL, 0, NULL);

                        states_clear();
                        si->save(h3, store_cb, NULL, 0, NULL);
                        got = state_by_suffix("settings");
                        printf("state: cards named at a path that no longer exists were %s\n",
                               (got && strcmp(got->val, settings_before) == 0)
                               ? "found again on the search path"
                               : "NOT recovered");
                        d->cleanup(h3);
                    }
                }
                free(moved);
            }

            free(nvram_before);
            free(settings_before);
        }
    }

    if (rt_audit_report) rt_audit_report();
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    printf("reported latency: %.0f host frames\n", latency);
    { static const char *const nm[4] = { "PART", "EDIT", "MIDI", "CLIP" };
      printf("lamps (percentage of the run lit):\n");
      for (int b = 0; b < 4; b++)
          printf("    %-5s %5.1f%%\n", nm[b],
                 100.0 * (double)lit_blocks[b] / (double)(blocks_total ? blocks_total : 1));
      printf("  MIDI lamp transitions: %ld in the run (%.1f per second)\n",
             midi_edges, (double)midi_edges / SECONDS);
      printf("  MIDI lamp seen by a 10 Hz observer: %5.1f%% of %ld samples\n",
             100.0 * (double)slow_midi_lit / (double)(slow_total ? slow_total : 1), slow_total); }
    printf("events-out: %ld state blobs to the UI, %ld MIDI atoms to the host\n",
           state_atoms, midi_out_atoms);
    printf("peak: %d (%s)\n", peak, peak > 0 ? "PLUGIN MAKES SOUND" : "SILENT");

    FILE *o = fopen(wav, "wb");
    if (o) {
        unsigned bytes = (unsigned)(written * 4);
        unsigned r32; unsigned short r16;
        fwrite("RIFF", 1, 4, o); r32 = 36 + bytes; fwrite(&r32,4,1,o); fwrite("WAVE",1,4,o);
        fwrite("fmt ",1,4,o); r32=16; fwrite(&r32,4,1,o); r16=1; fwrite(&r16,2,1,o);
        r16=2; fwrite(&r16,2,1,o); r32=(unsigned)RATE; fwrite(&r32,4,1,o);
        r32=(unsigned)RATE*4; fwrite(&r32,4,1,o); r16=4; fwrite(&r16,2,1,o);
        r16=16; fwrite(&r16,2,1,o);
        fwrite("data",1,4,o); fwrite(&bytes,4,1,o); fwrite(pcm,1,bytes,o); fclose(o);
        printf("wrote %s\n", wav);
    }
    free(pcm);
    return peak > 0 ? 0 : 1;
}
