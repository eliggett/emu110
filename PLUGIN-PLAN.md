# U-110 audio plugin — planning

Turning the MAME-based U-110 emulation into a cross-platform instrument plugin:
MIDI in and out, stereo audio out, a vector control panel, volume and EQ, patch
browsing, and user patch files.

Status: **planning only.** Nothing here is built yet. Decisions are recorded so
the reasoning survives; open questions marked `[?]` are things to verify before
they are relied on.

---

## 1. Licensing architecture

The key fact, established by reading the headers: **every device the U-110 needs
is BSD-3-Clause.**

| File | Origin | License | Copyright holders |
|---|---|---|---|
| `src/mame/roland/roland_u110.cpp` | new, ours | BSD-3-Clause | Elliott H. Liggett |
| `src/devices/sound/roland_lp.{cpp,h}` | MAME, heavily extended here | BSD-3-Clause | Valley Bell, Elliott H. Liggett |
| `src/devices/cpu/mcs96/*` | MAME, two small fixes here | BSD-3-Clause | Olivier Galibert |
| `src/devices/video/msm6222b.*` | MAME, unmodified | BSD-3-Clause | Olivier Galibert |
| `src/devices/machine/nvram.*` | MAME, unmodified | BSD-3-Clause | Aaron Giles |
| `tools/*` | ours | GPL-3.0-or-later | Elliott H. Liggett |

MAME as an aggregate is **GPL-2.0-only** — `mame/COPYING` has no "or later"
clause. That does not constrain us, because `COPYING` also says:

> MAME as a whole is made available under the terms of the GNU General Public
> License. **Individual source files may be made available under less restrictive
> licenses, as noted in their respective header comments.**

The GPL-2.0 covers MAME's *framework* — `emu.h`, the scheduler, the OSD, the UI —
which is exactly the part we replace with our own shim. **We never link MAME.**

### Three layers

| Layer | Contents | License |
|---|---|---|
| **U110Core** | MAME's BSD devices, compiled against our shim | **BSD-3-Clause** |
| **Plugin** | DPF, UI, patch management, DAW glue | **GPL-3.0-or-later** |
| **MAME** | reference implementation / test oracle | GPL-2.0, *never linked or shipped* |

MAME's role is as a program we *run* to generate reference audio for the null
test (§4). Running a program creates no licensing obligation — no different from
using GCC.

### Two rules that follow

**The BSD → GPLv3 flow is one-way.** Code written on the plugin side can never
move back into the core. The natural boundary handles this: anything that
*emulates the hardware* goes in the core; anything about *being a plugin* — UI,
DAW integration, patch files, volume — goes above it. Note this in the README so
contributors know which side their patch lands on.

**Keeping the core BSD keeps MAME upstreaming possible.** GPLv3 code cannot go
into a GPL-2.0-only project. If the core were GPLv3, the delta-integration
finding, the envelope ramp engine, the 1088 divisor and the output routing matrix
could never be contributed back to MAME.

### VST3 and Steinberg

The Steinberg VST3 SDK is dual-licensed GPLv3 / proprietary-with-registration.
We would qualify under the GPLv3 arm with no agreement needed — but the question
is likely moot: **DPF does not use the Steinberg SDK.** It carries its own
from-scratch reimplementation of the VST3 interfaces (the `travesty` directory),
ISC licensed, written specifically so DPF plugins are not bound by Steinberg's
terms.

`[?]` **Verify before relying on this:** clone DPF, read the license headers in
`distrho/src/travesty/`.

What survives regardless of SDK is the **trademark**: "VST" is Steinberg's
registered mark. Say "VST3-compatible", don't use the logo.

`[?]` "Roland" and "U-110" are also live trademarks. Ship under a distinct
product name and describe the lineage in prose.

---

## 2. Format targets

| Format | Why | Priority |
|---|---|---|
| **LV2** | Ardour on Linux — the primary development host | ship |
| **CLAP** | FL Studio; the best-designed of the three | ship |
| **Standalone** | daily dev loop, and a first-class deliverable (§2.1) | ship |
| **VST3** | free from the same DPF build; covers Ardour and older FL | ship, labelled untested |
| **AU / macOS** | deferred until a macOS volunteer appears | later |
| **VST2** | SDK withdrawn 2018, no legitimate distribution path | never |
| **AAX** | requires a paid Avid agreement and their signing | never |

Ardour 9 does **not** host CLAP (verified on the target machine), which is why
LV2 is not optional. Ardour's VST3 support on Linux is solid, so VST3 is a real
second path there.

Shipping a format means supporting it. VST3 goes out labelled in the README as
built-but-untested, community-supported.

### 2.1 Standalone application

**Yes — this is a normal DPF target, not a design compromise.** DPF builds a
standalone binary from the same source as the plugins, with the same UI, talking
to the system's MIDI and audio directly. Exactly the Dexed / Ultramaster KR-106
model.

On Linux the standalone speaks JACK, which under PipeWire means it Just Works via
`pipewire-jack` — no separate JACK daemon needed on a modern desktop.

`[?]` Confirm DPF's standalone backends for Windows and macOS. Historically the
target was JACK-only; newer DPF has native backends. Windows may need RtAudio or
equivalent.

This is also the **development loop**: MIDI in, audio out, iterate on the panel,
no host in the way. Build it first, not last.

### Windows

"CLAP for FL Studio" implies a **Windows build**, which is an unscoped target.

- `[?]` Check the FL Studio version. CLAP support arrived in the 2024 releases;
  older FL needs VST3 instead.
- **Deferred until Linux works.** Two paths when it comes up: cross-compile from
  Linux with mingw-w64, or build natively on a GitHub Actions Windows runner.
  The runner is the better option if it is available — it tests on the target
  platform instead of merely producing binaries for it, and it costs no local
  toolchain. Either way, prove it with a hello-world plugin *before* the UI
  exists.

### macOS

Deferred, not foreclosed. DPF builds for macOS, so it becomes a build-and-test
job rather than a redesign — **provided** no platform-specific code leaks into
the UI layer. Draw through DPF's DGL/NanoVG only: no direct X11 calls, no
`/proc`, no hardcoded Linux paths.

---

## 3. Staying locked to the MAME tree

