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

## Building the core

```sh
plugin/tools/build_core.sh
```

Compiles MAME's device sources against `compat/emu.h`, links them, and runs a smoke test
that starts each device and exercises the scheduler, the streams and a memory space.

The file list in that script is the **same source MAME builds** -- no copies, no patches.
If a file there ever needs editing to compile, the shim is wrong, not the file. `git -C
mame status src/devices/` should stay empty.

Three things to know before touching `compat/`:

- **`mame/src/emu` is deliberately not on the include path.** If it were, `#include
  "emu.h"` would find MAME's, and the build would silently compile the wrong thing.
- **The shim implements only what these sources actually use.** When a MAME update breaks
  the build, add the one thing it now needs -- do not widen the shim speculatively.
- **C++20**, matching MAME. `flt_biquad.cpp` uses `<numbers>`.

`mcs96.hxx`, `i8x9x.hxx` and `i8x9xd.hxx` are generated into `generated/` by MAME's own
`mcs96make.py` from MAME's own `mcs96ops.lst`, so the two builds cannot diverge there
either.

## The core

```sh
plugin/tools/build_core.sh                       # build everything
plugin/build/u110_render --roms roms --seconds 12 --lcd     # watch it boot
plugin/tools/null_test.py                        # MAME vs the core
```

`u110_render --lcd` is the fastest way to tell whether the core is alive: a working
machine prints its banner and then `P-01:Ac.Piano | MIDI.1.*.*.*.*.*`.

Useful environment variables, all off by default and all costing one branch when off:

| | |
|---|---|
| `U110_LCDTRACE=1` | every LCD control and data write with a timestamp -- the format MAME's `-log` uses, so the two can be diffed directly |
| `U110_TGTRACE=1` | every sound-chip register write, likewise |
| `U110_CYCLES=1` | CPU cycles per emulated second, plus register, ROM-read and interrupt counts |

Diffing these against MAME's `error.log` is how every bug in the core so far has been
found. **The traces line up event for event long before the audio does**, so a divergence
shows up as a timestamp difference in a trace rather than as a vague sense that a render
sounds wrong.

`U110_DITHER=0` does the corresponding job on the MAME side, switching off the TPDF dither
at the 16-bit output so the two ends can be compared sample for sample.

## Building the plugin

```sh
cd plugin && make            # core + standalone, LV2, CLAP, VST3
make selftest                # load the built LV2 and prove it makes sound
```

Output lands in `plugin/bin/`. The core is built separately into `build/libu110core.a` and
linked in, so DPF's compiler flags and ours stay independent and the core can be built and
null-tested with no DPF present at all.

**ROMs are never bundled.** The plugin looks for the user's own dumps, in this order
(PLUGIN-PLAN.md §9):

```
$U110_DATA_DIR/roms   $XDG_DATA_HOME/u110/roms   ~/.local/share/u110/roms   /usr/share/u110/roms
```

With none found it loads, stays silent, and says why on stderr.

### `make selftest`

Building the core and null-testing it proves the *emulation*. It says nothing about the
*plugin*: DPF, the resampler at the host's rate, MIDI arriving as LV2 atoms, the port
layout in the generated TTL. `tools/lv2_selftest.c` is a minimal LV2 host that loads the
built bundle exactly as Ardour would, plays a note and writes a wav — so what gets tested
is what a user loads.

### The resampler

`src/Resampler.hpp`, a Kaiser-windowed sinc polyphase at the exact rational ratio (2:3 to
48 kHz, 320:441 to 44.1 kHz). §11 warns that linear interpolation "would quietly undo a
lot" of the accuracy work, and with the core now bit-identical to MAME that would be
absurd. Measured, by fitting a sine and taking the residual:

| | 100 Hz | 1 kHz | 5 kHz | 12 kHz |
|---|---|---|---|---|
| level | ±0.000 dB | ±0.000 dB | ±0.000 dB | ±0.000 dB |
| residual below signal | 100 dB | 103 dB | 99 dB | 92 dB |

15 kHz sits in the transition band at −7.5 dB. The U-110's own output is already some
23 dB down at 14–16 kHz, so this is not the dominant term up there.

## Using it before the panel exists

