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
#include "Sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

START_NAMESPACE_DISTRHO

namespace {

/// Where ROM images are looked for, in order (PLUGIN-PLAN.md section 9).  ROMs are DATA,
/// not configuration, so they do not live in ~/.config.  Nothing is bundled with the
/// plugin: the user supplies their own dumps.
std::vector<std::string> romSearchPath()
{
    std::vector<std::string> bases;
    if (const char *env = std::getenv("U110_DATA_DIR"))
        bases.push_back(std::string(env) + "/roms");
    if (const char *xdg = std::getenv("XDG_DATA_HOME"))
        bases.push_back(std::string(xdg) + "/u110/roms");
    if (const char *home = std::getenv("HOME"))
        bases.push_back(std::string(home) + "/.local/share/u110/roms");
    bases.push_back("/usr/share/u110/roms");
    // Development convenience: the project's own roms/ directory.
    if (const char *env = std::getenv("U110_SOURCE_ROMS"))
        bases.push_back(env);

    // Section 9 puts cards in roms/cards/, but a flat roms/ is what most people end up
    // with, so both are searched and neither is required.
    std::vector<std::string> out;
    for (const std::string &b : bases)
    {
        out.push_back(b);
        out.push_back(b + "/cards");
    }
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

std::vector<uint8_t> findRom(const char *const *names, size_t count, size_t wantSize,
                             std::string *foundName = nullptr)
{
    for (const std::string &dir : romSearchPath())
        for (size_t i = 0; i < count; i ++)
        {
            std::vector<uint8_t> d = readFile(dir + "/" + names[i]);
            if (d.size() == wantSize)
            {
                if (foundName != nullptr)
                    *foundName = names[i];
                return d;
            }
        }
    return {};
}

std::string baseName(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Does this filename name an SN-U110 card, and if so which one?
///
/// DELIBERATELY LOOSE.  There is no database of known cards here and there never will be:
/// writing your own card image is a supported thing to do, so the only thing a name has to
/// carry is which SLOT NUMBER the image claims to be.  Separators are thrown away before
/// matching, so sn-u110-08.bin, SN_U110_08.BIN, "sn-u-110-08 (my edit).bin" and
/// roland sn u110 08.rom all name card 8, and anything containing the string anywhere is
/// enough.
///
/// The two digits must not be followed by a third, so a file called sn-u110-081 is not
/// silently read as card 8.
bool cardNumberFromName(const std::string &name, unsigned &number)
{
    std::string flat;
    flat.reserve(name.size());
    for (const char c : name)
    {
        if (c == '-' || c == '_' || c == ' ' || c == '.')
            continue;
        flat.push_back(char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }

    static const std::string kTag = "snu110";
    for (size_t at = flat.find(kTag); at != std::string::npos;
         at = flat.find(kTag, at + 1))
    {
        const size_t d = at + kTag.size();
        if (d + 1 >= flat.size())
            continue;
        const auto isDigit = [](char c)
            { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
        if (!isDigit(flat[d]) || !isDigit(flat[d + 1]))
            continue;
        if (d + 2 < flat.size() && isDigit(flat[d + 2]))
            continue;
        number = unsigned(flat[d] - '0') * 10 + unsigned(flat[d + 1] - '0');
        return true;
    }
    return false;
}

struct CardFile
{
    unsigned    number = 0;
    std::string path;
};

/// Every card image on the search path, lowest number first, one per number.
///
/// Earlier directories win a tie, so the same ordering that resolves the program ROM also
/// resolves a card the user has overridden locally.
std::vector<CardFile> scanForCards()
{
    std::vector<CardFile> out;
    for (const std::string &dir : romSearchPath())
    {
        DIR *d = ::opendir(dir.c_str());
        if (d == nullptr)
            continue;
        while (const dirent *e = ::readdir(d))
        {
            const std::string name = e->d_name;
            unsigned number = 0;
            if (!cardNumberFromName(name, number))
                continue;

            const std::string path = dir + "/" + name;
            struct stat st;
            if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            if (st.st_size == 0 || size_t(st.st_size) > voltaire::kCardBytes)
                continue;

            bool seen = false;
            for (const CardFile &c : out)
                seen = seen || c.number == number;
            if (!seen)
                out.push_back(CardFile { number, path });
        }
        ::closedir(d);
    }
    std::sort(out.begin(), out.end(),
              [](const CardFile &a, const CardFile &b) { return a.number < b.number; });
    return out;
}

/// Where the firmware keeps its patches.  ROM-ANALYSIS.md sections 2.1 and 4.
///
/// 0xE000 is bank switched on CPU P2.7: normally the battery-backed user patch SRAM, and
/// the EPROM's factory set only while the firmware is initialising memory from it.  The
/// layout is identical in both -- 64 records of 128 bytes, with a 10-byte ASCII name at
/// +4 -- so reading the live bank gives the names the machine will actually show: the
/// factory list on a machine nobody has edited, and the user's own names once there are
/// some.
constexpr uint16_t kPatchBase       = 0xE000;
constexpr uint16_t kPatchStride     = 128;
constexpr uint16_t kPatchNameOffset = 4;
constexpr unsigned kPatchNameLen    = 10;
constexpr unsigned kNumPatches      = 64;

/// Work RAM, battery backed: the patch the machine is on, counting from zero -- the
/// display adds one, so a 0 here reads as P-01 on the LCD.
constexpr uint16_t kCurrentPatchAddr = 0x274A;

/// What the plugin keeps.  Only the NVRAM is saved into a session; the panel is live
/// display pushed to the UI.
enum States
{
    kStatePanel = 0,
    kStateNvram,
    kStateSettings,
    kStatePatches,
    kStatePatchSel,
    kStateCount
};

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

    kParamCount
};


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
        // parameters, programs, STATES -- the third is what registers the "panel" blob.
        : Plugin(kParamCount, 0, kStateCount)
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
            if (index >= kParamButtonFirst && index < kParamCount)
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
            if (index >= kParamButtonFirst && index < kParamCount)
            {
                const uint32_t b = index - kParamButtonFirst;
                const bool down = value > 0.5f;
                if (down != m_buttons[b])
                {
                    m_buttons[b] = down;
                    applyButton(voltaire::Button(b));
                }
            }
            break;
        }
    }

    // ---- panel state: one struct, over the atom port -------------------------------
    //
    // Output control ports were the wrong shape for this.  A scalar port is meant to carry
    // ONE value with a meaningful range, and hosts apply their own change detection to it;
    // packing a bitfield into one and asking every host to notice a change of 4 in
    // 16777215 is asking for exactly the trouble it caused.  It also does not scale to what
    // is coming -- tone and patch NAMES, and a parameter editor -- which are text, not
    // numbers.
    //
    // So the panel goes as a single blob on the atom port, which is LV2's channel for
    // structured DSP->UI data and what responsive plugins actually use.  Scalars that
    // really are scalars, like the VU meters, stay as output ports; that is what those are
    // for.

    void initState(uint32_t index, State &state) override
    {
        switch (index)
        {
        case kStatePanel:
            state.key = "panel";
            state.label = "Panel";
            // Live display, pushed to the UI many times a second.  NOT host-readable:
            // there is no sense in a session storing what the LCD happened to show.
            state.hints = kStateIsOnlyForUI;
            state.defaultValue = "";
            break;

        case kStateNvram:
            state.key = "nvram";
            state.label = "Battery-backed memory";
            // What the real unit's battery preserves: the user's patches and setup.
            state.hints = kStateIsHostReadable;
            state.defaultValue = "";
            break;

        case kStateSettings:
            state.key = "settings";
            state.label = "Settings and cards";
            // Everything about the session that is NOT inside the machine: the front-panel
            // controls, and WHICH images were mounted.  Text, on purpose -- a session file
            // should be readable when you are trying to work out why a project came back
            // sounding wrong.
            state.hints = kStateIsHostReadable;
            state.defaultValue = "";
            break;

        case kStatePatches:
            state.key = "patches";
            state.label = "Patch names";
            // The 64 names as the machine has them, for the UI's PATCH menu.  Not saved
            // into a session: they are already inside the memory that is.
            state.hints = kStateIsOnlyForUI;
            state.defaultValue = "";
            break;

        case kStatePatchSel:
            state.key = "patchsel";
            state.label = "Select patch";
            // The other direction -- the UI asking for a patch by number.  Nothing to
            // save and nothing to show: which patch is current is the machine's own
            // business, and it comes back with the panel.
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;
        }
    }

    /// Asked by the host when it saves a session.
    String getState(const char *key) const override
    {
        if (std::strcmp(key, "settings") == 0)
            return getSettingsState();
        if (std::strcmp(key, "patches") == 0)
            return String(m_patchesText);
        if (std::strcmp(key, "nvram") != 0)
            return String();

        const size_t need = m_core.saveState(nullptr, 0);
        std::vector<uint8_t> raw(need);
        if (m_core.saveState(raw.data(), raw.size()) != need)
            return String();

        std::vector<char> hex(need * 2 + 1);
        encodeHex(raw.data(), need, hex.data());
        return String(hex.data());
    }

    // A host restores the two keys in whatever order it likes, and the cards have to be
    // in their slots BEFORE the machine boots into the patches that reference them --
    // otherwise every card tone in the restored setup comes back as "Illegal CARD".
    //
    // So neither key applies itself.  Each one records what it was given and then asks for
    // one reboot, which mounts whatever cards are known and puts back whatever memory is
    // known.  Restoring both keys costs two boots, at a few tens of milliseconds each, and
    // this runs when a project loads rather than from the audio callback.
    void setState(const char *key, const char *value) override
    {
        if (value == nullptr)
            return;

        if (std::strcmp(key, "patchsel") == 0)
        {
            // This arrives on the LV2 worker thread, and on the UI's own thread
            // everywhere else -- never on the audio thread.  So all that happens here is
            // that a number is left for run() to pick up: the presses that actually
            // select the patch have to be made in EMULATED time, which only run() has.
            //
            // A restored session hands back every key the host stored, and DPF stores
            // this one too even though there is nothing in it: without the digit test an
            // empty value would read as atoi("") == 0 and quietly select P-01 every time
            // a project was reopened.
            if (value[0] < '0' || value[0] > '9')
                return;
            const int n = std::atoi(value);
            if (n >= 0 && unsigned(n) < kNumPatches)
                m_patchRequest.store(n, std::memory_order_relaxed);
            return;
        }

        if (std::strcmp(key, "settings") == 0)
        {
            setSettingsState(value);
            rebootIntoRestoredState();
            return;
        }

        if (std::strcmp(key, "nvram") != 0 || value[0] == '\0')
            return;

        const size_t n = std::strlen(value) / 2;
        std::vector<uint8_t> raw(n);
        for (size_t i = 0; i < n; i ++)
        {
            const int hi = hexNibble(value[i * 2]), lo = hexNibble(value[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return;
            raw[i] = uint8_t((hi << 4) | lo);
        }
        m_savedNvram = std::move(raw);
        rebootIntoRestoredState();
    }

    void rebootIntoRestoredState()
    {
        if (!m_romsLoaded)
            return;
        if (m_savedNvram.empty())
        {
            // Cards changed but there is no saved memory -- an older session, or a project
            // saved before any patch was edited.  The machine still has to come up again
            // to see the slots.
            m_core.reset();
            m_core.runUntilIdle();
            return;
        }
        if (m_core.loadState(m_savedNvram.data(), m_savedNvram.size()))
            d_stdout("Voltaire 110: restored %zu bytes of battery-backed memory.",
                     m_savedNvram.size());
        else
            d_stderr2("Voltaire 110: saved memory did not load -- wrong version or size. "
                      "The machine keeps its own; nothing was overwritten.");
    }

    static int hexNibble(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
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

        // Before the render, so a button edge lands at the start of the block it belongs
        // to rather than at the start of the next one.
        tickPatchSelect(coreFrames);

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
        // The clip lamp is measured HERE, after the gain, because that is where clipping
        // actually happens -- the volume is a plugin-layer post-gain modelling an analogue
        // pot after the DAC, so the emulation never sees it.
        float peak = 0.0f;
        for (uint32_t i = 0; i < frames; i ++)
        {
            m_gain += (m_gainTarget - m_gain) * 0.001f;
            outL[i] *= m_gain;
            outR[i] *= m_gain;
            const float a = std::fabs(outL[i]), b = std::fabs(outR[i]);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        if (peak >= 1.0f)
            m_clipHold = uint32_t(m_hostRate * kClipHoldSeconds);
        else if (m_clipHold > frames)
            m_clipHold -= frames;
        else
            m_clipHold = 0;

        sendMidiOut();
        publishPanel(frames);
        reportLcd();
    }

private:
    static constexpr double kCoreRate = double(voltaire::kCoreSampleRate);

    /// How often the panel snapshot goes to the UI.  Fast enough that a button press feels
    /// immediate; the cost is a snapshot copy, which is a few hundred bytes.
    static constexpr double kPanelRefreshHz = 20.0;

    /// How long the clip lamp stays lit after a sample reaches full scale.  A single
    /// clipped sample is over in 20 us; without a hold you would never see it.
    static constexpr double kClipHoldSeconds = 0.4;

    /// Everything the panel needs, in one fixed layout.  Hex rather than base64 so the
    /// encoder is four lines and has no dependency; 99 bytes becomes 198 characters, which
    /// is nothing next to an audio buffer.
    struct PanelBlob
    {
        uint8_t lcd[32];
        uint8_t cgram[64];
        uint8_t leds, cursor_pos, cursor_flags;
        uint8_t patch;              ///< 0-based, so the LCD's P-01 is 0
    };

    static void encodeHex(const uint8_t *src, size_t n, char *dst)
    {
        static const char *const h = "0123456789abcdef";
        for (size_t i = 0; i < n; i ++)
        { dst[i * 2] = h[src[i] >> 4]; dst[i * 2 + 1] = h[src[i] & 0x0f]; }
        dst[n * 2] = 0;
    }

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

        // The blob: only when something in it actually changed.  An idle machine sends
        // nothing at all, which is what keeps the allocation DPF does inside
        // updateStateValue() off the audio thread in the common case.
        {
            PanelBlob blob;
            std::memcpy(blob.lcd, st.lcd, sizeof(blob.lcd));
            std::memcpy(blob.cgram, st.cgram, sizeof(blob.cgram));
            blob.leds = uint8_t(uint32_t(st.leds) | (m_clipHold ? 0x08u : 0x00u));
            blob.cursor_pos = st.cursor_pos;
            blob.cursor_flags = st.cursor_flags;
            blob.patch = m_core.readMem(kCurrentPatchAddr);
            if (std::memcmp(&blob, &m_lastBlob, sizeof(blob)) != 0)
            {
                m_lastBlob = blob;
                encodeHex(reinterpret_cast<const uint8_t *>(&blob), sizeof(blob), m_blobHex);
                // getenv cached: this is the audio thread.
                static const bool trace = std::getenv("VOLTAIRE_BLOB") != nullptr;
                const bool ok = updateStateValue("panel", m_blobHex);
                if (trace)
                {
                    static int n = 0;
                    if (++n <= 5)
                        std::fprintf(stderr, "blob %d: %s (%zu chars)\n",
                                n, ok ? "sent" : "REJECTED", std::strlen(m_blobHex));
                }
            }
        }

        refreshPatchNames();
    }

    /// The 64 patch names, as the machine has them, for the UI's menu.
    ///
    /// Pushed only when they change, like the panel blob, and that is what keeps the list
    /// honest without anyone having to ask for it: it goes out as dots while the machine
    /// is still booting, as the factory names a second later, and again if a restored
    /// session brings back a bank whose patches have been renamed.
    void refreshPatchNames()
    {
        char text[kNumPatches * (kPatchNameLen + 1) + 1];
        size_t at = 0;
        for (unsigned p = 0; p < kNumPatches; p ++)
        {
            const size_t start = at;
            for (unsigned i = 0; i < kPatchNameLen; i ++)
            {
                const uint8_t c = m_core.readMem(
                        uint16_t(kPatchBase + p * kPatchStride + kPatchNameOffset + i));
                // Patch memory is RAM and holds whatever it holds -- zeros, until the
                // firmware has copied the factory set into it.  A name is only ever
                // shown, never interpreted, so anything unprintable becomes a dot rather
                // than something that could break the encoding below.
                text[at ++] = (c >= 0x20 && c < 0x7f) ? char(c) : '.';
            }
            while (at > start && text[at - 1] == ' ')
                at --;                       // the names are padded out to ten
            text[at ++] = '\n';
        }
        text[at] = '\0';

        if (std::memcmp(text, m_patchesText, at + 1) == 0)
            return;
        std::memcpy(m_patchesText, text, at + 1);
        updateStateValue("patches", m_patchesText);
    }

    // ---- picking a patch by name ----------------------------------------------------
    //
    // The machine has no "go to patch N".  Patches are front-panel only -- a MIDI program
    // change selects a PART'S TONE, not a patch (SYSTEM-DESIGN.md section 5.3) -- so
    // reaching P-57 by hand is 56 presses of [INC], which is the thing the menu exists to
    // stop.
    //
    // What makes ONE press enough is that the number the firmware increments lives in
    // work RAM at 0x274A.  Set it to N-1, give the machine a single [INC], and it lands
    // on N and does the whole job itself: copies the record into the active patch buffer
    // at 0x2800, reloads the eight output-routing registers, redraws the display -- all
    // exactly as it would for a press somebody made.
    //
    // WRITING 0x274A ON ITS OWN CHANGES NOTHING.  What is playing is the copy at 0x2800,
    // and only the firmware's own patch-load routine puts one there; the number by itself
    // is just a number.  Rebooting would apply it -- that is what a restored session does
    // -- but a reboot is a gap in the sound and throws away everything else in RAM.  One
    // button press costs a fifth of a second and keeps the machine running.
    //
    // [INC] means something else in the menus, so nothing is pressed until the play
    // screen is up: a machine showing a menu page is walked out of with [EXIT] first, one
    // press at a time, checking after each.  All of it is button edges in EMULATED time,
    // so this is a small state machine ticked from run() rather than anything that waits.

    /// A press has to be held long enough for the firmware's debouncer at 0x4118 to see
    /// it, and let go of long enough not to be taken for auto-repeat.  Measured on the
    /// emulation: 20 ms already registers, and 1.2 s held runs the patch number away by
    /// five.  This sits well inside both.
    static constexpr uint32_t kPressFrames = voltaire::kCoreSampleRate *  60 / 1000;
    static constexpr uint32_t kGapFrames   = voltaire::kCoreSampleRate * 180 / 1000;

    /// Three [EXIT] presses reach the play screen from the deepest page in the firmware.
    static constexpr unsigned kMaxExits = 6;

    enum class PatchStep { Idle, Decide, HoldExit, HoldInc };

    void tickPatchSelect(uint32_t coreFrames)
    {
        if (m_patchStep == PatchStep::Idle)
        {
            const int want = m_patchRequest.exchange(-1, std::memory_order_relaxed);
            if (want < 0 || !m_romsLoaded)
                return;
            m_patchTarget = unsigned(want);
            m_patchExits = 0;
            m_patchStep = PatchStep::Decide;
            m_patchWait = 0;
        }

        if (m_patchWait > coreFrames)
        {
            m_patchWait -= coreFrames;
            return;
        }
        m_patchWait = 0;

        switch (m_patchStep)
        {
        case PatchStep::Decide:
            if (onPlayScreen())
            {
                // N-1, wrapping, so that the one press the firmware sees lands on N.
                m_core.writeMem(kCurrentPatchAddr,
                        uint8_t((m_patchTarget + kNumPatches - 1) % kNumPatches));
                autoButton(voltaire::kButtonIncEnter, true);
                m_patchStep = PatchStep::HoldInc;
                m_patchWait = kPressFrames;
            }
            else if (m_patchExits ++ < kMaxExits)
            {
                autoButton(voltaire::kButtonEditExit, true);
                m_patchStep = PatchStep::HoldExit;
                m_patchWait = kPressFrames;
            }
            else
            {
                d_stderr2("Voltaire 110: could not get back to the play screen, so P-%02u "
                          "was not selected. Press EXIT until the patch name shows and "
                          "try again.", m_patchTarget + 1);
                m_patchStep = PatchStep::Idle;
            }
            break;

        case PatchStep::HoldExit:
            autoButton(voltaire::kButtonEditExit, false);
            m_patchStep = PatchStep::Decide;
            m_patchWait = kGapFrames;
            break;

        case PatchStep::HoldInc:
            autoButton(voltaire::kButtonIncEnter, false);
            m_patchStep = PatchStep::Idle;
            break;

        case PatchStep::Idle:
            break;
        }
    }

    /// Is the machine showing the patch it is playing, rather than a menu?
    ///
    /// "P-01:Ac.Piano" normally, and "TEMP:" once a program change has replaced a part's
    /// tone and made the patch a temporary edit.  No menu page starts with either -- they
    /// read "Select Mode", "PATCH", "PATCH:COM", "PATCH:WRT" and so on -- so this cannot
    /// be talked into pressing [INC] on a page where it would edit a value instead.
    bool onPlayScreen() const
    {
        voltaire::PanelState st;
        m_core.snapshot(st);
        const uint8_t *const t = st.lcd;
        if (std::memcmp(t, "TEMP:", 5) == 0)
            return true;
        return t[0] == 'P' && t[1] == '-' && t[4] == ':'
                && t[2] >= '0' && t[2] <= '9' && t[3] >= '0' && t[3] <= '9';
    }

    /// The menu's own presses, kept apart from the user's.  A switch is down if either
    /// says so, so a press made from the menu cannot cancel one being made by hand.
    void autoButton(voltaire::Button b, bool down)
    {
        m_autoButtons[b] = down;
        applyButton(b);
    }

    void applyButton(voltaire::Button b)
    {
        m_core.setButton(b, m_buttons[b] || m_autoButtons[b]);
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

    // ---- cards, and the rest of what a session has to remember ---------------------
    //
    // A card is mounted by FILE, not by catalogue.  Nothing here knows or cares what
    // SN-U110-08 is supposed to contain -- writing your own image is a supported thing to
    // do -- so a fresh instance mounts whatever card-shaped files it finds, and a restored
    // instance mounts the exact files the session was saved with.
    //
    // The SHA-256 is recorded for that second case only, and it is never a gate: if the
    // file has changed since the session was saved it is still mounted, with a warning
    // saying so.  Refusing to load somebody's edited card because it no longer matches a
    // hash would make the checksum an obstacle rather than an explanation.

    struct MountedCard
    {
        bool        present = false;
        unsigned    number  = 0;
        std::string path;
        std::string sha;
    };

    /// Read an image, mount it, and record what it was.  Returns false and leaves the slot
    /// alone if the file cannot be read or the core rejects it.
    bool mountCard(unsigned slot, unsigned number, const std::string &path)
    {
        std::vector<uint8_t> img = readFile(path);
        if (img.empty())
        {
            d_stderr2("Voltaire 110: card slot %u: cannot read %s", slot, path.c_str());
            return false;
        }
        if (m_core.loadCard(slot, img.data(), img.size()) != voltaire::LoadResult::Ok)
        {
            d_stderr2("Voltaire 110: card slot %u: %s is %zu bytes, which is not a card "
                      "image the machine can address.", slot, path.c_str(), img.size());
            return false;
        }
        m_cards[slot] = MountedCard { true, number, path,
                                      voltaire::Sha256::of(img.data(), img.size()) };
        return true;
    }

    void ejectCard(unsigned slot)
    {
        m_core.loadCard(slot, nullptr, 0);
        m_cards[slot] = MountedCard();
    }

    /// A fresh instance: mount every card image on the search path, lowest number first.
    void loadCards()
    {
        const std::vector<CardFile> found = scanForCards();
        unsigned slot = 0;
        for (const CardFile &c : found)
        {
            if (slot >= voltaire::kNumCardSlots)
            {
                d_stderr2("Voltaire 110: more than %u card images found; %s and any after "
                          "it were left unmounted.",
                          voltaire::kNumCardSlots, baseName(c.path).c_str());
                break;
            }
            if (mountCard(slot, c.number, c.path))
            {
                d_stdout("Voltaire 110: card slot %u <- %s", slot, baseName(c.path).c_str());
                slot ++;
            }
        }
    }

    /// Where a saved session's card image lives NOW.  Sessions travel between machines and
    /// people reorganise their ROM directories, so the recorded path is a first guess, not
    /// an address.
    std::string resolveCardPath(const std::string &savedPath, unsigned number) const
    {
        struct stat st;
        if (!savedPath.empty() && ::stat(savedPath.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            return savedPath;

        const std::string want = baseName(savedPath);
        for (const std::string &dir : romSearchPath())
        {
            const std::string cand = dir + "/" + want;
            if (!want.empty() && ::stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                return cand;
        }

        // Same card, different filename -- the usual case when a session moves between
        // people, since nothing forces one spelling of the name.
        for (const CardFile &c : scanForCards())
            if (c.number == number)
                return c.path;

        return std::string();
    }

    String getSettingsState() const
    {
        std::string out;
        char line[1024];

        std::snprintf(line, sizeof(line), "volume %.4f\n", double(m_volumeDb));
        out += line;
        std::snprintf(line, sizeof(line), "hfcorrection %d\n", m_hfCorrection ? 1 : 0);
        out += line;

        // Informational: which firmware this session was made with.  v2.00 and v2.03 lay
        // out patch memory the same way, so this only ever produces a warning, never a
        // refusal -- but it is the first thing worth knowing if a project comes back odd.
        if (!m_pgmSha.empty())
        {
            std::snprintf(line, sizeof(line), "pgm %s %s\n",
                          m_pgmSha.c_str(), m_pgmName.c_str());
            out += line;
        }

        for (unsigned i = 0; i < voltaire::kNumCardSlots; i ++)
        {
            if (!m_cards[i].present)
                continue;
            // The path goes last so it may contain spaces without needing quoting.
            std::snprintf(line, sizeof(line), "card %u %u %s %s\n", i, m_cards[i].number,
                          m_cards[i].sha.c_str(), m_cards[i].path.c_str());
            out += line;
        }
        return String(out.c_str());
    }

    void setSettingsState(const char *text)
    {
        // The session is the authority on which cards were in the machine.  Anything the
        // auto-scan mounted at construction is ejected first, so a project that was saved
        // with no cards comes back with no cards -- otherwise a card image dropped into
        // the ROM directory later would silently rewrite an old project's sound.
        for (unsigned i = 0; i < voltaire::kNumCardSlots; i ++)
            ejectCard(i);

        std::string all(text);
        size_t pos = 0;
        while (pos <= all.size())
        {
            const size_t nl = all.find('\n', pos);
            const std::string line = all.substr(pos, nl == std::string::npos
                                                    ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? all.size() + 1 : nl + 1;
            if (line.empty())
                continue;

            float f = 0.0f;
            int d = 0;
            unsigned slot = 0, number = 0;
            char sha[80] = { 0 };
            int rest = 0;

            if (std::sscanf(line.c_str(), "volume %f", &f) == 1)
            {
                m_volumeDb = f;
                m_gainTarget = std::pow(10.0f, f / 20.0f);
            }
            else if (std::sscanf(line.c_str(), "hfcorrection %d", &d) == 1)
            {
                m_hfCorrection = d != 0;
                m_core.setHfCorrection(m_hfCorrection);
            }
            else if (std::sscanf(line.c_str(), "pgm %79s %n", sha, &rest) == 1)
            {
                if (!m_pgmSha.empty() && m_pgmSha != sha)
                    d_stderr2("Voltaire 110: this project was saved with a different "
                              "program ROM (%s...). The one loaded is %s.... Patches will "
                              "still load; sounds may differ.",
                              std::string(sha).substr(0, 8).c_str(),
                              m_pgmSha.substr(0, 8).c_str());
            }
            else if (std::sscanf(line.c_str(), "card %u %u %79s %n",
                                 &slot, &number, sha, &rest) == 3
                     && slot < voltaire::kNumCardSlots && rest > 0)
            {
                restoreCard(slot, number, sha, line.substr(size_t(rest)));
            }
        }
    }

    void restoreCard(unsigned slot, unsigned number, const std::string &wantSha,
                     const std::string &savedPath)
    {
        const std::string path = resolveCardPath(savedPath, number);
        if (path.empty())
        {
            d_stderr2("Voltaire 110: card slot %u is empty: this project used %s, which is "
                      "not on the ROM search path. Tones from that card will read as "
                      "\"Illegal CARD\" until it is put back.",
                      slot, baseName(savedPath).c_str());
            return;
        }
        if (!mountCard(slot, number, path))
            return;

        if (m_cards[slot].sha != wantSha)
            d_stderr2("Voltaire 110: card slot %u: %s has changed since this project was "
                      "saved (%s... now %s...). Mounted anyway.",
                      slot, baseName(path).c_str(), wantSha.substr(0, 8).c_str(),
                      m_cards[slot].sha.substr(0, 8).c_str());
        else if (path != savedPath)
            d_stdout("Voltaire 110: card slot %u <- %s (moved since the project was saved, "
                     "but byte for byte the same image).", slot, path.c_str());
    }

    void loadRoms()
    {
        static const char *const pgm[] = {
            "roland_u110_pgm_(15179960).bin", "roland_u110_pgm_15179960.bin",
            "U110v203.BIN", "u110_v203.bin", "U110v200.BIN", "u110_v200.bin",
        };
        std::vector<uint8_t> rom = findRom(pgm, sizeof(pgm) / sizeof(pgm[0]), 0x10000,
                                           &m_pgmFound);
        if (rom.empty())
        {
            d_stderr2("Voltaire 110: no U-110 program ROM found. Put your own dumps in "
                      "$XDG_DATA_HOME/u110/roms (or set U110_DATA_DIR). The plugin will "
                      "stay silent until then.");
            return;
        }
        if (m_core.loadProgramRom(rom.data(), rom.size()) != voltaire::LoadResult::Ok)
            return;
        m_pgmSha = voltaire::Sha256::of(rom.data(), rom.size());
        m_pgmName = m_pgmFound;

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

        loadCards();

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
    bool m_autoButtons[voltaire::kButtonCount] = { false };
    unsigned m_lcdAccum = 0;
    char m_lastLcd[40] = { 0 };
    uint32_t m_clipHold = 0;
    MountedCard m_cards[voltaire::kNumCardSlots];
    std::vector<uint8_t> m_savedNvram;
    std::string m_pgmSha, m_pgmName, m_pgmFound;

    char m_patchesText[kNumPatches * (kPatchNameLen + 1) + 1] = { 0 };
    std::atomic<int> m_patchRequest { -1 };
    PatchStep m_patchStep = PatchStep::Idle;
    uint32_t m_patchWait = 0;
    unsigned m_patchTarget = 0, m_patchExits = 0;

    PanelBlob m_lastBlob = {};
    char m_blobHex[sizeof(PanelBlob) * 2 + 1] = { 0 };
    uint32_t m_publishAccum = 0;
    uint32_t m_publishPeriod = 2400;


    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Voltaire110Plugin)
};


Plugin *createPlugin() { return new Voltaire110Plugin(); }

END_NAMESPACE_DISTRHO
