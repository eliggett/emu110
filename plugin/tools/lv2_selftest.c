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
#define SECONDS  16.0
#define ATOM_CAP 4096

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
    LV2_Feature f_opts = { LV2_OPTIONS__options, (void *)opts };
    LV2_Feature f_bounded = { LV2_BUF_SIZE__boundedBlockLength, NULL };
    const LV2_Feature *features[] = { &f_map, &f_opts, &f_bounded, NULL };

    LV2_Handle h = d->instantiate(d, RATE, "./", features);
    if (!h) { fprintf(stderr, "instantiate failed\n"); return 1; }

    static float outL[BLOCK], outR[BLOCK];
    static unsigned char ev_in[ATOM_CAP], ev_out[ATOM_CAP];
    float latency = 0, volume = (argc > 4) ? (float)atof(argv[4]) : 0.0f, hf = 1.0f;
    float btn[6] = { 0, 0, 0, 0, 0, 0 };
    static float outp[16];

    d->connect_port(h, 0, outL);
    d->connect_port(h, 1, outR);
    d->connect_port(h, 2, ev_in);
    d->connect_port(h, 3, ev_out);
    d->connect_port(h, 4, &latency);
    d->connect_port(h, 5, &volume);
    d->connect_port(h, 6, &hf);
    for (int i = 0; i < 6; i++) d->connect_port(h, 7 + i, &btn[i]);
    for (int i = 0; i < 15; i++) d->connect_port(h, 13 + i, &outp[i]);
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

    for (long done = 0; done < total; done += BLOCK)
    {
        /* Empty input sequence, except where a note goes. */
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)ev_in;
        seq->atom.type = urid_seq;
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->body.unit = 0;
        seq->body.pad  = 0;

        /* Note on at 12 s, off at 15 s -- after the machine has finished booting. */
        const long t12 = (long)(12.0 * RATE), t15 = (long)(15.0 * RATE);
        const unsigned char *msg = NULL;
        static const unsigned char on[3]  = { 0x90, 60, 100 };
        static const unsigned char off[3] = { 0x80, 60, 0 };
        /* Press EDIT/EXIT at 7 s and release at 8 s: the buttons are what drive the
         * machine's own menus, and this proves the whole loop from a host control port
         * through to the LCD. */
        btn[1] = (done >= (long)(7.0 * RATE) && done < (long)(8.0 * RATE)) ? 1.0f : 0.0f;

        if (done <= t12 && t12 < done + BLOCK) msg = on;
        if (done <= t15 && t15 < done + BLOCK) msg = off;
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
        {   /* Lamp bits, and how much of the run each was lit -- "ever seen" cannot show
             * a polarity mistake, and every lamp on this machine is active low. */
            const unsigned l = ((unsigned)outp[11]) & 0x0f;
            seen_leds |= l;
            blocks_total++;
            for (int b = 0; b < 4; b++) if (l & (1u << b)) lit_blocks[b]++;
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

    if (rt_audit_report) rt_audit_report();
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    printf("reported latency: %.0f host frames\n", latency);
    { static const char *const nm[4] = { "PART", "EDIT", "MIDI", "CLIP" };
      printf("lamps (percentage of the run lit):\n");
      for (int b = 0; b < 4; b++)
          printf("    %-5s %5.1f%%\n", nm[b],
                 100.0 * (double)lit_blocks[b] / (double)(blocks_total ? blocks_total : 1)); }
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
