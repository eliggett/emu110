# Roland U-110 — System Design Notes

Hardware architecture of the U-110 (PCM Sound Module, Jan 1989), reconstructed from the
factory service notes and cross-checked against the firmware.

**Companion document:** [`ROM-ANALYSIS.md`](ROM-ANALYSIS.md) covers the firmware image
itself — code structures, data formats, and modification. Where the two overlap, this
file is authoritative on *hardware* and that one on *what the code does*.

**Sources**

| | |
|---|---|
| `reference/ROLAND_U-110_SERVICE_NOTES.pdf` | p.5 block diagram, p.6 IC8 block diagram + address map, p.13-14 main board schematic |
| `U110v200.BIN`, `U110v203.BIN` | program EPROM images |
| `roland_u110_u220/*.bin` | wave ROM and cartridge dumps |

> **Confidence key** — `[S]` from the service notes / schematic, `[C]` confirmed from a
> ROM image, `[I]` inferred and still open.

---

## 1. Topology

```
                         8097BH  (IC3)
                         X1 = 12 MHz
                              |
                    AD0-AD15 multiplexed bus
                              |
      +-----------+-----------+-----------+--------------+
      |           |           |           |              |
    IC8         IC9        IC10/IC11    IC15           IC26/IC6
  M60012-      27C512      SRAM 8K x2  MB87419         BU3905S
  0141FP      program      /CS8 /CS9   tone gen        output control
  I/O gate     /CS7                    (16-bit port)   D0-D5, A0-A3
  array +                                   |
  ADDRESS                                   | WA0-WA18 (19-bit wave address)
  DECODE                                    |
  |  |  |                                   +--- IC18/19/20/21  WAVE ROM 4Mbit x4
  |  |  +-- LCD unit (16x2)                 +--- IC17  EFFECT RAM (CXK5814)
  |  +----- switch board (SW x6)            +--- CARTRIDGE BOARD - PCM CARD x4
  +-------- IC5 HC74 -> EDIT/PAGE LEDs      |    (selects decoded by IC13 HC02 / IC14 HC32)
                                            |
            wave/card DATA D0-D7 -----------+---> IC16  (NOT to IC15)
                                            |
                              IC15 <--9-bit bus--> IC16 MB87420
                                                     |  X2 = 34.816 MHz
                                    INH / MXA / MXD  +-----------> IC26/IC6
                                                     v                 | G0-G5
                                             IC22 PCM54HP 16-bit DAC   |
                                                     v                 v
                                             IC23 I-V amp -> IC24-26 analog sw x6
                                                     -> IC30-35 LPF A-F
                                                     -> JK4-JK9 individual outs
                                                     -> JK2/JK3 mix out, phones
```

### Chip roles

| Ref | Part | Role |
|---|---|---|
| IC3 | **N8097BH** | CPU. MCS-96 family, 68-pin, 16-bit multiplexed AD bus, 12 MHz |
| IC8 | **M60012-0141FP** | I/O gate array: **address decode** (`/CS1`-`/CS9`), LCD interface, switch scan, LED write, 16-bit address latch, `BUSWIDTH` generation |
| IC9 | **27C512** | Program EPROM, 64K x 8 (`/CS7`) |
| IC10, IC11 | SRAM 8K x 8 | Back-up / working RAM (`/CS8`, `/CS9`), battery-backed |
| IC15 | **MB87419** | Tone generator gate array. Generates the 19-bit wave address, owns the CPU register window. X2 = 34.816 MHz `[S]` (schematic p.13, pins 79/80), divided by **1088** for a 32 kHz engine rate — see §5 |
| IC16 | **MB87420** | Wave **data** gate array. Receives wave ROM data, feeds the DAC, drives output multiplex timing |
| IC17 | **CXK5814** | Effect RAM, private to IC15 |
| IC18-21 | mask ROM 4 Mbit | Wave ROM, 512 KB each, 2 MB total |
| IC13, IC14 | HC02, HC32 | Wave ROM / cartridge chip-select decode |
| IC22 | **PCM54HP** | 16-bit D/A converter (mono) |
| IC23 | HS5238 | I-V amplifier |
| IC24-26 | D6201ACJ | Analog switches (x6) |
| IC26 / IC6 | **BU3905S** | Output control gate array — see naming note below |
| IC30-35, IC39-41 | 4570 | Low-pass filters A-F |
| IC5 | HC74 | 2-bit latch, **EDIT/EXIT and PART/JUMP LEDs** — see §6.1 |
| IC2 | — | MIDI input opto-isolator |
| IC4 | 062 | Integration circuit — LCD contrast, driven by the CPU's PWM pin |
| IC1 | — | Reset generator |

> **Naming note.** The service notes are inconsistent about the output control chip. The
> p.5 block diagram calls it **IC6**; the p.14 schematic prints **IC26** with a
> hand-written "IC6" correction beside it. Same BU3905S. This document writes IC26/IC6.
> Note also that "IC24-26" separately denotes the D6201ACJ analog switch array, so the
> printed IC26 is very likely the error.

---

## 2. Address decode — IC8

IC8 is the memory controller. The service notes publish its internal block diagram and
address map (p.6, Figs. 1-a and 1-b). Everything below is `[S]`.

| IC8 signal | Destination |
|---|---|
| `/CS7` | IC9 `/CE` — program EPROM |
| `/CS8` | IC10 `/CS` — SRAM |
| `/CS9` | IC11 `/CS` — SRAM |
| `/CS1`-`/CS6` | I/O devices in the `0x1000-0x1FFF` block |
| `/RD` | tied with CPU pin 61 `/RD` to `/OE` of EPROM **and** both SRAMs |
| `/WA` | tied with CPU pin 40 `/WRL,/WR` to `R//W` of both SRAMs |
| `LED WR`, `LCD CS/E/RW/RS`, `SW CS` | panel and display |
| `BUSWIDTH` | back to the CPU — see §2.2 |
| pin 74 `BANK SELECT` | **input** from CPU pin 38, P2.7 — see §3 |

