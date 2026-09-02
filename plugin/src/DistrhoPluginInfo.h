// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "Voltaire"
#define DISTRHO_PLUGIN_NAME    "Voltaire 110"
#define DISTRHO_PLUGIN_URI     "https://github.com/eliggett/emu110/voltaire110"
#define DISTRHO_PLUGIN_CLAP_ID "org.emu110.voltaire110"

#define DISTRHO_PLUGIN_BRAND_ID  Vlt1
#define DISTRHO_PLUGIN_UNIQUE_ID V110

#define DISTRHO_PLUGIN_HAS_UI           1
#define DISTRHO_PLUGIN_IS_SYNTH         1
#define DISTRHO_PLUGIN_IS_RT_SAFE       1
#define DISTRHO_PLUGIN_NUM_INPUTS       0
#define DISTRHO_PLUGIN_NUM_OUTPUTS      2
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT  1
#define DISTRHO_PLUGIN_WANT_MIDI_OUTPUT 1
#define DISTRHO_PLUGIN_WANT_LATENCY     1

// The panel is drawn with NanoVG, which DGL already carries.  No widget toolkit: PUGL
// uses raw X11/Cocoa/Win32 plus GL and nothing else, which is what keeps the plugin out
// of a symbol fight with Ardour's own GTK.
#define DISTRHO_UI_USE_NANOVG           1
#define DISTRHO_UI_DEFAULT_WIDTH        1100
#define DISTRHO_UI_DEFAULT_HEIGHT       301
#define DISTRHO_UI_USER_RESIZABLE       1

#define DISTRHO_PLUGIN_LV2_CATEGORY "lv2:InstrumentPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Synth"
#define DISTRHO_PLUGIN_CLAP_FEATURES "instrument", "synthesizer", "stereo"

#endif // DISTRHO_PLUGIN_INFO_H_INCLUDED
