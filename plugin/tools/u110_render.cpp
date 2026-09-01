// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render through U110Core, for the null test and for seeing whether it boots at all.
//
//   u110_render --roms DIR [--seconds N] [--out FILE.wav] [--lcd]
//
// --lcd prints the LCD text once a second, which is the fastest way to tell whether the
// machine is alive: a working U-110 shows its boot banner and then the play screen.

#include "u110_core.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const std::string &path)
{
    std::vector<uint8_t> data;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(size_t(n < 0 ? 0 : n));
    if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size())
        data.clear();
    std::fclose(f);
    return data;
}

void write_wav(const std::string &path, const std::vector<int16_t> &interleaved, uint32_t rate)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    const uint32_t bytes = uint32_t(interleaved.size() * 2);
    auto u32w = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16w = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
    u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
    std::fwrite("data", 1, 4, f); u32w(bytes);
    std::fwrite(interleaved.data(), 1, bytes, f);
    std::fclose(f);
}

// MAME's quantiser with dither off, copied exactly from audio_dither::quantise() in
// emu/sound.h:  s16(clamp(int(floor(v + 0.5f)), -32768, 32767)), with the sample scaled
// by 32768 first, as sound_manager's wav writer does.
//
// floor(v + 0.5) is round-half-UP, not round-half-away-from-zero.  The two differ on
// exact negative halves, which is worth a couple of LSB on a couple of percent of frames
// -- invisible by ear, and the whole difference between "close" and bit-identical.
int16_t quantise(float v)
{
    const float scaled = v * 32768.0f;
    int i = int(std::floor(scaled + 0.5f));
    if (i >  32767) i =  32767;
    if (i < -32768) i = -32768;
    return int16_t(i);
}

} // namespace

int main(int argc, char **argv)
{
    std::string roms = "roms", out;
    double seconds = 10.0;
    bool show_lcd = false;
    bool play_note = false;
    uint32_t blocksize = 512;
    std::string midi_events;        // "t,byte" per line, absolute emulated seconds

    for (int i = 1; i < argc; i ++)
    {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--roms")    roms = next();
        else if (a == "--out")     out = next();
        else if (a == "--seconds") seconds = std::atof(next());
        else if (a == "--lcd")     show_lcd = true;
        else if (a == "--note")    play_note = true;
        else if (a == "--block")   blocksize = uint32_t(std::atoi(next()));
        else if (a == "--midi-at")  midi_events = next();
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    voltaire::U110Core core;

    // The firmware EPROM.  v2.03 is the one the driver defaults to.
    const char *pgm_names[] = { "roland_u110_pgm_(15179960).bin", "U110v203.BIN",
                                "roland_u110_pgm_15179960.bin", "U110v200.BIN" };
    bool loaded = false;
    for (const char *n : pgm_names)
    {
        auto d = read_file(roms + "/" + n);
        if (d.size() == 0x10000)
        {
            std::printf("program ROM: %s\n", n);
            loaded = core.loadProgramRom(d.data(), d.size()) == voltaire::LoadResult::Ok;
            break;
        }
    }
    if (!loaded) { std::fprintf(stderr, "no program ROM found under %s\n", roms.c_str()); return 1; }

    for (unsigned b = 0; b < voltaire::kNumWaveBanks; b ++)
    {
        char name[128];
        std::snprintf(name, sizeof(name),
                "%s/roland_t110_u110_u220_waverom%u.bin", roms.c_str(), b);
        auto d = read_file(name);
        if (d.size() != voltaire::kCardBytes)
        { std::fprintf(stderr, "wave ROM %u missing or wrong size\n", b); return 1; }
        core.loadWaveRom(b, d.data(), d.size());
    }
    std::printf("wave ROMs: 4 banks loaded and descrambled\n");

    core.reset();

    const uint32_t rate = voltaire::kCoreSampleRate;
    const uint32_t block = blocksize;
    const uint32_t total = uint32_t(seconds * rate);
    std::vector<float> l(block), r(block);
    std::vector<int16_t> pcm;
    pcm.reserve(size_t(total) * 2);

    // MIDI to inject at exact emulated times, taken from MAME's own "MIDI IN <t> <byte>"
    // trace.  Replaying the SAME instants is what removes the MIDI transport from the
    // comparison: MAME's -min has its own delivery lag and bit clock, and matching those
    // would be a test of the transport rather than of the emulation.
    struct TimedByte { double t; uint8_t b; };
    std::vector<TimedByte> injected;
    if (!midi_events.empty())
    {
        FILE *f = std::fopen(midi_events.c_str(), "r");
        if (!f) { std::fprintf(stderr, "cannot read %s\n", midi_events.c_str()); return 1; }
        double t; unsigned b;
        while (std::fscanf(f, "%lf,%x", &t, &b) == 2)
            injected.push_back({ t, uint8_t(b) });
        std::fclose(f);
        std::printf("injecting %zu MIDI bytes at MAME's own arrival times\n", injected.size());
    }
    size_t inject_pos = 0;

    // A byte handed in at offset N completes one byte time later, and MAME's trace records
    // COMPLETION, so aim earlier by that much.
    const double byte_time = 10.0 / 31250.0;

    std::string last_lcd;
    uint32_t done = 0;
    bool note_sent = false, note_off_sent = false;
    while (done < total)
    {
        const uint32_t n = std::min(block, total - done);

        while (inject_pos < injected.size())
        {
            const double start = injected[inject_pos].t - byte_time;
            const double blk0 = double(done) / rate, blk1 = double(done + n) / rate;
            if (start >= blk1) break;
            const uint32_t off = uint32_t(start <= blk0 ? 0 : (start - blk0) * rate);
            core.midiIn(&injected[inject_pos].b, 1, off);
            inject_pos ++;
        }

        // The machine needs about 5.5 s to boot before it will answer MIDI at all.
        if (play_note && !note_sent && done >= 7 * rate)
        {
            const uint8_t on[] = { 0x90, 60, 100 };
            core.midiIn(on, 3, 0);
            note_sent = true;
            std::printf("  note on at %.2f s\n", double(done) / rate);
        }
        if (play_note && note_sent && !note_off_sent && done >= 10 * rate)
        {
            const uint8_t off[] = { 0x80, 60, 0 };
            core.midiIn(off, 3, 0);
            note_off_sent = true;
        }

        core.renderStereo(l.data(), r.data(), n);
        for (uint32_t i = 0; i < n; i ++)
        {
            pcm.push_back(quantise(l[i]));
            pcm.push_back(quantise(r[i]));
        }
        done += n;

        if (show_lcd)
        {
            voltaire::PanelState st;
            core.snapshot(st);
            std::string text;
            for (int i = 0; i < 32; i ++)
            {
                const uint8_t c = st.lcd[i];
                text += (c >= 0x20 && c < 0x7f) ? char(c) : '.';
                if (i == 15) text += " | ";
            }
            if (text != last_lcd)
            {
                std::printf("  %7.3f s  [%s]\n", double(done) / rate, text.c_str());
                last_lcd = text;
            }
        }
    }

    int16_t peak = 0;
    for (int16_t s : pcm) { int16_t a = s < 0 ? int16_t(-s) : s; if (a > peak) peak = a; }
    std::printf("rendered %.2f s, peak %d\n", seconds, peak);

    if (!out.empty())
    {
        write_wav(out, pcm, rate);
        std::printf("wrote %s\n", out.c_str());
    }
    return 0;
}
