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
  tools/       build-time and test tools (panel export, null test, CGROM baking,
               the About text)
  generated/   build products, never edited by hand.  The exported ARTWORK is
               tracked (CLONING.md says why); everything else here is not.
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

reads `resources/graphics/overall_panel_inkscape.svg`, plus one file per DIVE tab,
and writes to `generated/`:

| File | What it is |
|---|---|
| `panel_geometry.h` | every control as a `constexpr Rect`, in design units |
| `panel_geometry.json` | the same, for tooling |
| `panel_flat.svg` | the artwork the renderer loads: text flattened to paths, editing-aid layers removed |
| `panel_svg.h` | the same, embedded as a C string |
| `panel_background.png` | the artwork's rasters, pre-rendered with their transforms and clips |
| `panel_background.h` | the same, embedded as bytes |
| `dive_<page>_flat.svg` | one per DIVE page, already translated into panel coordinates |
| `dive_<page>_raster.png` | that page's clipped elements, pre-rendered |
| `dive_pages.h` | all of them, embedded |

It also **lints the artwork against nanosvg's subset**.  nanosvg silently ignores
filters, clip paths, masks, patterns and text, so an unsupported construct
becomes a missing element at runtime with no error anywhere.  `--check` exits
non-zero if the artwork has problems, which makes it usable as a build step.

Regenerating needs **Inkscape** (to flatten text) and **rsvg-convert** (to render
the rasters).  Neither is needed to build from the generated files.

`make` does it for you: the generated headers depend on the panel SVG and on every
`dive_*.svg`, so editing artwork in Inkscape and running `make -j5` re-exports and
rebuilds.  Nothing needs running by hand.

#### `[!]` A multi-target rule runs once PER TARGET under `-j`

One recipe produces four headers, and written the ordinary way --

```make
a.h b.h c.h d.h: source
	recipe
```

-- make reads that as four separate rules that happen to share a recipe, and under `-j`
it runs the recipe up to four times at once.  Three copies of the exporter then wrote and
deleted each other's temporary files, and the build died in `render_raster` with a
`FileNotFoundError` -- but only in a parallel build, and only when an SVG had actually
changed, which is a good way to look like a flaky toolchain.

GNU Make 4.3's **grouped target** `&:` says one run makes all four, which is the truth.
The exporter also names its temporary files after its own pid now, so two of them running
at once cannot collide whatever drives them.

### The DIVE drawer is seven documents, not one

The panel is the frame; each tab's content is its own Inkscape file, so a page can
be laid out without the other six in the way.  They are **not** merged into a
single SVG, and the reason is ids: every page names its content box `rect11` and
its slider graticules `path290`, so a merge would collide — and ids are how the UI
finds a shape to recolour.  Each page is flattened separately and embedded
separately, and the UI parses one on first use.

Placement needs no constant anywhere.  Both documents carry a rect labelled
`dive_controls_max_outline`; the exporter measures where it sits in each and
translates the page by the difference.  Move the box in Inkscape and the page
follows it.

The two pages with no per-part sub-tabs — SET and COMMON — are shown with the
second tab row hidden, so the exporter also shifts them **up** by exactly that
row's height.  That shift is baked into both the page's artwork and its hit boxes,
which is what stops the two from disagreeing.  It leaves three window heights:
shut, open with one tab row, open with two.  All three come out of the artwork
(`kPanelShutHeight`, `kDiveOpenHeight1Row`, `kDiveOpenHeight`).

Two shapes are drawn by the UI rather than taken from the artwork, because their
size depends on which page is open: the drawer body and the content box.  The UI
paints them with the fill and stroke nanosvg parsed from those very shapes, so
their appearance still comes from Inkscape — only their height does not.

### The artwork is split in two, because nanosvg has no `<image>`

nanosvg's element dispatch knows `g`, `path`, `rect`, `circle`, `ellipse`, `line`,
`polyline`, `polygon` and the gradients.  There is no `<image>` and no clip path,
so a background photo drops out of the panel with nothing said anywhere.

So the exporter splits the artwork at the seam: every `<image>` **and everything
wearing a `clip-path`**, with its transforms, is rendered by rsvg-convert into a
PNG, and the vector remainder becomes the flat SVG.  The UI draws the PNG into the
design rectangle and then the vectors over it.

The rule is uniform on purpose — *if nanosvg cannot draw it, rsvg does* — so there
is no list of exceptions to keep up to date.  Two constructs in the artwork depend
on it.  The section frames (`L_*`) are rounded rectangles with a gap for their
title, and that gap is an Inkscape **power clip**: without the pre-render the frame
would draw closed and the title would sit on top of the stroke.  And the slider
graticules are clipped where the body covers them.  Both belong behind the page's
vectors, which is where the raster lands anyway.

Pre-rendering rather than teaching the UI about images is deliberate — the image is
not just a bitmap, it is a bitmap under a transform *and* a clip path, and rsvg
already implements both.  Two consequences worth knowing:

- Rasters always end up **behind** the vector artwork, whatever their z-order in
  Inkscape.  They are a separate layer by the time the UI sees them.
