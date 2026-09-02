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
    kParamCount
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
        }
    }

    float getParameterValue(uint32_t index) const override
    {
        switch (index)
        {
        case kParamVolume:       return m_volumeDb;
        case kParamHfCorrection: return m_hfCorrection ? 1.0f : 0.0f;
        }
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
        }
    }

    // ---- audio ---------------------------------------------------------------------

    void sampleRateChanged(double newRate) override { setupRate(newRate); }

    void activate() override
    {
        m_resampler.reset();
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

        const uint32_t coreFrames = m_resampler.inputsFor(frames);

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
        m_resampler.process(m_coreL.data(), coreFrames, outL, frames);
        m_resampler.process(m_coreR.data(), coreFrames, outR, frames);

        // Post-gain, smoothed.  No limiter: that would be dishonest about the signal.
        for (uint32_t i = 0; i < frames; i ++)
        {
            m_gain += (m_gainTarget - m_gain) * 0.001f;
            outL[i] *= m_gain;
            outR[i] *= m_gain;
        }

        sendMidiOut();
    }

private:
    static constexpr double kCoreRate = double(voltaire::kCoreSampleRate);

    void setupRate(double hostRate)
    {
        m_hostRate = hostRate;
        m_resampler.setup(kCoreRate, hostRate);
        setLatency(m_resampler.latency());
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
    voltaire::Resampler m_resampler;
    std::vector<float> m_coreL, m_coreR;

    double m_hostRate = 48000.0;
    float m_volumeDb = 0.0f;
    float m_gain = 1.0f, m_gainTarget = 1.0f;
    bool m_hfCorrection = true;
    bool m_romsLoaded = false;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Voltaire110Plugin)
};


Plugin *createPlugin() { return new Voltaire110Plugin(); }

END_NAMESPACE_DISTRHO