There is no custom GUI yet, so the six panel switches are exposed as **host parameters** --
`Part / Jump`, `Edit / Exit`, `Left`, `Right`, `Dec`, `Inc / Enter`. Any generic UI (Carla,
Ardour's own) gives you a toggle per button, and that is enough to drive the machine's own
menus, which is most of what a U-110 is. It works because the firmware's debouncer only
needs a press held for about 150 ms of emulated time, far less than anyone can click.

To see what the buttons did, set `VOLTAIRE_LCD=1` and the plugin prints the display
whenever it changes:

```
LCD [P-01:Ac.Piano    | MIDI.1.*.*.*.*.*]
```

Off by default -- printing from the audio thread is not something to do unasked.

## `[!]` One resampler per channel

The first build shared a single `Resampler` between left and right. The two channels then
pushed into one filter history and advanced the phase twice per block. The **pitch survives
that** -- the average rate is still right -- so it did not sound broken in any obvious way.
It sounded like crackle on every note.

The plugin's streaming now matches a single continuous offline pass at **correlation
1.000000**, and the resampler is bit-identical at every block size from 16 to 4096.

## The panel

`make` builds it; everything in `generated/` is a build product and none of it is checked
in, so a fresh clone regenerates from the Inkscape artwork and the font.

**No coordinate is typed into the UI source.** Every rectangle comes from
`generated/panel_geometry.h`, which `panel_export.py` composes out of the SVG. Move a
control in Inkscape, run `make`, and both the drawing and the hit box follow.

Two things are deliberately not in the SVG: the **knob pointer**, rotated in code rather
than exported as frames, and the **LCD**, which has to be built from character codes
because the firmware redefines its custom glyphs while it runs.

`VOLTAIRE_PANEL_SVG=/path/to.svg` overrides the built-in artwork, so the panel can be
redrawn in Inkscape and reloaded without a rebuild.

### `[!]` nanosvg matches tag names literally

The exporter rewrites the flattened SVG with ElementTree, and ElementTree invents
namespace prefixes (`ns0:svg`, `ns0:path`) unless the default namespace is registered.
A prefixed document **parses without error and yields zero shapes** — the panel simply
does not draw, with nothing in any log to say why. `panel_export.py` registers the
namespace and then checks its own output for `<svg`, because this is the failure with no
symptom.

nanosvg also resolves the document's own units. The artwork is in millimetres, so shapes
come back scaled by 96/25.4; the UI normalises them onto viewBox units, which is what
`panel_geometry.h` uses.

## `[!]` NanoVG forces every subpath to CCW

Letters with counters -- the hole in an "o", "a", "R", "0" -- filled solid. The winding
was not the problem: nanosvg hands over correctly opposed subpaths (the logo has 12 outer
contours and 4 counters). **NanoVG reverses them.** Each subpath defaults to `NVG_CCW`,
and `nvg__flattenPaths` enforces that by reversing anything wound the other way, so the
holes become solid outer contours.

The fix is to preserve each subpath's own direction with `pathWinding()`, computed from
its signed area. Any SVG renderer built on NanoVG needs this; without it the artwork looks
almost right, which is the hard kind of wrong.

Related: the knob pointer is drawn in code so it can rotate, so the artwork's own copy is
skipped by id (`kVolumeKnobPointerId`). Drawing both leaves the old pointer behind at its
zero position.

## Real-time safety

```sh
make rtaudit
```

The audio callback has a hard deadline -- 5.3 ms for a 256-frame block at 48 kHz -- and what
matters is the **worst case**, not the average. `malloc` may ask the kernel for pages; a
lock may wait on another thread. Either can blow the deadline occasionally, which is heard
as a click and is very hard to reproduce.

Reading the code cannot settle this, because most of the interesting calls are several
layers down in code we did not write. `tools/rt_audit.c` interposes the allocator, arms a
flag around `run()`, and counts -- and reports the call sites, so a hit has a name rather
than being an occasional click.

The first run found **384,184 mallocs and 23,383 getenv calls in six seconds** of a settled
machine. Current state:

```
    malloc  0    free  0    realloc  0    calloc  0    pthread_mutex_lock  0
    getenv  3    (one-time static initialisation inside roland_lp.cpp)
```

### `[!]` What was wrong, and why none of it was visible

- **`device_scheduler::advance_to` copied a `std::function` to call it.** That is one
  allocation and one free per timer expiry, and the envelope timer fires at 64 kHz -- two
  heap operations per core sample. Calling through a `const &` fixes it; timers are
  allocated once and never destroyed, so the reference cannot dangle.
- **`set_input_line` pushed onto a `std::vector` that was emptied every time.** Growing
  from empty allocates, so every interrupt-line change cost a malloc and a free. It is now
  a fixed array of 32, which is what MAME's own `device_input` uses, for this reason.
- **`getenv` was called per sound-register write** to test a debug flag. Now cached in a
  `bool` that is touched once at construction, off the audio thread.
- The MIDI queues now `reserve()` at construction rather than growing on the render path.

None of this was audible in ordinary playing, which is exactly why it needed measuring
rather than reasoning about.

## Panel redraw cost

The panel is not cheap to draw -- the whole SVG plus 32 characters of 40 LCD dots, about
**1.2 ms of CPU per redraw**. So what matters is how often a redraw is *asked for*.

It is **demand-driven**, not throttled: `parameterChanged()` compares each value and marks
the panel dirty only if something actually changed, and `uiIdle()` turns at most one dirty
flag into one repaint. An idle machine costs nothing; a machine being driven redraws
promptly. There is no refresh rate to compromise over.

```sh
VOLTAIRE_FPS=1 ./bin/Voltaire110      # prints the rate and the cost per redraw
```

```
panel: 11.2 redraws/s, 1.08 ms each -> 1% of a core     (booting)
panel:  4.1 redraws/s, 1.38 ms each -> 1% of a core     (idle, cursor blinking)
```

### `[!]` Repainting per parameter is 15 redraws per change

The DSP publishes the panel as fifteen output parameters. Calling `repaint()` from
`parameterChanged()` therefore asked for **fifteen full redraws for one change of the
display**, around 220 a second -- for a display that changes 20 times a second at most.
Coalescing in `uiIdle()` is what fixes it; comparing values before marking dirty is what
makes an idle panel free.