- The background is **resolution-limited** where the vectors are not.
  `BACKGROUND_SCALE` in the exporter sets how much detail is kept, at roughly
  660 KB per multiple of the design width.  `FRAME_SCALE` does the same for the
  page rasters and is set higher: those are strokes rather than photographs, and a
  few lines on a transparent ground cost almost nothing compressed.

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
| `T_<name>` | screenprint text, drawn from the artwork; its id is exported so the UI can recolour it |
| `M_<name>` | a menu button: the chosen value is drawn in it in the LCD font |
| `SB_<name>` | a slider body — the group, whose graticules give the travel |
| `ST_<name>` | that slider's tap, at the position Inkscape parked it |
| `LCD_<name>` | an LCD-style field |
| `L_<name>` | a section frame, titled by `T_<name>` |

A control and its screenprint are paired **by name, ignoring case**: `M_output_mode`
is titled `T_output_mode`, and `LCD_Patch_Name` by `T_patch_name`.  The same pairing
gives each tab the id of its own lettering, which is what lets a selected tab be
redrawn brighter.

A slider is a group because neither half of it says where the thing is.  The ten
graticules bound the **travel** — the artwork's own statement of how far the tap may
go — and the tap's rect gives its size.

A layer whose name ends in **`_do_not_include`** is never rendered — that is the
artwork saying so itself, and it needs nothing here.  `Example_LCD_Testing_only`
predates the convention and is named in the exporter.  Anything labelled
`*_duplicate` is dropped too.

**Lettering is flattened from the text on every export.**  There is no longer a
paths layer kept beside a text layer: `Foreground Text as Text` is the artwork, and
Inkscape converts it on the way out.  A single-line label comes back as one `<path>`
**keeping the text's id**, which is what makes a tab's lettering addressable.  (A
multi-line label becomes a group of paths with fresh ids, so it cannot be recoloured
— keep anything that needs to change colour on one line.)

**An element labelled `X` is an editing aid when one labelled `X_as_path`
exists.**  The path is what draws; `X` is what it was made from.  The exporter
neither renders `X`, nor lints it against nanosvg's subset, nor lets Inkscape
flatten it — that last one matters most: the path is *already in the artwork* and
may have been adjusted by hand since it was made, and re-flattening the source
would quietly replace the adjusted path with a fresh copy.  `logo_text` /
`logo_text_as_path` is the pair this exists for.

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
the source still contains the editing-aid layers, and its rasters are drawn from
`panel_background.png` rather than from the SVG.  Rendering the source is still
useful for a different question: it shows the panel as the artwork MEANS it, which
is what the two layers together should add up to.

## The DIVE editor

The drawer's 46 controls are wired to the machine through two files.  The artwork says
where a control is and what shape it takes; **`src/DiveParams.h`** says what it means --
which parameter, what range, how to print it.  The key joining them is the Inkscape
label, so renaming `SB_Part_Level` in Inkscape is caught at startup rather than silently
editing the wrong parameter:

```
DIVE: SB_Part_Level on page level is in the artwork but not in DiveParams.h
```

Two pages share a control name -- `SB_Channel_Pressure_Sensitivity` is on both LEVEL and
LFO and means a different parameter on each -- so the page is part of the key.

### Reading is one RQ1 per value

The 16-byte part record packs all 26 of a part's parameters and the packing is unmapped,
but **every address in the Owner's Manual's individual-parameter map answers an RQ1 with
the value already decoded**, so the packing never has to be solved.  See
`analysis/SYSTEM-DESIGN.md` section 5.3.2.

The size field on that map is ignored -- asking for a part's 26 parameters at once
returns exactly one byte -- so a page refresh is one request per control, serially.  Only
what is on screen is asked for.  SETUP has no SysEx address at all and comes from
battery-backed RAM, and so does the patch name.

Three keys carry it: `diveread` names what a page needs, `divewrite` sets one value, and
`divevals` brings back the answers together with the SETUP bytes and the name, which are
RAM reads and cost nothing.

### `[!]` Zero is a plausible answer, which is what makes it dangerous

The machine takes about five and a half seconds to boot, and a restored session can have
the drawer open before then.  RAM read that early is all zeros -- and zeros are a
perfectly reasonable-looking SETUP: control channel 1, master tune 0, every switch off.
The first version published them, and the page looked right and was wrong.

So the DSP answers nothing until the machine has reached its play screen once, and the UI
shows `...` and asks again about once a second until it does.  The gate is one-shot on
purpose: a machine sitting in one of its own menus can still answer perfectly well, and
gating every read on the display would stall the drawer for as long as somebody left a
menu open.

The same shape of bug bit the last value on every page: the read used to publish on a
fixed timer, and the final reply had not arrived yet, so PROGRAM CHANGE always read
`...`.  It now waits for each answer and moves on only when it has it, or after 60 ms.

### `[!]` A sentinel has to be outside EVERY range, not most of them

`rawOf()` returned -1 for "the machine has not said yet".  Master tune runs -99..+99, so
every negative setting read as unknown and the fader dropped to the bottom -- and it could
not be dragged into the negative half, because the moment it got there it stopped being a
value.  The sentinel is now -1000, which no parameter can reach.

### `[!]` The panel renders correctly, and three "fixes" to it were regressions

The lettering was reported as thick and crowded next to Inkscape.  Four rounds of work
followed -- a sub-pixel stroke rule, a finer antialiasing fringe, splitting the panel into
two frames, then a third frame for the palettes -- and every one of them was a fix for a
problem that did not exist.  All four are gone.

**NanoVG matches the reference renderer.**  With the background composited in, one
scanline through "CARTRIDGE MANAGER" reads, blue channel:

