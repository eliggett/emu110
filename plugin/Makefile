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

# ORDER MATTERS AND MAKE WILL NOT GUESS IT.  These were once prerequisites of one `all`,
# which is not the same thing: under -j make runs prerequisites in parallel, so `core` --
# whose whole job is to delete plugin binaries that are older than the archive -- would
# delete a binary that another job had linked seconds earlier, and the build would report
# success with no plugin on disk.  Nor is it enough to make `core` a prerequisite of the
# link targets, since those are phony wrappers whose own prerequisites then run alongside
# it.  Separate passes are what actually orders the work; the sub-makes inherit the
# jobserver, so -j still applies inside each one.
all:
	@$(MAKE) --no-print-directory patches generated core
	@$(MAKE) --no-print-directory $(TARGETS)
	@$(MAKE) --no-print-directory ttl

# DPF is a submodule and we do not fork it, so the changes we need travel as patches
# applied at build time.  Two of them are the same bug in two places: updateStateValue --
# the DSP -> UI channel -- was missing from the JACK standalone and, less obviously, from
# CLAP, whose implementation was a stub that answered true and dropped the value.  Without
# them the panel is drawn but never updated.  tools/clap_check.sh is what now notices.
# Idempotent: --check tells us whether they are already in.
patches:
	@for p in patches/*.patch; do \
		if git -C dpf apply --check --reverse "$(CURDIR)/$$p" 2>/dev/null; then \
			:; \
		elif git -C dpf apply "$(CURDIR)/$$p" 2>/dev/null; then \
			echo "  applied $$(basename $$p) to dpf"; \
		else \
			echo "  WARNING: $$(basename $$p) does not apply to dpf -- check the submodule"; \
		fi; \
	done

# The LV2 bundle is INCOMPLETE without its manifest: DPF builds Voltaire110_ui.so, but
# nothing declares it, so hosts load the plugin and report that it has no custom UI.  The
# generator has to run AFTER the UI is linked, which is exactly the ordering that gets
# missed when it is a separate manual step.
# lv2_ttl_generator is a DPF helper BINARY, and no target we build depends on it -- so on
# a machine that has built here before it is simply present, and on a fresh clone the ttl
# step dies with "No such file or directory" (exit 127) after everything else succeeded.
ttl: dpf/utils/lv2_ttl_generator
	@dpf/utils/generate-ttl.sh > /dev/null 2>&1
	@grep -q "ui:X11UI" bin/$(NAME).lv2/manifest.ttl \
		&& echo "  LV2 manifest declares the UI" \
		|| echo "  WARNING: LV2 manifest has no UI entry"

# The exported artwork IS CHECKED IN, which is deliberate.  The panel is lettered in a
# font that cannot be redistributed (CLONING.md), so a clone has to be able to build the
# panel we ship without owning that font -- and without Inkscape or rsvg-convert either.
# Only someone CHANGING the artwork needs any of them.
#
# tools/artwork.sh decides whether an export is needed by HASHING the inputs, because
# make cannot: git does not preserve modification times, so on a fresh clone a timestamp
# rule fires at random -- and when it fires without the font installed, Inkscape
# substitutes silently and the build replaces our panel with one set in Noto Sans.

# The DIVE drawer's content is one Inkscape file per tab; a change to any of them
# re-exports everything, because they are placed relative to the panel.
PANEL_SVG = ../resources/graphics/overall_panel_inkscape.svg
# dive_panel_inkscape.svg is a design scratchpad, not a page the exporter reads.
DIVE_SVG  = $(filter-out %dive_panel_inkscape.svg,\
              $(wildcard ../resources/graphics/dive_*.svg))

# The About box's text is resources/about.txt and nothing else; make_about.py compiles it
# in.  Unlike the artwork this is NOT checked in and needs no stamp -- it takes python3 and
# a text file, so every clone can just make it, and a timestamp rule is the right tool for
# once.  The About box has to work in a bundle a host has copied somewhere we never see,
# which rules out reading the file at runtime.
ABOUT_TXT = ../resources/about.txt
ABOUT_H   = generated/about_text.h

$(ABOUT_H): $(ABOUT_TXT) tools/make_about.py
	@mkdir -p generated
	@python3 tools/make_about.py $(ABOUT_TXT) $@
	@echo "  about text <- $(ABOUT_TXT)"

generated: $(ABOUT_H)
	@tools/artwork.sh

# Re-export whatever the hashes say.  For when the exporter's OUTPUT needs to change
# without its input having changed -- a new header field, say -- and for checking that a
# clean export still matches what is checked in.
artwork:
	@tools/artwork.sh --force

# The DPF targets link libu110core.a but make has no rule tying them to it, so a change
# in the core or the shim would build a fresh archive and quietly relink NOTHING -- the
# plugin kept running the old code, which is a very confusing way to test a fix.  Dropping
# the built binaries when the archive is newer forces the relink.
core: generated
	@tools/build_core.sh
	@for f in bin/$(NAME).lv2/$(NAME)_dsp.so bin/$(NAME).clap bin/$(NAME) \
	          bin/$(NAME).vst3/Contents/x86_64-linux/$(NAME).so; do \
		if [ -e "$$f" ] && [ "$(CORE_LIB)" -nt "$$f" ]; then \
			echo "  core is newer than $$f -- forcing a relink"; rm -f "$$f"; \
		fi; \
	done

# ---- Windows -------------------------------------------------------------------------
#
# Cross-compiled from Linux with mingw-w64; CLAP only, because that is what the Windows
# ask was.  Nothing about the plugin is conditional -- the same sources build -- so what
# this target is really for is keeping the two builds' intermediate files apart.  make
# decides what is up to date by comparing timestamps and knows nothing about target
# machines, so one shared build/ means the second build finds "current" objects for the
# wrong architecture and links them without a word of complaint.  Hence a separate object
# directory for the core, a separate one for DPF, a separate one for DGL (that is what
# BUILD_DIR_SUFFIX buys), and a separate bin-win/.
#
# These go on the sub-make's COMMAND LINE rather than being set here, because make passes
# command-line variables down through MAKEFLAGS to every sub-make on its own -- and DPF
# reaches DGL through one more level of $(MAKE) that an `export` in this file would not
# reliably cover.
CROSS_PREFIX ?= x86_64-w64-mingw32-
WIN_BUILD    = $(CURDIR)/build-win
WIN_BIN      = $(CURDIR)/bin-win

win:
	@command -v $(CROSS_PREFIX)g++ > /dev/null \
		|| { echo "  $(CROSS_PREFIX)g++ not found -- see CLONING.md, the Windows section"; exit 1; }
	@$(MAKE) --no-print-directory patches generated
	@U110_CORE_BUILD=$(WIN_BUILD) CXX=$(CROSS_PREFIX)g++ AR=$(CROSS_PREFIX)ar \
		tools/build_core.sh
	@if [ -e "$(WIN_BIN)/$(NAME).clap" ] \
	   && [ "$(WIN_BUILD)/libu110core.a" -nt "$(WIN_BIN)/$(NAME).clap" ]; then \
		echo "  core is newer than $(NAME).clap -- forcing a relink"; \
		rm -f "$(WIN_BIN)/$(NAME).clap"; \
	fi
	@$(MAKE) --no-print-directory clap \
		CC=$(CROSS_PREFIX)gcc CXX=$(CROSS_PREFIX)g++ AR=$(CROSS_PREFIX)ar \
		BUILD_DIR_SUFFIX=-win \
		DPF_BUILD_DIR=$(WIN_BUILD)/$(NAME) \
		DPF_TARGET_DIR=$(WIN_BIN) \
		CORE_LIB=build-win/libu110core.a
	@file $(WIN_BIN)/$(NAME).clap

.PHONY: win

dpf/utils/lv2_ttl_generator:
	@$(MAKE) --no-print-directory -C dpf/utils/lv2-ttl-generator > /dev/null

.PHONY: core generated artwork ttl patches

# A minimal LV2 host, so what gets tested is what a user loads.  Building the core and
# null-testing it proves the emulation; this proves the PLUGIN -- DPF, the resampler at the
# host's rate, MIDI as LV2 atoms, and the generated TTL's port layout.
selftest: all
	@gcc -O2 -o build/lv2_selftest tools/lv2_selftest.c -ldl $$(pkg-config --cflags lv2)
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} build/lv2_selftest \
		bin/Voltaire110.lv2/Voltaire110_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/lv2_selftest.wav
	@echo
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} build/lv2_selftest \
		bin/$(NAME).lv2/$(NAME)_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/lv2_state.wav 0 state \
		2>&1 | grep -E "^state:|restored"
	@echo
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} build/lv2_selftest \
		bin/$(NAME).lv2/$(NAME)_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/lv2_patch.wav 0 patch \
		2>&1 | grep -E "^patch:"
	@echo
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} build/lv2_selftest \
		bin/$(NAME).lv2/$(NAME)_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/lv2_tone.wav 0 tone \
		2>&1 | grep -E "^tone:"
	@echo
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} build/lv2_selftest \
		bin/$(NAME).lv2/$(NAME)_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/lv2_write.wav 0 write \
		2>&1 | grep -E "^write:"
	@echo
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} build/lv2_selftest \
		bin/$(NAME).lv2/$(NAME)_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/lv2_load.wav 0 load \
		2>&1 | grep -E "^load:"
	@echo
	@$(MAKE) --no-print-directory presetcheck
	@echo
	@$(MAKE) --no-print-directory clapcheck

# The DSP -> UI channel under CLAP, checked with a real CLAP host (tools/clap_selftest.c).
# Its own target as well as part of selftest, because it is the one check that needs a
# display and is therefore the one that gets skipped.
clapcheck:
	@tools/clap_check.sh bin/$(NAME).clap

.PHONY: clapcheck

# The preset file format, on its own: that a patch survives the round trip, and that
# nothing which is not a patch is ever believed.  These files are hand-editable by design
# and what comes out of one is handed to the machine's patch loader, so the refusals matter
# more than the happy path.  Needs no ROMs, no DPF and no display.
presetcheck:
	@g++ -O2 -std=c++20 -Isrc -Wall -o build/preset_check tools/preset_check.cpp
	@build/preset_check $(CURDIR)/build/preset_check_dir

.PHONY: presetcheck

# What the firmware's own menus do, which can only be found out by pressing the buttons in
# emulated time and reading memory between presses.  tools/write_check.txt replays every
# claim section 5.3.4 of analysis/SYSTEM-DESIGN.md makes about the WRITE procedure -- that
# it copies 116 bytes and not 128, that it sets 0x274A, that MEM PROTECT is 0x3C00 bit 0
# and refuses cleanly -- and fails if the machine ever disagrees.
#
# Builds the core directly rather than depending on `all`: none of this needs DPF, the
# artwork or a display, so the check stays runnable when those are in the way.
writecheck:
	@tools/build_core.sh
	@g++ -O2 -std=c++20 -Icore -o build/u110_panel tools/u110_panel.cpp $(CORE_LIB)
	@build/u110_panel --roms $${U110_DATA_DIR:-$(CURDIR)/..}/roms \
		--script tools/write_check.txt

.PHONY: writecheck

# Real-time safety.  The audio callback has a hard deadline and what matters is the worst
# case, so anything that can block -- malloc, free, locks, getenv -- must not be reachable
# from run().  Reading the code cannot settle that, because most of the calls are several
# layers down in code we did not write.  This interposes the allocator and counts.
rtaudit: all
	@gcc -shared -fPIC -rdynamic -o build/rt_audit.so tools/rt_audit.c -ldl
	@gcc -O2 -g -o build/lv2_selftest tools/lv2_selftest.c -ldl $$(pkg-config --cflags lv2)
	@U110_DATA_DIR=$${U110_DATA_DIR:-$(CURDIR)/..} LD_PRELOAD=$(CURDIR)/build/rt_audit.so \
		build/lv2_selftest bin/$(NAME).lv2/$(NAME)_dsp.so \
		"https://github.com/eliggett/emu110/voltaire110" build/rt_audit.wav 0 2>&1 \
		| sed -n '/real-time audit/,/^$$/p'

.PHONY: rtaudit selftest
