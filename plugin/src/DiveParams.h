// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    What each DIVE control is, in the machine's terms.

    The artwork says where a control is and what shape it takes; this says what it means.
    The key is the Inkscape label, because that is the one name both halves already agree
    on -- rename SB_Part_Level in Inkscape and the build fails here rather than silently
    editing the wrong parameter.

    Addresses come from the Owner's Manual section 4.2.2 and were each read back off the
    running machine; see analysis/SYSTEM-DESIGN.md section 5.3.2.  A part parameter is at
    00 1n <addr> for part n, a patch-common one at 00 01 <addr>, and both are reached by
    SysEx: DT1 to write, RQ1 to read.  SETUP has no SysEx address at all and goes to
    battery-backed RAM instead (section 5.3.3).
*/
#pragma once

#include <cstdint>

namespace voltaire {
namespace dive {

/// Where the value lives, which decides how it is read and written.
enum Where : uint8_t
{
    kPart,        ///< 00 1n <addr>, SysEx
    kCommon,      ///< 00 01 <addr>, SysEx
    kRam,         ///< a byte of battery-backed RAM
    kRamBit,      ///< one bit of it; `addr` is the address, `lo` the bit number
    kAction,      ///< a button that does something rather than holding a value
    kTone,        ///< the part's tone: two parameters and a name, so it has its own menu
    kName,        ///< the patch name, ten characters of RAM
    kUnmapped,    ///< drawn, but nothing behind it yet -- see SYSTEM-DESIGN 5.3.3
};

/// How a raw value is turned into the four or five characters that fit in the field.
enum Show : uint8_t
{
    kNumber,      ///< the value, plus `bias`
    kSigned,      ///< the value minus `bias`, with a sign
    kOnOff,       ///< "YES" / "NO"
    kNote,        ///< 0-127 as C-1..G9
    kAssign,      ///< 0-5 as outputs 1-6, 6 as OFF
    kPolyPress,   ///< a list, not a range: -24,-12,-7,-5..+5,+7,+12
    kText,        ///< characters rather than a number
};

/// What an action button does.  Named rather than numbered so the switch reads.
enum Action : uint8_t
{
    kActNone,
    kActDefaults,     ///< Basic: output assign 1, full key range, receive channel 1
    kActWrite,        ///< Common: store the temporary patch
    kActPresetLeft,   ///< Common: output mode 21, every part hard left
    kActPresetRight,  ///< Common: output mode 21, every part hard right
    kActPresetDry,    ///< Common: output mode 22, centred and dry
    kActPresetEfx,    ///< Common: output mode 21, the effected stereo pair
};

struct Param
{
    const char *page;    ///< which DIVE page, because two pages share control names
    const char *label;   ///< the Inkscape label
    uint8_t where;
    uint16_t addr;       ///< parameter offset, RAM address, or an Action
    uint8_t lo, hi;      ///< the wire range, inclusive; for kRamBit, lo is the bit
    uint8_t show;
    int16_t bias;
};

// SB_Channel_Pressure_Sensitivity and SB_Polyphonic_Pressure_Sensitivity each appear on
// two pages and mean different parameters there -- one scales level or pitch, the other
// the LFO -- which is why the page is part of the key and not a convenience.
inline constexpr Param kParams[] = {
    // ---- basic: the part's routing and range
    { "basic", "M_output_assign",    kPart, 0x00,  0,   6, kAssign,  0 },
    { "basic", "M_midi_rx_channel",  kPart, 0x01,  0,  15, kNumber,  1 },
    { "basic", "LCD_tone",           kTone, 0x02,  0,   0, kText,    0 },
    { "basic", "M_key_low",          kPart, 0x05,  0, 127, kNote,    0 },
    { "basic", "M_key_high",         kPart, 0x06,  0, 127, kNote,    0 },
    { "basic", "BUT_program_change", kPart, 0x15,  0,   1, kOnOff,   0 },
    { "basic", "M_map",              kPart, 0x16,  0,   5, kNumber,  1 },
    { "basic", "BUT_defaults",       kAction, kActDefaults, 0, 0, kText, 0 },

    // ---- level
    { "level", "SB_Part_Level",                   kPart, 0x07, 0, 127, kNumber, 0 },
    { "level", "SB_Velocity_Sensitivity",         kPart, 0x08, 0,  15, kNumber, 0 },
    { "level", "SB_Channel_Pressure_Sensitivity", kPart, 0x09, 0,  15, kNumber, 0 },
    { "level", "SB_ENV_Attack_Rate",              kPart, 0x0A, 1,  15, kSigned, 8 },
    { "level", "SB_ENV_Release_Rate",             kPart, 0x0B, 1,  15, kSigned, 8 },

    // ---- pitch.  Coarse and fine are stored offset, which is why they have a bias:
    // 64 is centre in both, and the wire ranges are the manual's own.
    { "pitch", "SB_Shift_Course",                     kPart, 0x0C, 52,  76, kSigned, 64 },
    { "pitch", "SB_Shift_Fine",                       kPart, 0x0D, 14, 114, kSigned, 64 },
    { "pitch", "SB_Bend_Range",                       kPart, 0x04,  0,  12, kNumber,  0 },
    { "pitch", "SB_Detune_Depth",                     kPart, 0x17,  0,  15, kNumber,  0 },
    { "pitch", "SB_Polyphonic_Pressure_Sensitivity",  kPart, 0x18,  0,  15, kPolyPress, 0 },

    // ---- LFO
    { "lfo", "SB_LFO_Rate",                         kPart, 0x0E, 0, 15, kNumber, 0 },
    { "lfo", "SB_Auto_Depth",                       kPart, 0x11, 0, 15, kNumber, 0 },
    { "lfo", "SB_Auto_Delay_Time",                  kPart, 0x0F, 0, 15, kNumber, 0 },
    { "lfo", "SB_Auto_Rise_Time",                   kPart, 0x10, 0, 15, kNumber, 0 },
    { "lfo", "SB_Manual_Depth",                     kPart, 0x13, 0, 15, kNumber, 0 },
    { "lfo", "SB_Manual_Rise_Time",                 kPart, 0x12, 0, 15, kNumber, 0 },
    { "lfo", "SB_Channel_Pressure_Sensitivity",     kPart, 0x14, 0,  7, kNumber, 0 },
    { "lfo", "SB_Polyphonic_Pressure_Sensitivity",  kPart, 0x19, 0,  7, kNumber, 0 },

    // ---- common.  Output mode is stored zero-based and shown one-based, so 21 on the
    // wire is the manual's mode 22.
    { "common", "SB_Chorus_Rate",     kCommon, 0x19, 0, 15, kNumber, 0 },
    { "common", "SB_Chorus_Depth",    kCommon, 0x1A, 0, 15, kNumber, 0 },
    { "common", "SB_Tremolo_Rate",    kCommon, 0x1B, 0, 15, kNumber, 0 },
    { "common", "SB_Tremolo_Depth",   kCommon, 0x1C, 0, 15, kNumber, 0 },
    { "common", "M_output_mode",      kCommon, 0x18, 0, 49, kNumber, 1 },
    { "common", "LCD_Patch_Name",     kName,   0x00, 0,  0, kText,   0 },
    { "common", "BUT_write",          kAction, kActWrite,       0, 0, kText, 0 },
    { "common", "BUT_preset_left",    kAction, kActPresetLeft,  0, 0, kText, 0 },
    { "common", "BUT_preset_right",   kAction, kActPresetRight, 0, 0, kText, 0 },
    { "common", "BUT_preset_LR_dry",  kAction, kActPresetDry,   0, 0, kText, 0 },
    { "common", "BUT_preset_LR_efx",  kAction, kActPresetEfx,   0, 0, kText, 0 },

    // ---- setup.  No SysEx addresses exist for any of this.
    { "set", "BUT_Exclusive",             kRamBit, 0x3C00, 5, 0, kOnOff,  0 },
    { "set", "M_Control_Channel",         kRam,    0x3C01, 0, 15, kNumber, 1 },
    { "set", "SB_Master_Tune",            kRam,    0x3C02, 0, 0, kSigned, 0 },
    // Five switches whose gate is not in 0x3C00 after all, and MAP EDIT whose byte is
    // only the right shape.  Drawn, and deliberately doing nothing until one of them can
    // be watched changing -- guessing a bit here would write into the machine's settings.
    { "set", "M_Map_Edit",                kUnmapped, 0, 0, 0, kNumber, 1 },
    { "set", "BUT_Control_Change",        kUnmapped, 0, 0, 0, kOnOff,  0 },
    { "set", "BUT_Program_Change",        kUnmapped, 0, 0, 0, kOnOff,  0 },
    { "set", "BUT_Channel_Pressure",      kUnmapped, 0, 0, 0, kOnOff,  0 },
    { "set", "BUT_Polyphonic_Pressure",   kUnmapped, 0, 0, 0, kOnOff,  0 },
    { "set", "BUT_Pitch_Bender",          kUnmapped, 0, 0, 0, kOnOff,  0 },
};

inline constexpr int kParamCount = int(sizeof(kParams) / sizeof(kParams[0]));

/// Polyphonic pressure sensitivity is a LIST, not a range: sixteen stored values that
/// step unevenly.  OM Parameter Table 1, Pitch Group.
inline constexpr int kPolyPressTable[16] = {
    -24, -12, -7, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 12
};

/// Output modes the Common page's preset buttons reach.  21 is <L31> <R31>, the
/// effected stereo pair; 22 is M31, centred and dry.  Stored zero-based.
inline constexpr uint8_t kModeStereoEfx = 21 - 1;
inline constexpr uint8_t kModeCentreDry = 22 - 1;

/// Ten characters, the machine's own patch name length.
inline constexpr int kNameLen = 10;

/// The row describing one control, or nullptr if the table has never heard of it.
inline const Param *find(const char *page, const char *label)
{
    for (int i = 0; i < kParamCount; i ++)
    {
        const Param &p = kParams[i];
        const char *a = p.page, *b = page;
        while (*a && *a == *b) { a ++; b ++; }
        if (*a != *b)
            continue;
        a = p.label; b = label;
        while (*a && *a == *b) { a ++; b ++; }
        if (*a == *b)
            return &p;
    }
    return nullptr;
}

/// Wire value -> the number a human reads.  kPolyPress is a list rather than a range,
/// so it is the one that cannot be done with arithmetic.
inline int display(const Param &p, int raw)
{
    switch (p.show)
    {
    case kSigned:    return raw - p.bias;
    case kPolyPress: return kPolyPressTable[raw & 15];
    default:         return raw + p.bias;
    }
}

/// Master tune is one signed byte for -99..+99; the RAM location is measured, this
/// encoding is not.  Kept here so it is one edit if it turns out to be otherwise.
inline constexpr int kMasterTuneMin = -99;
inline constexpr int kMasterTuneMax =  99;


} // namespace dive
} // namespace voltaire
