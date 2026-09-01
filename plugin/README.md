# Voltaire 110

A DAW plugin built on the emu110 Roland U-110 emulation: LV2, CLAP, VST3 and a
standalone application, from one source tree via DPF.

The design lives in [`../PLUGIN-PLAN.md`](../PLUGIN-PLAN.md).  This file covers
only how the directory is laid out and how to work in it.

## Layout

```
plugin/
  core/        BSD-3-Clause.  U110Core: the emulation with no MAME framework
               around it.  u110_core.h is the interface the plugin talks to.
  compat/      BSD-3-Clause.  Our drop-in emu.h -- lets MAME's device sources
               compile here unchanged.  See PLUGIN-PLAN.md section 3.
  src/         GPL-3.0-or-later.  The plugin: DPF glue, panel UI, patch management
  tools/       build-time and test tools (panel export, null test, CGROM baking)
  generated/   build products.  Not tracked, never edited by hand.
  build/       object files and the built plugin bundles.  Not tracked.
```

## The licence boundary

**It runs in one direction only.**  `PLUGIN-PLAN.md` section 1 has the reasoning;
the short version:

| Where | Licence | Rule |
|---|---|---|
| `mame/src/devices/...`, `core/`, `compat/` | BSD-3-Clause | anything that *emulates the hardware* |
| `src/`, `tools/` | GPL-3.0-or-later | anything about *being a plugin* |

Code may not move from `src/` down into the emulation.  Keeping the core BSD is
what preserves the option of contributing the emulation findings back to MAME,
which is GPL-2.0-only and so cannot accept GPLv3.

We never link MAME itself -- only its individually-BSD device sources, compiled
against `compat/emu.h`.

## The panel workflow

The Inkscape artwork is the single source of truth for panel geometry.  No
coordinate is typed into the C++.

```sh
plugin/tools/panel_export.py --text-to-path
```

reads `resources/graphics/overall_panel_inkscape.svg` and writes three things to
`generated/`:

| File | What it is |
|---|---|
| `panel_geometry.h` | every control as a `constexpr Rect`, in design units |
| `panel_geometry.json` | the same, for tooling |
| `panel_flat.svg` | the artwork the renderer loads: text flattened to paths, editing-aid layers removed |

It also **lints the artwork against nanosvg's subset**.  nanosvg silently ignores
filters, clip paths, masks, patterns and text, so an unsupported construct
becomes a missing element at runtime with no error anywhere.  `--check` exits
non-zero if the artwork has problems, which makes it usable as a build step.

### Conventions the exporter relies on

Every element needs an Inkscape label; the prefix says what it is.

| Prefix | Becomes |
|---|---|
| `BUT_<name>` | a button: hit rect and draw rect |
| `LED_<name>` | an indicator |
| `KNOB_<name>_outline` | a knob body: centre and radius |
| `KNOB_<name>_pointer` | its needle: rotated in code about the outline's centre |
| `VU_<name>` | a meter |
| `LCD_outer` / `LCD_inner` | the bezel and the glass |
| `T_<name>` | screenprint text, drawn from the artwork |

Two layers are treated as editing aids and never rendered: **`Foreground Text as
Text`** (the human's editable copy, whose flattened twin `Foreground Text as
Paths` is what actually draws) and **`Example_LCD_Testing_only`**.  Anything
labelled `*_duplicate` is dropped too.

A knob's `rotate()` is read as its **zero position**, not as artwork: the needle
is drawn where the SVG puts it when the parameter reads 0, and the code rotates
from there.

### Checking the render

`rsvg-convert` gives a ground-truth render to diff NanoVG's output against:

```sh
rsvg-convert -w 1600 plugin/generated/panel_flat.svg -o /tmp/panel_ref.png
```

This is worth doing because it is the only way to catch nanosvg quietly dropping
something.  Compare against `panel_flat.svg`, never against the source artwork --
the source still contains the editing-aid layers.

## The null test

`plugin/tools/null_test.py` is the acceptance test and, because the device sources are
shared with MAME rather than forked, a continuous regression check.

```sh
plugin/tools/null_test.py --self     # does MAME render reproducibly?  (the oracle)
plugin/tools/null_test.py            # MAME vs U110Core
```

The bar is **bit-identical**, not a residual floor. The core renders at the chip's native
32 kHz and so does MAME, so no resampler is in the path and a one-LSB drift is a real
emulation difference.

Two things the harness guards against, both of which bit it during development:

- **It refuses to compare a silent reference.** Two silent files match perfectly and prove
  nothing; the first run of the harness "passed" exactly that way.
- **MIDI files it writes always carry an explicit `set_tempo`.** MAME's reader falls back
  to 60 BPM without one, not the spec's 120, and the sequence then plays at half speed --
  which is indistinguishable from the emulator running slow.
