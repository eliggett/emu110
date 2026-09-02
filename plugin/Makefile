#!/usr/bin/make -f
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Voltaire 110.  The emulation core is built separately by tools/build_core.sh into a
# static library, and linked here -- so DPF's compiler flags and ours stay independent,
# and the core can be built and null-tested without DPF present at all.

NAME = Voltaire110

FILES_DSP = src/Voltaire110Plugin.cpp
FILES_UI  = src/Voltaire110UI.cpp

include dpf/Makefile.plugins.mk

CORE_LIB = build/libu110core.a

BUILD_CXX_FLAGS += -Icore -Isrc -Igenerated -std=c++20
LINK_FLAGS      += $(CURDIR)/$(CORE_LIB)

# The standalone is the daily development loop -- MIDI in, audio out, no host in the way --
# and a first-class deliverable, not a compromise.  On Linux it speaks JACK, which under
# PipeWire Just Works.
TARGETS = lv2_sep vst3 clap jack

all: generated core $(TARGETS)

# Everything in generated/ is a build product: the panel geometry and artwork come from the
# Inkscape SVG, the LCD character set from MatrixSans.  Neither is checked in, so a fresh
# clone rebuilds them rather than inheriting a stale copy.
GENERATED = generated/panel_geometry.h generated/panel_svg.h generated/u110_cgrom.h

generated: $(GENERATED)

generated/panel_geometry.h generated/panel_svg.h: ../resources/graphics/overall_panel_inkscape.svg tools/panel_export.py
	@tools/panel_export.py --text-to-path

generated/u110_cgrom.h: ../tools/make_lcd_cgrom.py
	@mkdir -p generated
	@python3 ../tools/make_lcd_cgrom.py --header generated/u110_cgrom.h

core: generated
	@tools/build_core.sh

.PHONY: core generated

# A minimal LV2 host, so what gets tested is what a user loads.  Building the core and
# null-testing it proves the emulation; this proves the PLUGIN -- DPF, the resampler at the
# host's rate, MIDI as LV2 atoms, and the generated TTL's port layout.
selftest: lv2_sep
	@dpf/utils/generate-ttl.sh > /dev/null 2>&1
	@gcc -O2 -o build/lv2_selftest tools/lv2_selftest.c -ldl $$(pkg-config --cflags lv2)
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} build/lv2_selftest \
		bin/Voltaire110.lv2/Voltaire110_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/lv2_selftest.wav

.PHONY: selftest