**Requirement:** changes made to the emulation in `mame/` must reach the plugin
without a manual port. A forked copy would drift within weeks.

**Solution: the plugin's build compiles MAME's device sources directly.**

`roland_lp.cpp` begins with `#include "emu.h"`. The include path decides which
`emu.h` it gets. So we write `plugin/compat/emu.h` — a small header presenting
MAME's API surface — and the plugin's build lists the MAME paths as sources:

```make
CORE_SOURCES = \
  $(MAME)/src/devices/sound/roland_lp.cpp \
  $(MAME)/src/devices/cpu/mcs96/mcs96.cpp \
  $(MAME)/src/devices/cpu/mcs96/i8x9x.cpp \
  $(MAME)/src/devices/video/msm6222b.cpp
CORE_CXXFLAGS = -I plugin/compat
```

The same file compiles in both places. Edit `roland_lp.cpp`, rebuild the plugin,
the change is in. No fork, no drift, one source of truth.

### What this costs

**The shim must match MAME's API, not a nicer one.** This is the whole
constraint: we are writing a drop-in `emu.h`, so we implement what these four
files actually use and no more — `device_t`, `device_sound_interface`,
`device_rom_interface<N>`, `device_execute_interface`, `devcb_write_line`,
`emu_timer` / `attotime`, `sound_stream`, `address_space`, `save_item`,
`logerror`, and the `DECLARE_/DEFINE_DEVICE_TYPE` macros.

Bounded: 80 total references to `machine()`, `save_item`, `logerror`, `attotime`
and `emu_timer` across the four device files. `msm6222b.cpp` has exactly one.

**`mcs96ops.lst` is generated** by `mcs96make.py`. The plugin build runs that
Python step too.

### The driver is different

`roland_u110.cpp` is a *driver*, not a device — `machine_config`,
`required_device`, `INPUT_PORTS`, `ROM_START`, screen and palette and speaker.
Shimming all of that is not worth it, and the machine wiring genuinely *should*
differ between MAME and a plugin.

So split it by kind:

| Kind | Where | Shared? |
|---|---|---|
| Constant tables — `OUTPUT_MODES[50]`, `PAN_L`/`PAN_R`, `UNSCRAMBLE_ADDR/DATA` | new header in the MAME tree, e.g. `roland_u110_data.h` | **yes**, `#include`d by both |
| Pure functions — `descramble_pcm`, voice-mask derivation, the MIDI rx deserialiser | same header, or a small `.cpp` compiled both ways | **yes** |
| Machine wiring — `machine_config`, input ports, ROM regions | `roland_u110.cpp` (MAME) and the plugin core, separately | no, and correctly so |

Refactoring `roland_u110.cpp` to pull the tables and pure logic into a shared
header is worth doing **now**, before the plugin exists. It is also a tidier
driver.

### Risks and mitigation

- **MAME upstream can change `device_t`'s surface and break the shim.** Pin the
  `mame/` submodule to a known commit; update deliberately, not automatically.
- **A change can compile in MAME and misbehave under the shim.** This is what the
  null test is for — and because the sources are shared, it becomes a *continuous*
  regression check rather than a one-time acceptance gate. Put it in CI.

---

## 4. Core extraction

MAME's framework is not viable inside a plugin: process-global state (so, one
instance per host), no real-time safety in the audio callback, a throttle and
frame scheduler that fight deterministic block-size-independent rendering, and
the 50 Hz screen refresh that imposes the 20 ms latency quantum we currently
measure.

### The interface

Define this **first**, so the plugin builds against it and the implementation can
change underneath:

```cpp
class U110Core {
public:
    void reset();
    void loadProgramRom(const uint8_t *, size_t);
    void loadWaveRom(int bank, const uint8_t *, size_t);
    void loadCard(int slot, const uint8_t *, size_t);   // nullptr = eject
    void midiIn(const uint8_t *bytes, size_t n, uint32_t sampleOffset);
    size_t midiOut(uint8_t *buf, size_t cap, uint32_t *offsets);   // §10.3
    void render(float *outs[6], uint32_t nframes);      // native 32 kHz
    void setButton(int sw, bool down);                  // 6 panel switches
    void snapshot(PanelState &out) const;               // §8
    uint8_t readMem(uint16_t addr) const;
    void    writeMem(uint16_t addr, uint8_t v);
    size_t  saveState(uint8_t *buf, size_t cap) const;
    bool    loadState(const uint8_t *buf, size_t n);
};
```

### Acceptance test — the null test

**Set this up before starting.** Render the same MIDI file through MAME and
through the core; require bit-identical output, or −120 dB residual if a
resampler is in the path.

Nearly all the harness exists: `-wavwrite`, `listen/`, `tools/render_u110.py`,
`tools/select_patch.lua`, `tools/capture_u110.py`.

### What this buys

- **Multiple instances** in one session — the thing that rules out linking MAME.
- **Real-time safety**: no allocation, no file I/O, no locks, no `logerror` in the
  audio callback.
- **Deterministic offline render**, identical at any block size, faster than
  realtime for bounces.
- **The 20 ms quantum disappears.** That number is MAME syncing to the 50 Hz LCD
  screen refresh, not anything inherent.
- **Sample-accurate MIDI**: run cycles to the event's block offset, clock the UART
  bytes in at 31250 baud, continue.

---

## 5. Framework and UI toolkit

**Decision: DPF (DISTRHO Plugin Framework).**

- Natively emits LV2, VST3, CLAP and a standalone from one source tree — no
  wrapper layer between us and the formats we actually use.
- ISC licensed, so the licensing question disappears entirely.
- Its DGL layer *is* PUGL + NanoVG, already assembled, already handling X11
  embedding and the VST3 `IRunLoop` dance on Linux.

Costs, honestly: a spartan framework with thin documentation and a simple
parameter/state model. Acceptable because our parameter surface is small and the
interesting work is in the core and the panel drawing.

Alternative considered and rejected: native CLAP + `clap-wrapper` for VST3/AU.
Ardour can't load CLAP, so the daily driver would be loading *wrapped* output.

**No GTK, no Qt — no widget toolkit at all.** A hard rule. Ardour is itself built
on GTK2 (vendored as YTK in recent versions), and plugins embedding their own GTK
have historically collided on symbols and main loops. PUGL uses raw
X11/Cocoa/Win32 plus GL and nothing else.