### 2.1 The published map

```
              BANK SELECT 1        BANK SELECT 0
  0xE000-0xFFFF   /CS7  PROGRAM       /CS9  EXT. MEMORY      <-- the bank switch
  0x4000-0xDFFF   /CS7  PROGRAM       same
  0x2100-0x3FFF   /CS8  BACK UP       same
  0x2000-0x20FF   /CS7  PROGRAM       same
  0x1000-0x1FFF   I/O (below)         same
  0x0000-0x0FFF   /CS7  PROGRAM       same
```

I/O block detail:

```
  0x1F00-0x1FFF   /CS6            output control (IC26/IC6)
  0x1E00-0x1EFF   /CS5
  0x1C00-0x1DFF   /CS4
  0x1400-0x1BFF   /CS3            tone generator (IC15)
  0x1300-0x13FF   SW SCAN CS      switch matrix read
  0x1200-0x12FF   LED CS          panel LED latch
  0x1100-0x11FF   LCD CS          LCD command / data
  0x1000-0x10FF   /CS1, /CS2
```

Three consequences worth stating plainly:

- **The EPROM appears in three separate windows** — `0x0000-0x0FFF`, `0x2000-0x20FF`, and
  `0x4000-0xDFFF` (plus `0xE000-0xFFFF` in bank 1). The middle window exists so the CPU
  can fetch its MCS-96 interrupt vectors, CCB and reset stub, which the silicon demands at
  `0x2000`/`0x2018`/`0x2080`, from ROM while RAM occupies the space just above.
- **`0x0000-0x0FFF` is real, addressable EPROM.** The 8097BH's internal register file
  shadows the bottom 256 bytes, leaving `0x0100-0x0FFF` usable. In both firmware images
  that region is entirely blank — see `ROM-ANALYSIS.md` §8.
- **Working RAM starts at `0x2100`**, not `0x2000`.

### 2.2 The bus is dynamically sized

`BUSWIDTH` is **not strapped** — the p.6 block diagram shows it as an output of IC8's
internal decoder, so the CPU is told the width per bus cycle.

- Memory devices are byte-wide: IC9 and both SRAMs expose `D0-D7` only, and a single
  `/WRL,/WR` strobe serves all of them. No `/WRH` exists in the system.
- **IC15 presents a full 16-bit port `D0-D15`** to the AD bus.

So EPROM and SRAM cycles run 8-bit while tone-generator cycles run 16-bit. The two SRAMs
are two independent byte-wide banks with separate chip selects, not a 16-bit pair.

---

## 3. The bank switch — P2.7

CPU pin 38 (`P2.7`) drives IC8 pin 74, `BANK SELECT`. It selects what occupies
`0xE000-0xFFFF`:

| P2.7 | `0xE000-0xFFFF` |
|---|---|
| **1** | `/CS7` — the program EPROM's top 8 KB: the **factory default patch set** |
| **0** | `/CS9` — IC11, the **battery-backed user patch store** |

The firmware confirms this independently `[C]`. The routine at `0x8475` copies a region
onto *itself*, toggling the line between the read and the write:

```asm
8475  LD   RW52,#0xE000
8479  ORB  PORT2,#0x80      ; BANK SELECT = 1  -> EPROM
847c  LD   RW50,[RW52]      ;   read factory default
847f  ANDB PORT2,#0x7F      ; BANK SELECT = 0  -> SRAM
8482  ST   RW50,[RW52]+     ;   write to the SAME address
848a  JNE  0x8479
```

That is the "Mem Initialized" factory reset. `ORB PORT2,#0x80` occurs **exactly once in
the entire ROM**, so every other access to `0xE000-0xFFFF` reaches user RAM. Reset clears
the line early (`0x4390`) and it stays clear.

Both regions hold 64 patches of 128 bytes — 8192 bytes, exactly one SRAM.

---

## 4. Wave subsystem

### 4.1 Division of labour between IC15 and IC16

This is the part most easily got wrong, and an earlier revision of the companion document
did get it wrong:

- **IC15 (MB87419) generates the address.** It drives `WA0-WA18`, a 19-bit bus reaching
  all four wave ROMs, the cartridge board, and IC17. 19 lines = 512 KB, exactly one wave
  ROM. All four ROMs share the bus, so bank selection is by **chip enable**, decoded by
  IC13/IC14 — not by extra address bits.
- **IC16 (MB87420) receives the data.** Wave ROM and cartridge `D0-D7` go to IC16, *not*
  to IC15. IC16 feeds the DAC and generates output multiplex timing.
- The two are joined by a **9-bit private bus** plus three series-resistored lines.

The CPU has no path of its own to any of this. When firmware reads a wave ROM or card
byte, the data travels **ROM → IC16 → 9-bit bus → IC15 → CPU** — through two gate arrays.
The read mechanism is described in `ROM-ANALYSIS.md` §6.1.

### 4.2 Address lines are permuted — **fully solved** `[S]` `[C]`

IC15's address pins do not map to wave ROM address bits in order:

```
IC15 pin : ROM bit
  67:A0  66:A1  64:A2  62:A3  44:A4  58:A5  56:A6  54:A7  55:A8  57:A9
  63:A10 60:A11 51:A12 53:A13 50:A14 49:A15 47:A16 48:A17 46:A18
```

Sorted by ROM bit the routing character is obvious — `A5:58, A6:56, A7:54` walking down
even pins while `A8:55, A9:57` walk back up odd pins, and `A4` alone on pin 44 far from
its neighbours. That is layout convenience, not a functional bus order.

