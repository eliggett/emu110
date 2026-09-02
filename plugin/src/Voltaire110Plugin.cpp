// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    Voltaire 110 -- the DPF plugin around U110Core.

    This layer is deliberately thin.  Everything that emulates the hardware is below it in
    plugin/core (BSD, and bit-identical to MAME); everything here is about BEING a plugin:
    the host's sample rate, its MIDI events, its parameters, and finding the ROMs.
    PLUGIN-PLAN.md section 1 has the reasoning, and the rule that code never moves
    downward across that line.
*/

#include "DistrhoPlugin.hpp"

#include "u110_core.h"
#include "Resampler.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

START_NAMESPACE_DISTRHO

namespace {

/// Where ROM images are looked for, in order (PLUGIN-PLAN.md section 9).  ROMs are DATA,
/// not configuration, so they do not live in ~/.config.  Nothing is bundled with the
/// plugin: the user supplies their own dumps.
std::vector<std::string> romSearchPath()
{
    std::vector<std::string> out;
    if (const char *env = std::getenv("U110_DATA_DIR"))
        out.push_back(std::string(env) + "/roms");
    if (const char *xdg = std::getenv("XDG_DATA_HOME"))
        out.push_back(std::string(xdg) + "/u110/roms");
    if (const char *home = std::getenv("HOME"))
        out.push_back(std::string(home) + "/.local/share/u110/roms");
    out.push_back("/usr/share/u110/roms");
    // Development convenience: the project's own roms/ directory.
    if (const char *env = std::getenv("U110_SOURCE_ROMS"))
        out.push_back(env);
    return out;
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::vector<uint8_t> data;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return data;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0)
    {
        data.resize(size_t(n));
        if (std::fread(data.data(), 1, data.size(), f) != data.size())
            data.clear();
    }
    std::fclose(f);
    return data;
}

std::vector<uint8_t> findRom(const char *const *names, size_t count, size_t wantSize)
{
    for (const std::string &dir : romSearchPath())
        for (size_t i = 0; i < count; i ++)
        {
            std::vector<uint8_t> d = readFile(dir + "/" + names[i]);
            if (d.size() == wantSize)
                return d;
        }
    return {};
}

enum Params
{
    kParamVolume = 0,
    kParamHfCorrection,

    // The six panel switches, as host parameters.
    //
    // Until the vector panel exists there is no other way to reach the machine's own menus,
    // which is most of what a U-110 is.  A generic host UI gives a toggle per button, and
    // that works because the firmware's debouncer only needs the press held for about
    // 150 ms of emulated time -- far less than anyone can click.
    kParamButtonFirst,
    kParamButtonPartJump = kParamButtonFirst,
    kParamButtonEditExit,
    kParamButtonLeft,
    kParamButtonRight,
    kParamButtonDec,
    kParamButtonIncEnter,

    // --- read-only, DSP -> UI ------------------------------------------------------
    //
    // PLUGIN-PLAN.md section 8 left the transport as an open question.  It is output
    // parameters: LV2 mandates the UI as a separate binary, so it cannot read the DSP's
    // memory, and output parameters are what DPF's own examples use.
    //
    // The LCD is 32 CHARACTER CODES, not pixels, packed three to a float -- a float32
    // holds 24 bits exactly, so this is lossless.  The custom glyphs are sent one per
    // frame in rotation rather than all at once, which keeps the parameter count sane;
    // all eight refresh in about a quarter second.
    kParamOutFirst,
    kParamOutLcd0 = kParamOutFirst,     // 11 floats x 3 chars = 33 >= 32
    kParamOutLcdLast = kParamOutLcd0 + 10,
    kParamOutStatus,                    // LEDs, cursor, card presence
    kParamOutCgramIndex,                // which custom glyph the next two carry
    kParamOutCgramA,                    // rows 0-3, five bits each
    kParamOutCgramB,                    // rows 4-7

    kParamCount
};
constexpr uint32_t kNumLcdParams = 11;

const char *const kButtonNames[] = {
    "Part / Jump", "Edit / Exit", "Left", "Right", "Dec", "Inc / Enter"
};
const char *const kButtonSymbols[] = {
    "btn_part_jump", "btn_edit_exit", "btn_left", "btn_right", "btn_dec", "btn_inc_enter"
};

} // anonymous namespace


class Voltaire110Plugin : public Plugin
{
public:
    Voltaire110Plugin()
        : Plugin(kParamCount, 0, 0)
    {
        loadRoms();
        setupRate(getSampleRate());
    }

protected:
    // ---- identity -----------------------------------------------------------------