```
rsvg-convert + background   ... 127  30  90 ... 81  33 106 ...
the plugin                  ... 147  31  90 ... 82  34  95 ...
```

Those are the antialiasing gradient pixels, and they agree.  The first version of the
plugin, built in a worktree and run side by side, matches its own reference the same way:
`124 56 155` against `124 56 155`, pixel for pixel.

**What went wrong was the reference, not the renderer.**  Every measurement that showed
"fattening" compared the plugin against `rsvg-convert panel_flat.svg` -- and the flat SVG
does **not contain the background**, which is a separate PNG the UI draws underneath.  So
the bright pixels read "inside the letter gaps" were the Earth photograph showing through
the gaps, exactly as it should.  The gaps were never closing.  There was nothing to fix.

The fix for a measurement error is to fix the measurement:

```sh
convert -size 1100x954 xc:'#141416' comp.png
convert comp.png \( generated/panel_background.png -resize 1100x \) \
        -gravity northwest -composite comp.png
rsvg-convert -w 1100 generated/panel_flat.svg -o flat.png
convert comp.png flat.png -gravity northwest -composite reference.png
```

**Three things this cost, worth naming.**  A threshold ink count says a shape has more ink
without saying where; it agreed with the wrong conclusion four times.  A pixel profile
across one gap disagreed with it immediately, once the reference was right.  And the
scale test that "proved" the excess was a fixed-pixel effect was measuring the background
too -- it proved nothing, and it was convincing.

**The rule that came out of it**: when comparing a render against a reference, the
reference has to be the *whole picture the plugin actually draws*, background and all --
and regenerated from the current artwork, since a stale one shifts everything sideways and
looks like a transform bug.

### `[!]` NanoVG flattens curves to half a pixel, and on lettering that shows

There WAS something wrong, and it is not what any of the above was chasing.  NanoVG
tessellates beziers itself, and `nvg__tesselateBezier` tests

```c
if ((d2 + d3)*(d2 + d3) < ctx->tessTol * (dx*dx + dy*dy))
```

which is a distance test with `tessTol` standing in for the SQUARE of the tolerance.
`tessTol` is 0.25, so the tolerance is **half a pixel** -- and points are transformed
before that runs, so it is half a pixel on screen, not half a unit of artwork.

On rectangles nobody would ever notice.  On lettering at 22 px it is plainly visible:
where a shallow curve meets a flat edge -- the top of an "S", the shoulder of a "P", the
bottom of a "D" -- the chord lands up to half a pixel proud of the true outline, and the
eye reads it as a notch, a stray dot above a letter, or a curve gone faceted.
`rsvg-convert` tessellates far finer, which is why the reference renders were clean and
the plugin was not.

The fix is to hand NanoVG geometry that is already flat enough: `shapePath()` subdivides
each cubic adaptively to a tenth of a pixel and emits line segments.  Adaptive rather than
uniform matters, because most of a glyph's outline is nearly straight and still costs two
segments.  Redraw cost did not measurably change.

This is also the one thing that could not have been found by comparing whole renders --
both were of the *same* geometry, and the difference was in what the renderer did with it.
It took a description of which letters were wrong and where.

### `[!]` Correction: the font was never the problem either

An earlier version of this section claimed the artwork had switched from **Earth** to
**Earth-Mod**, a bolder and 11% narrower face.  That was **wrong**: the two fonts' flattened
`d` strings were compared by treating every number in them as an alternating x,y pair,
which path data is not -- relative commands, `h`, `v` and arc flags all put numbers into
that stream that are not coordinates.

Rendered and compared as pixels, the two give 28575 against 28057 ink and identical
bounding boxes; they differ in one glyph, the "T".  Nor was it the conversion:
`--export-text-to-path` produces **byte-identical** output to the GUI's `object-to-path`,
checked on the real artwork by stripping the hand-made paths layer out of
`overall_panel_inkscape_prior.svg` and re-flattening the text beside it.

The artwork went to plain **Earth** everywhere while that was being chased, to hold one
variable still.  Now that the font was cleared, it is back on **Earth-Mod** -- the face the
panel was designed in -- in `overall_panel_inkscape.svg` and every `dive_*.svg` the
exporter reads.  The two differ in the "T" and in nothing else that renders -- though
getting them to render the SAME took a fix in the panel renderer, below.

One practical note for anyone regenerating: `earth.ttf` is TrueType and
`EarthNormal-Modified.otf` is CFF, so the flattened panel goes from quadratic `q`
segments to cubic `c` ones.  That is a quick way to tell which face an export actually
picked up -- a missing font falls back in SILENCE, and a fallback looks like a font
change rather than an error.

### `[!]` The panel rendered a `.otf` 31% heavier than a `.ttf`, and the font got blamed

Putting Earth-Mod back made the whole panel look squished -- letters touching, `CARTRIDGE
MANAGER` reading as one blob.  The obvious suspect is the face, and the obvious suspect
was innocent for the third time in this file.

The numbers, all at the same size, cyan ink in the panel header:

| | rsvg-convert (reference) | the plugin |
|---|---|---|
| Earth (`.ttf`) | 23744 | 24889 (+4.8%) |
| Earth-Mod (`.otf`) | 23015 | **30267 (+31.5%)** |

