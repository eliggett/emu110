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
#include <cstdarg>
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

/// One place the plugin looks, and what put it on the list.
///
/// The origin is carried only so the report the UI can show says WHY a directory is being
/// searched.  "It looked in C:/Users/me/AppData/Local/Voltaire110/roms" is a fact; "it
/// looked there because that is what %LOCALAPPDATA% expands to" is the half that tells you
/// what to fix when the answer is not the directory you filled.
struct RomDir
{
    std::string path;
    std::string origin;
    bool legacy = false;    ///< the old u110/ name: still read, no longer advertised
};

/// The directory the plugin keeps its data in, under whichever per-user or system-wide
/// data root the platform offers.  It is the plugin's name because that is what someone
/// scrolling through AppData or ~/.local/share will be looking for.
constexpr const char *kDataDirName = "Voltaire110";

/// What that directory used to be called.  Anyone who filled it before the rename should
/// not have to move anything, so every location is looked at under the old name too --
/// after all the new ones, and only documented in the report that lists what was searched.
constexpr const char *kLegacyDirName = "u110";

/// Where ROM images are looked for, in order (PLUGIN-PLAN.md section 9).  ROMs are DATA,
/// not configuration, so they do not live in ~/.config.  Nothing is bundled with the
/// plugin: the user supplies their own dumps.
std::vector<RomDir> romSearchDirs()
{
    std::vector<RomDir> bases;
    const auto fromEnv = [&bases](const char *var, const std::string &tail,
                                  bool legacy = false)
    {
        const char *const v = std::getenv(var);
        if (v == nullptr || v[0] == '\0')
            return;
        bases.push_back(RomDir { std::string(v) + tail, std::string(var), legacy });
    };

    // The override is the same everywhere, so a note telling someone where to put their
    // dumps does not have to ask which OS they are on first.
    fromEnv("U110_DATA_DIR", "/roms");

    // The new name in every location before the old name in any of them, so a machine
    // carrying both reads the one the documentation talks about.
    for (int pass = 0; pass < 2; pass ++)
    {
        const bool legacy = pass == 1;
        const std::string dir = std::string("/") + (legacy ? kLegacyDirName : kDataDirName);
       #ifdef _WIN32
        // Forward slashes are fine: the Win32 file APIs accept them, and keeping one
        // separator in this file means the paths a log line prints look the same on every
        // platform.
        fromEnv("LOCALAPPDATA", dir + "/roms", legacy);
        fromEnv("PROGRAMDATA", dir + "/roms", legacy);
       #else
        fromEnv("XDG_DATA_HOME", dir + "/roms", legacy);
        fromEnv("HOME", "/.local/share" + dir + "/roms", legacy);
        bases.push_back(RomDir { "/usr/share" + dir + "/roms", "built in", legacy });
       #endif
    }
    // Development convenience: the project's own roms/ directory.
    fromEnv("U110_SOURCE_ROMS", "");

    // Section 9 puts cards in roms/cards/, but a flat roms/ is what most people end up
    // with, so both are searched and neither is required.
    std::vector<RomDir> out;
    for (const RomDir &b : bases)
    {
        out.push_back(b);
        out.push_back(RomDir { b.path + "/cards", b.origin, b.legacy });
    }
    return out;
}

std::vector<std::string> romSearchPath()
{
    std::vector<std::string> out;
    for (const RomDir &d : romSearchDirs())
        out.push_back(d.path);
    return out;
}

/// The names the program ROM may go by, and how big it has to be.
const char *const kPgmNames[] = {
    "roland_u110_pgm_(15179960).bin", "roland_u110_pgm_15179960.bin",
    "U110v203.BIN", "u110_v203.bin", "U110v200.BIN", "u110_v200.bin",
};
constexpr size_t kNumPgmNames = sizeof(kPgmNames) / sizeof(kPgmNames[0]);
constexpr size_t kPgmBytes    = 0x10000;

/// Caps on the report: files listed per directory, and how long the whole thing may be.
/// The message that carries it to the UI is a fixed buffer, so something has to give --
/// and a cut made here, which can say it was cut, beats one made in the transport, which
/// cannot.
constexpr unsigned kDiagMaxEntries = 60;
constexpr size_t   kDiagMaxChars   = 15000;