    const char *getLabel() const override { return "Voltaire110"; }
    const char *getMaker() const override { return "Elliott H. Liggett"; }
    const char *getLicense() const override { return "GPL-3.0-or-later"; }
    const char *getHomePage() const override
    { return "https://github.com/eliggett/emu110"; }
    uint32_t getVersion() const override { return d_version(0, 1, 0); }
    int64_t getUniqueId() const override { return d_cconst('V', '1', '1', '0'); }

    const char *getDescription() const override
    {
        return "An emulation of the Roland U-110 PCM sound module, running its original "
               "firmware. Requires the user's own ROM images.";
    }

    // ---- parameters ---------------------------------------------------------------

    void initParameter(uint32_t index, Parameter &parameter) override
    {
        switch (index)
        {
        case kParamVolume:
            // The real unit's volume pot is analogue and sits AFTER the DAC, so a post-gain
            // is the physically accurate model -- this is not routed into the emulation.
            parameter.hints = kParameterIsAutomatable;
            parameter.name = "Volume";
            parameter.symbol = "volume";
            parameter.unit = "dB";
            parameter.ranges.def = 0.0f;
            parameter.ranges.min = -3.0f;
            parameter.ranges.max = 16.0f;
            break;
        case kParamHfCorrection:
            // A calibration, not a tone control.  See U110Core::setHfCorrection.
            parameter.hints = kParameterIsAutomatable | kParameterIsBoolean;
            parameter.name = "HF correction";
            parameter.symbol = "hfcorrection";
            parameter.ranges.def = 1.0f;
            parameter.ranges.min = 0.0f;
            parameter.ranges.max = 1.0f;
            break;
        default:
            if (index >= kParamOutFirst)
            {
                // Read-only, DSP -> UI.  Integer-valued so hosts do not interpolate them.
                parameter.hints = kParameterIsOutput | kParameterIsInteger;
                parameter.ranges.def = 0.0f;
                parameter.ranges.min = 0.0f;
                parameter.ranges.max = 16777215.0f;
                const uint32_t o = index - kParamOutFirst;
                if (index <= kParamOutLcdLast)
                {
                    std::snprintf(m_pname, sizeof(m_pname), "LCD %u", o);
                    std::snprintf(m_psym, sizeof(m_psym), "lcd_%u", o);
                }
                else
                {
                    static const char *const nm[] = { "Status", "CGRAM idx", "CGRAM a", "CGRAM b" };
                    static const char *const sy[] = { "status", "cgram_i", "cgram_a", "cgram_b" };
                    const uint32_t k = index - kParamOutStatus;
                    std::snprintf(m_pname, sizeof(m_pname), "%s", nm[k]);
                    std::snprintf(m_psym, sizeof(m_psym), "%s", sy[k]);
                }
                parameter.name = m_pname;
                parameter.symbol = m_psym;
                break;
            }
            if (index >= kParamButtonFirst && index < kParamOutFirst)
            {
                const uint32_t b = index - kParamButtonFirst;
                parameter.hints = kParameterIsAutomatable | kParameterIsBoolean;
                parameter.name = kButtonNames[b];
                parameter.symbol = kButtonSymbols[b];
                parameter.ranges.def = 0.0f;
                parameter.ranges.min = 0.0f;
                parameter.ranges.max = 1.0f;
            }
            break;
        }
    }

    float getParameterValue(uint32_t index) const override
    {
        switch (index)
        {
        case kParamVolume:       return m_volumeDb;
        case kParamHfCorrection: return m_hfCorrection ? 1.0f : 0.0f;
        }
        if (index >= kParamOutFirst)
            return m_outParams[index - kParamOutFirst];
        if (index >= kParamButtonFirst)
            return m_buttons[index - kParamButtonFirst] ? 1.0f : 0.0f;
        return 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        switch (index)
        {
        case kParamVolume:
            m_volumeDb = value;
            m_gainTarget = std::pow(10.0f, value / 20.0f);
            break;
        case kParamHfCorrection:
        {
            const bool on = value > 0.5f;
            if (on != m_hfCorrection)
            {
                m_hfCorrection = on;
                m_core.setHfCorrection(on);
            }
            break;
        }
        default:
            if (index >= kParamButtonFirst && index < kParamOutFirst)
            {
                const uint32_t b = index - kParamButtonFirst;
                const bool down = value > 0.5f;
                if (down != m_buttons[b])
                {
                    m_buttons[b] = down;
                    m_core.setButton(voltaire::Button(b), down);
                }
            }
            break;
        }
    }

    // ---- audio ---------------------------------------------------------------------

    void sampleRateChanged(double newRate) override { setupRate(newRate); }

    void activate() override
    {
        m_resampler[0].reset();
        m_resampler[1].reset();
        m_gain = m_gainTarget;
    }