The reference renderer makes Earth-Mod 2.5% *lighter*.  The plugin made it 31.5%
*heavier*.  So the weight was never in the font; it was in `shapePath()`:

```cpp
pathWinding(subpathArea(p) >= 0.0f ? CCW : CW);   // the bug
```

That declares each subpath by its OWN geometric direction.  It looks right, and the fill
IS right -- outer and hole stay opposed, so counters stay open and nothing looks broken.
But **NanoVG builds its antialias fringe along the point order**, so the half-pixel
feather lands outside the glyph for one winding direction and inside it for the other.

**TrueType winds outer contours clockwise; CFF/OpenType winds them counter-clockwise.**
Two font formats, opposite conventions, so the same artwork gained about a pixel per edge
purely by being set in an `.otf`.  At a 16 px cap height the 2 px gaps between letters are
gone, and the eye reads that as bad letter-spacing.

The fix is to decide solid-vs-hole RELATIVE to the outer contour -- taken as the largest
subpath -- instead of trusting the raw sign, which makes it independent of what the
artwork's face happens to be built in.  Both fonts now land at +5.0% and +5.2%, the
residual being the feather itself, which is the same for both and correct.

**Two lessons.** The first is that a rendering difference between two *fonts* is worth
suspecting the *renderer* for, because a font is data and data rarely has a 31% opinion.
The second is that the reference render is what settles it: comparing the two fonts
against each other only ever said "these differ", and both plausible culprits differed.
Comparing each against `rsvg-convert` at the plugin's real size said which one was wrong.

This is also why the check is a runtime capture and not a flattened-SVG render.  Every
measurement in this section that was made outside the running program agreed with the
wrong answer.


### What is not wired yet

- **WRITE** (Common).  Storing the temporary patch is the machine's own front-panel
  procedure and has no SysEx; it logs and does nothing rather than appearing to save.
- **Five SETUP switches and MAP EDIT.** Their gate is not in `0x3C00` as the obvious
  reading suggests -- see SYSTEM-DESIGN 5.3.3, where that reading is disproved.  They are
  drawn dimmed showing `--` and do nothing, because guessing a bit would write into the
  user's settings.

## `[!]` A demand-driven panel still has to redraw when the window changes size

`uiIdle()` turns a dirty flag into one repaint and an idle machine sets it for nothing,
which is what keeps the panel free while the instrument sits there.  But **a resize is not
a parameter change**, so nothing marked the panel dirty when the window changed size, and
what stayed on screen was the old framebuffer at the new dimensions -- torn, or layered
with earlier renderings, until some button press happened to dirty it.  There was no
`onResize` handler at all.  There is now, and it does one thing.

### Everything is clipped to the panel as it currently stands

The artwork is a single document 676 units tall whether the DIVE drawer is open or not,
and the background image spans all of it.  So a window taller than the shut panel showed
the photograph carrying on below the instrument -- reported as "content below the main UI
when the dive section is not even open", and reproducible by starting the UI at
1100 x 500.

One `scissor()` inside the panel transform fixes it, rather than teaching each piece its
own limit: anything drawn there later is bounded too.  The menus are drawn after the
`restore()`, in window pixels, so they are unaffected.

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

#### `[!]` Three bugs so far have been in the test host, not the plugin

Each one made a *correct* plugin look broken, and each was silent:

- The host returned `type = 0` from its retrieve callback. DPF checks the type and drops
  the value, with no error anywhere; `setState()` simply never ran.
- The host keyed retrieval on position rather than on the URID it was given.
- The host kept **one** stored value, so the second state key overwrote the first — the
  machine's memory came back and the cards silently did not.

A real host round-trips key, type and flags, and keeps a dictionary. Anything less tests the
host. When state does not come back, suspect this file first.

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

`make` builds it. Everything in `generated/` is a build product, but the exported artwork
is **checked in** and the rest is not: the panel is lettered in a font that cannot be
redistributed, so a clone has to be able to build the panel we ship without owning it.
`tools/artwork.sh` re-exports only when the hash of an Inkscape file actually changes --
hash rather than timestamp, because git does not preserve modification times and a
timestamp rule fires at random on a fresh clone. `make artwork` forces it. The full
reasoning, and the font's licence position, is in CLONING.md section 1.

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

## `[!]` DGL's `Color` takes RGB as bytes and alpha as a FLOAT

```cpp
Color(int red, int green, int blue, float alpha = 1.0f)   // dgl/Color.hpp
```

Three integers and a float, which is not the shape anyone expects. Passing the alpha
**byte** -- `Color(r, g, b, 88)` for 35% -- compiles without a murmur, converts 88 to
88.0f, and clamps to fully opaque.

Every translucent fill in the artwork had been forced opaque this way since the panel was
first drawn, and nothing showed it: an opaque panel over an opaque ground looks exactly
like a translucent panel over a similar ground. It surfaced only when a background image
went in behind a body fill of `fill-opacity:0.35` and never appeared. The first suspicion
was the new code -- the image, the pattern, the draw order -- and all of it was fine.

What settled it was printing the colour rather than reasoning about it:

```
FILLDEBUG rect9: raw=0x581a1a1a op=1.000 -> rgba 0.102 0.102 0.102 1.000
```

`0x58` is 88 is 35%, and the alpha that came out the other side is 1.000. Three lines of
`fprintf` after a long time spent on hypotheses that all had the same symptom.

