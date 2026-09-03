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

# DPF is a submodule and we do not fork it, so the one change we need travels as a patch
# applied at build time.  DPF implements updateStateValue -- the DSP -> UI channel -- for
# LV2, CLAP, AU and Carla but not for the JACK standalone, so without this the standalone's
# panel stays blank.  Idempotent: --check tells us whether it is already in.
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
ttl:
	@dpf/utils/generate-ttl.sh > /dev/null 2>&1
	@grep -q "ui:X11UI" bin/$(NAME).lv2/manifest.ttl \
		&& echo "  LV2 manifest declares the UI" \
		|| echo "  WARNING: LV2 manifest has no UI entry"

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

.PHONY: core generated ttl patches

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