    void run(const float **, float **outputs, uint32_t frames,
             const MidiEvent *midiEvents, uint32_t midiEventCount) override
    {
        float *const outL = outputs[0];
        float *const outR = outputs[1];

        if (!m_romsLoaded)
        {
            std::memset(outL, 0, sizeof(float) * frames);
            std::memset(outR, 0, sizeof(float) * frames);
            return;
        }

        // Both channels are in lockstep by construction, so either may be asked.
        const uint32_t coreFrames = m_resampler[0].inputsFor(frames);

        // MIDI first, timestamped into the core block about to be rendered.  The host's
        // offsets are in HOST frames; the core counts in its own 32 kHz frames.
        for (uint32_t i = 0; i < midiEventCount; i ++)
        {
            const MidiEvent &ev = midiEvents[i];
            const uint8_t *data = ev.size > MidiEvent::kDataSize ? ev.dataExt : ev.data;
            if (ev.size == 0)
                continue;
            uint32_t off = uint32_t(double(ev.frame) * kCoreRate / m_hostRate);
            if (coreFrames && off >= coreFrames)
                off = coreFrames - 1;
            m_core.midiIn(data, ev.size, off);
        }

        renderCore(coreFrames);
        m_resampler[0].process(m_coreL.data(), coreFrames, outL, frames);
        m_resampler[1].process(m_coreR.data(), coreFrames, outR, frames);

        // Post-gain, smoothed.  No limiter: that would be dishonest about the signal.
        for (uint32_t i = 0; i < frames; i ++)
        {
            m_gain += (m_gainTarget - m_gain) * 0.001f;
            outL[i] *= m_gain;
            outR[i] *= m_gain;
        }

        sendMidiOut();
        publishPanel(frames);
        reportLcd();
    }

private:
    static constexpr double kCoreRate = double(voltaire::kCoreSampleRate);

    /// How often the panel snapshot goes to the UI.  Fast enough that a button press feels
    /// immediate; the cost is a snapshot copy, which is a few hundred bytes.
    static constexpr double kPanelRefreshHz = 20.0;

    void setupRate(double hostRate)
    {
        m_hostRate = hostRate;
        m_publishPeriod = uint32_t(hostRate / kPanelRefreshHz);
        m_resampler[0].setup(kCoreRate, hostRate);
        m_resampler[1].setup(kCoreRate, hostRate);
        setLatency(m_resampler[0].latency());
        // Enough core frames for a generous host buffer, allocated once.
        m_coreL.assign(size_t(kCoreRate / 1000.0) * 512, 0.0f);
        m_coreR.assign(m_coreL.size(), 0.0f);
    }

    void renderCore(uint32_t coreFrames)
    {
        if (coreFrames > m_coreL.size())
        {
            // Only reachable if the host asks for a buffer far larger than anything it
            // announced.  Growing here is not real-time safe, but silently truncating
            // would desynchronise the resampler for the rest of the session.
            m_coreL.resize(coreFrames);
            m_coreR.resize(coreFrames);
        }
        m_core.renderStereo(m_coreL.data(), m_coreR.data(), coreFrames);
    }

    /// Pack the panel into the output parameters for the UI, at about 30 Hz.
    void publishPanel(uint32_t frames)
    {
        // Counting BLOCKS made the panel's refresh depend on the host's buffer size: at
        // 1024 frames it was over half a second, which feels like the machine is ignoring
        // you.  Count frames instead, so the rate is the same everywhere.
        m_publishAccum += frames;
        if (m_publishAccum < m_publishPeriod)
            return;
        m_publishAccum = 0;

        voltaire::PanelState st;
        m_core.snapshot(st);

        for (uint32_t i = 0; i < kNumLcdParams; i ++)
        {
            uint32_t v = 0;
            for (uint32_t k = 0; k < 3; k ++)
            {
                const uint32_t c = i * 3 + k;
                v |= uint32_t(c < 32 ? st.lcd[c] : 0) << (8 * k);
            }
            m_outParams[kParamOutLcd0 - kParamOutFirst + i] = float(v);
        }

        m_outParams[kParamOutStatus - kParamOutFirst] =
                float(uint32_t(st.leds) | (uint32_t(st.cursor_pos) << 8)
                      | (uint32_t(st.cursor_flags) << 16));

        // One custom glyph per frame, round robin.  All eight refresh in about a quarter
        // second, which is fine for everything except the boot logo animation -- that is
        // a CGRAM animation at ~19 fps and will come out approximate.  Accepted.
        m_cgramTurn = (m_cgramTurn + 1) & 7;
        uint32_t a = 0, b = 0;
        for (uint32_t r = 0; r < 4; r ++)
        {
            a |= uint32_t(st.cgram[m_cgramTurn * 8 + r] & 0x1f) << (5 * r);
            b |= uint32_t(st.cgram[m_cgramTurn * 8 + 4 + r] & 0x1f) << (5 * r);
        }
        m_outParams[kParamOutCgramIndex - kParamOutFirst] = float(m_cgramTurn);
        m_outParams[kParamOutCgramA - kParamOutFirst] = float(a);
        m_outParams[kParamOutCgramB - kParamOutFirst] = float(b);
    }