**When a value survives a conversion unchanged, print it.** The RGB channels being right
is what made the bug invisible for so long; they go through the integer path and only the
fourth argument does not.

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
VOLTAIRE_DIVE=P1 ./bin/Voltaire110    # start with the drawer open on a tab
VOLTAIRE_MENU=patch ./bin/Voltaire110 # start with a palette open (patch|tone)
VOLTAIRE_DIAG=1 ./bin/Voltaire110     # start with the self check open
```

`VOLTAIRE_TRACE_STATE=1` makes the UI count the panel updates it decodes, which is what
`tools/clap_check.sh` reads. It counts DECODED panels rather than state messages on
purpose: the snapshot a host hands over when the UI opens carries no panel, so a count of
even one can only have arrived by being pushed.

`VOLTAIRE_DIVE` takes a tab name as the artwork spells it (`set`, `common`, `P1`..`P6`,
`basic`, `level`, `pitch`, `LFO`) and is there so a page can be screenshotted headlessly
under Xvfb -- the drawer is otherwise only reachable by clicking, and a page whose layout
nobody can look at is a page nobody checked.

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

## DSP → UI: one struct on the atom port

The panel is sent to the UI as a **single blob** -- 32 LCD character codes, the eight live
custom glyphs, the lamps and the cursor -- hex encoded, over the LV2 atom port. Not as
individual output control ports.

That was the first design and it was wrong. A control port is meant to carry one value with
a meaningful range, and hosts apply their own change detection to it. Packing a bitfield
into one meant a lamp change moved the port by 4 parts in 16777215, which Ardour reasonably
declined to forward while it forwarded the LCD, whose changes move whole bytes. Text
updated instantly, lamps sat frozen. It also does not scale to what is coming: tone and
patch names, and a parameter editor, are text.

**What stays a control port:** things that genuinely are one number with a range -- volume,
the HF switch, the buttons, and the VU meters when they arrive. That is what those ports are
for, and a meter at 0..1 has none of the problems above.

### Cost

`updateStateValue()` allocates twice per call inside DPF (a `String` key and a map
assignment), so the blob is sent **only when it changes**. An idle machine sends nothing;
navigating the menus costs a few allocations a second. Measured with `make rtaudit`:

```
    malloc 44   free 44   over ~10 s of continuous playing -- two per blob sent
```

The figure moves with how much the panel is doing, because that is what decides how many
blobs get sent; two allocations per send is the constant. `make rtaudit` reports NOT CLEAN
on account of it, and is left that way on purpose — an audit tuned until it passes stops
being an audit. Not zero, and worth knowing. Continuous data must not go this way -- a VU meter at 30 Hz
would be 60 allocations a second, forever, which is why meters stay on control ports.

### `[!]` Two patches to DPF, for one hole in two backends

`updateStateValue()` -- the DSP->UI channel -- is implemented for LV2, AU and Carla, and
**not for the JACK standalone or VST3**, which pass `nullptr` for the callback.
`dpf/distrho/src/DistrhoPluginJACK.cpp` therefore carries a local patch: a fixed 16-slot
ring the audio thread writes without allocating or blocking, drained by the UI thread in
idle.

**CLAP looked implemented and was not.** `PluginCLAP::updateState()` was
`{ return true; }` -- it took every push, dropped it, and answered success. The wiring
around it was all present and correct, which is why it read as working: the callback is
passed, `setStateFromPlugin()` exists, and the UI is handed a full snapshot of state when
its window opens. Nothing ever pushed again. The plugin loads, the artwork draws, the
buttons work, and the LCD is blank forever.

That is a worse failure than the JACK one, because a blank display looks like a plugin
that has not found its ROMs. It cost a Windows debugging session to find, and it was never
a Windows bug -- the Linux CLAP in `bin/` had it too, and had had it all along, because
nothing in the suite had ever opened a UI under CLAP. `0003-clap-dsp-to-ui-state.patch`
gives CLAP the same ring, and `make selftest` now ends by running a real CLAP host
(`tools/clap_selftest.c`) against the built plugin and failing if the UI decodes no panel
updates. With the patch reversed it reports zero; with it in, 32.

**VST3 still has no path**, so its panel will not update. That target is already documented
as built-but-untested.

The patch lives in `plugin/patches/` and `make` applies it to the submodule, idempotently,
before building. DPF is not forked -- a fresh clone gets upstream DPF and the patch on top,
and if upstream ever moves under it the build says so rather than silently producing a
plugin whose panel does not update.

## The self check, and what the LCD says when there is no machine

A plugin that cannot find its ROMs still loads. The window opens, the artwork draws, the
buttons move -- and nothing else happens, because `run()` returns early and the panel is
never sent. Every diagnosis of that goes to `stderr`, and a DAW on Windows has no terminal
attached, so in the case where the message matters most there is nobody listening.

So the plugin diagnoses itself and reports **into its own window**, which is the one place
a user is certain to be looking.

- Before the machine's first word, the LCD shows a stand-in in its own dot-matrix font:
  `NO ROM FILE`, `BAD ROM FILE`, `NO WAVE ROM`, `NO DSP ANSWER`, over `CLICK FOR INFO`.
  The trigger is **whether a panel blob has ever decoded**, not what the LCD contains: an
  all-spaces display is something the firmware really does draw, so the content cannot
  tell the two apart. Once the machine speaks, the glass belongs to the firmware.
- Clicking the LCD opens the report: every directory searched and *why* it was searched
  (which environment variable put it there), a listing of what is actually in each one
  with sizes, the exact filenames wanted, what was loaded, and the live state -- host
  sample rate, and how many audio blocks the host has processed.

That last number answers a question nothing else could: **the host may never call
`run()`**. A muted track, a disabled instrument slot, nothing routed through it -- the
machine cannot start no matter how correct the ROMs are, and the report says so in those
words rather than blaming the files.

The report also separates *missing* from *present but wrong*, which is most of its value
in practice. `fileSize()` exists for exactly this: a dump one byte short, a `.zip` nobody
expanded, a zero-byte file from a failed copy, and a genuinely absent file all look
identical to code that only asks whether the read succeeded, and every one of them needs
something different done about it. A wrong-sized file is called out by name and size.

It is drawn as a page over the panel rather than as a native dialog. A message box would
mean one implementation per platform for the one feature whose entire job is to work when
something else did not, and it would block the host's UI thread while it was up.

The text is composed on the UI's thread in `setState("diagreq")` and only handed to
`run()` as a finished string -- `run()` may not allocate, and this walks directories and
builds kilobytes. `VOLTAIRE_DIAG=1` opens it at startup so it can be screenshotted
headlessly.

## The About box, and where its text lives

Clicking the logo opens the credits. It is the same kind of page as the self check above --
drawn over the panel, dismissed by a click anywhere, scrolled with the wheel -- for the same
reasons, and `VOLTAIRE_ABOUT=1` opens it at startup so it too can be screenshotted.

The text is **`resources/about.txt` and nowhere else**. `tools/make_about.py` turns that
file into `generated/about_text.h` at build time and the UI compiles it in. Two things
follow from compiling it rather than reading it:

- A plugin bundle is copied wherever the host wants it, so at runtime there is no
  directory to look the file up in. The credits have to be *inside* the binary.
- A build cannot ship without them. For a GPL plugin that links BSD sources and names a
  third party's font, that is the whole point of having credits at all.

Unlike the artwork it is **not checked in and needs no hash stamp**: making it takes
`python3` and a text file, which every clone has, so a plain timestamp rule is right here.
The artwork is the opposite case -- it needs Inkscape and a font that cannot be
redistributed -- which is why that one is committed instead. Same directory, opposite
answers, for a reason worth being able to state.

The file's format is that there isn't one: `#` in the first column is a comment, a blank
line is a blank line, and every other line appears as typed. Long lines wrap to the window,
and a line's leading spaces are repeated on every row it wraps onto, so an indented list
still reads as a list at any width. That last part is why the renderer does its own
wrapping instead of calling NanoVG's `textBox()`, which strips leading whitespace.