**The full 19-bit permutation** (logical / IC15 address bit → wave ROM chip bit):

| logical | → chip | | logical | → chip | | logical | → chip |
|---|---|---|---|---|---|---|
| A0 | A0 | | A7 | A8 | | A14 | A16 |
| A1 | A5 | | A8 | A10 | | A15 | A14 |
| A2 | A4 | | A9 | A13 | | A16 | A15 |
| A3 | A6 | | A10 | A9 | | A17 | A17 |
| A4 | A1 | | A11 | A7 | | A18 | A18 |
| A5 | A2 | | A12 | A11 |
| A6 | A3 | | A13 | A12 |

Bits **0-13** were derived here — first by fitting the firmware's 48-byte header table
(720 candidates, exactly one consistent assignment), then by extending against known
plaintext, the 99 preset tone names from the owner's manual. All 99 records decode cleanly
and in order, and the ID header simulation reproduces the expected signature byte for byte.

Bits **14-18** come from MAME, which had independently reversed the same bus for the
CM-32P (§4.6). **All 14 bits derived here match MAME's exactly.**

### 4.3 Data lines are permuted too — **confirmed** `[S]` `[C]`

Wave ROM `D0-D7` land on IC16 pins `6, 4, 2, 1, 3, 5, 7, 8` — sorted by pin, that reads
`D3 D2 D4 D1 D5 D0 D6 D7`, a butterfly fanning out from the middle.

The **net** permutation of the whole path (`ROM → IC16 → 9-bit bus → IC15 → CPU`) is the
**inverse** of the 256-byte table the firmware holds at `0x9257`. Chip bit → CPU bit:

| chip | → CPU | | chip | → CPU |
|---|---|---|---|---|
| D0 | D2 | | D4 | D1 |
| D1 | D7 | | D5 | D3 |
| D2 | D6 | | D6 | D0 |
| D3 | D4 | | D7 | D5 |

So the firmware's table and the hardware wiring compose to the identity. Confirmed by
decoding real dumps, and **matching MAME's `UNSCRAMBLE_DATA` for the CM-32P bit for bit**
(§4.6).

> **Correction.** An earlier revision of this table was transcribed wrongly — it listed the
> `0x9257` table's *forward* values against inverse labels, producing a mapping in which D2
> appeared twice as a target and which was therefore not even a permutation. No computation
> in this analysis was affected (they all used the table array directly), but the printed
> table was wrong.

### 4.4 Cartridge board

Four PCM card slots (`PCM CARD x4` in the block diagram), sharing the wave ROM's address
and data lines. A card and an internal wave ROM therefore sit on identical wiring, which
is why one pair of firmware tables serves both.

Card presence is sensed on CPU `PORT1` bits 0-3, **active low**; `PORT1` is initialised to
all-ones at `0x4378` (the quasi-bidirectional input idiom) and a set bit means "no card".

`[I]` The cartridge connector's address pins appear to be *labelled* in the reverse order
from the wave ROM's — e.g. wave `A0` against cartridge `A19`. Since one firmware routine
reads the ID header from both card and internal ROM successfully, the addressing must be
functionally identical, so this is almost certainly connector pin **numbering** running
opposite to signal order rather than a signal reversal.

### 4.5 Effect RAM — the effects are **hardware**, in IC15 `[S]` `[C]`

IC17 (CXK5814, **2K x 8**) hangs off the same wave address bus, taking a low slice of it. It is
written and read only by IC15 and is never visible to the CPU — which is why no firmware
access to it exists anywhere in the image. At the 32 kHz engine rate 2048 bytes is about
**64 ms**, which is a chorus delay line.

`[C]` **The CPU does no modulation.** Measured on P-52 Fantasy (the maximum tremolo depth,
`0F`) with a 10 s sustained note: **no register of any playing voice changes value** for the
whole note. The frequency word sits at `0x3BFE` and is merely rewritten ~325 times; volume is
written 4 times. There is no software LFO and no software envelope, so chorus, tremolo and the
amplitude envelope must all be generated inside the gate arrays — IC15 with IC17 as its RAM,
not IC16, which only carries wave data to the DAC.

`[C]` **Voice 0 is not a voice** — confirming what `EMULATOR-PLAN.md` §1 already recorded from
the ROM analysis ("voice 0 reserved for ROM reads"). The only registers that move during a note
belong to voice 0 (`09`/`0A`/`0B`, ~3000 writes, address stepping `0x37..0x9F`): it is the CPU's
**wave-ROM read port** — set an address, read the byte back from register `01`. Playing voices
are numbered **1-31**, and voice 0 was never allocated in any run.

`[C]` **Effect parameters live in the patch header.** Bytes `+0x0F`..`+0x12` hold the four
values named by the firmware's own strings at `0x09813`-`0x0987D`: **CHORUS RATE, CHORUS DEPTH,
TREMO. RATE, TREMO. DEPTH**. Eleven distinct combinations across the 64 factory patches, e.g.
Ac.Piano `07 07 07 07`, Wide Piano `07 03 00 00` (tremolo off), Fantasy `03 01 04 0F`. They are
written once when the patch loads.

`[C]` **Where they land in IC15's register window is now settled**, and the answer is that
they mostly do not: the four bytes are turned into ramp-generator segments in software. The
patch-load routine at `0xB4C6` indexes two 16-entry tables per effect by DEPTH, offsets the
rate byte by RATE, and programs the result into **slots `0x20` and `0x21`** -- two more slots
of the same envelope ramp generator the 32 voices use. `0x20` is chorus, `0x21` tremolo. The
interrupt handler turns each one round at its target, so the LFO is the ramp generator run
back and forth. Register `1D` is not a parameter at all: it is a config byte the firmware
looks up from the **OUTPUT MODE** (`0xA726 + 8*index`), and bits 1 and 3 are what enable
tremolo and chorus. Registers `19` and `1B` really are constant. Full decode, tables and all,
in `analysis/EFFECTS.md`.

