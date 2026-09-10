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
/* The host's buffer size.  Overridable because it is not cosmetic: everything the plugin
 * does between blocks happens at this granularity, so a small buffer is a different test
 * and not merely a slower one.  See the `cards` pass, which is run at both. */
#ifndef BLOCK
#define BLOCK    256
#endif
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

/* The panel, as the UI receives it: the plugin's PanelBlob, hex, inside a key/value
 * atom.  Decoding it here rather than scanning for a run of hex characters means the
 * test breaks LOUDLY if the struct grows, instead of quietly reading the wrong field. */
#define PANEL_BYTES 128
#define PANEL_PART_MEDIA 100               /* six bytes, then six of tone, then six flags */
#define PANEL_PART_TONE  106
#define PANEL_CARD_ID    124               /* four: the FIRMWARE's verdict per card slot */
static unsigned char g_panel[PANEL_BYTES];
static char g_patches[8192];              /* the patch names, one per line */
static char g_patchdump[512];             /* the active patch, as hex */
static char g_tones[16384];               /* the tone list, "G media label" / "T name" */
static char g_cardlist[8192];             /* "A num<tab>label<tab>path" / "S slot num..." */
static char g_cardpath[2][1024];          /* two images to play with, whatever is here */
static unsigned g_cardnum[2];
static int  g_ncards = 0;
static int  g_cardstage[5];               /* one per check, filled as the run goes */
static int  g_cardgroups[2];              /* tone groups with the card in, and after */

/* How many media the machine can currently offer tones from: the internal ROM is always
 * one, and each mounted card adds another. */