**Build a Dear ImGui debug window too.** Wrong aesthetic for the instrument face,
right tool for development: register views, live LCD text, RAM watch on `0x2743`
(card IDs), `0x280E` (output mode), `0x2814` (part records). A day's work, used
constantly.

---

## 6. Panel graphics — the Inkscape workflow

**Yes: draw the whole panel in Inkscape, and make the SVG the single source of
truth for geometry.** The code should not hardcode a single coordinate.

### Pipeline

Inkscape SVG → `nanosvg` at load → NanoVG paths. During development, watch the
file's mtime and re-parse on change: **hot-reload the panel while the plugin
runs.** This is the single biggest time-saver for panel iteration; build it
early.

For release, optionally bake the parsed paths into a C array at build time —
faster startup, no parser at runtime. Same drawing code either way.

### Rules for the SVG (nanosvg's limits)

nanosvg is a small parser and ignores what it does not understand — silently.
Staying inside its subset avoids a class of "why is that missing" bugs:

- **Convert all text to paths** (Path → Object to Path). No text support at all.
- **No filters** — no blur, no drop shadow. Do glow and shadow in NanoVG code,
  where they can also react to state.
- **No clip paths, no masks, no patterns.**
- **Fills, strokes, and linear/radial gradients only.**
- Set the document size to the panel's design size (e.g. 960×320) and scale in
  code. One number to change later.

### Structure — one file, named layers

Give **every element an `id`**; nanosvg keeps it (`NSVGshape::id`), so the code
looks shapes up by name instead of by index.

| Layer | Contains | Drawn how |
|---|---|---|
| `bg` | chassis, screenprint, logos, bezels | static, once |
| `lcd_bezel` | the LCD surround and glass only | static |
| `btn_<name>_up` / `btn_<name>_dn` | one group per button state | swap by state |
| `knob_static` | bezel, scale marks, pointer track | static |
| `led_<name>_off` / `_on` | LED states | swap by state |
| `hit` | invisible rects, `id="hit_btn_edit"` etc. | **never drawn** |

The `hit` layer is the trick worth adopting: draw invisible rectangles over each
control, read their `x/y/w/h` from the parsed SVG at load, and derive every
hit-box from the artwork. Move a button in Inkscape and the mouse target follows
with no code change.

### The exporter — `plugin/tools/panel_export.py`

Built. Reads the artwork, composes every transform down to canvas coordinates,
and writes `plugin/generated/`:

| File | What |
|---|---|
| `panel_geometry.h` | every control as a `constexpr Rect` in design units |
| `panel_geometry.json` | the same, for tooling |
| `panel_flat.svg` | what the renderer loads — text flattened, aid layers gone |

Three findings from running it on the first artwork, all of which shaped the tool:

- **Inkscape's `--export-plain-svg` strips `inkscape:label`.** So the editing-aid
  layers must be pruned *before* Inkscape runs, not after — afterwards they are
  unidentifiable. The exporter does this in the right order.
- **The `Foreground Text as Text` layer must not reach the renderer.** It and its
  flattened twin `Foreground Text as Paths` would otherwise draw on top of each
  other. Pruned by label, along with `Example_LCD_Testing_only` and anything
  labelled `*_duplicate`.
- **A `<text>` that survives text-to-path is dropped from the flat SVG**, because
  nanosvg would ignore it silently and the reference render (`rsvg-convert`)
  would then disagree with what the plugin actually draws. The exporter warns
  when it has to do this.

The linter matters more than it sounds: nanosvg drops unsupported constructs with
no error at all, so the failure mode is a control that is simply absent.
`--check` exits non-zero, making it a build step.

### The panel may grow downward

`BUT_dive` is earmarked for expanding the window to expose a second bank of
controls, not yet designed. Two consequences for anything built now:

- **Do not treat `kDesignHeight` as a constant of the design.** It is the height
  of the *collapsed* panel. The expanded region will be additive — a second
  Inkscape layer, exported the same way, with its own height.
- **Keep the window resize path working from the start**, even though nothing
  uses it yet. Retrofitting a resize into a UI that assumed a fixed size is
  considerably more work than allowing for it now.

### What to draw in code, not SVG

- **The knob pointer.** Draw the static bezel and scale in SVG; rotate the
  indicator in code. A transform, not 128 exported frames.
- **The LCD dot matrix** (§7). SVG supplies only the bezel and glass.
- **Glow, shadow, press highlights, LED bloom** — cheap in NanoVG, reactive to
  state, and outside nanosvg's subset anyway.

---

## 7. The LCD

### How the CPU talks to the LCD

Two addresses, and that is the entire interface:

```
0x1100  lcd_ctrl_w   commands
0x1102  lcd_data_w   data
```

**It is a character display, not a bitmap.** To print `PATCH:COM:OUT` the
firmware writes one command byte saying *where*, then thirteen ASCII bytes. It
never mentions a pixel. The controller holds two memories:

- **DDRAM** (80 bytes) — *what characters are on screen*. Write `0x41` and DDRAM
  holds `0x41`; the controller looks that code up in its internal **CGROM** and
  lights the 5×8 dot pattern. The font lives in the chip.
- **CGRAM** (64 bytes) — *8 custom characters, 8 rows each*. Codes `0x00`–`0x07`
  do **not** come from ROM. The CPU defines them by writing pixel rows, low 5
  bits = the 5 dots across. This is the only place the CPU talks pixels.

Bit 7 of the address counter selects which (`msm6222b.cpp:117`):

```cpp
if(adc & 0x80) {          // DDRAM: this byte is a CHARACTER CODE
    ddram[adr] = data;
} else {                  // CGRAM: this byte is a ROW OF PIXELS
    if(adc < 8*8) cgram[adc] = data;
}
```

Control command case 7 (`adc = data`, bit 7 set) sets a DDRAM address; case 6
(`adc = data & 0x3f`) sets a CGRAM address. The counter auto-increments after
every data write, so a string is one command followed by a burst of bytes.

DDRAM is 80 bytes for a 16×2 display: line 1 at `0x00`, line 2 at `0x40`, only
the first 16 of each visible. `data_w` discards the off-screen addresses and
folds line 2 down to 40–79 — the classic HD44780 layout.