`[C]` **No factory patch turns either effect on.** All 64 select an output mode whose config
byte has both bits clear, which is why every recording in `listen/hardware/` is dry and why
the stored `07 07 07 07` on Ac.Piano does nothing. The effects reach the outputs only in the
odd `<L>/<R>` modes 21-49.

`[C]` **Implemented.** `roland_lp.cpp` gained a per-voice output mask and a selectable
output count; `roland_u110.cpp` derives the mask from the Output Mode at RAM `0x280E` and sums
the six buses to the MIX pair at the measured pan gains. See `IMPLEMENTATION-PLAN.md` §4.7b.

`[C]` **What the two LFOs do, measured** (`listen/hardware/effects`, the first capture of
either effect). Both are **symmetric triangles**, advancing `2^(rate/8) * 4` per 32 kHz sample
in each direction -- the *falling* constant both ways, which is not what a voice does and is
the one open question left here. Chorus runs 0.42-1.73 Hz, tremolo 1.67-6.93 Hz.

The **tremolo is an auto-pan**: one output takes the slot's level and the other its
complement, so the pair sums to a constant. Depth 0-15 spans 0.4 to 30.1 dB, matching the
target table to within the measurement.

The **chorus is a delay line tapped at `level >> 14` samples** -- 1 to 32 ms of IC17's 64 ms
-- with a tap in *each* output half an LFO period apart, mixed roughly 50/50 with the dry.
Not a polarity flip: `L - R` cancels the dry by 32 dB while `L + R` leaves the wet almost
untouched.

`[C]` **Implemented.** `roland_lp.cpp`'s `fx_render()` carries a 2048-sample delay line and
the pan multiply, on Voice Group 1, driven from the two ramp slots. Rendered against the
hardware capture the LFO rates agree within 1% and the pan ratio at depth 15 measures
0.040..0.960 against 0.039..0.961. See `analysis/EFFECTS.md` §9 for what is *not* modelled.

`[C]` One detail worth recording as system design rather than emulation: with the tremolo on,
the **firmware asks for a voice level 16 log units higher** — volume word `F278` instead of
`E270`, exactly one octave. The pan divides the signal between two outputs and the CPU puts
the missing 6 dB back, which is independent confirmation that the pan really is a division
rather than a modulation about unity.

---

### 4.6 Independent confirmation — MAME's CM-32P driver `[C]`

MAME emulates the **Roland CM-32P**, a close sibling of the U-110 built on the same PCM
engine, in `src/mame/roland/roland_cm32p.cpp`, with the gate array pair implemented as
`mb87419_mb87420_device` in `src/devices/sound/roland_lp.cpp` (driver by Valley Bell). It
was reverse-engineered independently of this analysis, and it agrees throughout:

| Finding | Here | MAME |
|---|---|---|
| CPU family / clock | 8097BH, 12 MHz | P8098, 12 MHz |
| EPROM window for vectors | `0x2000-0x20FF`, reset at `0x2080` | same |
| RAM start | `0x2100` | `0x2100-0x3FFF` |
| LCD registers | `0x1100`, `0x1102` | same |
| Switch input | `0x1300` | test switch state |
| Sound chip window | `0x1400-0x143F` | `0x1400-0x14FF` |
| Address line scramble | 14 bits derived | **all 14 match**, plus 5 more |
| Data line scramble | 8 bits derived | **all 8 match** |
| Voice 0 reserved for ROM reads by firmware | derived (§6.1 of ROM-ANALYSIS) | *"Voice 0 is reserved by the firmware for reading data from the PCM ROM"* |
| Pitch format | 14-bit fraction | 2.14 fixed point, `0x4000` = 1 byte/sample |

MAME also states the scrambling *"matches the SN-U110 cards"*, consistent with the U-110's
cartridge and wave ROM sharing one bus (§4.4).

---

## 5. Audio output path

`[C]` `[S]` **Engine sample rate: 32,000 Hz.**

```
34,816,000 / 1088 = 32,000 exactly
```

Confirmed against real hardware: playing identical MIDI into a U-110 and into the emulator,
MAME's `clock / 2 / 512` divisor (1024, correct for the CM-32P) made the emulator run
**+104 cents** sharp, and `1200 * log2(34000/32000) = +104.96`. The crystal is confirmed at
**34.816 MHz** on the schematic (p.13, IC15 pins 79/80), so it is the **divisor** that
differs between the two machines, not the oscillator — both arrive at 32 kHz by different
routes.

> An earlier revision of this file claimed 34 kHz and dismissed the circulating 32 kHz WAVs
> as resampled. They are at the **native** rate. (They are separately known to have been
> interpolated by whoever produced them — 43,830 distinct values with gcd 1 — but that is a
> property of their extraction, not of the sample rate.) See `ROM-ANALYSIS.md` correction #22.

A **single mono PCM54HP DAC** serves six individual outputs plus a stereo mix, so the
analog side is time-multiplexed:

```
IC16 --> IC22 PCM54HP --> IC23 I-V amp --> IC47A~F analog switches (x6)
                                              --> IC30-35 LPF A..F --> JK4-JK9 individual
                                              --> IC38 summing amps --> JK2/JK3 mix out
IC16 --INH/MXA/MXD--> IC26/IC6 BU3905S --G0-G5--^
```

### 5.1 The output multiplex — 8 slots, 6 outputs `[S]` `[C]`

Service notes p.7 (Fig. 3) documents the scheme:

> *"MB87420 (IC16) outputs 31 voices sequentially in **8 slots**. The voices in a slot are
> adressed 0-8 by IC6 and can be selectively routed to any OUTPUT."*