static int count_tone_groups(void)
{
    int n = 0;
    const char *p = g_tones;
    while (*p) {
        const char *nl;
        if (p[0] == 'G') n++;
        nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

static int hexbyte(const char *p)
{
    int v = 0, i;
    for (i = 0; i < 2; i++) {
        const char c = p[i];
        if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
        else return -1;
    }
    return v;
}

/* The two-line display as text, for saying what the machine is actually showing. */
static void panel_lcd(char *out)
{
    int i;
    for (i = 0; i < 32; i++) {
        const unsigned char c = g_panel[i];
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    out[32] = 0;
}

/* Put a DPF key/value message on the input sequence -- this is exactly what the plugin's
 * own UI sends when someone picks a patch from the menu. */
static void put_keyvalue(LV2_Atom_Sequence *seq, uint32_t urid_kv,
                         const char *key, const char *val)
{
    const uint32_t klen = (uint32_t)strlen(key) + 1;
    const uint32_t vlen = (uint32_t)strlen(val) + 1;
    LV2_Atom_Event *e = (LV2_Atom_Event *)((char *)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq));
    e->time.frames = 0;
    e->body.type = urid_kv;
    e->body.size = klen + vlen;
    memcpy(LV2_ATOM_BODY(&e->body), key, klen);
    memcpy((char *)LV2_ATOM_BODY(&e->body) + klen, val, vlen);
    seq->atom.size += (uint32_t)lv2_atom_pad_size(sizeof(LV2_Atom_Event) + klen + vlen);
}

/* Read whatever the plugin put on its events-out port: MIDI for the host, key/value
 * messages for the UI.  Every one of those is "key\0value\0", so read it as one rather
 * than hunting through the payload for something that looks right. */
static void absorb_events_out(void *ev_out, uint32_t urid_midi,
                              long *n_state, long *n_midi)
{
    LV2_Atom_Sequence *os = (LV2_Atom_Sequence *)ev_out;
    LV2_ATOM_SEQUENCE_FOREACH(os, e)
    {
        const char *key, *val;
        if (e->body.type == urid_midi) { if (n_midi) (*n_midi)++; continue; }
        if (n_state) (*n_state)++;
        key = (const char *)LV2_ATOM_BODY_CONST(&e->body);
        val = key + strlen(key) + 1;
        if (strcmp(key, "panel") == 0 && strlen(val) == PANEL_BYTES * 2) {
            int i;
            for (i = 0; i < PANEL_BYTES; i++) {
                const int v = hexbyte(val + i * 2);
                if (v < 0) break;
                g_panel[i] = (unsigned char)v;
            }
        }
        else if (strcmp(key, "patches") == 0 && strlen(val) < sizeof(g_patches))
            strcpy(g_patches, val);
        else if (strcmp(key, "tones") == 0 && strlen(val) < sizeof(g_tones))
            strcpy(g_tones, val);
        else if (strcmp(key, "cardlist") == 0 && strlen(val) < sizeof(g_cardlist))
            strcpy(g_cardlist, val);
        else if (strcmp(key, "patchdump") == 0 && strlen(val) < sizeof(g_patchdump))
            strcpy(g_patchdump, val);
    }
}

/* Run an instance on its own for a while, with empty input, and keep whatever it sends
 * to the UI.  Used after a restore, to see what the machine came up showing. */
static void run_quietly(const LV2_Descriptor *d, LV2_Handle h, void *ev_in, void *ev_out,
                        uint32_t urid_seq, uint32_t urid_midi, double seconds)
{
    long b;
    memset(g_panel, 0, sizeof(g_panel));
    for (b = 0; b < (long)(seconds * RATE) / BLOCK; b++)
    {
        LV2_Atom_Sequence *is = (LV2_Atom_Sequence *)ev_in;
        LV2_Atom_Sequence *os = (LV2_Atom_Sequence *)ev_out;
        is->atom.type = urid_seq;
        is->atom.size = sizeof(LV2_Atom_Sequence_Body);
        is->body.unit = 0;
        is->body.pad  = 0;
        os->atom.type = urid_seq;
        os->atom.size = ATOM_CAP - sizeof(LV2_Atom);
        d->run(h, BLOCK);
        absorb_events_out(ev_out, urid_midi, NULL, NULL);
    }
}

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
    const int want_patch_test = (argc > 5) && strcmp(argv[5], "patch") == 0;
    const int want_tone_test  = (argc > 5) && strcmp(argv[5], "tone") == 0;
    const int want_write_test = (argc > 5) && strcmp(argv[5], "write") == 0;
    const int want_load_test  = (argc > 5) && strcmp(argv[5], "load") == 0;
    const int want_meter_test = (argc > 5) && strcmp(argv[5], "meter") == 0;
    const int want_card_test  = (argc > 5) && strcmp(argv[5], "cards") == 0;
    int patch_checks = 0, patch_pass = 0;

    /* The stereo meter and its peak-hold marker, as OUTPUT control ports.  A host reads
     * these whenever it likes, so the ballistics have to be finished by the time the
     * value lands here -- that is the whole reason they live in the DSP.  Ports 13-16. */
    float meter[2] = { 0, 0 }, hold[2] = { 0, 0 };

    /* The whole run as a series, sampled once per block, so the checks can measure the
     * ballistics instead of guessing when to look.  Fixed wall-clock sample points were
     * the first attempt and they were wrong: they assumed a sustained note, and the
     * machine's own patch decays -- the bar had reached the floor before the note was
     * even released.  What the constants promise is a RATE, so a rate is what to test. */
#define MSAMP 4096
    static float m_t[MSAMP], m_bar[MSAMP][2], m_hld[MSAMP][2];
    int m_n = 0;


    d->connect_port(h, 0, outL);
    d->connect_port(h, 1, outR);
    d->connect_port(h, 2, ev_in);
    d->connect_port(h, 3, ev_out);
    d->connect_port(h, 4, &latency);
    d->connect_port(h, 5, &volume);
    d->connect_port(h, 6, &hf);
    for (int i = 0; i < 6; i++) d->connect_port(h, 7 + i, &btn[i]);
    /* The panel itself arrives as a blob on events-out; these four are the exception,
     * because a level really is a scalar and that is what an output port is for. */
    d->connect_port(h, 13, &meter[0]);
    d->connect_port(h, 14, &meter[1]);
    d->connect_port(h, 15, &hold[0]);
    d->connect_port(h, 16, &hold[1]);
    if (d->activate) d->activate(h);

    const uint32_t urid_seq   = map_uri(NULL, LV2_ATOM__Sequence);
    const uint32_t urid_frame = map_uri(NULL, LV2_ATOM__frameTime);
    const uint32_t urid_midi  = map_uri(NULL, LV2_MIDI__MidiEvent);
    /* What DPF calls a key/value message, in both directions. */
    const uint32_t urid_kv    = map_uri(NULL, "urn:distrho:KeyValueState");

    const long total = (long)(SECONDS * RATE);
    short *pcm = malloc(sizeof(short) * 2 * total);
    long written = 0;
    int peak = 0;
    unsigned seen_leds = 0;
    long blocks_total = 0, lit_blocks[4] = { 0, 0, 0, 0 };
    long slow_total = 0, slow_midi_lit = 0;
    double next_slow = 0.0;
    unsigned last_midi = 0; long midi_edges = 0;
    long state_atoms = 0, midi_out_atoms = 0;

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
        btn[1] = (!want_fx && !want_patch_test && !want_tone_test && !want_write_test
                  && !want_load_test && !want_meter_test && !want_card_test
                  && done >= (long)(7.0 * RATE) && done < (long)(8.0 * RATE)) ? 1.0f : 0.0f;

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

        /* Choosing patches the way the UI does: a key/value message on the input port,
         * and then nothing but time.  The plugin has to reach the patch by pressing the
         * machine's own buttons in emulated time, so the check is deliberately made a
         * second and a half later rather than on the next block. */
        /* A session where the user chose a patch, which is the interesting kind.  P-43
         * is far from the power-on P-01, so "it came back on the right one" cannot be
         * satisfied by the machine simply booting. */
        if (want_state_test)
        {
            const double t = (double)done / RATE, dt = (double)BLOCK / RATE;
            if (t <= 9.0 && 9.0 < t + dt)
                put_keyvalue(seq, urid_kv, "patchsel", "42");
        }

        /* Tones go to ONE PART, over the machine's own SysEx, with no button pressed and
         * nothing assumed about what the display is showing.  The checks are made half a
         * second later: eleven bytes have to clock in at 31250 baud and be parsed. */
        if (want_tone_test)
        {
            const double t = (double)done / RATE, dt = (double)BLOCK / RATE;
            if (t <= 9.0 && 9.0 < t + dt)
                put_keyvalue(seq, urid_kv, "tonesel", "0 0 40");    /* part 1 <- SLAP 9 */
            else if (t <= 10.0 && 10.0 < t + dt) {
                patch_checks++;
                patch_pass += (g_panel[PANEL_PART_MEDIA] == 0 && g_panel[PANEL_PART_TONE] == 40);
                printf("tone: part 1 -> media %u tone %u\n",
                       g_panel[PANEL_PART_MEDIA], g_panel[PANEL_PART_TONE]);
            }
            else if (t <= 11.0 && 11.0 < t + dt)
                put_keyvalue(seq, urid_kv, "tonesel", "2 8 3");     /* part 3 <- a CARD tone */
            else if (t <= 12.0 && 12.0 < t + dt) {
                patch_checks++;
                patch_pass += (g_panel[PANEL_PART_MEDIA + 2] == 8
                               && g_panel[PANEL_PART_TONE + 2] == 3);
                printf("tone: part 3 -> media %u tone %u (a card tone, which a program "
                       "change cannot reach)\n",
                       g_panel[PANEL_PART_MEDIA + 2], g_panel[PANEL_PART_TONE + 2]);
                patch_checks++;
                patch_pass += (g_panel[PANEL_PART_MEDIA] == 0 && g_panel[PANEL_PART_TONE] == 40);
                printf("tone: part 1 still on media %u tone %u\n",
                       g_panel[PANEL_PART_MEDIA], g_panel[PANEL_PART_TONE]);
            }
        }

        /* Changing cards the way a user does: a path per slot, and then TIME.  Every
         * check below is on the FIRMWARE's own cache of what it thinks is in each slot,
         * which the panel blob carries, because that is the only opinion that decides
         * whether a card's tones can be played.  The plugin's own bookkeeping agreeing
         * with itself would prove nothing.
         *
         * A note is sounding from 12 s to 16 s, which covers the swap and the removal on
         * purpose: those are the cases that were broken until the CPU's PORT1 model was
         * fixed, and a test that only ever swapped cards in silence would not have
         * noticed. */
        if (want_card_test)
        {
            const double t = (double)done / RATE, dt = (double)BLOCK / RATE;
            const int at = (t <= 8.0 && 8.0 < t + dt) ? 0
                         : (t <= 8.6 && 8.6 < t + dt) ? 1
                         : (t <= 9.2 && 9.2 < t + dt) ? 2
                         : (t <= 9.8 && 9.8 < t + dt) ? 3
                         : (t <= 10.4 && 10.4 < t + dt) ? 4
                         : (t <= 11.5 && 11.5 < t + dt) ? 5
                         : (t <= 12.5 && 12.5 < t + dt) ? 6
                         : (t <= 13.5 && 13.5 < t + dt) ? 7
                         : (t <= 14.0 && 14.0 < t + dt) ? 8
                         : (t <= 15.0 && 15.0 < t + dt) ? 9
                         : (t <= 15.2 && 15.2 < t + dt) ? 10
                         : (t <= 15.4 && 15.4 < t + dt) ? 11
                         : (t <= 15.6 && 15.6 < t + dt) ? 12
                         : (t <= 16.6 && 16.6 < t + dt) ? 13 : -1;
            char req[1100];
            int i;
            switch (at)
            {
            case 0:
                put_keyvalue(seq, urid_kv, "cardscan", "1");
                break;
            case 1:
                /* Whatever this machine actually has.  Asking the plugin what it can
                 * offer keeps the test off any particular filename.
                 *
                 * Guarded because the window test above can match two consecutive blocks
                 * when the buffer is small: t advances by (done+BLOCK)/RATE, which is not
                 * bit-for-bit t + BLOCK/RATE, so the intervals can overlap by an ulp. */
                if (g_ncards != 0) break;
                for (const char *p = g_cardlist; *p; ) {
                    const char *nl = strchr(p, '\n');
                    unsigned num = 0;
                    const char *tab = strchr(p, '\t');
                    if (p[0] == 'A' && tab && (!nl || tab < nl)) {
                        const char *last = strchr(tab + 1, '\t');
                        num = (unsigned)strtoul(p + 2, NULL, 10);
                        if (last && (!nl || last < nl)) {
                            const size_t n = nl ? (size_t)(nl - (last + 1))
                                                : strlen(last + 1);
                            if (g_ncards == 0
                                    || (g_ncards == 1 && num != g_cardnum[0])) {
                                if (n < sizeof(g_cardpath[0])) {
                                    memcpy(g_cardpath[g_ncards], last + 1, n);
                                    g_cardpath[g_ncards][n] = 0;
                                    g_cardnum[g_ncards] = num;
                                    g_ncards++;
                                }
                            }
                        }
                    }
                    if (!nl || g_ncards >= 2) break;
                    p = nl + 1;
                }
                printf("cards: the plugin offers %d usable image%s", g_ncards,
                       g_ncards == 1 ? "" : "s");
                for (i = 0; i < g_ncards; i++) printf("%s %02u", i ? "," : ":", g_cardnum[i]);
                printf("\n");
                /* fall through to the ejects */
            case 2: case 3: case 4:
                snprintf(req, sizeof(req), "%d -", at - 1);
                put_keyvalue(seq, urid_kv, "cardset", req);
                break;
            case 5:
                g_cardstage[0] = (g_panel[PANEL_CARD_ID] == 0 && g_panel[PANEL_CARD_ID+1] == 0
                               && g_panel[PANEL_CARD_ID+2] == 0 && g_panel[PANEL_CARD_ID+3] == 0);
                printf("cards: with every slot emptied the machine reads %02X %02X %02X %02X\n",
                       g_panel[PANEL_CARD_ID], g_panel[PANEL_CARD_ID+1],
                       g_panel[PANEL_CARD_ID+2], g_panel[PANEL_CARD_ID+3]);
                break;
            case 6:
                if (g_ncards >= 1) {
                    snprintf(req, sizeof(req), "2 %s", g_cardpath[0]);
                    put_keyvalue(seq, urid_kv, "cardset", req);
                }
                break;
            case 7:
                g_cardstage[1] = (g_ncards >= 1 && g_panel[PANEL_CARD_ID+2] == g_cardnum[0]);
                g_cardgroups[0] = count_tone_groups();
                printf("cards: slot 3 <- card %02u, machine reads %02X, %d tone group(s)\n",
                       g_ncards ? g_cardnum[0] : 0, g_panel[PANEL_CARD_ID+2],
                       g_cardgroups[0]);
                break;
            case 8:
                /* The swap, with NO eject in between.  The plugin has to take the card
                 * out, wait for the firmware to notice, and only then put the other one
                 * in; do it in one step and the machine goes on serving the first card's
                 * ID from the second card's bytes. */
                if (g_ncards >= 2) {
                    snprintf(req, sizeof(req), "2 %s", g_cardpath[1]);
                    put_keyvalue(seq, urid_kv, "cardset", req);
                }
                break;
            case 9:
                g_cardstage[2] = (g_ncards >= 2 && g_panel[PANEL_CARD_ID+2] == g_cardnum[1]);
                printf("cards: swapped to card %02u without ejecting first, machine reads %02X\n",
                       g_ncards >= 2 ? g_cardnum[1] : 0, g_panel[PANEL_CARD_ID+2]);
                break;
            case 10:
                put_keyvalue(seq, urid_kv, "cardset", "2 /nonexistent/not-a-card.bin");
                break;
            case 11:
                g_cardstage[3] = (g_ncards >= 2 && g_panel[PANEL_CARD_ID+2] == g_cardnum[1]);
                printf("cards: after a path that does not exist, slot 3 still reads %02X\n",
                       g_panel[PANEL_CARD_ID+2]);
                break;
            case 12:
                put_keyvalue(seq, urid_kv, "cardset", "2 -");
                break;
            case 13:
                g_cardstage[4] = (g_panel[PANEL_CARD_ID+2] == 0);
                g_cardgroups[1] = count_tone_groups();
                printf("cards: pulled while a note was sounding, machine reads %02X, "
                       "%d tone group(s)\n", g_panel[PANEL_CARD_ID+2], g_cardgroups[1]);
                break;
            default: break;
            }
        }

        if (want_load_test)
        {
            /* A patch coming back OUT of the library: the DSP is handed 116 bytes and a
             * slot, and must store them and have the firmware load them.  The bytes here
             * stand in for a file the UI read -- the plugin's own patchdump, with the
             * ten-byte name changed, so the result is checkable by reading the name back
             * off the LCD and out of the patch list. */
            const double t = (double)done / RATE;
            const double dt = (double)BLOCK / RATE;
            char lcd[33];
            panel_lcd(lcd);

            if (t <= 10.0 && 10.0 < t + dt) {
                patch_checks++;
                patch_pass += (strlen(g_patchdump) == 116 * 2);
                printf("load: the machine published %zu hex characters of patch\n",
                       strlen(g_patchdump));
            }
            else if (t <= 11.0 && 11.0 < t + dt) {
                /* "Library Pt" over the name at +0x04, ten bytes. */
                static const char *const kName = "Library Pt";
                char msg[16 + 116 * 2 + 8];
                char rec[116 * 2 + 1];
                int i;
                strcpy(rec, g_patchdump);
                for (i = 0; i < 10; i++) {
                    static const char *const hexd = "0123456789abcdef";
                    rec[(4 + i) * 2]     = hexd[(unsigned char)kName[i] >> 4];
                    rec[(4 + i) * 2 + 1] = hexd[(unsigned char)kName[i] & 15];
                }
                snprintf(msg, sizeof(msg), "63 %s", rec);
                put_keyvalue(seq, urid_kv, "patchload", msg);
            }
            else if (t <= 15.0 && 15.0 < t + dt) {
                patch_checks++;
                patch_pass += (g_panel[99] == 63
                               && strncmp(lcd, "P-64:Library Pt", 15) == 0);
                printf("load: the audition slot now shows [%.16s] patch byte %u\n",
                       lcd, g_panel[99]);
            }
        }

        if (want_write_test)
        {
            /* The WRITE button, through the same key the UI sends.
             *
             * What makes this checkable without reading patchram is the NAME.  The edit
             * buffer starts as P-01 "Ac.Piano"; storing it into P-06 must therefore rename
             * slot 6, which shows up in the patch list AND on the LCD -- and it can only
             * happen if the record really was copied and the machine really did reload it.
             * See analysis/SYSTEM-DESIGN.md section 5.3.4. */
            const double t = (double)done / RATE;
            const double dt = (double)BLOCK / RATE;
            char lcd[33];
            panel_lcd(lcd);

            if (t <= 9.0 && 9.0 < t + dt)
                put_keyvalue(seq, urid_kv, "divewrite", "p0 07 51");   /* part 1 level */
            else if (t <= 11.0 && 11.0 < t + dt) {
                patch_checks++;
                patch_pass += (strncmp(lcd, "TEMP:", 5) == 0);
                printf("write: after an edit the machine shows [%.16s]\n", lcd);
            }
            else if (t <= 12.0 && 12.0 < t + dt)
                put_keyvalue(seq, urid_kv, "patchwrite", "5");         /* -> P-06 */
            else if (t <= 16.0 && 16.0 < t + dt) {
                patch_checks++;
                patch_pass += (g_panel[99] == 5
                               && strncmp(lcd, "P-06:Ac.Piano", 13) == 0);
                printf("write: after the write it shows [%.16s] patch byte %u\n",
                       lcd, g_panel[99]);
            }
        }

        if (want_patch_test)
        {
            const double t = (double)done / RATE;
            const double dt = (double)BLOCK / RATE;
            char lcd[33];
            panel_lcd(lcd);

            /* EDIT/EXIT pressed at 12 s puts the machine in its menus, which is the case
             * that matters: [INC] edits a value there, so the plugin must walk back out
             * to the play screen before it presses anything. */
            btn[1] = (t >= 12.0 && t < 12.6) ? 1.0f : 0.0f;

            if (t <= 9.0 && 9.0 < t + dt)
                put_keyvalue(seq, urid_kv, "patchsel", "42");      /* P-43 Brass */
            else if (t <= 11.0 && 11.0 < t + dt) {
                patch_checks++;
                patch_pass += (g_panel[99] == 42 && strncmp(lcd, "P-43:Brass", 10) == 0);
                printf("patch: from the play screen -> [%.16s] patch byte %u\n",
                       lcd, g_panel[99]);
            }
            else if (t <= 13.0 && 13.0 < t + dt) {
                printf("patch: after EDIT/EXIT the machine shows [%.16s]\n", lcd);
                patch_checks++;
                patch_pass += (strncmp(lcd, "P-", 2) != 0);        /* i.e. in a menu */
            }
            else if (t <= 13.5 && 13.5 < t + dt)
                put_keyvalue(seq, urid_kv, "patchsel", "0");       /* P-01 Ac.Piano */
            else if (t <= 16.0 && 16.0 < t + dt) {
                patch_checks++;
                patch_pass += (g_panel[99] == 0 && strncmp(lcd, "P-01:Ac.Piano", 13) == 0);
                printf("patch: from a menu page  -> [%.16s] patch byte %u\n",
                       lcd, g_panel[99]);
            }
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

        absorb_events_out(ev_out, urid_midi, &state_atoms, &midi_out_atoms);

        /* Run any scheduled work here -- off the audio thread, as a host would. */
        if (g_work_size != 0)
        {
            const LV2_Worker_Interface *wi =
                    d->extension_data ? d->extension_data(LV2_WORKER__interface) : NULL;
            if (wi && wi->work)
                wi->work(h, work_respond, NULL, g_work_size, g_work);
            g_work_size = 0;
        }
        if (want_meter_test && m_n < MSAMP)
        {
            /* The ports are read AFTER run(), which is where a host reads them too. */
            m_t[m_n] = (float)((double)done / RATE);
            m_bar[m_n][0] = meter[0]; m_bar[m_n][1] = meter[1];
            m_hld[m_n][0] = hold[0];  m_hld[m_n][1] = hold[1];
            m_n++;
        }

        {   /* Lamp bits, and how much of the run each was lit -- "ever seen" cannot show
             * a polarity mistake, and every lamp on this machine is active low. */
            const unsigned l = g_panel[96] & 0x0f;      /* struct field `leds` */
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
                d->connect_port(h2, 13, &meter[0]); d->connect_port(h2, 14, &meter[1]);
                d->connect_port(h2, 15, &hold[0]);  d->connect_port(h2, 16, &hold[1]);

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
                /* And the patch the session was left on.  This is the check that
                 * catches an empty "patchsel": the host stores that key like any other,
                 * hands the empty value straight back on restore, and atoi("") is a
                 * perfectly good zero -- which would silently reopen every project on
                 * P-01 no matter what was saved. */
                {
                    char lcd[33];
                    run_quietly(d, h2, ev_in, ev_out, urid_seq, urid_midi, 0.5);
                    panel_lcd(lcd);
                    printf("state: the restored session came up on [%.16s]%s\n", lcd,
                           strncmp(lcd, "P-43:Brass", 10) == 0
                                   ? "" : "   <-- NOT THE SAVED PATCH");
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
                        d->connect_port(h3, 13, &meter[0]); d->connect_port(h3, 14, &meter[1]);
                        d->connect_port(h3, 15, &hold[0]);  d->connect_port(h3, 16, &hold[1]);

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

    if (want_tone_test)
    {
        /* The list: one group per media, the internal 99 and then whatever is mounted. */
        const char *p = g_tones;
        int groups = 0, in_group = 0, internal = 0, card_total = 0;
        char first_card[64] = "";
        while (*p) {
            const char *nl = strchr(p, '\n');
            const int n = nl ? (int)(nl - p) : (int)strlen(p);
            if (p[0] == 'G') {
                if (groups == 1) internal = in_group;
                else if (groups > 1) card_total += in_group;
                if (groups == 1) snprintf(first_card, sizeof(first_card), "%.*s", n - 2, p + 2);
                in_group = 0;
                groups++;
                printf("tone: %.*s\n", n, p);
            }
            else in_group++;
            if (!nl) break;
            p = nl + 1;
        }
        if (groups == 1) internal = in_group; else card_total += in_group;
        printf("tone: %d groups, %d internal tones, %d card tones (second group: %s)\n",
               groups, internal, card_total, first_card);
        patch_checks++;
        patch_pass += (groups >= 2 && internal == 99);
        printf("tone: %d of %d checks passed%s\n", patch_pass, patch_checks,
               patch_pass == patch_checks ? "" : "   <-- FAILED");
    }

    if (want_load_test)
    {
        /* Slot 64 renamed, slot 1 not: a load that went to the wrong place, or to all of
         * them, would pass a check that only looked at the destination. */
        char name64[32] = { 0 }, name1[32] = { 0 };
        int line = 0;
        for (const char *p = g_patches; *p; ) {
            const char *nl = strchr(p, '\n');
            const size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (line == 0  && n < sizeof(name1))  memcpy(name1, p, n);
            if (line == 63 && n < sizeof(name64)) memcpy(name64, p, n);
            line++;
            if (!nl) break;
            p = nl + 1;
        }
        printf("load: P-01 is \"%s\", P-64 is now \"%s\"\n", name1, name64);
        patch_checks++;
        patch_pass += (strcmp(name64, "Library Pt") == 0 && strcmp(name1, "Ac.Piano") == 0);
        printf("load: %d of %d checks passed%s\n", patch_pass, patch_checks,
               patch_pass == patch_checks ? "" : "   <-- FAILED");
    }

    if (want_card_test)
    {
        int cc = 0, cp = 0;
        if (g_ncards < 2) {
            printf("cards: fewer than two card images on the search path -- nothing to "
                   "swap between, so this proves nothing.  Put two SN-U110 dumps in the "
                   "ROM directory.\n");
        }
        cc++; cp += (g_ncards >= 2);
        if (g_ncards < 2) printf("cards: the plugin offered fewer than two images\n");

        cc++; cp += g_cardstage[0];
        if (!g_cardstage[0]) printf("cards: emptying every slot did not clear the "
                                    "firmware's cache\n");
        cc++; cp += g_cardstage[1];
        if (!g_cardstage[1]) printf("cards: the machine did not mount the card that was "
                                    "put in slot 3\n");
        cc++; cp += g_cardstage[2];
        if (!g_cardstage[2]) printf("cards: after a swap the machine is still serving the "
                                    "OLD card's ID -- the eject/settle/insert sequence is "
                                    "not working\n");
        cc++; cp += g_cardstage[3];
        if (!g_cardstage[3]) printf("cards: a request naming a file that does not exist "
                                    "changed the slot, or wedged the state machine\n");
        cc++; cp += g_cardstage[4];
        if (!g_cardstage[4]) printf("cards: removing a card while a note sounded was not "
                                    "noticed\n");
        cc++; cp += (g_cardgroups[0] == 2 && g_cardgroups[1] == 1);
        if (!(g_cardgroups[0] == 2 && g_cardgroups[1] == 1))
            printf("cards: the tone list did not follow the card in and out "
                   "(%d groups with it, %d without)\n", g_cardgroups[0], g_cardgroups[1]);

        printf("cards: %d of %d checks passed%s\n", cp, cc,
               cp == cc ? "" : "   <-- FAILED");
    }

    if (want_meter_test)
    {
        /* Everything here is provable from the PORTS alone, the way a host sees them,
         * and from the constants the DSP promises: floor -42, ceiling +12, bar 20 dB/s,
         * dwell 1.5 s, marker 12 dB/s.  Nothing depends on WHEN the machine makes a
         * sound or for how long, only on how the meter responds to what it made. */
        const float FLOORDB = -42.0f, CEILDB = 12.0f;
        /* Duplicated from the DSP on purpose -- the test has no way to read a
         * constexpr, and a check that derived its expectation from the code under test
         * would pass whatever that code did.  These are what the ballistics PROMISE. */
        const double BARFALL = 60.0, HOLDFALL = 12.0, DWELL = 1.5;
        const double dt = (double)BLOCK / RATE;
        int mc = 0, mp = 0, c, i;

        int    ok_range = 1, ok_below = 0, ok_idle = 1, ok_barrate = 1;
        int    ok_dwell = 1, ok_rose = 1, ok_holdrate = 1, ok_marker = 1;
        float  vpeak[2] = { -99.0f, -99.0f };
        int    ipeak[2] = { 0, 0 };
        double hrate[2] = { 0.0, 0.0 };

        for (c = 0; c < 2; c++)
        {
            for (i = 0; i < m_n; i++)
            {
                /* 1. A port may never leave the range it advertised to the host. */
                if (m_bar[i][c] < FLOORDB - 0.01f || m_bar[i][c] > CEILDB + 0.01f) ok_range = 0;
                if (m_hld[i][c] < FLOORDB - 0.01f || m_hld[i][c] > CEILDB + 0.01f) ok_range = 0;

                /* 2. The marker may never sit BELOW the bar.  If it could, a rising
                 *    signal would swallow it and a peak hold that can be swallowed is
                 *    not a peak hold. */
                if (m_hld[i][c] < m_bar[i][c] - 0.01f) ok_below++;

                /* 3. Silence -- after the boot, before the note -- reads the floor
                 *    EXACTLY.  A meter that idles above the floor never resets. */
                if (m_t[i] > 10.0f && m_t[i] < 11.9f
                    && (m_bar[i][c] != FLOORDB || m_hld[i][c] != FLOORDB)) ok_idle = 0;

                if (m_bar[i][c] > vpeak[c]) { vpeak[c] = m_bar[i][c]; ipeak[c] = i; }
            }

            /* 4. Something lifted it off the floor at all: the check that fails if the
             *    ports were never written. */
            if (vpeak[c] <= FLOORDB + 1.0f) ok_rose = 0;

            /* 5. The bar never falls FASTER than its constant, at any pair of adjacent
             *    samples.  An upper bound rather than a rate, because between the peak
             *    and the floor the bar may be tracking a signal that decays more slowly
             *    than the ballistics would -- and it should. */
            for (i = 1; i < m_n; i++)
                if (m_bar[i][c] < m_bar[i-1][c] - (float)(BARFALL * dt) - 0.02f)
                    ok_barrate = 0;

            /* 6. The dwell: once past the peak nothing re-arms the marker, so it must
             *    stand still for the full 1.5 s. */
            for (i = ipeak[c]; i < m_n && m_t[i] < m_t[ipeak[c]] + (float)DWELL - 0.05f; i++)
                if (m_hld[i][c] != vpeak[c]) ok_dwell = 0;

            /* 7. ...and then fall, at its own rate.  Deterministic once the peak has
             *    passed, so this one is measured against BOTH bounds. */
            {
                int a = -1, b = -1;
                for (i = ipeak[c]; i < m_n; i++)
                {
                    if (m_hld[i][c] < vpeak[c] - 1.0f && a < 0) a = i;
                    if (a >= 0 && m_hld[i][c] > FLOORDB + 1.0f) b = i;
                }
                if (a < 0 || b <= a) ok_holdrate = 0;
                else
                {
                    hrate[c] = (m_hld[a][c] - m_hld[b][c]) / (m_t[b] - m_t[a]);
                    if (hrate[c] < HOLDFALL * 0.95 || hrate[c] > HOLDFALL * 1.05)
                        ok_holdrate = 0;
                    /* 8. Slower than the bar, which is what keeps it readable: over the
                     *    same window the bar must already be below it. */
                    if (!(m_hld[b][c] > m_bar[b][c])) ok_marker = 0;
                }
            }
        }

        printf("meter: %d samples, peak %.1f/%.1f dB at %.2f/%.2f s\n",
               m_n, vpeak[0], vpeak[1], m_t[ipeak[0]], m_t[ipeak[1]]);
        printf("meter: marker falls at %.1f/%.1f dB/s (asked for %.1f)\n",
               hrate[0], hrate[1], HOLDFALL);

        mc++; mp += ok_range;
        mc++; mp += (ok_below == 0);
        mc++; mp += ok_idle;
        mc++; mp += ok_rose;
        mc++; mp += ok_barrate;
        mc++; mp += ok_dwell;
        mc++; mp += ok_holdrate;
        mc++; mp += ok_marker;

        if (!ok_range)    printf("meter: a port left its declared range\n");
        if (ok_below)     printf("meter: marker below the bar in %d samples\n", ok_below);
        if (!ok_idle)     printf("meter: silence does not read the floor\n");
        if (!ok_rose)     printf("meter: the bar never moved -- ports not written?\n");
        if (!ok_barrate)  printf("meter: the bar fell faster than %.0f dB/s\n", BARFALL);
        if (!ok_dwell)    printf("meter: the marker moved during its dwell\n");
        if (!ok_holdrate) printf("meter: the marker's fall rate is wrong\n");
        if (!ok_marker)   printf("meter: the marker did not stay above the bar\n");

        printf("meter: %d of %d checks passed%s\n", mp, mc,
               mp == mc ? "" : "   <-- FAILED");
        patch_checks += mc; patch_pass += mp;
    }

    if (want_write_test)
    {
        /* Slot 6 must now be named after what was stored into it, and slot 1 must not
         * have moved -- a write that hit every slot, or the wrong one, would pass a
         * check that only looked at the destination. */
        char name6[32] = { 0 }, name1[32] = { 0 };
        int line = 0;
        for (const char *p = g_patches; *p; ) {
            const char *nl = strchr(p, '\n');
            const size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (line == 0 && n < sizeof(name1)) memcpy(name1, p, n);
            if (line == 5 && n < sizeof(name6)) memcpy(name6, p, n);
            line++;
            if (!nl) break;
            p = nl + 1;
        }
        printf("write: P-01 is \"%s\", P-06 is now \"%s\"\n", name1, name6);
        patch_checks++;
        patch_pass += (strcmp(name6, "Ac.Piano") == 0 && strcmp(name1, "Ac.Piano") == 0);
        printf("write: %d of %d checks passed%s\n", patch_pass, patch_checks,
               patch_pass == patch_checks ? "" : "   <-- FAILED");
    }

    if (want_patch_test)
    {
        /* The list itself: 64 names, in the machine's order, as the menu shows them. */
        int lines = 0;
        const char *p = g_patches, *first = g_patches, *last = g_patches;
        size_t firstn = 0, lastn = 0;
        while (*p) {
            const char *nl = strchr(p, '\n');
            const size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (lines == 0) { first = p; firstn = n; }
            last = p; lastn = n;
            lines++;
            if (!nl) break;
            p = nl + 1;
        }
        printf("patch: the list arrived with %d names, \"%.*s\" .. \"%.*s\"\n",
               lines, (int)firstn, first, (int)lastn, last);
        patch_checks++;
        patch_pass += (lines == 64 && strncmp(first, "Ac.Piano", 8) == 0
                       && strncmp(last, "Multi-Set5", 10) == 0);
        printf("patch: %d of %d checks passed%s\n", patch_pass, patch_checks,
               patch_pass == patch_checks ? "" : "   <-- FAILED");
    }
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