The logo's hit box comes out of the artwork like every other one: `panel_export.py` exports
the bounds of the element labelled `logo_text_as_path` as `kLogo`, so moving the logo in
Inkscape moves the button. If that label ever goes away the rect exports as zero and the UI
reads that as "no About button" rather than putting one in the corner of the panel.

## RESET, and the two boot options that are the machine's own

The RESET button opens a four-entry palette in the same style as the About box: reboot,
reboot into the service test menu, initialise the memory and reboot, and cancel. Hover
highlights an entry, a click anywhere else or Escape closes it, and `VOLTAIRE_RESET=1`
opens it at startup for a headless screenshot.

**Two of the three are not plugin features.** The firmware reads the key matrix exactly
once while it boots and branches on what it finds (`analysis/ROM-ANALYSIS.md` section 8.5):

| held at power-on | what the machine does |
|---|---|
| DEC + INC | the eleven-entry service test menu |
| PART + EDIT | the `Mem Initialized` copy loop -- the factory patch set, back from EPROM |

So there is no test mode to write and no memory to erase. There is a pair of keys to hold
down while the machine comes up, and the whole implementation is holding them for long
enough. The menu's entries say which keys they stand in for, because that is what they are.

**How long is long enough was measured, not guessed.** The look at the key matrix lands
between **3.62 s and 3.68 s** of emulated time after `reset()`: release at 3.6 s and the
machine boots normally, release at 4.0 s and it comes up in the test menu. The keys are
held for **4.5 s** and let go. Holding them longer is harmless -- nothing scans them again
until the play screen is up, and a 6 s hold tested identically -- so the margin is free.

The reboot runs from `run()`, like the patch selector and the DIVE editor, because a boot
is five and a half seconds of *emulated* time and the audio thread is the only one that
has any. The user watches the machine's own boot screen scroll past on the LCD, which is
exactly what a U-110 does. `U110Core::reset()` allocates nothing on the way through --
checked with `tools/rt_audit.c` rather than assumed, since that is the one thing that would
make this the wrong thread for it.

A reboot cancels whatever the other state machines were half way through: their held
buttons, their pending requests, the queued DIVE writes, and `m_machineUp`, so the drawer
waits for the play screen again instead of reading RAM out of a machine that is still
booting. The borrowed `SETUP:MIDI:EXCLUSIVE` switch is handed back there and then rather
than by a timer that would fire into a half-booted machine -- except on an initialise,
where putting one byte back into memory the user just asked to have wiped would be the one
thing they did not ask for.