**"8" is the address counter, not a number of outputs.** Fig. 3 draws exactly *one* analog
switch + S/H + buffer as a representative channel; the eight appears in the strip across the
top, `ADD 0,1,2,3,4,5,6,7,0` over `VOICE 0,1 | 2,3 | 4,5 | ... | 14,15`. The English caption's
"adressed 0-8" is a typo -- the Japanese column reads `0～7までのアドレス`, addresses 0 to 7.
So the hardware is **8 time slots feeding 6 analog outputs**, and there is no contradiction
with the six switches IC47A~F / six filters IC30-35 / six gates `G0-G5`.

The `64` on the latch output in Fig. 3 is **8 slots x 8 bits**, written one byte at a time
with `A0-A3` selecting the slot. That is exactly the eight registers `0x1F00-0x1F07`.

#### The routing byte is a 6-bit output mask `[C]`

`[C]` **Bit k of a slot's byte enables output k+1.** Established from the firmware's own
preset table (below): every byte is drawn from `00, 01, 02, 04, 08, 10, 20` (a single output)
or a combination such as `03` (outputs 1+2, the stereo MIX pair). No byte in the table sets
bit 6 or 7 -- all 400 bytes fit six bits, matching six outputs.

This is why single-part patches measure L/R identical at r = 0.999996: their slot carries
`0x03`, driving the MIX pair from one value.

#### The eight bytes are loaded as a block from a ROM table `[C]`

The CPU does not compute the routing. The routine at `0xB721` copies eight consecutive bytes
from a table at **`0xA8B6`**, indexed by a single byte in RAM at `0x280E`:

```
B721: ldbze 50, 280e        ; N = output-assign index
B726: mulub 50, #08         ; N * 8
B729: ldb   54, #01
B72C: stb   54, 1f08        ; enable
B731: clr   52
B733: ldb   54, a8b6[50]    ; preset table
B738: stb   54, 1f00[52]    ; -> slot register
B73D: incb 50 / incb 52 / cmpb 52,#08 / jlt b733
```

`[C]` **`N` is patch header byte `+0x0E`** — open question #5, now settled. Verified by
emulation: Ac.Piano has `+0x0E = 21`, and the driver logs preset 21 (`00 03 00 00 00 00 00 00`)
written to `0x1F00-0x1F07` at exactly `0xB73D`. The table holds **50 entries (N = 0..49)**,
`0xA8B6`-`0xAA45`; the 64 factory patches use only `{7, 12, 19, 21, 49}`.

| N | bytes | slots 0-7 → outputs | used by |
|---|---|---|---|
| 7 | `00 02 00 00 00 01 00 00` | – / 2 / – / – / – / 1 / – / – | Double A.P, Double E.P |
| 12 | `04 04 01 01 01 02 02 02` | 3 / 3 / 1 / 1 / 1 / 2 / 2 / 2 | |
| 19 | `10 20 01 01 02 02 04 08` | 5 / 6 / 1 / 1 / 2 / 2 / 3 / 4 | **Wide Piano** |
| 21 | `00 03 00 00 00 00 00 00` | – / **1,2** / – / – / – / – / – / – | most patches (mono → MIX) |
| 30 | `01 02 04 08 10 20 00 00` | 1 / 2 / 3 / 4 / 5 / 6 / – / – | the fully-multitimbral case |
| 49 | `03 03 04 04 08 08 10 20` | 1,2 / 1,2 / 3 / 3 / 4 / 4 / 5 / 6 | |

Entry 30 is the clearest statement of the architecture: six slots to six distinct outputs,
**slots 6 and 7 idle**. That is how 8 slots and 6 outputs coexist.

#### Two consequences for emulation `[C]`

- **Routing is per-slot, not per-voice** — but a part still reaches a *fixed* output, because
  the Output Mode partitions the voices into **groups** and the firmware allocates a note a
  voice from the group its part's Output Assign names (§5.4). There is no per-voice
  output-assignment register; the constraint lives in the allocator.
- **An output can be fed by several slots.** N=12 sends slots 2, 3 and 4 all to output 1, and
  N=49 sends slots 0 and 1 both to outputs 1+2. So an output **accumulates** its slots; a
  model that lets the last slot overwrite the output is wrong.

#### The DAC has no clock