The **cursor belongs to the controller**, not the firmware: `cursor_on`,
`cursor_blinking` and `blink_on()` run off the emulated clock. The CPU says
"blink" once and the chip does the rest.

Each write arms a 40 µs timer (`lcd_ctrl_w:323`) that pulses HSI0 — the
"controller ready" interrupt the firmware uses to drain its text queue. That is
the interrupt the `i8x9x.cpp` HSI.0 fix unblocked; before it the queue filled and
the firmware spun forever.


### Where the glyphs come from

The U-110's display is a **5×8 dot matrix** per character (MSM6222B,
HD44780-alike; `msm6222b_device::render()` expects 16 bytes per character, rows
0–7, bits 4–0 = pixels left to right).

There are **two sources of glyphs**:

1. **Characters `0x20`–`0x7F`** from the controller's CGROM. **Not dumped.**
   `tools/make_lcd_cgrom.py` currently synthesizes them from DejaVu Sans Mono.
2. **Characters `0x00`–`0x0F` from CGRAM** — 5×8 bitmaps the *firmware writes at
   runtime*.

The obvious shortcut is to dump the eight custom glyphs once and add them to a
font. **Measured against a boot trace, that does not work: CGRAM is dynamic.**

Reconstructing CGRAM writes from a 6-second boot (`-log`, decoding the address
counter to separate DDRAM from CGRAM traffic):

| What | When | Writes |
|---|---|---|
| Chars 0–3 redefined, 8 frames, ~52.5 ms apart | t = 1.798–2.168 s | 32 bytes per frame |
| All 8 chars redefined at once | t = 3.513 s | 64 bytes |

**The boot logo is a CGRAM animation.** At t = 1.853 s the firmware writes codes
`0,1,2,2,3` to line 2, columns 9–13 — five cells spelling **U-110**, with code 2
reused for both `1`s. DDRAM is then never touched again. Instead the *glyph
definitions* are rewritten eight times, each shifted up one row, so the logo
rises into view over 370 ms at ~19 fps. The text does not move; the font does.
This is the standard character-LCD vertical-scroll trick, and it is only possible
because CGRAM is writable.

At t = 3.513 s all eight slots are redefined for the play screen. Code 7 becomes
a vertical bar, placed six times across line 2 at columns 4, 6, 8, 10, 12, 14 —
one per PART. Chars 3, 4 and 5 become a divider bar plus small digits 1, 2, 3. So
char 3 is the `0` of the logo during boot and something else entirely afterwards.

**Conclusion: custom glyphs must be rendered live from CGRAM.** A font baked once
can neither animate nor be reused. And since a dot-matrix renderer is therefore
required anyway, running ASCII through the same path costs nothing and guarantees
the two match. A font is a *build-time source* for the 5×8 table, not a runtime
dependency.

### MatrixSans — measured

`resources/fonts/MatrixSans` is **SIL OFL**, and the copyright statement declares
**no Reserved Font Name**, so even modified versions may keep the name.
Attribution is still owed; keep `OFL.txt` and `FONTLOG.txt` in the tree.

Only `.woff2` files are present. NanoVG's `stb_truetype` cannot read woff2 — but
this does not matter, because nothing ships at runtime. `fontTools` (already
installed) converts in three lines at build time.

The **Screen** variant is the right one: FONTLOG describes it as "separate square
dots, like an LCD screen". (Print is circular dots; Raster is CRT scanlines.)

Measured by rasterizing the converted TTF on its own dot pitch:

| Metric | MatrixSans Screen | U-110 LCD |
|---|---|---|
| Columns | 5 | 5 |
| Rows above baseline | **7** | 7 (rows 0–6) |
| Descender rows | **2** | 1 (row 7) |
| **Total cell** | **5×9** | **5×8** |

**It does not fit.** MatrixSans wants nine rows; the hardware has eight.

### Recommendation

Bake MatrixSans Screen to 5×8 at build time, **clipping descenders from two rows
to one**. That is exactly the compromise the real HD44780 A00 table makes, so the
result is *more* hardware-faithful, not less. Extend `tools/make_lcd_cgrom.py`:
swap the DejaVu source for the converted MatrixSans TTF, keep the existing
16-bytes-per-character output layout.

Expect to **hand-fix a handful of glyphs**. Five columns is a brutal grid and an
automatic rasterization will mangle a few (`M`, `W`, `%`, `@` are the usual
casualties). The whole table is 96 glyphs × 8 bytes = 768 bytes — hand-editing is
entirely feasible, and worth it.

This improves MAME's display too: MatrixSans is designed on a dot grid, DejaVu
rasterized to 5×8 is mush.

**Alternative for maximum fidelity:** use a published HD44780 A00 5×8 table
instead. More accurate to the real part; less pretty. `[?]` Worth generating both
and comparing side by side before committing.

Whichever wins, name the output distinctly (`u110_cgrom.bin`, not "MatrixSans")
and credit the source.

### Rendering

Draw every character — ASCII and CGRAM alike — as a grid of square dots in
NanoVG, from the 5×8 table. Then the details that sell it are free:

- unlit dots faintly visible, not absent
- backlight glow behind the glass
- the block cursor and its blink phase, from the snapshot
- CGRAM glyphs redrawn whenever the firmware redefines them

**The boot logo is the acceptance test for this path.** If the `U-110` animation
rises smoothly out of the bottom of the cells at ~19 fps, the live CGRAM
rendering and the push-on-change transport (§8) are both correct. If it
stutters, jumps, or shows a static logo, one of them is wrong. It is a better
test than any still frame because it exercises timing, transport and rendering at
once — and it runs on every boot, for free.

---

## 8. Data flow between the emulated system and the UI

**Constraint that drives the design:** LV2 mandates the UI as a *separate binary*
talking to the DSP over ports, and DPF models that separation across all its
formats. Having the UI thread read emulated RAM directly is **off the table**.
Design the boundary as serialized messages from the start.

The data is tiny.

### DSP → UI: the panel snapshot, ~30 Hz