The destructive entry is coloured as such and says what it costs on its own line, so it
cannot be picked out of a list of four by muscle memory. It is also recoverable in the way
that matters in a DAW: the project on disk still holds the old NVRAM until the host saves
again, so reopening the project brings the patches back.

**CARTRIDGE MANAGER opens the self check**, which is the same page the LCD opens and
deliberately the same code path -- what the cards are and whether the machine can see them
is one question with one answer, and two pages that drifted apart would be worse than one
page reached two ways. Which also means the button feedback deliberately does *not* light
CARTRIDGE MANAGER while that page is up: it may equally have come from the LCD, and
lighting a button that was not pressed is the bug that was fixed when the About box was
added.

## Session state

What persists is the **NVRAM, and only the NVRAM** — the work/setup RAM at `0x2100`–`0x3FFF`
and the 64 user patches at `0xE000`–`0xFFFF`. That is exactly what the real unit's battery
holds; everything else comes back from ROM when the power does.

Restoring therefore **reboots the machine**, which is what a U-110 does when it is switched
off and on. It costs a fraction of a second of wall time (nothing forces realtime), and it
is the only way to be consistent: the firmware caches the active patch into work RAM at
`0x2800`, and the CPU's registers are not saved, so resuming in place would leave the
machine half in one session and half in another.

This is deliberately **not** a MAME-style machine snapshot. A snapshot would have to carry
every device's internal state and the exact scheduler phase, and would break whenever any of
that changed. The NVRAM layout is fixed by the hardware and cannot.

`make selftest` round-trips it through the LV2 state extension the way a DAW does — save,
instantiate a fresh plugin, restore, save again — and checks the user patch store byte for
byte. It also checks the fresh instance *differs* before restoring, so the comparison cannot
pass for the wrong reason.

### Cards, and the rest of the session

The NVRAM is not everything a project needs. The front-panel volume and HF correction are
in there too, and so is **which card image was in which slot** — patches name their tones by
slot, so a project that comes back without its cards comes back reading `Illegal CARD`.

That all goes in a second state key, `settings`, as **text**, so it can be read out of a
session file when a project comes back wrong:

```
volume 6.5000
hfcorrection 0
pgm b9e60aaf... roland_u110_pgm_(15179960).bin
card 0 8 fe8eb62e... /home/you/.local/share/u110/roms/sn-u110-08.bin
card 1 9 c964b871... /home/you/.local/share/u110/roms/sn-u110-09.bin
```

Volume and HF correction are also ordinary automatable parameters, and in LV2 the host's
control ports are the authority — they are saved with the session and they win on the first
`run()`. Keeping them here as well costs nothing, makes the session self-describing, and
covers a host that stores state but not ports.

#### Finding cards

A fresh instance mounts whatever card-shaped files are on the ROM search path, lowest number
first. A file names a card if its name contains `sn-u110-NN` **anywhere**, with separators
ignored — so `sn-u110-08.bin`, `SN_U110_02.BIN`, `my sn-u-110-03 copy.bin` and
`roland_u220_waverom4_(sn-u110-08).bin` all work. Two digits, and not three: `sn-u110-081`
is not silently read as card 8.

There is **no catalogue of known cards and there will not be one.** Writing your own image is
a supported thing to do, so the only thing a filename has to carry is which slot number the
image claims to be. Nothing is checked against a database.

#### Coming back to the same images

The SHA-256 is recorded for one purpose: answering "is this the same file the project was
saved with?". It is never a gate. Resolution goes recorded path, then the same basename on
the search path, then any file claiming the same card number; if the bytes have changed since
the project was saved, the image is mounted anyway with a warning saying so. Refusing to load
somebody's edited card because it no longer matches a hash would make the checksum an
obstacle rather than an explanation.

The program ROM's hash is recorded the same way, and only ever produces a warning.

#### `[!]` Two keys, and the order they arrive in

The host restores `nvram` and `settings` in whatever order it likes — with DPF that is
memory first — and the cards have to be in their slots **before** the machine boots into the
patches that name them. So neither key applies itself: each records what it was given and
asks for one reboot. Restoring both costs two boots, at a few tens of milliseconds each, and
it runs when a project loads rather than from the audio callback.

`make selftest` covers all of it: the keys stored and returned, the patch store byte for
byte, the settings text identical, volume and HF correction saved as set, a project whose
card paths no longer exist finding its images again by name, and the session coming back on
the patch it was left on.

---

## The PATCH menu

Sixty-four patches, and the machine's own way to reach one is `[INC]` pressed as many times
as it takes: P-57 is fifty-six presses. The **PATCH** button opens a list of all of them —
four columns of sixteen, the current one marked, one click to load.

### Reading the names

Straight out of the machine's memory: 64 records of 128 bytes at `0xE000`, with a ten-byte
ASCII name at `+4` (ROM-ANALYSIS.md §4). No emulated time is spent driving menus to collect
them, and nothing is hard-coded — a patch the user has renamed shows its new name, because
what is read is the bank the machine is playing from, not a table in this repository.

They travel to the UI as a `patches` state key, one name per line, pushed **only when they
change**. That is what keeps the list honest without anyone asking for it: dots while the
machine is still booting, the factory names a second later, and the user's own names again
after a session restore brings back an edited bank.

### Changing patch without rebooting the machine

`[INC]` and `[DEC]` are the only things that select a patch — MIDI program change selects a
*part's tone*, not a patch (SYSTEM-DESIGN.md §5.3) — and they wrap modulo 64, so no number
of presses can home the selection.