The PCM54's pin list is 16 data bits plus analog and supply pins -- no latch, strobe or chip
select. It converts continuously, settling in **350 ns** on the current output (`IOUT`, which
is what IC23's I-V amp uses; the voltage output would be 3 µs). "When are the bits ready" is
answered by the **S/H switch**, not the converter: IC16 puts a slot's code on the bus and the
switch closes only after settling, so the code-change glitch never reaches the hold capacitor.
This is the deglitcher arrangement the PCM54 datasheet recommends in its own Figure 7/8.

`[C]` **Confirmed by measurement**: on real hardware, two outputs carrying the same tone
correlate at **1.000 with zero phase difference** and zero broadband lag. The multiplexing is
inaudible; it performs routing and nothing else. An emulation needs slot *routing* and slot
*accumulation*, but no slot *timing*.

### 5.2 The anti-aliasing filter `[S]`

Fig. 4 gives IC30-35 (uPC4570) as two active sections plus an output RC:

| Section | Components | Corner | Q |
|---|---|---|---|
| 1 | R39/R40 10k, C53 8200p, C52 680p | ~6.7 kHz | 1.74 |
| 2 | R41/R42 10k, C55 3300p, C54 560p | ~11.7 kHz | 1.21 |
| output | R78 10k, C56 2200p | ~7.2 kHz | 1 pole |

Fifth order overall. The service notes' own simulation plot gives `max: 2.17 dB`,
`min: -48.57 dB` -- flat with a slight peak, then a cliff.

`[C]` **Implemented in the driver** as two `FILTER_BIQUAD` sections plus a `FILTER_RC`, given
the schematic's component values directly. It removes 17-19 dB above 12 kHz — but it is flat
at 2.5-6.3 kHz, where the emulator is still 22-33 dB hot against hardware, so the filter was
never the cause of that gap. See `IMPLEMENTATION-PLAN.md` §4.7b.

`[C]` **Verified on hardware.** Service test 8 injects a square wave at the analog switches
(the CPU toggles `0x1F08`), and its harmonics come out on the ideal 1/h law to within 0.5 dB
up to 1437 Hz, with even harmonics absent at -52 dB. The output stage is flat well past
1.4 kHz, so it cannot account for harmonic differences in that region.

`[S]` The p.5 block diagram names the analog switch array **IC47A~F**, and shows the six
filtered outputs both going to the individual jacks and being summed by **IC38** through a
resistor network to the L/R mix and phones. (The chip-role table above lists IC24-26 for the
switches, from the schematic; the block diagram disagrees. IC47A~F is the more specific
label.)

IC26/IC6 takes `D0-D5` and `A0-A3` from the CPU bus and drives six gate outputs `G0-G5`
that steer the analog switches. Its **timing** comes from three lines out of IC16 —
`INH`, `MXA`, `MXD` (inhibit, multiplex address, multiplex data). So IC16 drives the
multiplex phase and IC26/IC6 performs the analog demultiplex and hold.

The CPU's only involvement is loading eight routing registers at `0x1F00-0x1F07` plus an
enable at `0x1F08`, as a group selected per patch. See `ROM-ANALYSIS.md` §3.1.

### 5.3 Selecting a patch, and what MIDI program change really does `[C]`

`[C]` **MIDI program change does not select a patch.** It selects a **part's TONE** — the 99
internal tones (`0 = A. Piano 1` … `98 = Drums`, listed in `reference/U-110.ins`, taken from
OM p.86). Sending one leaves the display on the same patch name with a **`TEMP:`** prefix,
because the patch has become an edited temporary copy with one part's tone replaced. Patches
are **panel-only**: `[INC]` / `[DEC]` from the play screen.

Three facts an automated run has to respect:

- The current patch number is in **battery-backed RAM**, saved by MAME on exit. Successive
  runs therefore start wherever the previous one finished.
- `[INC]`/`[DEC]` **wrap modulo 64**, so no number of presses can home the selection —
  70 × `[DEC]` from P-04 lands on P-62, not P-01. A known starting patch is the only way to
  address one absolutely.
- Start from a scratch NVRAM directory (`-nvram_directory`) to get a deterministic P-01.

`tools/select_patch.lua` does this (`U110_PATCH=4 mame u110 -nvram_directory /tmp/nv
-autoboot_script tools/select_patch.lua`). Two MAME Lua traps it documents: the handle from
`add_machine_frame_notifier` **must be kept alive** or the callback is silently unsubscribed
with no error, and the key state must be **re-asserted every frame** — releasing once and
returning early leaves the firmware seeing a held key, which auto-repeats.

`[C]` **This confirmed the routing chain end-to-end.** Selecting P-04 Wide Piano makes the
firmware write `10 20 01 01 02 02 04 08` to `0x1F00`-`0x1F07` — exactly preset **19**, which
is what that patch's header byte `+0x0E` predicts. Patch byte -> preset table at `0xA8B6` ->
slot registers, verified in emulation rather than by reading code. Wide Piano also brings all
six parts live on one MIDI channel (`.1.1.1.1.1.1`), key-split into six zones at notes 36, 48,
60, 72 and 84, each with a distinct `+0x0B` value (`00, 80, 40, 60, A0, 20`) — the per-part
pan that gives the patch its name.

### 5.4 Output assignment — **solved from the Owner's Manual** `[C]` `[S]`

`[S]` OM p.5 "Patch Setting Chart (Factory Preset)" gives, for every factory patch, each
part's **Tone Name / Output Assign / MIDI Channel / Key Range**, plus an **Output Mode**
column. For P-04 Wide Piano: six parts, all A.PIANO 2, key ranges `C-1..B1`, `C2..B2`,
`C3..B3`, `C4..B4`, `C5..B5`, `C6..G9`, and Output Assigns **1, 5, 3, 4, 6, 2**.

`[C]` **Two patch fields decoded against that chart:**

| field | meaning | check |
|---|---|---|
| part byte `+0x0B` **>> 5** | **Output Assign**, 0-5 → outputs 1-6 | Wide Piano `00 80 40 60 A0 20` >> 5 = `0 4 2 3 5 1` → **1, 5, 3, 4, 6, 2** — matches the chart exactly |
| patch byte `+0x0E` **+ 1** | **Output Mode** | Ac.Piano 21→**22**, Wide Piano 19→**20**, Double A.P 7→**8**, all matching the chart |

So the assignment is **per part, fixed, and readable straight from the patch data**. Ordered by
ascending key range, Wide Piano's outputs run 1, 5, 3, 4, 6, 2 — a deliberate spread, which is
what the patch is named for.

#### Correction: an earlier reading here was wrong `[C]`

This section previously concluded that a note's output followed from whichever voice the
allocator happened to pick, so the same key would pan differently on successive presses.
**That is wrong** — a U-110 owner confirms the panning is fixed and does not move at all, and
the chart above shows why: the assignment is a property of the part, not of the voice.

The error came from a test that could not have detected the truth. I compared registers across
three plays of the *same note* on different voices, found them byte-identical, and read that as
"no output field exists". But an assignment that is **per part** is identical across plays of
the same note *by definition*, so that comparison excludes nothing. Testing across *parts* was
the discriminating experiment, and it was the one I had already run and set aside.

#### The mechanism: Output Modes are **voice groups** `[S]` `[C]`

`[S]` OM p.35 "Output Modes" lists **50 modes** — exactly the size of the preset table at
`0xA8B6`, with **mode = table index + 1**. Each row gives six "Voice Group" columns holding
**voice counts**, summing to 31:

| mode | group sizes | |
|---|---|---|
| 1 | 31 | one group, everything to output 1 |
| 20 | 7, 8, 4, 4, 4, 4 | **Wide Piano** |
| 21 | `<L31> <R31>` | |
| 22 | `M31` | **Ac.Piano** and most single-part patches |
| 50 | `M8`, 7, 8, 4, 4 | |

So a mode **partitions the 31 voices into contiguous blocks**, and **Output Assign N selects
group N**. The firmware allocates a note a free voice *from that group*.

`[C]` **Verified against measurement.** Mode 20 gives groups `1-7 | 8-15 | 16-19 | 20-23 |
24-27 | 28-31`. Every one of Wide Piano's six parts landed in the group its Output Assign
names:

| part | assign | group | observed voices |
|---|---|---|---|
| 1 | 1 | 1-7 | 1, 2 |
| 6 | 2 | 8-15 | 8, 9 |
| 3 | 3 | 16-19 | 16, 17 |
| 4 | 4 | 20-23 | 20, 21 |
| 2 | 5 | 24-27 | 24, 25 |
| 5 | 6 | 28-31 | 28, 29 |

Voices are numbered **1-31**; voice 0 was never allocated in any run.

`[C]` This also explains the earlier confusion. Replaying one note took voices 1,2 → 3,4 →
5,6 → 1,7 — **all inside group 1**. Allocation is round-robin *within the group*, so the
output never changes and the pan is fixed, exactly as a U-110 owner reports. Ac.Piano is
mode 22 = `M31`: a single group of all 31 voices, mono and centred, which is why it draws
voices 1-6 freely and why its chart row shows only part 1.

`[C]` **The table is transcribed and validated** in `tools/output_modes.py`. The OCR of the
scanned page drops cells, but every row must partition **exactly 31 voices**, and that
invariant recovers them: all 50 rows check out, and mode 10 (`15, 8, 8`) was reconstructed
from the constraint alone where the scan showed only `15`. Mode 20 reproduces the six Wide
Piano groups exactly as measured, and mode 22 (`M31`) explains Ac.Piano drawing voices 1-6
freely from a single 31-voice pool.

`[C]` **The mode is recoverable at run time.** All 50 presets in the table at `0xA8B6` are
**distinct**, so a driver can identify the current Output Mode by matching the eight bytes the
firmware writes to `0x1F00`-`0x1F07` — no need to read the patch or peek at RAM `0x280E`.

`[I]` **One chart discrepancy.** OM p.5 gives P-17 12str A.G an Output Mode of **21**; all
three firmware images (v2.00, v2.03 and the `15179960` dump) hold `+0x0E = 21`, i.e. mode
**22**, and the running machine writes preset `00 03 00 00 00 00 00 00` to prove it. Mode 21
is `<L31><R31>` (wet stereo) and 22 is `M31` (dry centre), so the chart's value is the more
musical one for a 12-string — but the ROM is what the machine does, and it says 22.

`[I]` The footnote to the table: *"In the Output modes 21 to 50, Multi Outputs 1 and 2 are
regarded as the same Voice Group, and effect can be turned on or off. The one without effect
(M) is set to the center position of the sound imaging, and the one with effect is stereo
output (L and R)."* So `M` groups are centred and dry, `L`/`R` groups are the wet stereo pair —
which is where IC17's effect RAM enters, still unmodelled.

`[I]` How the eight bytes at `0x1F00`-`0x1F07` encode a group partition is only partly worked
out. For mode 20 they read as eight voice-quartets with the slot for voice *v* at register
`((v >> 2) + 2) mod 8`, which reproduces the group → output mapping for all 31 voices. Mode 22
does not fit that rule (its single non-zero byte would leave the voices it actually uses
unrouted), so the effect modes evidently route differently. **Not needed for emulation:** the
mode table plus Output Assign gives the part → output mapping directly.

---

## 6. Panel, display and MIDI

| Function | Path |
|---|---|
| LCD (16x2) | IC8 LCD interface; `0x1100` command, `0x1102` data — `A1` is the controller's `RS` line. IC8 drives `LCD E`/`R/W`/`RS`, takes `BUSY` back, and raises **`LCD INT`** on completion. That line reaches the CPU as **HSI.0** (vector `0x2008`); its handler at `0x4032` drains a 32-entry text ring at `0x2700`. Starve it and the firmware wedges at `0xD2F6` — see §6.2 |
| LCD contrast | CPU `PWM` pin → IC4 integration circuit |
| Panel LEDs | `0x1200`, written **inverted** by the firmware (`NOT RW30` then `ST RW30,0x1200`). Only D0/D1 are used — see §6.1 |
| EDIT/EXIT and PART/JUMP LEDs | IC5 HC74, clocked by `LED WR` from IC8 **pin 72** |
| Switches | Six panel switches on IC8 `READ0`-`READ5`, **pins 64-69**; read as one byte at `0x1300` — see §6.1 |
| MIDI IN | JK1 → IC2 opto-isolator → CPU `RXD`, pin 18 |
| MIDI OUT | CPU `TXD`, pin 17.  Emulated: serialised to a `midi_port` at 31250 baud 8N1. The firmware transmits nothing unprompted -- SysEx replies and bulk dumps only |
| MIDI THRU | JK3, buffered off the **IC2 opto-isolator output** -- the same node that feeds the CPU `RXD`.  A wire, not a feature: no CPU involvement, no deserialising, so THRU repeats MIDI IN bit for bit and keeps working while the firmware is busy or halted.  Confirmed against the service manual; an earlier revision of this table wrongly put THRU on `TXD` alongside OUT, which would have made it carry OUT's data instead of IN's.  Emulated |
| MIDI activity LED | CPU `P2.6`, pin 33, via a 2SA1115 driver. **Active LOW**: 2SA is the JIS prefix for a PNP transistor, which conducts when its base is pulled low, so `P2.6` = 0 lights the lamp. Observed on the emulated panel and confirmed by the part number. The two panel LEDs at `0x1200` are active low too, inverted by the firmware — the machine drives every lamp this way |

### 6.1 Panel switches and LEDs `[S]` `[C]`

**Six switches, no matrix.** They sit on IC8's `READ0`-`READ5` inputs (pins 64, 65, 66, 67,
68, 69) and appear as one byte at `0x1300`:

| Bit | IC8 pin | Button |
|---|---|---|
| 0 | 64 (`READ0`) | PART / JUMP |
| 1 | 65 (`READ1`) | EDIT / EXIT |
| 2 | 66 (`READ2`) | LEFT |
| 3 | 67 (`READ3`) | RIGHT |
| 4 | 68 (`READ4`) | DEC |
| 5 | 69 (`READ5`) | INC / ENTER |

Each is pulled up and grounded when pressed, so the sense is **active low**. The firmware
agrees: its edge detector at `0x4118` keeps `~raw` as "previously pressed", which only works
if a clear bit means pressed.

```asm
4118  LDB  R31,0x1300      ; raw read
411D  ANDB R30,R31,RD4     ; raw & prev_pressed   -> RELEASE events, accumulated in RD6
4127  ORB  R30,RD4
412A  NOTB R30             ; ~(raw | prev)        -> PRESS   events, accumulated in RD5
4131  LDB  RDA,#0x04       ; 4-tick debounce, armed on any edge
4137  NOTB R31
4139  LDB  RD4,R31         ; keep inverted state as "previously pressed"
```

> **A note on Fig. 1-a.** IC8's block diagram shows a 2-to-4 decoder driven by `A0`/`A1`
> producing `SW SELECT 0`-`3`, which reads like a 4x8 matrix. It is not used as one here:
> the firmware reads `0x1300` and nothing else, so only one column is ever selected. The
> decoder is presumably a general IC8 facility that other Roland machines using this gate
> array exercise. `[I]`