/// The first `n` bytes, for asking a file what it is without reading half a megabyte of
/// it.  Short files come back short rather than padded, so the caller can tell.
std::vector<uint8_t> readFileHead(const std::string &path, size_t n)
{
    std::vector<uint8_t> data(n);
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return std::vector<uint8_t>();
    const size_t got = std::fread(data.data(), 1, n, f);
    std::fclose(f);
    data.resize(got);
    return data;
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

/// How big a file is, or -1 if there is no such file.
///
/// Telling "not there" apart from "there but the wrong size" is the whole point.  A dump
/// that is a byte short, a .zip nobody expanded, a 0-byte file left by a failed copy --
/// all of them look exactly like a missing ROM to code that only asks whether the read
/// worked, and all of them need a different thing done about them.
long fileSize(const std::string &path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return long(st.st_size);
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
    // Both separators, on every platform.  A saved session carries the path the ROM had on
    // the machine that saved it, so a Windows path can turn up in a Linux plugin and the
    // other way round -- and this is what turns it back into a name to look for.
    const size_t slash = path.find_last_of("/\\");
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

/// What an image says it is, read out of the image itself.
///
/// The 16 bytes at offset 0 are the signature the FIRMWARE checks before it will mount a
/// card (`0x93A2`, ROM-ANALYSIS.md section 6.5), the name sits at +0x10 and the catalogue
/// ID at +0x20.  A dump is linear here -- the hardware's address and data permutations
/// compose to the identity over the header, which is section 6.4 -- so this reads the file
/// as it is, with no descrambling.
///
/// This is a better answer than the filename: it is what the machine itself will conclude.
struct CardIdent
{
    bool        valid = false;      ///< the machine will accept this as a card
    unsigned    number = 0;         ///< catalogue ID, e.g. 8 for SN-U110-08
    std::string label;              ///< what it calls itself, trimmed
};

CardIdent identifyCard(const uint8_t *img, size_t n)
{
    static const uint8_t kSig[16] = {
        'R','o','l','a','n','d','U','-','1','1','0',' ','N', 0xB1, 0x53, 0xAC
    };
    CardIdent id;
    if (img == nullptr || n < 0x21)
        return id;

    // The label is worth having even when the signature does not check out: a file that
    // says SN-U110-09 in plain text is worth naming that way in a list, and whether the
    // machine will take it is a separate question the list answers separately.
    for (size_t i = 0x10; i < 0x20; i ++)
    {
        const uint8_t c = img[i];
        if (c < 0x20 || c >= 0x7f)
            break;
        id.label.push_back(char(c));
    }
    while (!id.label.empty() && id.label.back() == ' ')
        id.label.pop_back();

    id.valid = std::memcmp(img, kSig, sizeof(kSig)) == 0;
    if (id.valid)
        id.number = img[0x20];
    return id;
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

/// How much of that stride is the PATCH: 4 header + 10 name + 2 + six 16-byte parts.  The
/// remaining 12 bytes are not part of the record -- the firmware's own WRITE copies 0x74
/// bytes and leaves them alone, and they read the same constant in all 64 slots.  See
/// analysis/SYSTEM-DESIGN.md section 5.3.4, which `make writecheck` keeps honest.
constexpr uint16_t kPatchRecordBytes = 0x74;
constexpr uint16_t kPatchNameOffset = 4;
constexpr unsigned kPatchNameLen    = 10;
constexpr unsigned kNumPatches      = 64;

/// Work RAM, battery backed: the patch the machine is on, counting from zero -- the
/// display adds one, so a 0 here reads as P-01 on the LCD.
constexpr uint16_t kCurrentPatchAddr = 0x274A;

/// TONES.  ROM-ANALYSIS.md section 6.6: a tone parameter record is 80 bytes inside the
/// wave ROM or card, and the first ten of them are its name -- so the whole list can be
/// read without driving the machine's menus and reading the LCD back.
constexpr uint32_t kToneBase    = 0x1000;
constexpr uint32_t kToneStride  = 0x50;
constexpr unsigned kToneNameLen = 10;

/// The internal wave ROM holds exactly 99, and that is a FIXED count rather than a scan:
/// record 100 in bank 0 decodes to a perfectly printable "JIG" because sample data starts
/// there.  99 is also the firmware's own limit -- program change reaches 0..98.
constexpr unsigned kNumInternalTones = 99;

/// A card holds however many it holds, so a card IS scanned, and stops at the first
/// record without a usable name.  Both cards to hand end cleanly that way: SN-U110-08 has
/// 28 tones and SN-U110-09 has 16, each followed by blanks.
constexpr unsigned kMaxCardTones = 64;

/// Where the firmware caches each slot's card ID: 0 = empty, 0xFF = the mount failed,
/// otherwise the catalogue number (8 for SN-U110-08).  ROM-ANALYSIS.md section 6.5.
///
/// That byte is the same value a patch part record uses to name a card, which is the
/// point: a part addresses a card by ID and not by slot, so the same patch works whichever
/// slot the card is in.
constexpr uint16_t kCardIdAddr = 0x2743;

/// The six PART RECORDS inside the active patch's edit buffer at 0x2800, 16 bytes each.
/// ROM-ANALYSIS.md sections 4 and 6.7.
constexpr uint16_t kPartBase          = 0x2814;
constexpr uint16_t kPartStride        = 0x10;
constexpr unsigned kNumParts          = 6;
constexpr uint16_t kPartMediaOffset   = 0x00;   ///< 0 = internal, otherwise a card ID
constexpr uint16_t kPartToneOffset    = 0x01;   ///< tone within that media, counting from 0
constexpr uint16_t kPartChannelOffset = 0x02;   ///< MIDI receive channel in the low nibble
constexpr uint16_t kPartFlagsOffset   = 0x0B;   ///< (b & 0xE0) == 0xC0: the part is off

/// SETUP:MIDI.  The receive switches gate what the machine will listen to; bit 5 is
/// EXCLUSIVE, and with it clear every SysEx message is discarded in silence.
constexpr uint16_t kRxSwitchAddr = 0x3C00;
constexpr uint8_t  kRxExclusive  = 0x20;
constexpr uint16_t kDeviceIdAddr = 0x3C01;

/// The active patch's own fields, inside the same edit buffer as the part records.
constexpr uint16_t kActivePatch   = 0x2800;   ///< same layout as a stored patch

/// What the plugin keeps.  Only the NVRAM is saved into a session; the panel is live
/// display pushed to the UI.
enum States
{
    kStatePanel = 0,
    kStateNvram,
    kStateSettings,
    kStatePatches,
    kStatePatchSel,
    kStatePatchWrite,
    kStatePatchDump,
    kStatePatchLoad,
    kStateTones,
    kStateToneSel,
    kStateDiveVals,
    kStateDiveRead,
    kStateDiveWrite,
    kStateDiag,
    kStateDiagReq,
    kStateReboot,
    kStateCardList,
    kStateCardSet,
    kStateCardScan,
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

    // OUTPUT ONLY: the stereo meter, and its peak-hold marker.
    //
    // These are appended AFTER the buttons and nothing may ever be inserted before them:
    // a host stores automation against the index in some formats, so moving an existing
    // parameter silently rewires somebody's session.
    //
    // Output PARAMETERS rather than a field in the panel blob, and the reason is the
    // blob's economy: it is sent only when memcmp says it changed, which is what keeps
    // updateStateValue()'s allocation off the audio thread while the machine is idle.  A
    // continuously moving float would make that comparison always-true and allocate every
    // 50 ms forever.  A scalar with a real range and a real unit is exactly what an
    // output port is for -- see the note above initState().
    kParamMeterFirst,
    kParamMeterL = kParamMeterFirst,
    kParamMeterR,
    kParamHoldL,
    kParamHoldR,

    kParamCount
};


const char *const kButtonNames[] = {
    "Part / Jump", "Edit / Exit", "Left", "Right", "Dec", "Inc / Enter"
};
const char *const kButtonSymbols[] = {
    "btn_part_jump", "btn_edit_exit", "btn_left", "btn_right", "btn_dec", "btn_inc_enter"
};

const char *const kMeterNames[] = {
    "Meter L", "Meter R", "Peak hold L", "Peak hold R"
};
const char *const kMeterSymbols[] = {
    "meter_l", "meter_r", "hold_l", "hold_r"
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
            if (index >= kParamMeterFirst && index < kParamCount)
            {
                const uint32_t m = index - kParamMeterFirst;
                // NOT automatable: an output parameter is the plugin telling the host, and
                // a host that tried to write one back would be fighting run().
                parameter.hints = kParameterIsOutput;
                parameter.name = kMeterNames[m];
                parameter.symbol = kMeterSymbols[m];
                parameter.unit = "dB";
                parameter.ranges.def = kMeterFloorDb;
                parameter.ranges.min = kMeterFloorDb;
                parameter.ranges.max = kMeterCeilDb;
            }
            else if (index >= kParamButtonFirst && index < kParamMeterFirst)
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
        case kParamMeterL:       return m_meterDb[0];
        case kParamMeterR:       return m_meterDb[1];
        case kParamHoldL:        return m_holdDb[0];
        case kParamHoldR:        return m_holdDb[1];
        }
        if (index >= kParamButtonFirst && index < kParamMeterFirst)
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
            // kParamMeterFirst and up are outputs: run() owns them, a setter must not.
            if (index >= kParamButtonFirst && index < kParamMeterFirst)
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

        case kStatePatchWrite:
            state.key = "patchwrite";
            state.label = "Store the patch";
            // The UI asking for the WRITE the machine's own PATCH:WRT page does.  Nothing
            // to save: what it produces is inside the memory that already is.
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;

        case kStatePatchDump:
            state.key = "patchdump";
            state.label = "The patch being edited";
            // The active patch buffer as hex, so the UI can save it to the library without
            // the DSP ever opening a file.  Not saved into a session: it is a copy of
            // something already inside the memory that is.
            state.hints = kStateIsOnlyForUI;
            state.defaultValue = "";
            break;

        case kStatePatchLoad:
            state.key = "patchload";
            state.label = "Load a patch from the library";
            // "<slot> <232 hex characters>".  The other direction: the UI read the file.
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;

        case kStateTones:
            state.key = "tones";
            state.label = "Tone names";
            // Every tone the machine can reach: the internal 99 and whatever is on the
            // mounted cards, grouped by which.  Read out of the ROMs, so it costs the
            // emulation nothing.
            state.hints = kStateIsOnlyForUI;
            state.defaultValue = "";
            break;

        case kStateToneSel:
            state.key = "tonesel";
            state.label = "Select tone";
            // "part media tone" from the UI.  See sendToneSelect().
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;
        case kStateDiveVals:
            state.key = "divevals";
            state.label = "DIVE values";
            // The answers to the last diveread, plus the SETUP bytes and the patch
            // name, which cost nothing because they are read straight out of RAM.
            state.hints = kStateIsOnlyForUI;
            state.defaultValue = "";
            break;
        case kStateDiveRead:
            state.key = "diveread";
            state.label = "Read DIVE values";
            // "<part> p07 p08 c18 ..." -- the UI names what its open page needs.
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;
        case kStateDiveWrite:
            state.key = "divewrite";
            state.label = "Write a DIVE value";
            // "p <part> <addr> <value>", "c <addr> <value>", "r <addr> <value>",
            // "b <addr> <bit> <0|1>", or "n <name>".
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;
        case kStateDiag:
            state.key = "diag";
            state.label = "Why the machine is or is not running";
            // The self-diagnosis, in words, for the UI to show when the LCD is clicked.
            // Not saved: it describes THIS machine's filesystem, and a session carried to
            // another one would be reporting somebody else's directories.
            state.hints = kStateIsOnlyForUI;
            state.defaultValue = "";
            break;
        case kStateDiagReq:
            state.key = "diagreq";
            state.label = "Ask for the report";
            // The UI asking.  Composed on demand rather than pushed continuously: it is
            // some kilobytes of directory listing and nothing looks at it until somebody
            // clicks.
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;

        case kStateCardList:
            state.key = "cardlist";
            state.label = "Cards available and mounted";
            // What images are on the ROM search path, and what is in each of the four
            // slots -- including the FIRMWARE's verdict on it, which is the only opinion
            // that decides whether a card's tones can be played.  Not saved into a
            // session: which images exist is a fact about this machine's disk, and the
            // slot assignments are already in "settings".
            state.hints = kStateIsOnlyForUI;
            state.defaultValue = "";
            break;

        case kStateCardSet:
            state.key = "cardset";
            state.label = "Put a card in a slot";
            // "<slot> <path>", or "<slot> -" to empty it.  Nothing to save: what it
            // produces is recorded in "settings" like any other mounted card.
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;

        case kStateCardScan:
            state.key = "cardscan";
            state.label = "Look for card images again";
            // The UI asking, when it opens the page.  Rescanned rather than watched, for
            // the same reason the preset library is: somebody may have just dropped a
            // file in, and nothing else would ever notice.
            state.hints = kStateIsOnlyForDSP;
            state.defaultValue = "";
            break;

        case kStateReboot:
            state.key = "reboot";
            state.label = "Restart the machine";
            // The RESET button.  Nothing to save: a session records what the memory HOLDS,
            // and how the machine last came up is not part of that.
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
        if (std::strcmp(key, "patchdump") == 0)
            return String(m_patchDumpHex);
        if (std::strcmp(key, "tones") == 0)
            return String(m_tonesText);
        // Composed on the spot rather than answered from the cache: a UI opening into a
        // host that is not running audio yet has never had a push, and this is the page
        // that has to be right at that moment.
        if (std::strcmp(key, "cardlist") == 0)
            return String(composeCardList().c_str());
        // Answered here as well as pushed, because a UI opening into a host that is not
        // running audio yet gets its whole first picture through getState() -- and a
        // machine that never started is exactly the case this report is for.
        if (std::strcmp(key, "diag") == 0)
            return String(composeDiag().c_str());
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

        if (std::strcmp(key, "patchwrite") == 0)
        {
            // Same thread story as patchsel, and the same digit test: a restored session
            // hands every key back, and an empty value would read as slot 0.
            if (value[0] < '0' || value[0] > '9')
                return;
            const int n = std::atoi(value);
            if (n >= 0 && unsigned(n) < kNumPatches)
                m_writeRequest.store(n, std::memory_order_relaxed);
            return;
        }

        if (std::strcmp(key, "patchload") == 0)
        {
            // "<slot> <hex>".  Everything is checked HERE, on the UI's thread, so that
            // run() is handed either a whole valid record or nothing at all: these bytes
            // came out of a file that anybody may have written.
            char *end = nullptr;
            const long slot = std::strtol(value, &end, 10);
            if (end == value || slot < 0 || slot >= long(kNumPatches))
                return;
            while (*end == ' ')
                end ++;
            if (std::strlen(end) != size_t(kPatchRecordBytes) * 2)
                return;
            PatchLoad job;
            job.slot = uint8_t(slot);
            for (unsigned i = 0; i < kPatchRecordBytes; i ++)
            {
                const int hi = hexNibble(end[i * 2]), lo = hexNibble(end[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                    return;
                job.rec[i] = uint8_t((hi << 4) | lo);
            }
            m_loadJob = job;
            m_loadReady.store(true, std::memory_order_release);
            return;
        }

        if (std::strcmp(key, "tonesel") == 0)
        {
            // "part media tone".  Same thread story as patchsel above, and the same reason
            // for the digit test: the host stores this key and hands the empty value back.
            unsigned part = 0, media = 0, tone = 0;
            if (std::sscanf(value, "%u %u %u", &part, &media, &tone) != 3)
                return;
            if (part >= kNumParts || media > 0x1f || tone > 0x7f)
                return;
            m_toneRequest.store(int(part << 16 | media << 8 | tone),
                                std::memory_order_relaxed);
            return;
        }

        if (std::strcmp(key, "diveread") == 0)
        {
            // "<part> p07 p08 c18 ..." -- one token per value the open page wants.
            // Left for run() to carry out, like every other request from the UI: the
            // answers come back over MIDI in emulated time, which only run() has.
            DiveJob job;
            const char *s = value;
            char *end = nullptr;
            const long part = std::strtol(s, &end, 10);
            if (end == s || part < 0 || part >= long(kNumParts))
                return;
            s = end;
            while (*s != '\0' && job.n < kDiveMax)
            {
                while (*s == ' ') s ++;
                const char kind = *s;
                if (kind != 'p' && kind != 'c')
                    break;
                const long addr = std::strtol(s + 1, &end, 16);
                if (end == s + 1 || addr < 0 || addr > 0x7f)
                    break;
                job.ask[job.n].kind = uint8_t(kind);
                job.ask[job.n].addr = uint8_t(addr);
                job.n ++;
                s = end;
            }
            job.part = uint8_t(part);
            m_diveJob = job;
            m_diveJobReady.store(true, std::memory_order_release);
            return;
        }

        if (std::strcmp(key, "divewrite") == 0)
        {
            queueDiveWrite(value);
            return;
        }

        if (std::strcmp(key, "reboot") == 0)
        {
            // Left for run() to carry out, like every other request from the UI: a reboot
            // is five and a half seconds of EMULATED time, which only run() has.
            if (std::strcmp(value, "warm") == 0)
                m_rebootRequest.store(kRebootWarm, std::memory_order_relaxed);
            else if (std::strcmp(value, "test") == 0)
                m_rebootRequest.store(kRebootTest, std::memory_order_relaxed);
            else if (std::strcmp(value, "init") == 0)
                m_rebootRequest.store(kRebootInit, std::memory_order_relaxed);
            return;
        }

        if (std::strcmp(key, "cardscan") == 0)
        {
            // Composed HERE, like diagreq below and for the same reason: it opens
            // directories and builds a string, and run() may do neither.
            m_cardListOut = composeCardList();
            m_cardListPending.store(true, std::memory_order_release);
            return;
        }

        if (std::strcmp(key, "cardset") == 0)
        {
            queueCardSet(value);
            return;
        }

        if (std::strcmp(key, "diagreq") == 0)
        {
            // Composed HERE, on the UI's thread, and only handed over as a finished
            // string: run() may not allocate, and this one allocates freely -- it walks
            // directories and builds some kilobytes of text.
            m_diagOut = composeDiag();
            m_diagPending.store(true, std::memory_order_release);
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

        // Counted before anything can return early, because "the host has never called
        // this" is itself one of the answers the report has to be able to give.
        m_runCount.fetch_add(1, std::memory_order_relaxed);

        // Above the ROM check on purpose: the report matters MOST when the machine did
        // not start, and everything below this line is skipped in that case.  The text
        // was built on the UI's thread; all that happens here is handing it over.
        if (m_diagPending.load(std::memory_order_acquire))
        {
            m_diagPending.store(false, std::memory_order_relaxed);
            updateStateValue("diag", m_diagOut.c_str());
        }

        // Same story: built on another thread, handed over here.  Above the ROM check
        // too, because "no card images anywhere" is one of the things it has to be able
        // to say.
        if (m_cardListPending.load(std::memory_order_acquire))
        {
            m_cardListPending.store(false, std::memory_order_relaxed);
            updateStateValue("cardlist", m_cardListOut.c_str());
        }

        if (!m_romsLoaded)
        {
            std::memset(outL, 0, sizeof(float) * frames);
            std::memset(outR, 0, sizeof(float) * frames);
            return;
        }

        // Both channels are in lockstep by construction, so either may be asked.
        const uint32_t coreFrames = m_resampler[0].inputsFor(frames);

        // Before the render, so a button edge lands at the start of the block it belongs
        // to rather than at the start of the next one.  The reboot goes first because it
        // cancels the other three.
        tickReboot(coreFrames);
        tickCards(coreFrames);
        tickPatchWrite();
        tickPatchSelect(coreFrames);
        tickToneSelect(coreFrames);
        tickDive(coreFrames);

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
        float peakL = 0.0f, peakR = 0.0f;
        for (uint32_t i = 0; i < frames; i ++)
        {
            m_gain += (m_gainTarget - m_gain) * 0.001f;
            outL[i] *= m_gain;
            outR[i] *= m_gain;
            const float a = std::fabs(outL[i]), b = std::fabs(outR[i]);
            if (a > peakL) peakL = a;
            if (b > peakR) peakR = b;
        }
        updateMeters(peakL, peakR, frames);

        // The lamp is the louder of the two: one channel over full scale is a clipped
        // output whichever one it was.
        const float peak = peakL > peakR ? peakL : peakR;
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
    ///
    /// Deliberately SHORTER than the meter's peak-hold dwell below, so after a transient
    /// over 0 dB there is a window where the lamp is dark and the marker is still in the
    /// red.  They are not the same statement: the lamp is an EVENT ("you clipped"), the
    /// marker is a LEVEL ("your peak was here").  Syncing them would lose that.
    static constexpr double kClipHoldSeconds = 0.4;

    // ---- the meter's scale ----------------------------------------------------------
    //
    // -42 dB is not the usual -60, and the reason is specific to this machine: there is
    // no fixed noise floor to reveal.  Both of its error sources are signal-scaled -- the
    // wave ROM is an 8-bit mini-float whose error is proportional to level by
    // construction, and the interpolator "follows the note down instead of sitting at a
    // fixed floor" (roland_lp.cpp, sample_interpolate_raw).  So a deeper floor would
    // spend a third of a 132-unit box on a region that never has anything in it, at the
    // cost of resolution where the music actually is.
    //
    // +12 dB of over-range because above 0 is NORMAL here, not an error state.  0 dBFS is
    // calibrated as ONE VOICE at full envelope with a full-scale sample (roland_lp.h,
    // VOICE_FS), the volume pot adds up to another 16 dB after the DAC, and every voice
    // that joins the mix is a plain float += with nothing to clip against.  So the
    // question a user has is not "am I over" but "by how much", and only a scale that
    // extends past 0 can answer it.  Attenuating afterwards recovers the signal exactly.
    static constexpr float kMeterFloorDb = -42.0f;
    static constexpr float kMeterCeilDb  =  12.0f;

    /// How fast the bar falls.  Deliberately quick: the bar answers "what is happening
    /// now", and a slow one smears a phrase into one long slope that says little about
    /// the notes in it.  60 dB/s empties the whole scale in 0.9 s, one 3 dB segment every
    /// 50 ms, so the bar follows anything the machine's own envelopes do.
    ///
    /// It can be this fast ONLY because the peak-hold marker below exists.  Without it
    /// the bar's decay was also the only memory of the peak, and it had to fall slowly
    /// enough that a 25 Hz reader could not miss a transient between frames.  The marker
    /// took that job, so the bar no longer has two to do.
    ///
    /// The ballistics are HERE and not in the UI because the read cadence is the host's
    /// to choose: the value on the wire has to be correct at any instant it is read.
    static constexpr double kMeterFallDbPerSec = 60.0;

    /// The peak-hold marker: sit still long enough to be read, then slide down slower
    /// than the bar so it stays clear of it.  12 dB/s is one 3 dB segment per 250 ms,
    /// which reads as a falling marker rather than a jump -- and at a fifth of the bar's
    /// rate it separates from it immediately, which is what makes the two legible as two
    /// different statements rather than one thick bar.
    static constexpr double kHoldDwellSeconds  = 1.5;
    static constexpr double kHoldFallDbPerSec  = 12.0;

    /// Everything the panel needs, in one fixed layout.  Hex rather than base64 so the
    /// encoder is four lines and has no dependency; 99 bytes becomes 198 characters, which
    /// is nothing next to an audio buffer.
    struct PanelBlob
    {
        uint8_t lcd[32];
        uint8_t cgram[64];
        uint8_t leds, cursor_pos, cursor_flags;
        uint8_t patch;              ///< 0-based, so the LCD's P-01 is 0
        // What each part is playing, straight out of the active patch's part records.
        uint8_t part_media[6];      ///< 0 = internal, otherwise a card ID
        uint8_t part_tone[6];       ///< tone within that media, counting from 0
        uint8_t part_flags[6];      ///< as the firmware keeps them; see kPartFlagsOffset
        uint8_t part_chan[6];       ///< MIDI receive channel in the low nibble
        // The FIRMWARE's verdict on each card slot, from its own cache at 0x2743:
        // 0 = nothing there, 0xFF = it looked at the card and refused it ("Illegal CARD"),
        // otherwise the card's catalogue ID.  This and not the plugin's own bookkeeping is
        // what decides whether a card's tones can be played, so it is what the cartridge
        // page shows.  Four bytes that move only when a card does, so the memcmp above
        // stays quiet.
        uint8_t card_id[4];
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

    /// Peak, decay and peak-hold, once per block.
    ///
    /// Everything here is in HOST FRAMES rather than blocks, so the ballistics are the
    /// same whatever buffer size the host runs -- the same trap publishPanel() documents.
    /// Two log10 and a handful of compares per block; the per-sample work was already
    /// being done for the clip lamp.
    void updateMeters(float peakL, float peakR, uint32_t frames)
    {
        const float peak[2] = { peakL, peakR };
        const double secs = double(frames) / m_hostRate;
        const float barFall  = float(kMeterFallDbPerSec * secs);
        const float holdFall = float(kHoldFallDbPerSec * secs);
        const uint32_t dwell = uint32_t(kHoldDwellSeconds * m_hostRate);

        for (int c = 0; c < 2; c ++)
        {
            // The floor is applied to the ARGUMENT as well as the result: log10(0) is
            // -inf, and an -inf that reached the port would poison every host that
            // displays it.  1e-6 is -120 dB, far below anything the scale shows.
            const float db = peak[c] > 1.0e-6f
                    ? 20.0f * std::log10(peak[c]) : kMeterFloorDb;

            // Instant attack, exponential fall -- which is linear in dB, so the rate is
            // just a subtraction.
            m_meterDb[c] = std::max(db, m_meterDb[c] - barFall);
            m_meterDb[c] = std::min(std::max(m_meterDb[c], kMeterFloorDb), kMeterCeilDb);

            // The marker takes the BAR's value, never this block's raw peak.  That is
            // what makes "the marker is never below the bar" true by construction rather
            // than by luck: the bar has an instant attack, so it is always at least the
            // raw peak, and the marker is therefore the running maximum of something the
            // bar already reached.  Assigning `db` here instead puts the marker below the
            // lit segments for a third of the run -- measured, not guessed.
            //
            // The dwell re-arms every block the bar is still at its maximum, so the
            // countdown starts when the bar begins to fall, not when the note began.
            if (m_meterDb[c] >= m_holdDb[c])
            {
                m_holdDb[c] = m_meterDb[c];
                m_holdDwell[c] = dwell;
            }
            else if (m_holdDwell[c] > frames)
            {
                m_holdDwell[c] -= frames;
            }
            else
            {
                m_holdDwell[c] = 0;
                m_holdDb[c] = std::max(m_meterDb[c], m_holdDb[c] - holdFall);
            }
        }
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
            for (unsigned i = 0; i < kNumParts; i ++)
            {
                const uint16_t rec = uint16_t(kPartBase + kPartStride * i);
                blob.part_media[i] = m_core.readMem(uint16_t(rec + kPartMediaOffset));
                blob.part_tone[i]  = m_core.readMem(uint16_t(rec + kPartToneOffset));
                blob.part_flags[i] = m_core.readMem(uint16_t(rec + kPartFlagsOffset));
                blob.part_chan[i]  = m_core.readMem(uint16_t(rec + kPartChannelOffset));
            }
            for (unsigned i = 0; i < voltaire::kNumCardSlots; i ++)
                blob.card_id[i] = m_core.readMem(uint16_t(kCardIdAddr + i));
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
        refreshPatchDump();
        refreshToneNames();
    }

    /// The patch the machine is playing, as 116 bytes of hex, for the UI to save.
    ///
    /// Pushed only when it changes, like the panel and the names.  0x2800 is the single
    /// source of truth for the current patch -- panel edits and DIVE edits both land
    /// there -- so reading it back is what stops the library from saving a patch the
    /// machine is not actually playing.
    void refreshPatchDump()
    {
        uint8_t rec[kPatchRecordBytes];
        for (unsigned i = 0; i < kPatchRecordBytes; i ++)
            rec[i] = m_core.readMem(uint16_t(kActivePatch + i));

        char hex[sizeof(m_patchDumpHex)];
        encodeHex(rec, sizeof(rec), hex);
        if (std::memcmp(hex, m_patchDumpHex, sizeof(hex)) == 0)
            return;
        std::memcpy(m_patchDumpHex, hex, sizeof(hex));
        updateStateValue("patchdump", m_patchDumpHex);
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

    /// Every tone the machine can reach, for the UI's menu.
    ///
    /// One line per record so it can be read in a session file: "G <media> <label>" opens
    /// a group and "T <name>" is a tone in it, the marker being the first character rather
    /// than anything in the text, so a name may say whatever it likes.
    ///
    /// The names come out of the ROM images the core already holds, descrambled -- see
    /// U110Core::readWaveRom.  The alternative was walking the machine's own TONE menu and
    /// reading the LCD back, which costs emulated time and only works when the display is
    /// on that page.
    void refreshToneNames()
    {
        // Tones live in ROM and never change; what changes is which cards are mounted.
        // The firmware's own cache of that is four bytes, so reading those and rebuilding
        // only when they move keeps the whole list off the 20 Hz path in the ordinary
        // case, which is every case after the machine has finished booting.
        bool same = m_tonesText[0] != '\0';
        for (unsigned slot = 0; slot < voltaire::kNumCardSlots; slot ++)
        {
            const uint8_t id = m_core.readMem(uint16_t(kCardIdAddr + slot));
            same = same && id == m_cardIdSeen[slot];
            m_cardIdSeen[slot] = id;
        }
        if (same)
            return;

        size_t at = 0;
        appendToneGroup(at, 0, "Internal");
        for (unsigned n = 0; n < kNumInternalTones; n ++)
        {
            uint8_t name[kToneNameLen];
            m_core.readWaveRom(0, kToneBase + kToneStride * n, name, sizeof(name));
            appendToneName(at, name);
        }

        for (unsigned slot = 0; slot < voltaire::kNumCardSlots; slot ++)
        {
            // The firmware's own view of what is mounted, not the plugin's: a card the
            // machine refused (0xFF) has no tones to offer whatever the file was called.
            const uint8_t media = m_core.readMem(uint16_t(kCardIdAddr + slot));
            if (media == 0x00 || media == 0xff)
                continue;

            char label[32];
            if (!m_cards[slot].label.empty())
                std::snprintf(label, sizeof(label), "%s", m_cards[slot].label.c_str());
            else
                std::snprintf(label, sizeof(label), "Card %02u", media);
            appendToneGroup(at, media, label);

            for (unsigned n = 0; n < kMaxCardTones; n ++)
            {
                uint8_t name[kToneNameLen];
                m_core.readCardRom(slot, kToneBase + kToneStride * n, name, sizeof(name));
                bool blank = true;
                for (const uint8_t c : name)
                    blank = blank && (c == ' ' || c < 0x20 || c >= 0x7f);
                if (blank)
                    break;
                appendToneName(at, name);
            }
        }
        m_toneScratch[at] = '\0';

        if (std::memcmp(m_toneScratch, m_tonesText, at + 1) == 0)
            return;
        std::memcpy(m_tonesText, m_toneScratch, at + 1);
        updateStateValue("tones", m_tonesText);
    }

    void appendToneGroup(size_t &at, unsigned media, const char *label)
    {
        if (at + 48 >= sizeof(m_toneScratch))
            return;
        at += size_t(std::snprintf(m_toneScratch + at, sizeof(m_toneScratch) - at,
                                   "G %u %s\n", media, label));
    }

    void appendToneName(size_t &at, const uint8_t *name)
    {
        if (at + kToneNameLen + 4 >= sizeof(m_toneScratch))
            return;
        m_toneScratch[at ++] = 'T';
        m_toneScratch[at ++] = ' ';
        const size_t start = at;
        for (unsigned i = 0; i < kToneNameLen; i ++)
            m_toneScratch[at ++] = (name[i] >= 0x20 && name[i] < 0x7f) ? char(name[i]) : '.';
        while (at > start && m_toneScratch[at - 1] == ' ')
            at --;
        m_toneScratch[at ++] = '\n';
    }

    // ---- picking a tone for one part ------------------------------------------------
    //
    // Not by poking the part record.  Writing the tone number into the active patch does
    // change what the record SAYS, and nothing else happens: the sound comes from the
    // 80-byte parameter record the firmware copies to 0x2880 + 0x50 * part, plus the
    // twelve sample records it then builds at 0x2A60 and the voice state at 0x3760.  The
    // routine that does all that is at 0x80D3, and there is no way to call it from out
    // here.  Copying the parameter record by hand gets the bytes right and the sound
    // wrong, which was worth finding out once.
    //
    // So this asks the machine the way anything else would: a Roland SysEx DT1 write to
    // the part's temporary tone parameters, address 00 1n 02 (media) and 00 1n 03 (tone
    // number).  The firmware then does its own job properly, per PART, with no dependence
    // on what the display is showing and no button pressing -- and it reaches CARD tones,
    // which a MIDI program change cannot: program change carries a tone number and nothing
    // to say which card it is on.
    //
    // The one gate is SETUP:MIDI:EXCLUSIVE, bit 5 of 0x3C00, which the firmware re-reads
    // for every message at 0x561F.  With it clear the message is dropped in silence, so if
    // the user has turned it off it is turned on for as long as the message takes to
    // arrive and then put back exactly as it was.

    /// Long enough for eleven bytes to clock in at 31250 baud and be parsed -- measured at
    /// well under 30 ms of emulated time for the pair.
    static constexpr uint32_t kSysexFrames = voltaire::kCoreSampleRate * 50 / 1000;

    void tickToneSelect(uint32_t coreFrames)
    {
        if (m_rxRestoreWait != 0)
        {
            if (m_rxRestoreWait > coreFrames)
                m_rxRestoreWait -= coreFrames;
            else
            {
                m_rxRestoreWait = 0;
                m_core.writeMem(kRxSwitchAddr, m_rxSaved);
            }
        }

        const int want = m_toneRequest.exchange(-1, std::memory_order_relaxed);
        if (want < 0 || !m_romsLoaded)
            return;

        const unsigned part  = unsigned(want) >> 16 & 0xff;
        const unsigned media = unsigned(want) >> 8 & 0xff;
        const unsigned tone  = unsigned(want) & 0xff;

        openExclusive(kSysexFrames);

        const uint8_t dev = m_core.readMem(kDeviceIdAddr) & 0x7f;
        sendDt1(dev, 0x00, uint8_t(0x10 + part), 0x02, uint8_t(media));
        sendDt1(dev, 0x00, uint8_t(0x10 + part), 0x03, uint8_t(tone));
    }

    /// Hold SETUP:MIDI:EXCLUSIVE open for `frames`, remembering what it was.
    ///
    /// The switch is the user's, and a machine with it off is a machine that has been
    /// told not to listen; borrowing it for the length of a message and handing it back
    /// is the only way to edit a parameter without overriding that.
    void openExclusive(uint32_t frames)
    {
        const uint8_t rx = m_core.readMem(kRxSwitchAddr);
        if ((rx & kRxExclusive) == 0)
        {
            // Only save the switches if we are not already holding them open, or a second
            // request would record the value we ourselves put there.
            if (m_rxRestoreWait == 0)
                m_rxSaved = rx;
            m_core.writeMem(kRxSwitchAddr, uint8_t(rx | kRxExclusive));
        }
        if (m_rxRestoreWait != 0 || (rx & kRxExclusive) == 0)
            m_rxRestoreWait = frames;
    }

    /// One Roland RQ1 read request.  The machine answers with a DT1 carrying the value.
    void sendRq1(uint8_t dev, uint8_t a1, uint8_t a2, uint8_t a3)
    {
        const int sum = a1 + a2 + a3 + 0 + 0 + 1;
        const uint8_t msg[] = { 0xf0, 0x41, dev, 0x23, 0x11, a1, a2, a3, 0x00, 0x00, 0x01,
                                uint8_t((128 - (sum & 0x7f)) & 0x7f), 0xf7 };
        m_core.midiIn(msg, sizeof(msg), 0);
    }

    /// One Roland DT1 write: F0 41 <dev> 23 12 <address> <data> <sum> F7, the checksum
    /// covering address and data and summing to zero in seven bits.
    void sendDt1(uint8_t dev, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t value)
    {
        const int sum = a1 + a2 + a3 + value;
        const uint8_t msg[] = { 0xf0, 0x41, dev, 0x23, 0x12, a1, a2, a3, value,
                                uint8_t((128 - (sum & 0x7f)) & 0x7f), 0xf7 };
        m_core.midiIn(msg, sizeof(msg), 0);
    }

    // ---- the DIVE editor -------------------------------------------------------------
    //
    // Reading a value back is one RQ1 per parameter.  The 16-byte part record packs all
    // 26 of a part's parameters into it and the packing is unmapped, but every address in
    // the Owner's Manual's individual-parameter map answers an RQ1 with the value already
    // decoded -- so the packing never has to be solved.  See SYSTEM-DESIGN 5.3.2, where
    // each of these was read off the running machine.
    //
    // The size field is ignored on that map: asking for a part's 26 parameters at once
    // returns exactly one byte.  So a page is one request per control, serially, which at
    // 22 bytes on the wire each is a few milliseconds apiece -- cheap enough to redo on
    // every tab click, and the reason not to reverse-engineer the record.
    //
    // SETUP has no SysEx address anywhere, so those go straight to battery-backed RAM,
    // and so does the patch name: nothing is derived from it, unlike a tone.

    /// How long to wait for one answer before giving up on it and asking the next
    /// question.  A reply lands about 15 ms after the request starts clocking in -- 11
    /// bytes out at 31250 baud, then the firmware's own turnaround -- and the wait ends
    /// as soon as the answer is actually in, so this is only the failure case.
    static constexpr uint32_t kRq1Timeout = voltaire::kCoreSampleRate * 60 / 1000;

    static constexpr int kDiveMax = 48;

    struct DiveAsk { uint8_t kind, addr; };     ///< 'p' a part parameter, 'c' patch common

    struct DiveJob
    {
        uint8_t part = 0;
        uint8_t n = 0;
        DiveAsk ask[kDiveMax] = {};
    };

    /// One queued write.  'p'/'c' go out as SysEx, 'r'/'b' straight into RAM.
    struct DiveWrite { uint8_t kind, part, value; uint16_t addr; uint8_t bit; };

    void queueDiveWrite(const char *value)
    {
        DiveWrite w {};
        unsigned a = 0, b = 0, v = 0, p = 0;
        switch (value[0])
        {
        case 'p':
            if (std::sscanf(value + 1, "%u %x %u", &p, &a, &v) != 3) return;
            if (p >= kNumParts || a > 0x7f || v > 0x7f) return;
            w = { 'p', uint8_t(p), uint8_t(v), uint16_t(a), 0 };
            break;
        case 'c':
            if (std::sscanf(value + 1, "%x %u", &a, &v) != 2) return;
            if (a > 0x7f || v > 0x7f) return;
            w = { 'c', 0, uint8_t(v), uint16_t(a), 0 };
            break;
        case 'r':
            if (std::sscanf(value + 1, "%x %u", &a, &v) != 2) return;
            if (v > 0xff) return;
            w = { 'r', 0, uint8_t(v), uint16_t(a), 0 };
            break;
        case 'b':
            if (std::sscanf(value + 1, "%x %u %u", &a, &b, &v) != 3) return;
            if (b > 7) return;
            w = { 'b', 0, uint8_t(v ? 1 : 0), uint16_t(a), uint8_t(b) };
            break;
        case 'n':
        {
            // The patch name, straight into the edit buffer.  Ten characters, padded,
            // and nothing outside the machine's own character set gets through.
            const char *s = value + 1;
            while (*s == ' ') s ++;
            for (unsigned i = 0; i < kPatchNameLen; i ++)
            {
                const char c = s[i] != '\0' ? s[i] : ' ';
                m_nameWrite[i] = (c >= 0x20 && c < 0x7f) ? uint8_t(c) : uint8_t(' ');
                if (s[i] == '\0')
                    for (unsigned j = i + 1; j < kPatchNameLen; j ++)
                        m_nameWrite[j] = ' ';
                if (s[i] == '\0')
                    break;
            }
            m_nameWritePending.store(true, std::memory_order_release);
            return;
        }
        default:
            return;
        }

        const unsigned head = m_diveWrHead.load(std::memory_order_relaxed);
        const unsigned next = (head + 1) % kDiveWriteQueue;
        if (next == m_diveWrTail.load(std::memory_order_acquire))
            return;                                  // full: drop rather than block
        m_diveWrites[head] = w;
        m_diveWrHead.store(next, std::memory_order_release);
    }

    void tickDive(uint32_t coreFrames)
    {
        if (!m_romsLoaded)
            return;

        // Nothing is answered until the machine has finished booting.  It takes about
        // five and a half seconds, and a session that comes back with the drawer open
        // asks before then -- at which point RAM reads as zeros, which is a perfectly
        // plausible answer and therefore the worst possible one.  Saying nothing leaves
        // the UI showing dots and asking again, which is the truth.
        //
        // Reaching the play screen once is the signal, and it is only needed once: a
        // machine sitting in one of its own menus is still perfectly able to answer, and
        // gating every read on the display would stall the drawer for as long as somebody
        // left a menu open.
        if (!m_machineUp)
        {
            if (!onPlayScreen())
                return;
            m_machineUp = true;
        }

        // Writes first, so a value the user just moved is in the machine before the read
        // that would otherwise report the old one back at them.
        drainDiveWrites();

        if (m_nameWritePending.exchange(false, std::memory_order_acquire))
            for (unsigned i = 0; i < kPatchNameLen; i ++)
                m_core.writeMem(uint16_t(kActivePatch + kPatchNameOffset + i), m_nameWrite[i]);

        if (!m_diveBusy)
        {
            if (!m_diveJobReady.exchange(false, std::memory_order_acquire))
                return;
            m_diveRun = m_diveJob;
            m_diveAt = 0;
            m_diveWait = 0;
            m_diveRxLen = 0;
            std::memset(m_diveHave, 0, sizeof(m_diveHave));
            m_diveBusy = m_diveRun.n != 0;
            if (!m_diveBusy)
            {
                publishDiveVals();                   // SETUP and the name, at least
                return;
            }
        }

        // Wait for the answer to the question already asked, not for a fixed delay.
        // Publishing on a timer instead was the bug that left the last value on every
        // page showing dots: the reply to the final request had not arrived yet.
        if (m_diveAt > 0 && !m_diveHave[m_diveAt - 1])
        {
            if (m_diveWait > coreFrames)
            {
                m_diveWait -= coreFrames;
                return;
            }
            // Timed out.  Move on rather than stalling the page on one dead address.
        }

        if (m_diveAt >= m_diveRun.n)
        {
            m_diveBusy = false;
            publishDiveVals();
            return;
        }

        openExclusive(kRq1Timeout);
        const uint8_t dev = m_core.readMem(kDeviceIdAddr) & 0x7f;
        const DiveAsk &a = m_diveRun.ask[m_diveAt];
        sendRq1(dev, 0x00,
                a.kind == 'p' ? uint8_t(0x10 + m_diveRun.part) : uint8_t(0x01), a.addr);
        m_diveAt ++;
        m_diveWait = kRq1Timeout;
    }

    void drainDiveWrites()
    {
        unsigned tail = m_diveWrTail.load(std::memory_order_relaxed);
        const unsigned head = m_diveWrHead.load(std::memory_order_acquire);
        if (tail == head)
            return;
        while (tail != head)
        {
            const DiveWrite &w = m_diveWrites[tail];
            if (w.kind == 'r')
                m_core.writeMem(w.addr, w.value);
            else if (w.kind == 'b')
            {
                const uint8_t cur = m_core.readMem(w.addr);
                const uint8_t bit = uint8_t(1u << w.bit);
                const uint8_t now = uint8_t(w.value ? (cur | bit) : (cur & ~bit));
                m_core.writeMem(w.addr, now);
                // EXCLUSIVE is the switch this code borrows to speak at all.  If the user
                // has just turned it off, the borrow must not put it back on afterwards.
                if (w.addr == kRxSwitchAddr && w.bit == 5)
                {
                    m_rxSaved = now;
                    m_rxRestoreWait = 0;
                }
            }
            else
            {
                openExclusive(kSysexFrames);
                const uint8_t dev = m_core.readMem(kDeviceIdAddr) & 0x7f;
                sendDt1(dev, 0x00,
                        w.kind == 'p' ? uint8_t(0x10 + w.part) : uint8_t(0x01),
                        uint8_t(w.addr), w.value);
            }
            tail = (tail + 1) % kDiveWriteQueue;
        }
        m_diveWrTail.store(tail, std::memory_order_release);
    }

    /// Collect one byte of the machine's answer.  Only complete, well-formed DT1s whose
    /// address is one this read asked for are kept; anything else is discarded, which is
    /// what stops a stray reply being reported as a value.
    void diveReplyByte(uint8_t b)
    {
        if (b == 0xf0)
            m_diveRxLen = 0;
        if (m_diveRxLen < sizeof(m_diveRx))
            m_diveRx[m_diveRxLen ++] = b;
        if (b != 0xf7 || m_diveRxLen != 11)
            return;
        if (m_diveRx[0] != 0xf0 || m_diveRx[1] != 0x41 || m_diveRx[3] != 0x23
                || m_diveRx[4] != 0x12)
            return;
        const uint8_t a2 = m_diveRx[6], a3 = m_diveRx[7], v = m_diveRx[8];
        if (m_diveRx[5] != 0x00)
            return;
        for (uint8_t i = 0; i < m_diveRun.n; i ++)
        {
            const DiveAsk &a = m_diveRun.ask[i];
            const uint8_t want = a.kind == 'p' ? uint8_t(0x10 + m_diveRun.part) : 0x01;
            if (a2 == want && a3 == a.addr)
            {
                m_diveVal[i] = v;
                m_diveHave[i] = true;
                return;
            }
        }
    }

    /// Everything the open page needs, in one string.  The SETUP bytes and the patch name
    /// ride along because they are RAM reads and cost nothing.
    void publishDiveVals()
    {
        char *p = m_diveText;
        char *const end = m_diveText + sizeof(m_diveText) - 1;
        p += std::snprintf(p, size_t(end - p), "%u", m_diveRun.part);
        for (uint8_t i = 0; i < m_diveRun.n && p < end; i ++)
        {
            if (!m_diveHave[i])
                continue;
            p += std::snprintf(p, size_t(end - p), " %c%02x=%02x",
                               char(m_diveRun.ask[i].kind), m_diveRun.ask[i].addr,
                               m_diveVal[i]);
        }
        for (uint16_t a = kRxSwitchAddr; a <= kRxSwitchAddr + 3 && p < end; a ++)
            p += std::snprintf(p, size_t(end - p), " r%04x=%02x", a, m_core.readMem(a));
        if (p < end)
        {
            p += std::snprintf(p, size_t(end - p), " n=");
            for (unsigned i = 0; i < kPatchNameLen && p < end; i ++)
            {
                const uint8_t c = m_core.readMem(
                        uint16_t(kActivePatch + kPatchNameOffset + i));
                *p ++ = (c >= 0x20 && c < 0x7f) ? char(c) : ' ';
            }
        }
        *p = '\0';
        updateStateValue("divevals", m_diveText);
    }

    // ---- the RESET button -----------------------------------------------------------
    //
    // Three ways of starting the machine again, and all three are the HARDWARE'S, not the
    // plugin's.  The firmware reads the key matrix once during boot and branches on what
    // it finds (ROM-ANALYSIS.md section 8.5): DEC+INC gives the service test menu, and
    // PART+EDIT runs the "Mem Initialized" copy loop that puts the factory patches back.
    // So there is no test mode to write and no memory to erase -- there is only a pair of
    // keys to hold down while the machine comes up.
    //
    // It runs from run() for the same reason the patch selector does: a boot is five and a
    // half seconds of EMULATED time and the audio thread is the only one that has any.
    // The machine plays its own boot screen while it happens, which is what a U-110 does.
    // reset() allocates nothing -- checked with tools/rt_audit.c, not assumed.

    enum { kRebootNone = 0, kRebootWarm, kRebootTest, kRebootInit };

    /// How long the boot keys stay down, in emulated frames.
    ///
    /// MEASURED, not guessed.  The firmware's one look at the key matrix lands between
    /// 3.62 s and 3.68 s after reset -- release at 3.6 s and the machine boots normally,
    /// release at 4.0 s and it comes up in the test menu.  4.5 s clears that with room to
    /// spare.  Holding longer is harmless (nothing scans the keys again until the play
    /// screen is up, and 6 s tested identically), so the margin costs nothing.
    static constexpr uint32_t kBootHoldFrames = voltaire::kCoreSampleRate * 4500 / 1000;

    /// The second half of a card change: the part that has to happen in the machine's own
    /// time.  See queueCardSet() for the first.
    ///
    /// A slot is always emptied first, even one that was already empty, because the
    /// firmware compares the presence pins against a snapshot it took last pass
    /// (0x2747, ROM-ANALYSIS.md section 6.9).  Install over a slot that still reads
    /// "present" and there is no edge to see: the machine goes on serving the old card's
    /// tones from the new card's bytes.  Measured, not guessed -- 600 ms of it.
    ///
    /// Each wait is on the firmware's OWN cache of what it thinks is in the slot, not on
    /// a frame count, because that is the thing that actually has to be true before the
    /// next step is safe.  The timeout is only for a machine that is not answering at all
    /// -- booting, or sitting in the service menu -- and it is generous: mounting takes
    /// 320-330 ms of emulated time, nearly all of it reading the header and the tone
    /// records through the borrowed voice-0 ROM port.
    void tickCards(uint32_t coreFrames)
    {
        if (!m_romsLoaded)
            return;

        if (m_cardStep == CardStep::Idle)
        {
            CardJobState want = CardJobState::Ready;
            if (!m_cardJobState.compare_exchange_strong(want, CardJobState::Running,
                                                        std::memory_order_acquire))
                return;
            m_core.ejectCard(m_cardJobSlot);
            m_cardStep = CardStep::EjectWait;
            m_cardStepFrames = 0;
            return;
        }

        m_cardStepFrames += coreFrames;
        const uint8_t id = m_core.readMem(uint16_t(kCardIdAddr + m_cardJobSlot));

        if (m_cardStep == CardStep::EjectWait)
        {
            // 0 is "nothing there".  0xFF would mean the firmware had looked at a card and
            // refused it, which it cannot still be saying about a slot we just emptied.
            if (id != 0x00 && m_cardStepFrames < kCardStepTimeout)
                return;
            if (!m_cardJobInstall)
            {
                finishCardJob();
                return;
            }
            m_core.installCard(m_cardJobSlot, m_cardStage.data());
            m_cardStep = CardStep::MountWait;
            m_cardStepFrames = 0;
            return;
        }

        // Done when the machine has an opinion either way: an ID, or 0xFF for a card it
        // refused.  Nothing here treats the refusal as an error -- the UI shows what the
        // firmware decided, which is the only verdict that counts.
        if (id == 0x00 && m_cardStepFrames < kCardStepTimeout)
            return;
        finishCardJob();
    }

    void finishCardJob()
    {
        m_cardStep = CardStep::Idle;
        m_cardJobState.store(CardJobState::Idle, std::memory_order_release);
    }

    /// A reboot is about to happen, so finish any card change now rather than half way.
    /// Nothing is lost by hurrying: the machine that comes up runs the same poller with
    /// its "everything is newly inserted" flag set (0x45DE), so it will find whatever is
    /// in the slots without being told.
    void settleCardJob()
    {
        CardJobState want = CardJobState::Ready;
        if (m_cardStep == CardStep::Idle
                && !m_cardJobState.compare_exchange_strong(want, CardJobState::Running,
                                                           std::memory_order_acquire))
            return;
        if (m_cardJobInstall)
            m_core.installCard(m_cardJobSlot, m_cardStage.data());
        else
            m_core.ejectCard(m_cardJobSlot);
        finishCardJob();
    }

    void tickReboot(uint32_t coreFrames)
    {
        const int want = m_rebootRequest.exchange(kRebootNone, std::memory_order_relaxed);
        if (want != kRebootNone && m_romsLoaded)
        {
            // Whatever the other state machines were half way through pressing is over:
            // the machine they were talking to is about to stop existing.  Their requests
            // go too, so a click that arrived a moment before the reboot does not get
            // carried out against the machine that replaces it.
            m_patchStep = PatchStep::Idle;
            m_patchRequest.store(-1, std::memory_order_relaxed);
            m_toneRequest.store(-1, std::memory_order_relaxed);
            m_diveBusy = false;
            m_diveJobReady.store(false, std::memory_order_relaxed);
            // The exception to "their requests go too": a card change is about the
            // SLOTS, not about the machine that was reading them, and the slots survive
            // a reboot exactly as they do on the bench.
            settleCardJob();
            m_nameWritePending.store(false, std::memory_order_relaxed);
            m_diveWrTail.store(m_diveWrHead.load(std::memory_order_acquire),
                               std::memory_order_release);
            // The drawer waits for the play screen again; until then RAM would read as
            // whatever a booting machine happens to have there.
            m_machineUp = false;

            // The borrowed SETUP:MIDI:EXCLUSIVE switch, handed back now rather than by a
            // timer that would fire into a machine mid-boot.  Not on an initialise: that
            // memory is being wiped on purpose and putting one byte of it back would be
            // the one thing the user did not ask for.
            if (m_rxRestoreWait != 0)
            {
                if (want != kRebootInit)
                    m_core.writeMem(kRxSwitchAddr, m_rxSaved);
                m_rxRestoreWait = 0;
            }

            for (int b = 0; b < voltaire::kButtonCount; b ++)
                m_autoButtons[b] = false;
            if (want == kRebootTest)
            {
                m_autoButtons[voltaire::kButtonDec] = true;
                m_autoButtons[voltaire::kButtonIncEnter] = true;
            }
            else if (want == kRebootInit)
            {
                m_autoButtons[voltaire::kButtonPartJump] = true;
                m_autoButtons[voltaire::kButtonEditExit] = true;
            }
            // The keys have to be down BEFORE the reset, because the firmware's look at
            // them is part of the boot it is about to start.
            for (int b = 0; b < voltaire::kButtonCount; b ++)
                applyButton(voltaire::Button(b));

            m_core.reset();
            m_rebootHold = want == kRebootWarm ? 0 : kBootHoldFrames;
        }

        if (m_rebootHold == 0)
            return;
        if (m_rebootHold > coreFrames)
        {
            m_rebootHold -= coreFrames;
            return;
        }
        m_rebootHold = 0;
        for (int b = 0; b < voltaire::kButtonCount; b ++)
        {
            m_autoButtons[b] = false;
            applyButton(voltaire::Button(b));
        }
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

    // ---- storing a patch ------------------------------------------------------------
    //
    // The machine's own WRITE is PATCH:WRT:WRITE on the front panel, and there is no SysEx
    // for it.  It does not have to be driven through those menus: it copies the edit
    // buffer into the slot and nothing else, so the same thing can be done with writeMem.
    // Measured rather than assumed -- analysis/SYSTEM-DESIGN.md section 5.3.4, and
    // `make writecheck` runs both paths and compares what they leave behind.
    //
    // 116 bytes, NOT the 128-byte stride.  The last 12 bytes of a slot are not part of the
    // record and the firmware leaves them untouched; copying 128 would overwrite something
    // the machine put there for its own reasons.
    //
    // MEM PROTECT (0x3C00 bit 0, on from the factory) gates the FIRMWARE's write path, and
    // this one goes around it.  That is deliberate: the button is the plugin's, the
    // confirmation is the plugin's, and quietly refusing here because of a bit the user
    // has never seen would be a lock nobody could find the key to.
    //
    // The reselect afterwards is not decoration.  It is what takes the display out of
    // TEMP: -- that flag lives in the CPU's internal RAM and cannot be written -- and it
    // makes the firmware reload the buffer from the slot, so the machine ends up playing
    // the record that was actually stored rather than the one we believe we stored.

    void tickPatchWrite()
    {
        const bool wantWrite = m_writeRequest.load(std::memory_order_relaxed) >= 0;
        const bool wantLoad  = m_loadReady.load(std::memory_order_acquire);
        if (!wantWrite && !wantLoad)
            return;

        // Held, not dropped, until the machine is up.  Before then 0x2800 reads as zeros,
        // which is a perfectly plausible patch and therefore the worst possible one to
        // store over somebody's work -- and a load before the firmware has finished
        // initialising patchram would be overwritten by it a moment later.
        if (!m_romsLoaded || !m_machineUp)
            return;

        // The WRITE button: the patch being edited, into a slot the user chose.
        if (wantWrite)
        {
            const int n = m_writeRequest.exchange(-1, std::memory_order_relaxed);
            if (n >= 0 && unsigned(n) < kNumPatches)
                storeRecordFrom(unsigned(n), kActivePatch);
        }

        // The library: a patch that came out of a file, into the audition slot.
        if (wantLoad && m_loadReady.exchange(false, std::memory_order_acquire))
        {
            const unsigned n = m_loadJob.slot;
            if (n < kNumPatches)
            {
                const uint16_t dst = uint16_t(kPatchBase + n * kPatchStride);
                for (unsigned i = 0; i < kPatchRecordBytes; i ++)
                    m_core.writeMem(uint16_t(dst + i), m_loadJob.rec[i]);
                m_patchRequest.store(int(n), std::memory_order_relaxed);
            }
        }
    }

    /// Copy one 116-byte record into a slot and then have the firmware load it.
    ///
    /// Order matters: the record is written FIRST and the select triggered after, because
    /// the firmware reads a patch record only during a load -- which makes a torn read
    /// impossible without anything having to be locked.
    void storeRecordFrom(unsigned slot, uint16_t src)
    {
        const uint16_t dst = uint16_t(kPatchBase + slot * kPatchStride);
        for (unsigned i = 0; i < kPatchRecordBytes; i ++)
            m_core.writeMem(uint16_t(dst + i), m_core.readMem(uint16_t(src + i)));
        m_patchRequest.store(int(slot), std::memory_order_relaxed);
    }

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
            // While a DIVE read is in flight, what comes back is the answer to it.  The
            // machine says nothing unprompted, and a read lasts a few milliseconds per
            // value, so swallowing the stream for that long costs the host nothing it
            // would otherwise have received.
            if (m_diveBusy)
            {
                diveReplyByte(buf[i]);
                continue;
            }
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
        std::string label;      ///< what the image calls itself; see mountCard()
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
        // The image names itself, and that beats the filename.  `number` is only what the
        // name suggested; a dump carries its catalogue ID at +0x20 and its own text at
        // +0x10, and that is what the machine reads too.  A file whose header does not
        // check out keeps the filename's number -- it is going to be refused anyway, and
        // saying which card it was MEANT to be is more use in a session file than 0.
        const CardIdent id = identifyCard(img.data(), img.size());
        m_cards[slot] = MountedCard { true, id.valid ? id.number : number, path,
                                      voltaire::Sha256::of(img.data(), img.size()),
                                      id.label };
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

    // ---- changing a card while the machine plays ------------------------------------
    //
    // Two halves, because the work is two very different costs.  Everything expensive
    // happens HERE, on whatever thread the UI's request arrived on: opening the file,
    // reading half a megabyte, descrambling it into the staging buffer.  What is left for
    // run() is a memcpy and a bit.
    //
    // And it cannot be one act even there, because the firmware is edge triggered against
    // its own snapshot of the presence pins: replacing a card means eject, wait for the
    // machine to notice, then install.  Waiting is what tickCards() is for.
    //
    // "<slot> <path>", or "<slot> -" to empty the slot.  The path goes last and is taken
    // to the end of the line, so it may contain spaces.
    void queueCardSet(const char *value)
    {
        char *end = nullptr;
        const long slot = std::strtol(value, &end, 10);
        if (end == value || slot < 0 || slot >= long(voltaire::kNumCardSlots))
            return;
        while (*end == ' ')
            end ++;
        std::string path(end);
        while (!path.empty() && (path.back() == '\n' || path.back() == ' '))
            path.pop_back();
        if (path.empty())
            return;
        const bool eject = path == "-";

        // One change at a time.  They come from a person clicking, so a second one while
        // the first is still in the machine means the person is ahead of a state machine
        // that takes about a third of a second -- say so and drop it, rather than queue
        // work against a slot whose contents are about to change anyway.
        CardJobState expected = CardJobState::Idle;
        if (!m_cardJobState.compare_exchange_strong(expected, CardJobState::Filling,
                                                    std::memory_order_acq_rel))
        {
            d_stderr2("Voltaire 110: still changing a card; slot %ld ignored.", slot);
            return;
        }

        if (!eject)
        {
            const std::vector<uint8_t> img = readFile(path);
            if (img.empty())
            {
                d_stderr2("Voltaire 110: card slot %ld: cannot read %s",
                          slot, path.c_str());
                m_cardJobState.store(CardJobState::Idle, std::memory_order_release);
                return;
            }
            if (voltaire::U110Core::prepareCard(img.data(), img.size(), m_cardStage)
                    != voltaire::LoadResult::Ok)
            {
                d_stderr2("Voltaire 110: card slot %ld: %s is %zu bytes, which is not a "
                          "card image the machine can address.",
                          slot, path.c_str(), img.size());
                m_cardJobState.store(CardJobState::Idle, std::memory_order_release);
                return;
            }

            // The bookkeeping is updated HERE and not when the machine accepts it,
            // because what a session records is which IMAGE is in the slot, and that is
            // true whatever the firmware makes of it.  A card the machine refuses is
            // still the card somebody put in, and it should come back in the same slot
            // and be refused again rather than quietly vanish from the project.
            const CardIdent id = identifyCard(img.data(), img.size());
            unsigned number = id.number;
            if (!id.valid)
            {
                cardNumberFromName(baseName(path), number);
                d_stderr2("Voltaire 110: card slot %ld: %s has no U-110 card header. "
                          "The machine will show \"Illegal CARD\".",
                          slot, baseName(path).c_str());
            }
            m_cards[slot] = MountedCard { true, number, path,
                                          voltaire::Sha256::of(img.data(), img.size()),
                                          id.label };
        }
        else
        {
            m_cards[slot] = MountedCard();
        }

        m_cardJobSlot = unsigned(slot);
        m_cardJobInstall = !eject;
        m_cardJobState.store(CardJobState::Ready, std::memory_order_release);

        // The list the UI is showing has just gone stale in a way only this side knows
        // about, so refresh it without waiting to be asked.
        m_cardListOut = composeCardList();
        m_cardListPending.store(true, std::memory_order_release);
    }

    /// What the cartridge page needs: every image on the search path, and what is in each
    /// slot.  One line per record, marker first, TABS between the fields that may contain
    /// spaces -- a label like "SN-U110-09 0.27" and a path both can:
    ///
    ///     A <number>\t<label>\t<path>              an image that could be mounted
    ///     S <slot> <number>\t<label>\t<path>       what is in a slot now
    ///
    /// The firmware's VERDICT on each slot is deliberately not here.  It changes without
    /// anybody asking -- a card is mounted a third of a second after it goes in, and may
    /// be refused -- so it belongs on the channel that carries live state, which is the
    /// panel blob's card_id[].  This list changes only when somebody changes it.
    ///
    /// Composed off the audio thread, always: it opens directories and builds a string.
    std::string composeCardList() const
    {
        std::string out;
        char line[1200];

        for (const CardFile &c : scanForCards())
        {
            // Only the header is read, not the half megabyte behind it.
            const std::vector<uint8_t> head = readFileHead(c.path, 0x21);
            const CardIdent id = identifyCard(head.data(), head.size());
            std::snprintf(line, sizeof(line), "A %u\t%s\t%s\n",
                          id.valid ? id.number : c.number,
                          id.label.empty() ? "" : id.label.c_str(), c.path.c_str());
            out += line;
        }

        for (unsigned i = 0; i < voltaire::kNumCardSlots; i ++)
        {
            if (!m_cards[i].present)
                continue;
            std::snprintf(line, sizeof(line), "S %u %u\t%s\t%s\n",
                          i, m_cards[i].number, m_cards[i].label.c_str(),
                          m_cards[i].path.c_str());
            out += line;
        }
        return out;
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

    static const char *loadResultName(voltaire::LoadResult r)
    {
        switch (r)
        {
        case voltaire::LoadResult::Ok:        return "ok";
        case voltaire::LoadResult::WrongSize: return "wrong size";
        case voltaire::LoadResult::BadImage:  return "the image did not check out";
        case voltaire::LoadResult::NoSuchSlot: return "no such slot";
        }
        return "unknown";
    }

    void loadRoms()
    {
        loadRomImages();
        if (m_romsLoaded)
            return;
        // Whatever went wrong, say the one thing that leads somewhere.  A DAW on Windows
        // has no terminal attached, so every d_stderr2 above this line went nowhere any
        // user will ever read; the LCD and the report behind it are the only channel.
        d_stderr2("Voltaire 110: the machine is NOT running -- no sound, and the panel "
                  "stays blank. Click the LCD in the plugin's own window for the full "
                  "report of where it looked and what it found.");
    }

    void loadRomImages()
    {
        m_diagStatus = "no-program-rom";
        m_diagNotes.clear();

        // Walked by hand rather than through findRom(), because a file that is THERE but
        // the wrong size is the single most useful thing this can report and findRom()
        // cannot tell the difference -- it answers with an empty vector either way.
        std::vector<uint8_t> rom;
        for (const std::string &dir : romSearchPath())
        {
            for (size_t i = 0; i < kNumPgmNames && rom.empty(); i ++)
            {
                const std::string path = dir + "/" + kPgmNames[i];
                const long n = fileSize(path);
                if (n < 0)
                    continue;
                if (size_t(n) != kPgmBytes)
                {
                    addNote("%s is %ld bytes; the program ROM has to be exactly %u",
                            path.c_str(), n, unsigned(kPgmBytes));
                    continue;
                }
                rom = readFile(path);
                if (rom.size() == kPgmBytes)
                    m_pgmFound = kPgmNames[i];
                else
                    addNote("%s is the right size but could not be read", path.c_str());
            }
            if (!rom.empty())
                break;
        }

        if (rom.empty())
        {
            d_stderr2("Voltaire 110: no U-110 program ROM found. Put your own dumps in "
                     #ifdef _WIN32
                      "%LOCALAPPDATA%\\Voltaire110\\roms"
                     #else
                      "$XDG_DATA_HOME/Voltaire110/roms"
                     #endif
                      " (or set U110_DATA_DIR). The plugin will stay silent until then.");
            return;
        }

        const voltaire::LoadResult pr = m_core.loadProgramRom(rom.data(), rom.size());
        if (pr != voltaire::LoadResult::Ok)
        {
            // This used to return in silence, which meant a corrupt dump of exactly the
            // right length was indistinguishable from having no dump at all.
            m_diagStatus = "bad-program-rom";
            addNote("%s is the right size but the core rejected it (%s)",
                    m_pgmFound.c_str(), loadResultName(pr));
            d_stderr2("Voltaire 110: %s was read but the core rejected it (%s).",
                      m_pgmFound.c_str(), loadResultName(pr));
            return;
        }
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
                m_diagStatus = "no-wave-rom";
                addNote("wave ROM bank %u is missing: nothing called %s, %u bytes long, "
                        "anywhere on the search path", b, name,
                        unsigned(voltaire::kCardBytes));
                d_stderr2("Voltaire 110: wave ROM bank %u not found; no sound.", b);
                return;
            }
            const voltaire::LoadResult wr = m_core.loadWaveRom(b, w.data(), w.size());
            if (wr != voltaire::LoadResult::Ok)
            {
                m_diagStatus = "no-wave-rom";
                addNote("wave ROM bank %u was read but the core rejected it (%s)",
                        b, loadResultName(wr));
                d_stderr2("Voltaire 110: wave ROM bank %u rejected (%s); no sound.",
                          b, loadResultName(wr));
                return;
            }
        }

        loadCards();

        // The staging buffer for later card changes.  Allocated here, once, so that the
        // only thing a change costs the audio thread is the memcpy out of it.
        m_cardStage.resize(voltaire::kCardBytes);

        m_core.reset();
        m_romsLoaded = true;
        m_diagStatus = "ok";
        d_stdout("Voltaire 110: ROMs loaded, core running.");
    }

    /// One line of the report, printf style.
    void addNote(const char *fmt, ...)
    {
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        m_diagNotes += "  ";
        m_diagNotes += line;
        m_diagNotes += "\n";
    }

    /// Everything the plugin knows about why the machine is or is not running.
    ///
    /// Built fresh each time it is asked for, so it reports the filesystem as it is NOW
    /// rather than as it was when the plugin was constructed -- somebody who copies the
    /// ROMs in, then asks again, should see them.
    std::string composeDiag() const
    {
        std::string r;
        // First line is machine-readable and everything after it is for a person.  The UI
        // needs one word to decide what to put on the LCD, and would rather not parse
        // prose to get it.
        r  = "status: ";
        r += m_romsLoaded ? "ok" : m_diagStatus;
        r += "\n";

        char buf[1024];
        std::snprintf(buf, sizeof(buf),
                "Voltaire 110 -- where the ROMs were looked for\n\n%s\n"
                "Host sample rate %.0f Hz; the host has processed %llu audio blocks.\n",
                m_romsLoaded
                    ? "The machine IS running."
                    : "The machine is NOT running: no sound, and the panel stays blank.",
                m_hostRate, (unsigned long long) m_runCount.load());
        r += buf;
        if (m_runCount.load() == 0)
            r += "\nThe host has never asked this plugin for audio. Until it does, the\n"
                 "machine cannot run even with every ROM in place -- check that the track\n"
                 "is not muted or disabled, and that something is routed through it.\n";

        if (!m_diagNotes.empty())
        {
            r += "\nWorth knowing:\n";
            r += m_diagNotes;
        }

        if (!m_pgmName.empty())
        {
            std::snprintf(buf, sizeof(buf), "\nProgram ROM in use: %s  (sha %s)\n",
                          m_pgmName.c_str(), m_pgmSha.substr(0, 12).c_str());
            r += buf;
        }

        unsigned mounted = 0;
        for (unsigned i = 0; i < voltaire::kNumCardSlots; i ++)
            if (m_cards[i].present)
            {
                if (mounted ++ == 0)
                    r += "\nCards mounted:\n";
                std::snprintf(buf, sizeof(buf), "  slot %u: SN-U110-%02u  %s\n",
                              i + 1, m_cards[i].number, m_cards[i].path.c_str());
                r += buf;
            }
        if (mounted == 0)
            r += "\nNo cards mounted. Cards are optional; the internal tones do not need "
                 "them.\n";

        r += "\nIt needs, in one of the directories below:\n";
        std::snprintf(buf, sizeof(buf),
                "  the program ROM, exactly %u bytes, under any one of these names:\n",
                unsigned(kPgmBytes));
        r += buf;
        for (size_t i = 0; i < kNumPgmNames; i ++)
        {
            r += "    ";
            r += kPgmNames[i];
            r += "\n";
        }
        std::snprintf(buf, sizeof(buf),
                "  all %u wave ROMs, %u bytes each:\n",
                unsigned(voltaire::kNumWaveBanks), unsigned(voltaire::kCardBytes));
        r += buf;
        for (unsigned b = 0; b < voltaire::kNumWaveBanks; b ++)
        {
            std::snprintf(buf, sizeof(buf),
                          "    roland_t110_u110_u220_waverom%u.bin\n", b);
            r += buf;
        }
        r += "  cards are optional: any file whose name contains sn-u110-NN.\n";
        r += "  Names are matched exactly, so a file called u110.bin will not be found.\n";

        r += "\nPlaces looked, in order:\n";
        int nth = 0;
        for (const RomDir &d : romSearchDirs())
        {
            const std::string why = (d.origin == "built in"
                                     ? std::string("it is built in")
                                     : "it is where " + d.origin + " points")
                                  + (d.legacy ? ", under the name this plugin used to go by"
                                              : "");
            std::snprintf(buf, sizeof(buf), "\n [%d] %s\n     (because %s)\n",
                          ++ nth, d.path.c_str(), why.c_str());
            r += buf;

            DIR *dir = ::opendir(d.path.c_str());
            if (dir == nullptr)
            {
                r += "     -- there is no such directory\n";
                continue;
            }
            unsigned shown = 0, total = 0;
            while (const dirent *e = ::readdir(dir))
            {
                const std::string name = e->d_name;
                if (name == "." || name == "..")
                    continue;
                total ++;
                if (shown >= kDiagMaxEntries)
                    continue;
                const long n = fileSize(d.path + "/" + name);
                if (n < 0)
                    std::snprintf(buf, sizeof(buf), "     %-44s  (not a plain file)\n",
                                  name.c_str());
                else
                    std::snprintf(buf, sizeof(buf), "     %-44s  %ld bytes\n",
                                  name.c_str(), n);
                r += buf;
                shown ++;
            }
            ::closedir(dir);
            if (total == 0)
                r += "     -- the directory exists but is empty\n";
            else if (total > shown)
            {
                std::snprintf(buf, sizeof(buf), "     ... and %u more\n", total - shown);
                r += buf;
            }
        }

        // The channel to the UI carries a fixed-size message, so a report longer than it
        // would arrive silently cut in half.  Cut it HERE instead, and say so.
        if (r.size() > kDiagMaxChars)
        {
            r.resize(kDiagMaxChars);
            r += "\n... report truncated.\n";
        }
        return r;
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

    // ---- self-diagnosis.  See composeDiag(): the only channel to a user in a DAW that
    // has no terminal attached is the plugin's own window.
    const char *m_diagStatus = "not-loaded";
    std::string m_diagNotes;
    std::string m_diagOut;
    std::atomic<bool> m_diagPending { false };
    std::atomic<unsigned long long> m_runCount { 0 };
    bool m_buttons[voltaire::kButtonCount] = { false };
    bool m_autoButtons[voltaire::kButtonCount] = { false };
    unsigned m_lcdAccum = 0;
    char m_lastLcd[40] = { 0 };
    uint32_t m_clipHold = 0;

    // The meter, in dB, as the UI reads it.  Written only by updateMeters() on the audio
    // thread and read by getParameterValue() on that same thread (LV2 writes the output
    // ports from run(); CLAP polls them from process()), so no atomics are needed.
    float m_meterDb[2] = { kMeterFloorDb, kMeterFloorDb };
    float m_holdDb[2]  = { kMeterFloorDb, kMeterFloorDb };
    uint32_t m_holdDwell[2] = { 0, 0 };     ///< host frames left before the marker falls
    MountedCard m_cards[voltaire::kNumCardSlots];

    // ---- a card change in flight.
    //
    // m_cards[] above stays owned by the thread that takes the request, so that a host
    // saving the session never reads a std::string another thread is writing.  What
    // crosses to the audio thread is this: a slot number, a flag, and half a megabyte of
    // already-descrambled image.
    enum class CardJobState : int { Idle, Filling, Ready, Running };
    enum class CardStep : int { Idle, EjectWait, MountWait };

    /// Allocated once, at construction.  Handing the audio thread a std::vector to take
    /// ownership of would mean freeing the old one somewhere, and there is nowhere safe.
    std::vector<uint8_t> m_cardStage;
    std::atomic<CardJobState> m_cardJobState { CardJobState::Idle };
    unsigned m_cardJobSlot = 0;
    bool     m_cardJobInstall = false;

    CardStep m_cardStep = CardStep::Idle;
    uint32_t m_cardStepFrames = 0;      ///< core frames spent waiting on this step

    /// Two seconds of emulated time, against a mount that takes a third of one.  This is
    /// not a timing assumption, it is a way out for a machine that is not answering.
    static constexpr uint32_t kCardStepTimeout = uint32_t(2.0 * voltaire::kCoreSampleRate);

    std::string m_cardListOut;              ///< composed off the audio thread
    std::atomic<bool> m_cardListPending { false };
    std::vector<uint8_t> m_savedNvram;
    std::string m_pgmSha, m_pgmName, m_pgmFound;

    char m_patchesText[kNumPatches * (kPatchNameLen + 1) + 1] = { 0 };

    // Room for the internal 99 plus a full complement of cards, at one short line each.
    static constexpr size_t kTonesTextMax =
            (kNumInternalTones + voltaire::kNumCardSlots * kMaxCardTones)
                    * (kToneNameLen + 4) + (voltaire::kNumCardSlots + 1) * 48 + 1;
    char m_tonesText[kTonesTextMax] = { 0 };
    char m_toneScratch[kTonesTextMax] = { 0 };
    std::atomic<int> m_toneRequest { -1 };

    // ---- the DIVE editor.  The job is written by setState on the UI's thread and taken
    // by run() on the audio thread; the flag is what hands it over.  Writes go through a
    // single-producer ring for the same reason.
    static constexpr unsigned kDiveWriteQueue = 32;
    DiveJob m_diveJob, m_diveRun;
    std::atomic<bool> m_diveJobReady { false };
    bool m_machineUp = false;
    bool m_diveBusy = false;
    uint8_t m_diveAt = 0;
    uint32_t m_diveWait = 0;
    uint8_t m_diveVal[kDiveMax] = { 0 };
    bool m_diveHave[kDiveMax] = { false };
    uint8_t m_diveRx[16] = { 0 };
    size_t m_diveRxLen = 0;
    char m_diveText[512] = { 0 };
    DiveWrite m_diveWrites[kDiveWriteQueue] = {};
    std::atomic<unsigned> m_diveWrHead { 0 }, m_diveWrTail { 0 };
    uint8_t m_nameWrite[kPatchNameLen] = { 0 };
    std::atomic<bool> m_nameWritePending { false };
    uint8_t m_cardIdSeen[voltaire::kNumCardSlots] = { 0 };
    uint32_t m_rxRestoreWait = 0;
    uint8_t m_rxSaved = 0;
    std::atomic<int> m_rebootRequest { kRebootNone };
    uint32_t m_rebootHold = 0;
    std::atomic<int> m_patchRequest { -1 };
    std::atomic<int> m_writeRequest { -1 };

    /// One patch handed down from the library, whole.  Filled on the UI's thread and read
    /// by run(), like the DIVE job above it.
    struct PatchLoad { uint8_t slot = 0; uint8_t rec[kPatchRecordBytes] = { 0 }; };
    PatchLoad m_loadJob;
    std::atomic<bool> m_loadReady { false };

    char m_patchDumpHex[kPatchRecordBytes * 2 + 1] = { 0 };
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