What makes **one** press enough is that the number the firmware increments lives in work RAM
at `0x274A`. Set it to N-1, give the machine a single `[INC]`, and it lands on N and does the
whole job itself: copies the record into the active patch buffer at `0x2800`, reloads the
eight output-routing registers, redraws the LCD — all exactly as it would for a press
somebody made.

**Writing `0x274A` on its own changes nothing.** What is playing is the copy at `0x2800`, and
only the firmware's own patch-load routine puts one there; the number by itself is just a
number. Rebooting the machine would apply it — that is what a restored session does — but a
reboot is a gap in the sound and throws away everything else in RAM. One button press costs a
fifth of a second and keeps the machine running underneath it.

#### `[!]` `[INC]` means something else in the menus

On the play screen it selects a patch. On `PATCH:COM:OUT` it edits the output mode. So
nothing is pressed until the play screen is up: a machine showing a menu page is walked out
of with `[EXIT]` first, one press at a time, **checking after each** — three presses reach
the play screen from the deepest page in the firmware.

The check is the LCD's top line. The play screen reads `P-01:Ac.Piano`, or `TEMP:` once a
program change has replaced a part's tone; every menu page reads something else — `Select
Mode`, `PATCH`, `PATCH:COM`, `PATCH:WRT`. A guard that cannot be talked into pressing `[INC]`
somewhere it would edit a value.

All of it is button *edges in emulated time*, so it is a small state machine ticked from
`run()` — a press held 60 ms and released for 180 ms, which sits well inside what the
firmware's debouncer needs and well outside its auto-repeat.

#### `[!]` An empty state key is a perfectly good zero

The UI asks for a patch by setting a `patchsel` state key. DPF stores **every** key in the
session, that one included, and hands the empty value straight back on restore — where
`atoi("")` is `0`, a valid patch number. Without a digit test in `setState()` every project
would quietly reopen on P-01 no matter what was saved. `make selftest` selects P-43 before
saving and checks the restored instance comes up on it, which is what caught this.

---

## The TONE menu

A patch has six **parts** and each part names one **tone**, so the TONE button opens two
choices: a row of tabs for the parts, showing what each is playing now, and under it the
tones — the internal 99, plus a tab per mounted card.

Nothing here reads the LCD or presses a button. Which part you are editing is the menu's
own business, not the machine's cursor, so the answer never depends on what the display
happens to be showing.

### Reading the names

A tone parameter record is 80 bytes and starts with its ten-character name
(ROM-ANALYSIS.md §6.6), so the whole list is `0x1000 + 0x50*n` in the wave ROM or card
image. `U110Core::readWaveRom` and `readCardRom` hand those over with the address and data
scrambling already undone — the core holds the descrambled images anyway, and this is the
only thing above it that wants to read them.

**99 internal tones is a fixed count, not a scan.** Record 100 in bank 0 decodes to a
perfectly printable `"JIG"`, because sample data begins there; a scan that stops at the
first unreadable name overruns by dozens. Cards *are* scanned, and do terminate cleanly:
SN-U110-08 has 28 and SN-U110-09 has 16, each followed by blank names. A card tab is
labelled with what the image calls itself — `SN-U110-08` sits in plain text at offset 0x10
of the dump — falling back to its catalogue number for a card somebody wrote themselves.

### Changing a tone: two SysEx writes

`[!]` **Writing the tone number into the part record does nothing.** It changes what the
record *says* and not what plays: the sound comes from the 80-byte parameter record the
firmware copies to `0x2880 + 0x50*part`, plus the twelve sample records it builds at
`0x2A60` and the voice state at `0x3760`. The routine that does all that is at `0x80D3`
and there is no way to call it from out here.

That is not a guess. Two machines were booted from the same images and given the same
tone, one over MIDI and one by copying the 80-byte record by hand: work RAM matched **byte
for byte** across `0x2880`, and they still sounded different, because `0x2A60` and
`0x3760` had not been rebuilt.

So the menu asks the machine the way anything else would — a Roland DT1 write to the
part's temporary tone parameters:

| address | value |
|---|---|
| `00 1n 02` | tone media: `0` internal, otherwise a **card ID** (`8` for SN-U110-08) |
| `00 1n 03` | tone number within that media, counting from 0 |

`n` is the part. The firmware then does its own job properly, per part, and the LCD picks
up its `TEMP:` prefix exactly as it would for an edit made by hand.

**This is also the only route that reaches a card tone.** A MIDI program change carries a
tone number and nothing to say which card it is on, and it addresses a *channel* — so on a
patch with six parts on channel 1, it would change all six.

#### `[!]` SETUP:MIDI:EXCLUSIVE can be switched off

Bit 5 of `0x3C00`, and with it clear the machine discards every exclusive message without
a word. The dispatcher reloads that byte for **every message** rather than caching it at
boot (`561F: ldb 41, 3c00`), so the plugin checks it, holds it open for the 50 ms the two
messages take to arrive, and puts it back exactly as it was. A user who has turned
EXCLUSIVE off has turned it off against other gear on the MIDI bus, not against their own
front panel.

`make selftest` covers the list and both directions of the selection: 99 internal tones in
the first group, a card group after it, a tone set on part 1, a **card** tone set on part 3,
and part 1 still holding what it was given.