**Two LEDs.** IC5 (HC74 dual D flip-flop) latches `D0` and `D1` on IC8's `LED WR` strobe
(IC8 **pin 72**): `D0` = PART/JUMP, `D1` = EDIT/EXIT. Since the firmware inverts before
storing, a **clear** bit lights the lamp. Confirmed in emulation: entering edit mode writes
`0xFFFD` — `NOT 0x0002` — lighting EDIT/EXIT alone.

---

### 6.2 The LCD interrupt path `[S]` `[C]`

IC8's Fig. 1-a block diagram shows the `LCD INTERFACE` block taking `A1`, `AD0`-`AD7`, `ALE`,
`LCD CS`, `WR` and `RST`, driving `LCD D0`-`D7` plus `E`, `R/W`, `RS`, and producing two
outputs of its own: it consumes the controller's **`BUSY`** flag and emits **`INT`**, which
leaves the chip as the `LCD INT` pin.

So IC8 runs the whole LCD transaction autonomously — strobe, busy-poll, done — and
interrupts the CPU when the controller is ready for the next byte. The firmware exploits
this fully: it never polls the display. It pushes characters into a 32-entry ring buffer at
`0x2700` and lets the HSI.0 handler at `0x4032` drain one per interrupt.

The dependency is hard. With `LCD INT` absent the ring fills after 32 characters and the
producer spins forever:

```asm
D2F6  ADDB R51,RCA,#0x01   ; head + 1
D2FA  ANDB R51,#0x1F       ; wrap to 32 entries
D2FD  CMPB R51,0x00CB      ; == tail ?
D302  JE   0xD2F6          ; buffer full -> wait, forever
```

`[C]` This is exactly where the machine hangs if the interrupt is not modelled — the boot
splash reaches `LCD cmd 0x80` (set cursor to position 0) and stops. Note the interrupt is
enabled through `INT_MASK` bit 4 while `IOC1` bit 1 stays **clear**, so `EXTINT` remains
available to IC15 at the same time; the two are independent sources and both are in use.

---

MIDI baud comes from the CPU's own serial port: `BAUD_RATE` is loaded with `0x8005`,
divisor 6, giving `12 MHz / (64 x 6)` = **31250 baud** exactly.

---

## 7. Open hardware questions

| # | Question | Why it matters |
|---|---|---|
| 1 | ~~Wave ROM address bits A14-A18~~ | **SOLVED** via MAME (§4.2, §4.6). The full 19-bit permutation is known. |
| 2 | What the three series-resistored lines between IC15 and IC16 carry | Series resistors suggest edge-rate control on something fast — a clock and strobes `[I]`. Not blocking |
| 3 | Exact `/CS1`, `/CS2`, `/CS4`, `/CS5` assignments | The firmware never touches them; harmless, but the map is incomplete. `/CS6` is resolved: it covers `0x1F00-0x1FFF`, the output control chip |
| 4 | What `PORT1` bit 4 drives | Set and cleared around card operations (`0x6AFE`, `0xB759`, `0xC0EC`) with no obvious purpose. Still open under emulation — the machine mounts cards correctly with it ignored |
| 5 | ~~Field meaning of the eight output-routing bytes~~ | **SOLVED** `[C]` — each byte is a 6-bit output mask (bit k → output k+1); all eight are block-copied from the 50-entry preset table at `0xA8B6` by the routine at `0xB721`, indexed by patch header byte `+0x0E`. See §5.1 |
| 6 | Which LCD controller the panel actually uses | Unidentified, and no CGROM dump exists. The emulator substitutes a synthesised font purely for legibility |
