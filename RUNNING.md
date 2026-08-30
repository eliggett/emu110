# Running the U-110 emulator

Everything here was checked against the build in `mame/u110` (MAME 0.289). Commands assume
you are **in the `mame/` directory** — `rompath` and `hashpath` are relative (`roms`, `hash`),
so running from anywhere else will not find the wave ROMs.

```sh
cd mame
./u110 u110
```

That opens a window showing the 16x2 LCD, with sound on the default audio device and no MIDI
input attached. Everything below is optional on top of it.

> **Rebuild first if you have touched the source.** `tools/u110run.sh` refreshes `mame/u110`
> before every run; a bare `./u110` does not. Running the stale binary and wondering why a
> change had no effect is the single easiest mistake to make in this project.

---

## MIDI input

### Find the port

```sh
./u110 -listmidi
```

```
MIDI input ports:
Midi Through Port-0 (default)
4ACBCC15 MIDI 1

MIDI output ports:
Midi Through Port-0 (default)
4ACBCC15 MIDI 1
```

The names come from PortMidi, which on Linux enumerates ALSA sequencer ports. Anything that
shows up in `aconnect -l` as a readable client shows up here.

### Attach it

The MIDI In port is a MAME *media slot* named `midiin`, short name `min`. Pass the port name
exactly as `-listmidi` printed it, quoted:

```sh
./u110 u110 -min "4ACBCC15 MIDI 1"
```

That is the whole thing — the U-110 responds on its configured MIDI channels immediately, and
audio comes out of the default sink.

To use the ALSA virtual through port instead (handy for feeding it from software on the same
machine):

```sh
./u110 u110 -min "Midi Through Port-0"
```

…and then from another terminal, `aplaymidi -p 14:0 something.mid`, or connect a soft
sequencer to `Midi Through` with `aconnect`.

### The same option also plays files

`-min` accepts `.mid` and `.syx` files as well as live port names:

```sh
./u110 u110 -min song.mid
./u110 u110 -min patchdump.syx
```

This is how the capture and render tools drive the machine.

### MIDI output is not wired up

The U-110's MIDI OUT/THRU (CPU TXD, pin 17) is currently only written to the log —
see the `TODO` at `mame/src/mame/roland/roland_u110.cpp:745`. Nothing you can pass on the
command line will make it reach the host. It has no effect on playing notes; it matters only
if you want to pull patch data out over SysEx.

### If no note ever sounds

Check the first lines of the console output. If you see

```
U-110: *** SERVICE TEST MODE *** auto-navigating to test 11 ... No MIDI notes will sound.
```

then a stale `cfg/u110.cfg` has the AUTOTEST machine-configuration setting turned on, left
behind by an earlier session. MAME writes that file on exit, so one run that enabled a service
test silently poisons every later run. Fix it by deleting `mame/cfg/u110.cfg`, or by setting
**Tab → Machine Configuration → Auto-navigate to service test** back to *Off*. A normal boot
prints `U-110: normal boot (no service test).` instead.

### If notes play but the bender, program change or aftertouch does nothing

The U-110 has a *receive switch* per MIDI message type — `SETUP: PITCH BENDER`, `PGM CHANGE`,
`CH PRESSURE`, `POLY PRESS`, `CTRL CHANGE`, `EXCLUSIVE` — and all six live in one byte of
battery-backed RAM (`0x3C00`). The MIDI parser loads it before it dispatches anything:

```
561F: ldb 41, 3c00        ; the receive-switch mask
5B49: jbc 41, 4, 5b79     ; bit 4 clear -> throw the pitch bend away
```

Note On is **not** gated by it, so the machine plays normally while every controller message is
silently discarded. Bend Range in the part will still read `+2`; nothing ever looks at it.

A factory-initialised mask is `0x7F` (everything on). If yours is `0x00`, every switch is off at
once — which is not something you can do from the menu one at a time, and means the mask was
wiped rather than edited. Check it without booting anything:

```sh
python3 -c "print('%02X' % open('nvram/u110/workram','rb').read()[0x3C00-0x2100])"
```

`7F` is healthy, `00` is the wiped state. Zap the NVRAM to clear it — see below.

> **How it gets wiped.** The firmware can temporarily override the mask, stashing the real one
> at `0x26F8` and setting bit 7 of `0x3C00` as an "overridden" marker (`6CB4`-`6CC4`). Boot
> clears the stash *before* it checks that marker (`43D5`, then `45CD`), so powering off while
> the override is live makes the next boot restore `0x00 & 0x7F` — all switches off, and it
> survives every later boot because the `0x7F` default is only written by the one-time memory
> initialisation. Real firmware behaviour, but the emulator makes it easy to hit: closing the
> window and `-seconds_to_run` are both hard power cuts, and MAME saves the NVRAM on the way out.

---

## Operating the panel

The U-110's six front-panel switches are mapped to the keyboard. Click on the MAME window
first so it has focus; these go straight to the emulated machine, with no "partial keyboard"
mode to toggle (the driver uses button inputs, not keyboard inputs).

| Panel switch | Key | What it does |
|---|---|---|
| Part / Jump | <kbd>1</kbd> | part select; jump between edit pages |
| Edit / Exit | <kbd>2</kbd> | enter/leave edit mode |
| Left | <kbd>←</kbd> | previous parameter |
| Right | <kbd>→</kbd> | next parameter |
| Dec | <kbd>↓</kbd> | decrement value |
| Inc / Enter | <kbd>↑</kbd> | increment value; confirm |

Patch selection is Inc/Dec from the top-level play screen, so <kbd>↑</kbd>/<kbd>↓</kbd> steps
through the patches once the machine has booted. They wrap modulo 64, and there is no way to
home the selection by pressing keys — if you need a known patch, start from the power-on one.

**MIDI program change does not select a patch.** On a U-110 it selects a *part's tone* (one of
the 99 tones), which is why sending one leaves the display on the same patch name with a
`TEMP:` prefix. Patches are panel-selected only. `tools/select_patch.lua` automates the
key-tapping for scripted runs.

### MAME's own controls

These are MAME defaults in 0.289 — note that Pause is **F5**, not P:

| Key | Action |
|---|---|
| <kbd>Tab</kbd> | show/hide the MAME menu |
| <kbd>Esc</kbd> | exit (or back out of a menu) |
| <kbd>~</kbd> | on-screen sliders — master volume, speed |
| <kbd>F5</kbd> | pause / resume |
| <kbd>F3</kbd> | soft reset |
| <kbd>Shift</kbd>+<kbd>F3</kbd> | hard reset |
| <kbd>F10</kbd> | toggle throttling (runs as fast as it can) |
| <kbd>F12</kbd> | save a screenshot to `snap/` |

Inside a menu the arrow keys navigate the menu rather than the U-110 panel; outside a menu
they go to the panel. There is no conflict during normal play.

The Tab menu is where the rest lives:

- **Machine Configuration** — the two boot-time settings, *Power-on key combination*
  (service test menu / initialise memory) and *Auto-navigate to service test*.
- **Input (this Machine)** — rebind any of the six panel keys.
- **File Manager** — insert or eject PCM cards while the machine is running.
- **Slider Controls** — volume.

### Mouse

`ui_mouse` is on by default, so the MAME menus are fully clickable — you can drive the Tab
menu, File Manager and sliders with the mouse. The LCD itself is **not** clickable: this
driver has no artwork layout with panel buttons on it, so the six switches are keyboard-only.

---

## Zapping the NVRAM back to defaults

The battery-backed RAM is two files under `mame/nvram/u110/`:

| File | Size | Holds |
|---|---|---|
| `patchram` | 8192 | the 64 user patches — `0xE000-0xFFFF` in the user bank |
| `workram` | 7936 | `0x2100-0x3FFF`: setup parameters, the MIDI receive switches, the active patch edit buffer, and the firmware's scratch |

Delete them and boot. MAME's `nvram_device` is declared `DEFAULT_ALL_0`, so absent files mean
the RAM comes up all zeros, the firmware's own validity check fails, and it runs the
"Mem Initialized" copy — the same path a real U-110 takes on a dead battery.

```sh
cd mame
rm -rf nvram/u110
./u110 u110            # let it boot, then quit with Esc
```

Verified afterwards: `patchram` is **byte-identical** to the factory patch table in the program
EPROM, and the MIDI receive-switch mask at `0x3C00` is back to `0x7F`.

Three things worth knowing:

- **Let it finish.** The initialisation takes about four seconds of emulated time. Quitting
  sooner leaves a half-written store (measured: 6218 of 8192 patch bytes still wrong, and
  `0x3C00` still `00`). It is not fatal — the validity check still fails, so the next full boot
  redoes the job — but the store is useless until then.
- **Quit cleanly.** MAME writes the NVRAM on exit. `Esc`, or `-seconds_to_run`, both count;
  killing the process does not, and you keep the old contents.