    /// Print the LCD when it changes, if asked.
    ///
    /// There is no panel yet, so without this the machine's own menus are invisible: you
    /// can press the buttons but not see what they did.  Off unless VOLTAIRE_LCD is set,
    /// because printing from the audio thread is not something to do by default.
    void reportLcd()
    {
        static const bool want = std::getenv("VOLTAIRE_LCD") != nullptr;
        if (!want)
            return;
        m_lcdAccum += 1;
        if (m_lcdAccum < 8)
            return;
        m_lcdAccum = 0;

        voltaire::PanelState st;
        m_core.snapshot(st);
        char line[40];
        for (int i = 0; i < 32; i ++)
        {
            const uint8_t c = st.lcd[i];
            line[i + (i >= 16 ? 3 : 0)] = (c >= 0x20 && c < 0x7f) ? char(c) : '.';
        }
        line[16] = ' '; line[17] = '|'; line[18] = ' ';
        line[35] = 0;
        if (std::memcmp(line, m_lastLcd, sizeof(line)) != 0)
        {
            std::memcpy(m_lastLcd, line, sizeof(line));
            d_stdout("LCD [%s]", line);
        }
    }

    void sendMidiOut()
    {
        // The U-110 sends nothing unprompted -- no active sensing, no keyboard -- so this
        // is empty except after a SysEx request or a bulk dump.
        uint8_t buf[256];
        uint32_t offs[256];
        const size_t n = m_core.midiOut(buf, sizeof(buf), offs);
        for (size_t i = 0; i < n; i ++)
        {
            MidiEvent ev;
            ev.frame = 0;
            ev.size = 1;
            ev.data[0] = buf[i];
            ev.dataExt = nullptr;
            writeMidiEvent(ev);
        }
    }

    void loadRoms()
    {
        static const char *const pgm[] = {
            "roland_u110_pgm_(15179960).bin", "roland_u110_pgm_15179960.bin",
            "U110v203.BIN", "u110_v203.bin", "U110v200.BIN", "u110_v200.bin",
        };
        std::vector<uint8_t> rom = findRom(pgm, sizeof(pgm) / sizeof(pgm[0]), 0x10000);
        if (rom.empty())
        {
            d_stderr2("Voltaire 110: no U-110 program ROM found. Put your own dumps in "
                      "$XDG_DATA_HOME/u110/roms (or set U110_DATA_DIR). The plugin will "
                      "stay silent until then.");
            return;
        }
        if (m_core.loadProgramRom(rom.data(), rom.size()) != voltaire::LoadResult::Ok)
            return;

        for (unsigned b = 0; b < voltaire::kNumWaveBanks; b ++)
        {
            char name[96];
            std::snprintf(name, sizeof(name),
                          "roland_t110_u110_u220_waverom%u.bin", b);
            const char *names[] = { name };
            std::vector<uint8_t> w = findRom(names, 1, voltaire::kCardBytes);
            if (w.empty())
            {
                d_stderr2("Voltaire 110: wave ROM bank %u not found; no sound.", b);
                return;
            }
            m_core.loadWaveRom(b, w.data(), w.size());
        }

        m_core.reset();
        m_romsLoaded = true;
        d_stdout("Voltaire 110: ROMs loaded, core running.");
    }

    voltaire::U110Core m_core;

    // ONE RESAMPLER PER CHANNEL.  They share a ratio but not their state: the filter
    // history and the input/output positions are per-channel, and running both channels
    // through a single instance interleaves their samples in one history and advances the
    // phase twice per block.  The pitch survives that -- the average rate is still right --
    // so it does not sound broken in an obvious way.  It sounds like crackle on every note.
    voltaire::Resampler m_resampler[2];
    std::vector<float> m_coreL, m_coreR;

    double m_hostRate = 48000.0;
    float m_volumeDb = 0.0f;
    float m_gain = 1.0f, m_gainTarget = 1.0f;
    bool m_hfCorrection = true;
    bool m_romsLoaded = false;
    bool m_buttons[voltaire::kButtonCount] = { false };
    unsigned m_lcdAccum = 0;
    char m_lastLcd[40] = { 0 };
    char m_pname[32] = { 0 }, m_psym[32] = { 0 };
    uint32_t m_publishAccum = 0;
    uint32_t m_publishPeriod = 2400;
    uint32_t m_cgramTurn = 0;
    float m_outParams[kParamCount - kParamOutFirst] = { 0.0f };

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Voltaire110Plugin)
};


Plugin *createPlugin() { return new Voltaire110Plugin(); }

END_NAMESPACE_DISTRHO