```c
struct PanelState {          // versioned, little-endian, fixed layout
    uint16_t version;
    uint8_t  lcd[32];        // 2 lines x 16 visible chars (DDRAM line 2 at 0x40)
    uint8_t  cursor_pos;     // 0..31
    uint8_t  cursor_flags;   // on / blink phase
    uint8_t  cgram_dirty;    // set when the 8 custom glyphs changed
    uint8_t  cgram[64];      // 8 chars x 8 rows, sent only when dirty
    uint8_t  leds;           // from led_w at 0x1200
    uint8_t  card_present;   // 4 bits, mirrors PORT1 (active low)
    uint8_t  patch;          // current patch number
    uint8_t  part_tone[6];   // per-part tone index
    uint32_t voice_active;   // 32 bits, one per voice — activity display
    float    peak[2];        // L/R meters, post-gain (drives the clip indicator)
};
```

~120 bytes ordinarily, ~190 when CGRAM changes. At 30 Hz that is 4–6 KB/s. It
marshals fine over anything, including LV2 atoms.

**CGRAM must be pushed on change, not sampled.** The boot animation redefines
glyphs every ~52.5 ms (§7); polling at 30 Hz would alias it into a stutter. Send
the CGRAM block whenever `cgram_dirty` is set, independently of the 30 Hz text
cadence.

`[?]` **Confirm DPF's DSP→UI push mechanism.** Output parameters
(`kParameterIsOutput`) are floats and unsuitable for a blob; DPF's state
mechanism is the likely carrier.

**Publishing must be fire-and-forget.** If the UI is closed, or the host never
opens it, the emulation must run bit-identically. The snapshot is never in the
emulation's timing path.

### UI → DSP: commands

- **Button press / release**, one of six. Send *edges*, not levels — the
  firmware's edge detector at `0x4118` needs real transitions, and the press must
  be held long enough in emulated time for the debouncer to see it.
  `tools/select_patch.lua` already learned this: re-assert key state every frame.
- **Patch / tone select request** → the panel automaton (§10.4).
- **Load bank, load card, load ROM** → a request naming a file; the *worker*
  reads it (§8.1), never the audio thread.
- **Volume and EQ** do *not* go through this channel; they are real automatable
  host parameters.

### 8.1 Threading

| Thread | Does |
|---|---|
| **Audio** | runs the core, renders, drains the command queue (SPSC lock-free ring), publishes the snapshot |
| **UI** | ~30 Hz: reads the latest snapshot, pushes commands |
| **Worker** | file I/O — ROMs, cards, patch banks — handing completed buffers over by atomic pointer swap |

**File I/O must never touch the audio thread.** Card ROMs are 512 KB, far too
large for a message: the worker reads and descrambles, then hands the finished
buffer over, and the DSP swaps it in at a block boundary.

---

## 9. File locations

ROMs are **data**, not configuration, so they belong under the data directory,
not `~/.config`.

| Platform | Base |
|---|---|
| Linux | `$XDG_DATA_HOME/u110` (default `~/.local/share/u110`) |
| macOS | `~/Library/Application Support/u110` |
| Windows | `%LOCALAPPDATA%\u110` |

```
<base>/
  roms/
    program/     u110 firmware EPROM images (v2.00, v2.03)
    wave/        internal wave ROMs
    cards/       cartridge wave ROM images
  patches/       user patches and banks, subdirectories = categories
  cgrom/         u110_cgrom.bin (§7), if not compiled in
```

- **Search order**: `$U110_DATA_DIR` (override) → user base → system-wide
  (`/usr/share/u110`, `%PROGRAMDATA%`) → the directory beside the plugin binary.
  First hit wins; log which was used.
- **Never bake absolute ROM paths into project state.** Store **name + SHA-256**
  and re-resolve through the search path at load. Warn clearly on a hash
  mismatch — the same image under a different filename should still load.
- **Nothing is bundled.** The plugin ships with no ROM images at all; a first-run
  panel explains what is needed and where to put it.
- Config (window size, last-used directories, UI preferences) goes to
  `$XDG_CONFIG_HOME/u110/` — separate, and safe to delete.

`[?]` **Future: a ROM-authoring utility.** ROM-ANALYSIS §8 concludes card
authoring "is now within reach" — the ID header (§6.5), the address and data
permutations (§6.4) and the tone record layout (§6.6) are known; what is missing
is wave ROM address bits A14–A18, needed only to place sample data. This is a
separate tool, not part of the plugin. But the plugin should **load any
conforming image from `roms/cards/`** without special-casing the factory dumps,
so user-built cards work the day the utility does.

---

## 10. Features

### 10.1 Volume

Range **−3 dB to +16 dB, default 0 dB** (unity — current output level). A
smoothed gain on the summed stereo output, in the plugin layer, exposed as an
automatable parameter.

**Do not route it into the emulation.** The real U-110's volume pot is analog and
sits after the DAC, so a post-gain is the physically accurate model.

At +16 dB a hot patch will clip in the host. No limiter — that would be dishonest
about the signal — but **drive a clip indicator on the panel** from `peak[]` in
the snapshot. Cheap, and it tells the user what is actually happening.

### 10.2 EQ and the output filter

**Three** things, and conflating them is how modelling bugs get hidden:

| | What it is | Where | Default |
|---|---|---|---|
| **Modelled hardware filter** | the Sallen-Key + RC reconstruction chain from the service notes | core audio path | always on, not switchable |
| **Measured correction** | one peaking cut that makes the emulator match the hardware captures | core audio path, after the chain | **on**, switchable |
| **User EQ** | taste, and matching a room | plugin layer | off — *not built* |

#### The measurement

Done, against `listen/hardware/3` (hardware, UA-25 capture) and `listen/emulated/emu3` (emulator),
level-matched over 200 Hz–2 kHz. Plot: **`analysis/hf_excess.pdf`**.

| Band | Emulator − hardware |
|---|---|
| 20–200 Hz | **+0.1 / +0.0 / −0.1 dB** — already correct |
| 2 kHz | +0.8 dB |
| 4 kHz | +3.3 dB |
| **6 kHz** | **+7.0 dB** |
| 8 kHz | +4.6 dB |
| 10 kHz | 0 dB |
| 12–16 kHz | −4 to −24 dB — *deficit*, see below |

Four checks before trusting it:

- **Not patch-specific.** Seven patches agree, +5.9 to +8.9 dB at 6 kHz.
- **Not the resampler.** Rendering natively at 32 kHz reproduces it (+6.8 dB).
- **Not added noise.** On a sustained note the harmonic peaks and the valleys
  between them rise *together*; comb contrast drops only 24.3 → 22.2 dB. Noise
  would fill the valleys and collapse it. So it is a tilt on real signal, and EQ
  is the right class of fix.
- **The bass claim does not survive.** Below 200 Hz the two agree to a tenth of a
  dB. A thin-sounding bottom end is the treble excess pulling the ear up, not a
  missing octave — boosting bass to chase it would *add* an error.

#### The correction

One RBJ peaking cut, in `roland_u110.cpp` as `m_eq[]`, between the RC section and
the speaker:

```
fc   = 5933.3 Hz
Q    = 1.9306          (MAME's K = tan(pi*fc/fs) convention)
gain = 0.436787        = -7.195 dB
```

Weighted rms residual against the measured target: **0.53 dB** over 300 Hz–10 kHz.
It deliberately does nothing below 2 kHz or above 10 kHz. Measured on a real
render, the realized filter tracks the design to within **0.05 dB** across the
band (toggle the port and difference the two outputs).

`11_fing_bass` is the one poor case (5% better, against 49–82% elsewhere) — its
error peaks nearer 4 kHz. It was excluded from the fit rather than allowed to drag
the centre frequency down.

#### Toggling it at runtime

**In MAME**, it is a machine configuration, live while the driver runs:

> **Tab → Machine Configuration → "Output EQ correction" → On / Off**

No restart, no rebuild. `PORT_CHANGED_MEMBER` calls `update_eq()`, which calls
`filter_biquad_device::modify()`; that flushes the stream before retuning, so the
switch is clean mid-note. **Off is an exact bypass, not a filter tuned flat** —
MAME's `PEAK` biquad with gain 1.0 produces identical numerator and denominator,
so the samples pass through untouched.

`machine_reset()` re-applies the port, so a saved configuration survives.

**In the plugin**, expose it the same way volume is (§10.1): a host-automatable
boolean parameter, default on, labelled *"HF correction"*. It is not a tone
control and should not be presented as one — it is a calibration, and the reason
to switch it off is to hear the uncorrected model, not to taste.

Per §3 the three constants belong in the shared `roland_u110_data.h` when that
refactor happens, so the driver and the plugin core cannot drift apart. Both sides
realize the same prototype through their own bilinear transform; the shape is
stable across rates (at 44.1 kHz and 32 kHz it lands within 0.6 dB of the 48 kHz
curve everywhere below 8 kHz).

#### `[!]` This is a correction layer, not the physics

**The reconstruction chain is the more likely culprit, and it should be fixed.**
As modelled, the Sallen-Key cascade peaks **+4.29 dB at 6065 Hz** — but the
service notes' own simulation of that same circuit gives **+2.17 dB max**. Twice
the resonance, in exactly the place the measured error lives. Either the component
values, the topology, or MAME's `opamp_sk_lphp_calc` interpretation of them is
wrong.

Two consequences worth stating plainly:

1. When the chain is fixed, **this EQ must be re-measured, not kept**. Stacking a
   corrective bell on a filter that is itself wrong is exactly the trap §10.2
   warned about; the EQ is honest only as long as it is labelled as compensation
   and its cause is being pursued.
2. **The top-octave deficit is a second, separate defect and the EQ does not touch
   it.** Above 10 kHz the emulator has *too little* — on drums, −6.9 / −14.8 /
   −23.5 dB in the 10–12 / 12–14 / 14–16 kHz bands, with the hardware sitting
   32–39 dB above the noise floor there, so it is real signal being lost. Part of
   it is the chain being realized digitally: the filters are
   `SAMPLE_RATE_OUTPUT_ADAPTIVE`, so at 48 kHz the bilinear transform costs a
   further −1.5 / −3.0 / −6.8 dB at 10 / 12 / 14 kHz against the analog
   prototype, and at a 32 kHz output rate it is far worse. **No EQ can restore
   it** — it needs the chain running at a higher internal rate, or a better
   analog-to-digital mapping.

#### Verified on a full re-render

`listen/emulated/emu4` is the whole session re-rendered with the correction on, measured
against `listen/hardware/3` exactly as before:

| | 6 kHz error, before | after | rms error 300 Hz–10 kHz |
|---|---|---|---|
| strings1 | +6.9 dB | **−0.1 dB** | 2.83 → 0.52 |
| strings3 pingpong | +7.1 dB | **−0.1 dB** | 2.84 → 0.53 |
| shakuhachi | +5.6 dB | −1.5 dB | 2.48 → 0.79 |
| choir3 pingpong | +6.8 dB | −0.4 dB | 2.50 → 0.81 |
| drums | +5.7 dB | −1.5 dB | 2.35 → 1.07 |
| **mean of 13 patches** | | | **2.91 → 1.38 dB (−53%)** |

Two patches resist, and both are worth chasing separately rather than retuning
the filter around them:

- `[?]` **`14_fantasy` is a genuine outlier**: +14.0 dB at 6 kHz before, +6.9
  after — roughly double every other patch. A fixed output-stage error cannot do
  that, so something patch-specific is going on. **Chorus and tremolo are not
  implemented**, and Fantasy is exactly the kind of patch that would use them.
  Re-measure once they exist.
- `[?]` **`11_fing_bass`** (3.53 → 2.88) peaks nearer 4 kHz than 6 kHz. Whether
  that is a second mechanism or the same one shifted by the patch's spectral
  support is unresolved.

#### The user EQ

Still unbuilt, and still worth keeping separate. When it happens, keep it small:
low shelf (~120 Hz), one sweepable peak (300 Hz–5 kHz, fixed Q ~ 0.7), high shelf
(~6 kHz), +/-12 dB each, plus bypass. Enough to match a capture without becoming a
project.

### 10.3 MIDI

**Input** works today: raw bytes into the UART deserialiser at
`roland_u110.cpp:754`. SysEx in is already handled by the firmware — the dispatch
table at `0x56C3` routes `0xF0` to `0x5B94` and `0xF7` to `0x5B9F`.

**Output works.** `midi_tx_w` serialises the CPU's TXD to a `midi_port` at 31250
baud 8N1 — the mirror image of the rx deserialiser — with a 16-byte FIFO between
the CPU's byte-at-a-time model and the bit clock.