- **Nothing else is touched.** Machine-configuration settings live in `cfg/u110.cfg`, not here.
  If a service test is stuck on, that is the file to delete instead — see
  [If no note ever sounds](#if-no-note-ever-sounds).

To keep a good store while trying something risky, back it up rather than relying on undo:

```sh
cp -r nvram/u110 nvram/u110.bak
```

### Leaving it alone for one run

To start a single run from factory state without disturbing what you have saved, point MAME at
a scratch directory instead of deleting anything:

```sh
./u110 u110 -nvram_directory "$(mktemp -d)"
```

This is what `tools/u110run.sh` does for every render, which is why scripted runs always come up
on patch P-01 and never inherit a stale setting.

---

## PCM cards (`-cart1` … `-cart4`)

The U-110 has four card slots, and the driver models all four. They are separate media
devices, so each gets its own option:

| Slot | Long name | Short name |
|---|---|---|
| 1 | `-cartridge1` | `-cart1` |
| 2 | `-cartridge2` | `-cart2` |
| 3 | `-cartridge3` | `-cart3` |
| 4 | `-cartridge4` | `-cart4` |

There is no bare `-cart`; you must say which slot.

### By file path

Point it at a raw card dump:

```sh
./u110 u110 -min "4ACBCC15 MIDI 1" \
  -cart1 "../roms/roland_u220_waverom4_(sn-u110-08).bin" \
  -cart2 "../roms/roland_u220_waverom5_(sn-u110-09).bin"
```

Notes on the file:

- **Raw, still scrambled.** Give it the dump exactly as read off the card ROM; the driver
  applies the same address/data descrambling it uses for the internal wave ROMs.
- **512 KB maximum.** A larger file is rejected with `Invalid size`.
- **Undersized dumps are mirrored** up to 512 KB in 128 KB pages, because the address
  descrambling cannot mirror at a finer granularity.
- Extension is `.bin`.

Card presence is reported to the firmware on PORT1, so the U-110 notices the card and its
tones become selectable — you do not have to tell it anything else.

### By software-list short name

`hash/u110_card.xml` carries 19 known cards. `./u110 u110 -listsoftware` prints them all.

```sh
./u110 u110 -cart1 sn_u110_08
```

For this to work the dump has to sit where MAME looks for software-list ROMs, which is a
directory **named after the software item**, under a directory named after the list:

```
<rompath>/u110_card/sn_u110_08/sn-u110-08.bin
```

The filename and CRC must match the XML entry. (Both card dumps in this project's `roms/`
match their software-list entries exactly — `roland_u220_waverom4_(sn-u110-08).bin` is CRC
`104f3974`, which is `sn_u110_08`.) You can add a second rompath rather than reorganising
your dumps:

```sh
./u110 u110 -rompath "roms;/path/to/cards" -cart1 sn_u110_08
```

The available names:

| Short name | Card |
|---|---|
| `sn_u110_01` | SN-U110-01 Pipe Organ & Harpsichord |
| `sn_u110_02` | SN-U110-02 Latin & F.X. Percussions |
| `sn_u110_03` | SN-U110-03 Ethnic |
| `sn_u110_04` | SN-U110-04 Electric Grand & Clavi |
| `sn_u110_05` | SN-U110-05 Orchestral Strings |
| `sn_u110_06` | SN-U110-06 Orchestral Winds |
| `sn_u110_07` | SN-U110-07 Electric Guitar |
| `sn_u110_08` | SN-U110-08 Synthesizer |
| `sn_u110_09` | SN-U110-09 Guitar & Keyboards |
| `sn_u110_10` | SN-U110-10 Rock Drums |
| `sn_u110_11` | SN-U110-11 Sound Effects |
| `sn_u110_12` | SN-U110-12 Sax & Trombone |
| `sn_u110_13` | SN-U110-13 Super Strings |
| `sn_u110_14` | SN-U110-14 Super Ac Guitar |
| `sn_u110_15` | SN-U110-15 Super Brass |
| `sn_mv30_01` | SN-MV30-01 Rhythm Section (U-31) |
| `sn_mv30_02` | SN-MV30-02 Orchestral (U-30) |
| `sn_spla_01` | SN-SPLA-01 Sound Elements Vol. 1 (U-01) |
| `mus1_akk`   | Musitronics 1 Akkordeon (U-25) |

### Using a card's tones — the part that trips people up

A card being fitted is not enough; you have to point a patch part at it, and the control is
easy to misread.

The TONE parameter lives at **Edit → PATCH → PART → BAS**, on that page's *first* screen, and
it is **two fields**:

```
08-02:BELL PAD
^^ ^^
|  +-- tone number within that group
+----- group
```

The group is a **card ID — the SN-U110 catalogue number — not a slot number**. It runs `I`
(the 99 internal tones), then `1`, `2`, `3` … up to `31`. Any ID with no matching card fitted
displays ` No Card! `, which is most of them. Tapping Inc a few times from `I` lands you on
`1`…`7` and shows nothing but "No Card!" even though your cards are mounted and fine.

For SN-U110-08 you need group **8**; for SN-U110-09, group **9**.

**How these menus work:** each menu screen lists its choices along the bottom line, with one
of them blinking. <kbd>←</kbd> and <kbd>→</kbd> move the blinking highlight along that list;
<kbd>↑</kbd> enters whatever is currently highlighted. There is no separate "enter" key and
nothing is chosen by typing its name — you move the highlight onto it and press <kbd>↑</kbd>.

Full sequence from the play screen, one keypress per row:

| # | Press | Display afterwards |
|---|---|---|
| 1 | <kbd>2</kbd> | `Select Mode` / `SETUP.PATCH.UTIL` — highlight on SETUP |
| 2 | <kbd>→</kbd> | highlight moves to PATCH |
| 3 | <kbd>↑</kbd> | `PATCH` / `COMMON.PART.WRT` — highlight on COMMON |
| 4 | <kbd>→</kbd> | highlight moves to PART |
| 5 | <kbd>↑</kbd> | `PATCH:PART1` / `BAS.LEVL.PIT.LFO` — highlight on BAS |
| 6 | <kbd>↑</kbd> | `PATCH:PART1:BAS` / `I-02:A.PIANO 2` — the TONE page |
| 7 | <kbd>↑</kbd> ×8 | group `I` → `8`: `08-02:BELL PAD` |
| 8 | <kbd>→</kbd> | cursor moves onto the **tone number** |
| 9 | <kbd>↑</kbd>/<kbd>↓</kbd> | `08-03:SYN CHOIR`, `08-04:BREATH VOX`, … |

Steps 4 and 6 look inconsistent but are not: at step 4 the item you want (PART) is second in
the list so it needs one <kbd>→</kbd>, while at step 6 the item you want (BAS) is already the
highlighted one, so <kbd>↑</kbd> goes straight in.

<kbd>2</kbd> backs out one level per press, and it restores the highlight to whatever you had
entered rather than resetting it to the first item — back out of BAS and PART is still the
highlighted choice, so going back in is a single <kbd>↑</kbd>. Holding <kbd>↑</kbd>
auto-repeats, which is the quick way to cross the group range at step 7.

The second <kbd>→</kbd> is the non-obvious one: on the TONE page, Right moves the cursor from
group to tone number first, and only advances to the next parameter (OUTPUT ASGN) on the press
after that. Holding <kbd>↑</kbd> auto-repeats, which is the quick way to cross the group range.

Because the group is a catalogue number rather than a slot, a patch that names card 8 works
whichever slot the card is in — that is what `PATCH:CARD ASGN` exists for, and it is also why
you do not have to assign anything before a card tone becomes selectable.

### Checking that a card really mounted

If you want to confirm it independently of the menus, the firmware caches one ID byte per slot
at RAM `0x2743`, and stores `0xFF` for a slot whose header failed to verify:

```sh
./u110 u110 -debug -cart1 "../roms/roland_u220_waverom4_(sn-u110-08).bin"
```

then in the debugger, after letting it boot, `dump /dev/stdout,0x2743,4,1`. With SN-U110-08 in
slot 1 and SN-U110-09 in slot 2 it reads `08 09 00 00`. A bad or unrecognised dump shows `FF`
and the machine displays `  Illegal CARD` when mounting it.

### Swapping cards while running

<kbd>Tab</kbd> → **File Manager** → pick a slot → choose a file or software item. Ejecting
clears that slot's PCM region, so the machine correctly sees the card go away.

---

## Output routing — when a part only comes out of one channel

The U-110 has six MULTI OUTPUT jacks plus a MIX L/R pair that sums them, and the emulator
models the sum. The jacks are not centred: measured on hardware with service test 11, they sit
at

| Jack | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| position | hard **L** | hard **R** | centre | centre | +6.7 dB L | +6.8 dB R |

So a part routed to jack 1 comes out of the **left channel only** — down 72 dB on the right,
measured. That is almost always the cause when a tone sounds in one ear.

Which jack a part uses is decided by the **patch's Output Mode**, not by anything global. Every
patch carries its own, so it changes as you change patches:

**Edit → PATCH → COMMON → OUT**

| # | Press | Display afterwards |
|---|---|---|
| 1 | <kbd>2</kbd> | `Select Mode` / `SETUP.PATCH.UTIL` |
| 2 | <kbd>→</kbd> <kbd>↑</kbd> | `PATCH` / `COMMON.PART.WRT` — highlight on COMMON |
| 3 | <kbd>↑</kbd> | `PATCH:COM` / `NAME.OUT.EFFECT` — highlight on NAME |
| 4 | <kbd>→</kbd> | highlight moves to OUT |
| 5 | <kbd>↑</kbd> | `PATCH:COM:OUT#22` / `M31.M31.*.*.*.*` |
| 6 | <kbd>↑</kbd>/<kbd>↓</kbd> | choose a mode |

The bottom line shows how the 31 voices are split across the six jacks, and it tells you
immediately what you will hear:

```
OUT# 1   31.*.*.*.*.*        all 31 voices on jack 1  ->  HARD LEFT
OUT#22   M31.M31.*.*.*.*     all 31 voices centred    ->  both channels
OUT#23   L16.R16.15.*.*.*    16 left, 16 right, 15 on jack 3
```

Modes **1-20** put the first group on jack 1 alone, so they are left-only through the MIX
outputs — they are meant for people using the individual jacks. Modes **21-50** pair jacks 1
and 2: the `M` variants (22, 24, 26 …) are dry and centred, the `L`/`R` variants (21, 23, 25 …)
are a stereo pair. **If you want both channels, pick an `M` mode — #22 is the simple one.**

Two things worth knowing:

- The per-part **OUTPUT ASGN** (Edit → PATCH → PART → BAS, two pages right of the TONE) picks
  *which group* that part uses, numbered as on the bottom line above. It cannot move a part to
  a jack the Output Mode did not create — in mode 22 there is only one group, so setting a part
  to OUTPUT ASGN 3 or higher makes it **silent** rather than moving it.
- This is a patch parameter, so it reverts when you change patches unless you save it with
  **PATCH → WRT**.

## Audio and latency

The machine holds exactly real time — measured at 100.00% both windowed and headless, at
about 13% of one CPU core. Measured MIDI-in to audio-out latency inside the emulator is
roughly **20–35 ms**, plus whatever your audio sink adds.

Most of that is the throttle quantum: the LCD screen is defined at 50 Hz
(`roland_u110.cpp:961`), so MAME syncs in 20 ms steps. MIDI input jitter is not a factor —
MAME polls the port at 1500 Hz.

If you want it tighter:

```sh
./u110 u110 -min "4ACBCC15 MIDI 1" -lowlatency 1
```

`-lowlatency` draws the frame before throttling and costs nothing.

For finer control you have to change the audio backend. **The `pulse` and `pipewire` modules
ignore `-audio_latency` entirely** — only PortAudio honours it, in units of 20 ms:

```sh
./u110 u110 -min "4ACBCC15 MIDI 1" -sound portaudio -audio_latency 1
```

PortAudio also enumerates ALSA hardware devices directly, so it can bypass the PipeWire graph
and talk to an interface as `hw:N,M`. Run with `-verbose` to see the device list it found.

Available backends on this build: `sdl`, `portaudio`, `pulse`, `none`. A `pipewire` module
exists in the MAME tree but is **not compiled into this binary** — asking for it prints
`Value pipewire not supported for option sound - falling back to auto` and you get PulseAudio.

---

## Typical example on this machine:
```sh
./u110 u110 -midiin "UM-4 MIDI 1" -cart1 "../roms/roland_u220_waverom4_(sn-u110-08).bin" -cart2 "../roms/roland_u220_waverom5_(sn-u110-09).bin" -window -resolution 640x480
```

## Other useful options

| Option | Effect |
|---|---|
| `-window` | run in a window rather than full screen |
| `-resolution 380x280` | set the window size |
| `-wavwrite out.wav` | record the stereo output to a file (works alongside live audio) |
| `-seconds_to_run N` | quit after N emulated seconds |
| `-nothrottle` | run as fast as possible — for rendering, not for playing |
| `-video none` | no window at all (pair with `SDL_VIDEODRIVER=dummy`) |
| `-volume -6` | attenuate the output, in dB |
| `-verbose` | print the audio/MIDI backend it chose |
| `-log` | write `error.log`, which is where the driver's own `logerror` output goes |

State persists between runs unless you redirect it: user patches live in the battery-backed
RAM MAME saves to `nvram/`, and machine-configuration settings go to `cfg/u110.cfg`. The
capture scripts deliberately point both at scratch directories so every run starts from P-01;
for ordinary use, leave them alone so your edits survive. To wipe that state deliberately, see
[Zapping the NVRAM back to defaults](#zapping-the-nvram-back-to-defaults).

A known warning on every run is harmless:

```
u110_lcd_cgrom.bin ROM NEEDS REDUMP
```

The U-110's LCD controller has never been identified and its character generator has never
been dumped, so the driver ships a synthesised ASCII font, correctly flagged `BAD_DUMP`.