The firmware **sends nothing unprompted**: no active sensing, and there is no
keyboard. So MIDI OUT carries SysEx replies and bulk dumps only, and a host
should not wait for traffic that will never come.

Verified against a real ALSA port, not just the driver's own log — MAME's
`midiout_device` decodes the bit stream with an independent 31250 8N1 receiver,
so the timing is proven rather than assumed:

| Request | Result |
|---|---|
| `RQ1 00 01 1A` (chorus depth) | `F0 41 0F 23 12 00 01 1A 07 5E F7` |
| `RQ1 02 00 00 / 01 00 00` (patches 1-64) | **129 packets, 17706 bytes**, byte-identical to the driver's trace, 0 malformed, 0 bad checksums, addressed `010000` then `020000`..`027F00` |

A 1005 s audio render is byte-identical before and after, so nothing leaked into
the audio path.

`[?]` **MIDI THRU** is a separate jack on the real unit and is often a hardware
pass-through rather than CPU-generated. Check the schematic before emulating it.

`[?]` **Large SysEx through plugin ports.** DPF's `MidiEvent` has a small fixed
buffer with an extension pointer, and LV2 atom sequences have a buffer size limit.
A full bank dump may need chunking. This only affects *interop*; the plugin's own
patch files (§10.5) read and write `patchram` directly and are unaffected.

Note for VST3: it delivers `NoteOn`/`NoteOff`/`PolyPressure` structs plus SysEx
via `DataEvent::kMidiSysEx`, and bytes must be reconstructed. Running status and
realtime bytes do not survive, including `0xFE` active sensing. Harmless in
practice — the firmware only arms its active-sensing timeout once it has *seen* a
`0xFE`, so never receiving one is the safe state.

### 10.4 Patch and tone browsing

**Keep the OEM menu system exactly as it is** — it is part of what this project
is — and add a right-click browser beside it.

**An important distinction the hardware makes and the UI must too:**

| | Lives in | Named at | Count |
|---|---|---|---|
| **Tone** | wave ROMs and cartridges | directory at `0x0100 + 10*n` | 99 internal + card tones |
| **Patch** | `patchram` (`0xE000`–`0xFFFF`) | patch record | 64 |

Wave ROMs and cartridges hold **tones**, not patches. A patch is a combination of
six parts, each part *referencing* a tone. So the right-click menu is really two
menus:

- **Tone browser** — every tone across the internal wave ROM and all mounted
  cards, grouped by card, names read straight from the directories. Selecting one
  sets the current part's tone.
- **Patch browser** — the 64 patches in `patchram`, names read from the patch
  records, plus the user patch directory (§10.5).

**Read names directly, not through the firmware.** Instant, no emulated time. Use
the panel automaton only to *change* things.

**Show what is unavailable rather than hiding it.** A part selects its tone by
**card ID, not slot** (ROM-ANALYSIS §6.7), and IDs run `I, 1…31` — which is
exactly why 29 of 32 values legitimately display `" No Card! "` on the hardware
(see `RUNNING.md`). The browser should list a patch's required card IDs and say
*"needs card 08 (SN-U110-08) — not mounted"* instead of silently offering
something that will not sound.

### 10.5 User patch files

`patchram` is `0xE000`–`0xFFFF`, 8 KB, 64 patches → 128 bytes each. `[?]` Confirm
no bank header eats into that.

**Recommended format: our own container, not SysEx.**

The reasoning: the RAM layout is *known*; the SysEx address map is *not* — the
handler at `0x5B7F` is identified but its model ID and addresses are undecoded.
Blocking a core feature on unfinished reverse engineering is the wrong trade.

| File | Contents |
|---|---|
| `.u110patch` | header + one 128-byte patch record |
| `.u110bank` | header + the full 8 KB `patchram` image |

Header carries: magic, format version, a UTF-8 display name, the **required card
IDs**, and the SHA-256 of the wave ROMs it was authored against. That last field
is what lets the browser warn intelligently instead of loading something silently
wrong.

**Add `.syx` import/export later**, once the SysEx map is decoded — that is the
interop path to real hardware and to the U-110 patch collections already
circulating. High user value, but it follows the RE, not the other way round.

**Not JSON, yet.** Tempting for diffing and version control, but ROM-ANALYSIS
notes the 80-byte tone record is only *partly* decoded — a JSON format today
would be half named fields and half opaque byte blobs. Revisit when the decoding
is complete; the binary format is the honest representation until then.

**Loading subtlety:** the firmware caches the active patch into work RAM at
`0x2800`/`0x280E`, so poking `patchram` behind its back leaves it stale. Cleanest
fix — write the bank, reset the CPU, then **spin the core at maximum speed**
(nothing forces realtime) until the firmware reaches idle, and resume. A second of
emulated boot costs a few milliseconds of wall time.

**DAW project state** holds both NVRAM regions (`workram` `0x2100`–`0x3FFF` and
`patchram`) plus parameters and slot config, with ROMs referenced by name + hash
per §9.

### 10.6 Output routing

**Stereo out only** — decided. The `PAN_L` / `PAN_R` matrix at
`roland_u110.cpp:1036` stays in the audio path, and the six MULTI OUTPUT jacks
are summed exactly as the hardware sums them.

One consequence worth fixing: Output Modes 1–20 put all voices on jack 1, which
is **hard left** in that matrix. A patch authored for the individual jacks
therefore sounds one-sided as a plugin (see `RUNNING.md`, "Output routing").

Add a **"mono-safe" toggle**, off by default: when on, any output mode that uses
only one jack group is rendered centred instead of panned. Preserves the hardware
behaviour as the default, gives the user one click to fix a patch that was never
meant to be heard through a stereo mix.

---

## 11. Audio path

**Resampling.** The chip runs at exactly 32 kHz — 2:3 to 48 kHz, 147:160 to
44.1 kHz. Use a windowed-sinc polyphase, **not** linear interpolation; the whole
project has been meticulous about audio accuracy and linear would quietly undo a
lot of it.

**Report resampler latency** through the host's latency mechanism so delay
compensation works.

**Signal order:** core (32 kHz, 6 jacks) → pan matrix → stereo sum → resampler →
reconstruction chain → HF correction (§10.2) → user EQ → volume → clip meter.

**Run the reconstruction chain at the output rate, not at 32 kHz** — hence its
position after the resampler. It models an *analog* filter that in the real unit
sits after the DAC, and realizing it digitally costs accuracy in proportion to how
close its poles are to Nyquist. MAME's filters are
`SAMPLE_RATE_OUTPUT_ADAPTIVE`, and the difference is measurable: against the
analog prototype the digital cascade loses 1.5 / 3.0 / 6.8 dB at 10 / 12 / 14 kHz
when run at 48 kHz, and 4.9 / 10.1 / 31.0 dB at 32 kHz. Oversampling that stage
further would buy back the rest.

**Never volunteer to sleep.** Formats offer a "no processing needed" contract
(CLAP's `CLAP_PROCESS_SLEEP` and equivalents). Always decline: the emulated CPU
has to keep advancing or the LCD cursor freezes and MIDI timing drifts.

**CPU budget.** Full MAME with video runs at 100% speed for ~13% of one core.
Extracted and headless should be a few percent — comfortable for many instances.

---

## 12. Work order

1. ~~Refactor `roland_u110.cpp`: tables and pure functions into a shared header (§3).~~
   **Done** — `mame/src/mame/roland/roland_u110_data.h`. It depends on nothing but
   `<cstdint>`/`<cstddef>`, so the plugin compiles it with no MAME tree in the include
   path. Verified two ways: the reimplemented `bitswap` matches MAME's over the whole
   2^19 address domain and all 256 data bytes (and is bijective), and a 1005 s render
   is byte-identical before and after, 13/13 files.
2. ~~Wire MIDI OUT in the MAME driver, testable against `-mout` (§10.3).~~ **Done.**
   Serialised to a `midi_port`, verified end-to-end over ALSA with a 17706-byte,
   129-packet bulk dump. MIDI THRU deliberately not emulated — see below.
3. Define `U110Core` (§4) and stand up the null-test harness against MAME.
4. Write `plugin/compat/emu.h`; compile MAME's four device sources against it;
   drive the null test to green and put it in CI (§3).
5. DPF standalone, no UI, plus the ImGui debug window (§2.1, §5).
6. Panel snapshot protocol and command queue (§8).
7. Bake the CGROM from MatrixSans Screen; hand-fix the mangled glyphs (§7).
8. Live CGRAM rendering and push-on-change transport; verify against the boot
   logo animation (§7, §8).
9. LV2 and CLAP targets; VST3 falls out.
10. Panel automaton, direct name reads, bank files (§10.4, §10.5).
11. Inkscape panel and the vector UI (§6).
12. **Fix the Sallen-Key chain** (§10.2): it peaks +4.29 dB where the service
    notes say +2.17 dB. Then re-measure and re-fit the HF correction — do not
    keep the current one on top of a corrected chain. *(The measurement itself is
    done: `analysis/hf_excess.pdf`, correction shipped and switchable.)*
13. Windows cross-build; macOS when a volunteer appears.

**Tooling gap:** the CGRAM findings in §7 came from an ad-hoc decoder written
against `error.log` — it tracks the address counter to separate DDRAM from CGRAM
traffic and reconstructs both the visible text and the glyph bitmaps over time.
That belongs in the tree as `tools/lcd_trace.py`; it is the only way to see what
the firmware is drawing, and it will be needed again for the panel automaton
(§10.4) and for identifying the remaining custom glyphs.

Chorus and tremolo remain outstanding and slot in independently. `roland_lp.h`
already records that registers `0x19`/`0x1B`/`0x1D` and RAM `0x378C`/`0x378E` are
that subsystem, so the hook point is known.

---

## 13. Open questions

- `[?]` DPF's `travesty` VST3 licensing — read the headers in `distrho/src/travesty/`.
- `[?]` DPF's DSP→UI push mechanism for a ~120-byte blob.
- `[?]` DPF standalone audio backends on Windows and macOS.
- `[?]` DPF / LV2 SysEx size limits — does a bank dump need chunking?
- `[?]` FL Studio version on the target machine — does it host CLAP?
- ~~`[?]` U-110 SysEx model ID and address map (gates `.syx` interop).~~ **Largely
  resolved.** Model ID `0x23`, framing checked against the firmware's own parser in
  `tools/u110_sysex.py`, and the bulk-dump map confirmed empirically now that MIDI
  OUT works: `010000` SETUP (1 packet), `020000`..`027F00` patches 1-64 (128
  packets). Reading a bank out is proven; writing one back is untested.
- `[?]` **MIDI THRU — hardware pass-through or CPU-generated?** Still open, and now
  the *only* thing blocking the third jack. `analysis/SYSTEM-DESIGN.md` §6 claimed
  THRU comes off `TXD` alongside OUT; the owner's manual contradicts that ("the MIDI
  messages fed into the MIDI IN connector are output through the MIDI THRU
  connector"), and if it were `TXD` then THRU would carry OUT's data rather than
  IN's. The doc is now marked unverified. **This needs an eye on the schematic** —
  the service notes PDF is not OCR'd, so it cannot be grepped.
- `[?]` Per-patch record size in `patchram` — is 8192/64 = 128 exact?
- `[?]` CGROM: baked MatrixSans vs a published HD44780 A00 table — compare.
- `[?]` What CGRAM chars 0, 1 and 2 are for after boot. They are redefined at
  t = 3.513 s but never placed in DDRAM during the first six seconds, so they
  belong to a screen the machine has not reached yet — drive further into the
  menus with `tools/lcd_trace.py` running.
- `[?]` Why the modelled Sallen-Key cascade peaks +4.29 dB at 6065 Hz when the
  service notes' own simulation of the same circuit says +2.17 dB max — component
  values, topology, or MAME's `opamp_sk_lphp_calc`? This is the leading suspect
  for the measured HF excess (§10.2).
- `[?]` The top-octave deficit above 10 kHz — how much is bilinear warping and how
  much is the chain model itself? Oversampling the filter stage would separate them.
- `[?]` `14_fantasy`'s +14 dB error — chorus/tremolo, or something else?
- ~~`[?]` Product name, given the Roland and VST trademarks.~~ **Resolved:
  `Voltaire 110`.** Distinct mark; the U-110 lineage is described in prose.
