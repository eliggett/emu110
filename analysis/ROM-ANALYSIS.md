# Roland U-110 Firmware ROM — Analysis Notes

Subject files:

| File | Size | MD5 |
|---|---|---|
| `U110v200.BIN` | 65,536 | `514ec754d47f8d27a7b2815a4e2ba0ff` |
| `U110v203.BIN` | 65,536 | `90e8aa77fac93f96704607a09ad48307` |

`chip.txt` records the carrier: **Fujitsu MBM27C512, DIP-28** — a 64K x 8 EPROM.

All findings below were derived from the images themselves using Ghidra 12.1.2, then
cross-checked against the factory service notes. Offsets are file offsets, which equal
CPU addresses. Analysis was done against **v2.03** unless noted; v2.00 matches
structurally.

**Companion document:** [`SYSTEM-DESIGN.md`](SYSTEM-DESIGN.md) covers the hardware —
board topology, IC roles, address decode, the wave subsystem wiring and the audio path.
Where the two overlap, that file is authoritative on *hardware* and this one on *what the
code does*. A **corrections log** for both is in §9 below.

> **Confidence key** — `[C]` confirmed from the image, `[S]` confirmed against the
> service notes / schematic, `[I]` inferred and still open.

### Terminology

These two are easy to conflate and this document keeps them strictly separate:

| Term | Meaning | Where it lives |
|---|---|---|
| **Tone** | One of the raw sampled sounds. **99 internal**, plus whatever a fitted card adds. Selected by a 1-byte number. Tones **do** have 10-character names — see §6.6. | Name and parameters in the **wave ROM / card** (§6.6). No tone data of any kind is in the program EPROM. |
| **Part** | One of **six** slots inside a patch. Names a tone, a MIDI channel, and per-part settings. | 16-byte record inside a patch (§4) |
| **Patch** | One of **64** memory locations. Six parts plus output assignment and chorus/tremolo settings. | 128-byte record at `0xE000` (§4) |

`[C]` **99 internal tones — confirmed in firmware.** The default-table builder at `0x8494`
reduces modulo 99:

```asm
84a3  CMPB R54,#0x62      ; 98?
84a6  JNH  0x84AD
84a8  SUBB R54,#0x63      ; subtract 99 and retry  -> value mod 99
84ab  SJMP 0x84A3
```

**Factory patch names look like tone names, and are not.** Patch 0 is `'Ac.Piano  '` and
patch 47 is `'Shakuhachi'` because a single-part factory patch is naturally named after
the tone it uses. The rest of the list settles the question — nothing that is one raw
sample is called `'Multi-Set1'`..`'Multi-Set5'` (patches 59-63), `'Brs + Str '`,
`'5th Br+Str'`, `'Choir+Str '` (layers), or `'Guit>Piano'`, `'Trump>Sax '`, `'Sax / Tp  '`
(splits). Those are multi-part constructions, which is exactly what a patch is.

---

## 1. Target CPU: Intel 8097BH `[S]`

Not a 6303, 8051, Z80 or 68k. The image carries the MCS-96 boot furniture exactly
where the silicon expects it:

| Evidence | Address | Meaning |
|---|---|---|
| 9-entry vector table | `0x2000`–`0x2011` | MCS-96 interrupt vectors |
| Chip Configuration Byte = `0xFB` | `0x2018` | MCS-96-specific CCB slot |
| `FA E7 7C 1F` → `DI ; LJMP 0x4000` | `0x2080` | MCS-96 reset entry point |
| `F2 … F3 F0` → `PUSHF … POPF ; RET` | `0x4004`+ | MCS-96 ISR prologue/epilogue |
| Operands resolve to `INT_MASK`(08), `INT_PEND`(09), `SP_STAT`(11), `SBUF`(07), `IOS1`(16) | throughout | Register-file layout matches |

Following only the reset vector and the nine interrupt vectors, Ghidra recovered
**8,834 instructions across 148 functions with no bad-instruction fallout.** A wrong
architecture does not produce that.

**Exact part: Intel 8097BH `[S]`** — confirmed from the schematic. The image was
already consistent with the NMOS 8x9x generation rather than an 80C196KB, since only
9 vectors are populated and `0x2030`+ is blank (the 80C196KB has an extended vector
block there).

> **Correction.** An earlier revision of this document inferred an *8098-class 48-pin*
> part, reasoning that a single 64K x 8 EPROM implied an 8-bit external bus. That was
> wrong. The 8097BH is the 68-pin device with a full 16-bit multiplexed bus, and the
> schematic shows all of **AD0-AD15** running to the system bus.

**Reconciling the bus width.** A 16-bit-bus CPU and a byte-wide EPROM coexist because
MCS-96 samples `BUSWIDTH` per cycle, and in this design IC8 *generates* that signal — so
memory cycles run 8-bit and tone-generator cycles run 16-bit. See `SYSTEM-DESIGN.md` §2.2.

### Interrupt vector table

| Vector | Source | Target | Handler |
|---|---|---|---|
| `0x2000` | Timer overflow | `0x4004` | → `0x4028` (ack + return) |
| `0x2002` | A/D done | `0x4008` | → `0x4030` (bare return — A/D is **polled**, not interrupt-driven) |
| `0x2004` | HSI data avail | `0x400C` | → `0x4028` |
| `0x2006` | High-speed out | `0x4010` | → `0x4028` |
| `0x2008` | HSI.0 | `0x4014` | → `0x4032` — **LCD queue drain** |
| `0x200A` | Software timer | `0x4018` | → `0x407C` |
| `0x200C` | Serial port | `0x401C` | → `0x4154` — **MIDI IN/OUT** (pins 17/18) `[S]` |
| `0x200E` | External INT | `0x4020` | → `0x41BB` (v2.00; byte at `0x4022` differs in v2.03) |
| `0x2010` | Trap | `0x4024` | → `0x4028` |

### 1.1 On-chip peripherals the firmware uses

Board-level wiring for these is in `SYSTEM-DESIGN.md`; this table records what the *code*
does with each.

| Peripheral | SFR / pin | Use | Evidence |
|---|---|---|---|
| Serial port | `SBUF`(07), `SP_STAT`(11), pins 17/18 | MIDI IN/OUT | ISR `0x4154`, §5 |
| `PORT2` bit 6 | pin 33, P2.6 | **MIDI activity LED** | `ORB PORT2,#0x40` / `ANDB PORT2,#0xBF` at `0x4107`,`0x410C`,`0x8788`,`0x878D` |
| `PORT2` bit 7 | pin 38 → IC8 pin 74 | **BANK SELECT** — swaps EPROM/SRAM at `0xE000-0xFFFF`, see §2.1 | `0x4390`, `0x8479`, `0x847F`, `0x8572` |
| `PORT1` bits 0-3 | — | **cartridge presence**, active low | `ANDB R30,PORT1,0x9042[slot]`, §6.3 |
| `PORT1` bit 4 | — | output; set/cleared around card operations `[I]` | `0x6AFE`, `0xB759`, `0xC0EC` |
| HSO software timers 0/1/2 | `HSO_COMMAND`(06)=`0x18`/`0x19`/`0x1A`, `HSO_TIME`(04) | all periodic ticks | `0x408E`, `0x40A0`, `0x4145` |
| `TIMER1` | (0A) | time base for the above | scheduled as `ADD HSO_TIME,TIMER1,#n` |
| A/D converter | `AD_COMMAND`(02), `AD_RESULT_HI`(03) | **battery voltage**, channel 0, **polled**. Accepts `AD_RESULT_HI` in **`0x85`-`0xCB`**; outside that it sets the flag behind `Check Battery!` | `LDB AD_COMMAND,#0x08` (ch 0 + GO) then `LDB R30,AD_RESULT_HI` at `0x44D7`/`0x44E0` and `0x880D`/`0x8816`; window from `CMPB #0xCB`/`JH` and `CMPB #0x84`/`JNH` at `0x44E3`-`0x44EB` |
| Baud rate | `BAUD_RATE`(0E) | `0x8005` — two-byte load | `LDB BAUD_RATE,#0x05` then `#0x80` at `0x4384`/`0x4387` |
| Serial control | `SP_CON`(11) = `0x09` | mode 1, 8-bit, receive enabled | `0x438A` |
| `HSI_MODE`(03) = `0x04` | — | HSI channel config | `0x437B` |
| `IOC0`(15) = `0x00`, `IOC1`(16) = `0x21` | — | `IOC1` bit 0 enables **PWM** (LCD contrast via IC4), bit 5 enables **TXD** (MIDI OUT). Bit 1 is **clear**, so EXTINT is taken from the EXTINT pin, not ACH7 | `0x437E`, `0x4381` |
| `INT_MASK`(08) = `0xF8` | — | enables HSO, **HSI.0**, software timer, serial, EXTINT; masks timer overflow, A/D and HSI-data | `0x4530`, re-set at `0x449A` |

#### Reading the listing: Ghidra names SFRs by their *read* function

Several MCS-96 SFRs are different registers depending on direction, and Ghidra's module
only ever shows the read-side name. **Every one of these is a write in this firmware**,
so the label is misleading:

| SFR | Ghidra shows | On a write it is really |
|---|---|---|
| `0x02` | `AD_resultlo` | `AD_COMMAND` |
| `0x03` | `AD_resulthi` | `HSI_MODE` |
| `0x04`/`0x05` | `HSI_time` | `HSO_TIME` |
| `0x06` | `HSI_status` | `HSO_COMMAND` |
| `0x0E` | `PORT0` | `BAUD_RATE` |
| `0x11` | `SP_STAT` | `SP_CON` |
| `0x15`/`0x16` | `IOS0`/`IOS1` | `IOC0`/`IOC1` |

`HSO_COMMAND` values `0x18`, `0x19`, `0x1A` select software timers 0, 1 and 2 with
interrupt enabled.

> **This caused a real error.** An earlier revision of this document claimed the A/D was
> unused, based on grepping for the names `AD_COMMAND`/`AD_RESULT` — which Ghidra's
> MCS-96 spec does not define — and on the A/D-done vector being a bare `RET`. Both were
> misleading. The A/D is used in **polled** mode, which is exactly *why* the interrupt
> vector is empty.

`[S]` **Crystal frequency: 12 MHz — confirmed.** The baud register is loaded with
`0x8005`, i.e. divisor `B+1 = 6`. For MCS-96 serial mode 1, `baud = XTAL / (64 x (B+1))`,
so MIDI's 31250 baud implies `XTAL = 31250 x 64 x 6 = 12.0 MHz`. The service notes
schematic labels **X1 = 12 MHz** on the CPU, matching exactly. (IC15 has its own
oscillator, **X2 = 34.816 MHz**.)

---


## 2. The address space as the firmware sees it

The decode itself — which IC8 chip select covers what, and why — is documented in
`SYSTEM-DESIGN.md` §2. Roland publishes the map in the service notes (p.6, Fig. 1-b), and
it confirmed two things this document had derived from code alone and carried as `[I]`:
the EPROM window at `0x2000-0x20FF`, and work RAM beginning at `0x2100`.

One structural fact matters enough to restate here, because it changes how several
sections read.

### 2.1 `0xE000-0xFFFF` is bank-switched on P2.7 `[C]` `[S]`

The firmware proves it in six instructions. `0x8475` copies a region **onto itself**,
toggling `BANK SELECT` between the read and the write:

```asm
8475  LD   RW52,#0xE000
8479  ORB  PORT2,#0x80      ; BANK SELECT = 1  -> EPROM visible at 0xE000
847c  LD   RW50,[RW52]      ;   read the factory default
847f  ANDB PORT2,#0x7F      ; BANK SELECT = 0  -> SRAM visible at 0xE000
8482  ST   RW50,[RW52]+     ;   write it to the SAME address
8485  CMP  ZR,0x52          ; until the pointer wraps past 0xFFFF
848a  JNE  0x8479
```

Source and destination addresses are identical; only the bank line differs. So:

- **P2.7 = 1** → `0xE000-0xFFFF` reads the EPROM's **factory default patch set**
- **P2.7 = 0** → `0xE000-0xFFFF` is the **battery-backed user patch SRAM** (IC11)

That routine is the *"Mem Initialized"* factory reset, reachable from `0x8557`.

`ORB PORT2,#0x80` occurs **exactly once in the entire ROM** — right there at `0x8479`.
Every other access to `0xE000-0xFFFF` therefore runs with the bank line low and touches
**user patch RAM, not the EPROM**. Reset clears it early (`0x4390`) and it stays clear.

### 2.2 The map

```
                                                              owner
0x0000-0x00FF   8097BH internal register file            [C]   CPU (shadows /CS7)
0x0100-0x0FFF   /CS7 - EPROM, decoded but BLANK          [S]   IC9   <-- usable, §8
0x1000-0x10FF   /CS1, /CS2                               [S]   IC8
0x1100          LCD CS  - command register (write-only)  [C][S] IC8
0x1102          LCD CS  - data register    (write-only)  [C][S] IC8
0x1200          LED CS  - 16-bit latch, written INVERTED [C][S] IC8
                (`NOT RW30` then `ST RW30,0x1200`) - panel LED drive
0x1300          SW SCAN CS - switch input  (read-only)   [C][S] IC8
0x1400-0x1BFF   /CS3 - TONE GENERATOR window  <- §3      [C][S] IC15 MB87419
                (firmware only ever touches 0x1400-0x143F; 16-bit port)
   0x1402         data-in port for wave ROM / cartridge reads  <- §6.1
   0x1404         bank + card/internal select                  <- §6.2
0x1C00-0x1EFF   /CS4                                     [S]   IC8
0x1F00-0x1F07   /CS5 - 8 output-routing registers        [C][S] IC26 BU3905S
0x1F08          /CS5 - output-routing enable / latch     [C][S] IC26 BU3905S
0x1FFF          /CS6                                     [S]   IC8
0x2000-0x20FF   /CS7 - vectors, CCB, reset stub          [C][S] IC9  (EPROM)
0x2100-0x3FFF   /CS8 - WORK / BACK-UP RAM                [C][S] IC10 (SRAM, 8K)
   0x2100-0x21FF  MIDI IN ring buffer  (256 B)                 <- §5
   0x2200-0x22FF  MIDI OUT ring buffer (256 B)                 <- §5
   0x2720         LCD text queue
   0x2743-0x2746  cached cartridge ID per slot; 0x00 = empty,   <- §6.5
                  0xFF = mount failed
   0x274A         current patch number
   ~0x27B0-0x27FF STACK - SP initialised to 0x2800, grows DOWN
   0x2800-0x287F  ACTIVE PATCH edit buffer, 128 B              <- §4
   0x2880+0x50*n  tone parameter records copied from card      <- §6.6
   0x3600,0x3620,0x3660,0x3680,0x3700,0x3720,0x3850  per-voice arrays
   0x3C00-0x3C07  global/system settings
0x4000-0xD6C6   /CS7 - PROGRAM CODE                     [C][S] IC9  (EPROM)
0xD6C7-0xDFFF   /CS7 - unused, 0xFF fill                [C][S] IC9  (EPROM)   <-- usable
0xE000-0xFFFF   BANK SWITCHED on P2.7  <- §2.1           [C][S]
                  P2.7=0 (normal): /CS9  64 USER patches       IC11 (SRAM, 8K)
                  P2.7=1 (init):   /CS7  factory defaults      IC9  (EPROM)
```

> **Stack warning for patch authors.** `SP` is initialised to `0x2800` at `0x436B` and
> grows down, while live data reaches up to about `0x27A8`. That leaves on the order of
> **80 bytes of stack headroom**. Patch code that pushes freely, or that calls deeply,
> will quietly corrupt the data below it.

> **A methodology limit worth remembering.** The claim "no write targets `0x20xx`", which
> originally anchored the split decode, was derived by scanning instructions with a
> **direct address operand**. It is blind to the 32 stores in this ROM that go through a
> pointer (`ST Rxx,[RWyy]`) — including the bank-switched write at `0x8482`. The
> conclusion happened to be right (the service notes later confirmed it), but the evidence
> was weaker than it looked. A pointer-aware dataflow pass would be the sound version.

> **A scanning trap.** `LD RW52,#0x1000` and `LD RW52,#0x1010` (`0xD4EA`, `0xD4F4`) look
> like I/O accesses but are not — they are packed arguments to the string-print routine
> at `0xD3A0`, encoding cursor position 0 and 16 of the 16x2 display.

## 3a. The byte is a DELTA, and how the ping-pong loop welds `[C]`

`[C]` **The format from §3 is right; what it means is not.** Sign, 3-bit exponent, 4-bit
mantissa, the 1-3-4 rule, full scale +/-1984 -- all correct. But the decoded number is a
**first difference**, and the chip integrates it.

`[C]` **Why §3 could not have caught it.** §3 rests on wave-ROM sample 212, the Sound
Check tone, which decodes to a clean sine. The derivative of a sine is a sine, so that one
signal cannot distinguish a waveform from its derivative. The test had no discriminating
power, which is also why the result sounded convincing.

`[C]` **The evidence.** Dividing a hardware capture by a dry render of the same ROM data
gives a clean **-5.46 dB/octave from 120 Hz to 12 kHz**, twelve tones agreeing, against
-6.02 for an ideal integrator (`tools/derive_deemphasis.py`, `analysis/deemphasis.pdf`).
The integration *is* the de-emphasis; no separate filter is needed.

`[C]` **The loop points are zero crossings of the integral.** This is the fact that unlocked
the ping-pong, and it only makes sense under the delta reading:

| | at `loop` | at `end` | waveform swing |
|---|---|---|---|
| sample 121 (Strings 1, ref 65) | **-1** | **-1** | +/-800 |
| sample 122 (Strings 1, ref 78) | **-15** | **-15** | +/-800 |

Roland placed both turning points where the integrated waveform crosses zero. At a zero
crossing an *inverted* reflection is continuous -- the waveform carries straight on through
zero instead of bouncing off it.

`[C]` **The welding rules**, each one established by measurement:

1. **Delta sign follows direction.** Forward, add the delta at the byte arrived at;
   backward, subtract the delta at the byte being left. The accumulator then retraces
   exactly, so the loop is a true loop and the amplitude never jumps.
2. **`end` is inclusive.** The zero crossing at the top is at `end`, not `end - 1`. Turning
   one byte early lands on +151 instead of -1 and the join jumps by 402.
3. **The endpoint is played once**, not repeated. Repeating it inserts a zero-difference
   step in the middle of a steep slope -- itself an audible corner, and the source of a
   512-unit "curvature" that was misattributed to the data for a long time.
4. **The reverse pass is inverted, reflected about the accumulator's value AT THE TURN**,
   not about zero. With `out = s*acc + o`, continuity at a turn where `acc = c` requires
   `o_new = o_old + 2*s_old*c`, `s_new = -s_old`. The loop regions integrate to exactly
   zero but the attack does not (it leaves -1 and -15), so `c` is not zero: reflecting
   about zero leaves a kink of `2c` at every join and makes the DC alternate at the
   traverse rate.
5. **No leak.** Drift per full cycle is exactly **+0.0** -- a ping-pong path traverses every
   delta once forward and once backward, so the sum is identically zero whatever the data.
   Sweeping a leak makes things worse: the turn value goes from +15 at 0 Hz to +62 at
   20 Hz and the kink from 30 to 126.

`[C]` With all five, curvature at the pivot is **0** (both turns, both samples) against
5.2-7.8x the typical curvature before. `tools/stitch_pingpong.py` demonstrates the whole
scheme offline, with no emulator involved.

`[C]` **One off-by-one in the port.** The device fetched the sample *before* testing for the
turn, which took the address one byte past the turning point -- outside the sample -- and
applied that byte's delta, so the value reflected about was wrong. Small per turn, but it
accumulates: measured against the offline reference the residual went 0.30 before the first
turn, 0.69 after it, 0.94 by the third. Folding before the fetch cuts the whole-note
residual from 0.58 to 0.21 (sample 121) and 0.73 to 0.37 (sample 122).

`[C]` **The second off-by-one: the reflection was done in the wrong space.** After the fix
above the emulator still tracked the offline reference without matching it, and the error
still stepped up at every turn. The cause is that `sample_interpolate(a, b, f)` computes
`a*(1-f) + b*f`, and after a fetch at byte `b` the *other* tap is whichever byte was fetched
before it -- `w[b-1]` running forward, `w[b+1]` running backward. So the output position is
`addr - 1` forward but `addr` backward:

| direction | smpl_cur | smpl_nxt | expression | output position |
|---|---|---|---|---|
| forward  | `w[b-1]` | `w[b]` | `interp(cur, nxt, f)` | **addr - 1** |
| backward | `w[b+1]` | `w[b]` | `interp(nxt, cur, f)` | **addr** |

A constant one-byte lag is inaudible in a forward loop. At a ping-pong turn it *changes
sign*. Reflecting the address about the pivot, `addr' = 2*p - addr`, therefore reflects the
OUTPUT about `p - 0.5`; and at the `lo` turn the output position does not even reverse --
with step 0.7 it runs `lo+0.4, lo-0.7, lo+0.0, lo+0.7`, descending through the pivot before
climbing again. Reflecting the output position instead, `q' = 2*p - q`, is

    addr' = 2*p + 0x4000 - addr

at both turns. Against an ideal continuous-path integrator, a bit-exact transcription of the
device loop goes from **-13.1 dB** (sample 121) and **-6.2 dB** (122) to **exactly zero
residual**, over four full ping-pong cycles at the quantised step the firmware actually
writes. The built emulator then matches that transcription to **-48.2 dB** and **-43.7 dB**,
the floor being the analog filter chain and the fixed-point rounding.

`[C]` **Every byte on the path has to be integrated, including bytes never output.** The
device fetched at most one byte per output sample. For ordinary PCM a skipped byte is just
aliasing; for delta data the skipped delta is never applied, the loop stops summing to zero
and the accumulator ramps. Measured on Choir 3 at note 67 (step `0x42CF` = 1.044
bytes/sample) the DC walked to -6468 over a 15 s hold while the hardware held +/-1. The
forward-loop wrap had the same fault, in a way that hid it: `reachedEnd` wraps *after* the
read, and below one byte per sample the byte index lands exactly on `end`, where
`W[end] == W[loop]` makes the wrap seamless -- only above one byte per sample does it
overshoot and apply a delta from beyond the loop. 8 of 228 voices in the reference capture
exceed one byte per sample, the worst at 1.880. Both now walk byte by byte and fold before
the read; ping-pong output below one byte per sample is bit-identical to before.

`[C]` **Every real loop is DC-balanced.** Over one loop traversal the deltas sum to exactly
zero for all 226 wave ROM samples -- 30 ping-pong, 26 one-shot, 170 forward. This is what
makes a leak-free integrator safe, and it is strong independent confirmation of the delta
reading. The 12 table entries that appear to violate it are the ones with
`looplen > length`, which is impossible for a real loop: entries 214-221 are 127 bytes of
`0x80` padding, and 222-225 are **demo song data**, whose bytes read as ASCII -- "T-Jazz #1",
"Swing High", "Cloud 9", "NoOne Home". The sample table's tail points at the demo sequences,
not at audio.

## 3. Driving the tone generator

**No PCM sample data lives in this ROM**, and the CPU has no bus to the wave ROMs. It
talks to **IC15 (MB87419)** through a register window at **`0x1400-0x143F`**, and to the
output stage through **`0x1F00-0x1F08`**. Everything else in the audio path — IC16, the
DAC, the analog multiplex — is configured by IC15 rather than by the CPU, and the firmware
never addresses any of it.

For the board-level picture (which chip owns the wave address bus, which owns the data
bus, how the single DAC feeds six outputs), see `SYSTEM-DESIGN.md` §4 and §5. This section
covers only the register protocol the firmware uses.

### Register access protocol `[C]`

Every parameter write is a two-step sequence, and 16-bit values are **written twice**:

```asm
6630  c7013e1454   STB R54,0x143E      ; 1. select voice  (R54 = voice index)
6635  c301041444   ST  RW44,0x1404     ; 2. write parameter
663a  c301041444   ST  RW44,0x1404     ;    ...again - hardware latch requirement
```

`0x143E` is the **voice-select latch**; it is re-written before essentially every
parameter store. The duplicated store appears at every 16-bit register write in the
image, so treat it as mandatory, not as redundancy to optimise away.

Register writes are bracketed by an interrupt critical section:

```asm
6624  918008       ORB  INT_MASK,#0x80   ; mask top interrupt
...   (touch the LSI)
662d  717f08       ANDB INT_MASK,#0x7F   ; unmask
```

**Any patch that writes this window must preserve both the double-write and the
critical section.**

### Register window map — **complete** `[C]`

The U-110 reaches IC15 over a **16-bit** bus, so each chip register appears at **twice** its
internal offset. MAME's `mb87419_mb87420_device` (§6.4) documents the chip side; combining
the two gives the full map, and every address this analysis had identified independently
lands correctly.

| U-110 addr | chip reg | Function | Derived here? |
|---|---|---|---|
| `0x1400` | `0x00` | channel mode | — |
| `0x1402` | `0x01` | **ROM/card data read port** | **yes** (§6.1) |
| `0x1404` | `0x02` | ROM bank (bits 10-13) + loop mode (bits 14-15) | **yes** (§6.2) |
| `0x1408` | `0x04` | **pitch**, 2.14 fixed point | partly |
| `0x140C` | `0x06`/`0x07` | **envelope**: low byte = signed ramp rate, high byte = target level (log, 16/octave) | **yes**, see below |
| `0x1410` | `0x08` | sample start address, **fractional** part | **yes** |
| `0x1414` | `0x0A` | sample start address, **high word** | **yes** |
| `0x1418` | `0x0C` | sample **end** address | **yes** |
| `0x141C` | `0x0E` | sample **loop** address | **yes** |
| `0x1420` | `0x10` | **status select**: `0x1404` then reads the HIGH 16 bits of the voice's current level | **yes** |
| `0x1422` | `0x11` | **voice enable**, written 16-bit -> covers regs `0x11`+`0x12`, voices 0-15 | **emulator** |
| `0x1424` | `0x12` | **voice enable, voices 8-15** `[C]`; also **status select**: `0x1404` then reads the LOW 10 bits of the current level | **emulator** + **yes** |
| `0x142A` | `0x15` | **voice enable**, written 16-bit -> covers regs `0x15`+`0x16`, voices 16-31 | **emulator** |
| `0x142C` | `0x16` | **voice enable, voices 24-31** `[C]`; the envelope handler also writes the voice index here before reading its level at `0x1404` — `[I]` the two uses collide under MAME's `addr/2` decode, so the U-110's word bus must not map this register the way the rest do | **emulator** |
| `0x1432`,`0x1434`,`0x1436` | `0x19`,`0x1A`,`0x1B` | undocumented in MAME (`??`), written at init | **emulator** |
| `0x143A` | `0x1D` | undocumented in MAME (`??`) | **emulator** |
| `0x143E` | `0x1F` | **voice select (0-31)** | **yes** |

**32 voices**, and MAME states outright that *"voice 0 is reserved by the firmware for
reading data from the PCM ROM"* — exactly the mechanism deduced in §6.1 from the U-110's own
code. Voices are allocated back to front, 31 first.

The pitch register resolves what §3's "Sample address construction" had inferred: `0x4000`
in 2.14 format advances one byte per output sample, which is why the firmware's phase maths
shifts left by 14.

### Voice enable is 16 bits wide `[C]`

The firmware turns voices on with exactly two stores: a 16-bit write to `0x1422` covering
voices **0-15**, and one to `0x142A` covering **16-31**. Proven by playing a 12-note chord
and watching the masks accumulate:

```
voice 1 on : reg 11 = 02   reg 12 = 00
voice 2 on : reg 11 = 06   reg 12 = 00
   ...
voice 7 on : reg 11 = FE   reg 12 = 00      <- voices 1-7
voice 8 on : reg 11 = FE   reg 12 = 01      <- bit 0 of the high half
voice 9 on : reg 11 = FE   reg 12 = 03
voice 10 on: reg 11 = FE   reg 12 = 07
```

So chip registers `0x11`/`0x12` are the low and high halves of one 16-bit enable, and
`0x15`/`0x16` likewise. **MAME's device decodes only `0x11`/`0x13`/`0x15`/`0x17`** (the
CM-32P reaches the same groups one byte at a time over its 8-bit bus), so a U-110 driver that
forwards the high byte to `0x12`/`0x16` silently loses voices 8-15 and 24-31 — two thirds of
the polyphony. Its own `((offset >> 1) & 3)` grouping does pair `0x12` with `0x13`, so
routing the high half to the odd offset fixes it without touching the shared device.

`[I]` Register `0x10` remains unexplained; it takes small ascending values as voices are
allocated and is *not* a bitmask.

### The amplitude envelope lives in the CPU, in the `EXTINT` handler `[C]`

`0x140C` is **not** a 16-bit volume, and the low byte is not a level. The pair is

| chip reg | field | meaning |
|---|---|---|
| `0x07` (high) | **target level** | log domain, 16 units per octave = 0.3763 dB/unit, `0xFF` = full scale |
| `0x06` (low)  | **ramp rate**, *signed 8-bit* | `+1..+0x7F` rises, `0x80..0xFF` (i.e. `-128..-1`) falls |

The chip ramps each voice's internal level toward the target at that rate and raises
**`EXTINT`** when the voice needs its next segment. The CPU's handler then reads which
voice from chip register `0x00`, reads the voice's current level back through the status
port, and writes the next `(target, rate)` pair. **That handler is the U-110's entire
TVA envelope generator** — attack, decay, sustain and release are firmware, not chip.

Vector `0x200E` -> `0x4020` -> **`0x41C4`**:

```
41C4: orb int_mask, #40 / ei          ; re-enable, nested
41D0: ldbze 54, 1400                  ; chip reg 00 = the voice asking for service
41D5: cmpb 54, #20 / #21              ; 0x20, 0x21 are two non-voice slots
41E6: ldb  50, 3600[54] / 51, 3660[54]  ; 16-bit rate, per voice
41F0: ldbze 52, 3680[54]              ; note-on timestamp
41F5: ldb  56, 3700[54]               ; flags: bits 0-2 = envelope PHASE, bit 4 = service due
41FF: andb 57, 56, #07                ; dispatch on the phase
4208:   1 -> lcall b932   2 -> baf4   3 -> bd42
4226:   4 -> be7f        5 -> bebd    6 -> bfac
42B3: (bit 4 path) recompute the rate and write it back
4334: st 52, 140c                     ; new (target, rate)
435B: ldb 50, #10 / djnz 50           ; ~16-iteration settle delay before RTI
```

Six phase handlers, one per envelope segment. Each reads the tone's own envelope bytes —
`0x28A9`/`0x28AA`/`0x28AF` for the first partial, `0x28C9`/`0x28CA`/`0x28CF` for the second —
and applies **key scaling** about key `0x45` (A4):

```
B988: ldbze 4e, 0051 ; sub 4e, #0045  ; key - 69
B991: mulb  4e, 004c                  ; x the tone's key-scale nibble
B99D: shra  4e, #04 ; add 44, 004e    ; -> the segment's rate
```

Three write sites pin the encoding beyond doubt:

* **note-on**, `0x69F0`-`0x6A5E`. `reg07` = the velocity/level result (clamped 0..0xFF);
  `reg06 = (reg07 * K) >> 8 + 16*(nibble-8)`, clamped `1..0x7F` and then capped by a
  256-entry ROM table at **`0xB0C6`** indexed by `reg07` — a level-dependent ceiling on how
  fast a voice may rise. Where that table reads 0 (any `reg07 < 0x30`) the firmware
  substitutes `ld 44, #3001`, i.e. target `0x30` at rate `+1`: silence.
* **note-off**, `0x649A`-`0x6529`. The rate is built from a ROM table at `0xAFC6`, reduced
  by a term proportional to **how long the note was held** (`f2 - 3680[voice]`), trimmed by
  a per-tone nibble, clamped to `0..0x7F` — and then `not 44`, one's complement, making it
  **negative**. Phase goes to 5.
* **voice kill / power-up**, `0x721C` and `0xCBFC`. Both write the constant `0x0080`:
  target `0x00`, rate `-128`, the steepest fall the field can express. The power-up loop
  then spins on the status port until the reported level reaches zero.

The status port confirms the level readback, and the routine at `0x7655` gives its exact
shape. It reads the same voice three times, once per select:

```
7658: st  54, 142c ; ld 3e, 1404     ; select 0x16 -> one field
7662: stb 54, 1420 ; ld 44, 1404     ; select 0x10 -> the HIGH 16 bits
766C: stb 54, 1424 ; ld 46, 1404     ; select 0x12 -> the LOW 10 bits
7679: and  46, #03ff
767D: shll 44, #06                   ; normalise the 32-bit pair 44:46 ...
7683: (loop) shll 44, #01 until bit 15 of byte 47 is set, counting 15 down
7690: 40 = (count << 4) | (bits 6..3 of byte 47)
```

That is normalise-and-take-the-exponent: the result is `exponent * 16 + mantissa nibble` —
**the same 16-units-per-octave log scale as `reg07`**. So a voice's current level is a
**26-bit linear value** split across two selects, and the firmware converts it to the log
domain itself. The power-up loop at `0xCC0F` uses only the low field, spinning until
`(0x1404 & 0x03FF) == 0`. The envelope handler selects with `0x142C` and takes the high
byte, then at `0x432C` copies it straight back into `reg07` — the sustain segment holds by
setting the target to wherever the ramp has got to.

Measured against this, the earlier reading of `reg06` as "a 7-bit level, role unknown" was
wrong, and so was the statistical result that `reg06` predicts an instrument's decay at
r = -0.874. It does, but only because `reg06` *is* a rate: percussive tones are written a
steep attack and a steep decay, sustaining ones a shallow pair.

**`[I]` What the emulator is missing.** `roland_lp.cpp` calls `m_int_callback(CLEAR_LINE)`
once at reset and never asserts it, and has no per-voice level, ramp or status port. So the
handler above never runs: over the whole 392 s reference session the emulator issues
**490 writes to `0x06`/`0x07` for 228 note-ons** — the two identical stores at `0x6A59`/
`0x6A5E` and nothing else. Every voice holds its note-on target forever, which is exactly
the "no decay" the renders show.

### Pitch: the reference note is at the engine rate `[C]`

MAME documents the sample table's `+0x08` field as the reference note *"when played back at
32000 Hz"*. That is CM-32P-specific. On the U-110 the field is relative to **its own** engine
rate of 34 kHz, and no cross-rate correction is applied anywhere. Measured by playing notes
and matching each voice setup against the sample table:

| Played | Sample | Ref note | Implied note | fine-tune `+0x07` |
|---|---|---|---|---|
| 36 | 135 | 73 | 35.82 | `0x48` |
| 48 | 135 | 73 | 47.83 | `0x48` |
| 60 | 135 | 73 | 59.83 | `0x48` |
| 72 | 136 | 78 | 71.90 | `0x43` |
| 84 | 138 | 88 | 83.94 | `0x40` |

Reading the field as 32 kHz-relative would put every note **0.88 semitones** out; reading it
as engine-relative leaves ≤0.18, and that residual tracks `+0x07` at roughly **1.3 cents per
unit** (subtracted, matching the `SUB RW40,0xA516[RW42]` at `0x66D5`) plus a constant
**-6.5 cents** of master tune. So the shared wave ROM stores reference notes, and each
machine interprets them at whatever rate its own crystal gives.

### Sample format — **8-bit float: sign + 3-bit exponent + 4-bit mantissa** `[C]`

> This section has been wrong four times: *memoryless* (right, badly argued), *differential*
> (wrong), *companded* (dismissed — but this **is** a companded format), and *linear PCM*
> (wrong). See corrections #15-#17, #21-#24 and **#30**.

**The byte is a compressed magnitude, expanded on read. It is not a delta.**

```c
int16_t decode(int8_t b) {
    int sign = (b < 0) ? -1 : +1;
    int v    = (b < 0) ? -b : b;      // magnitude of the TWO'S COMPLEMENT byte
    int sh   = v >> 4;                // 3-bit exponent
    v       &= 0x0F;                  // 4-bit mantissa
    return sign * (sh ? ((0x10 + v) << (sh - 1)) : v);
}
```

Full scale is **±1984**, so the format carries about 11 bits of range in 8 bits of storage,
with quantisation that coarsens as amplitude grows — the point of companding.

Note the sign handling: the byte is a **two's complement value whose magnitude is the float
code**, not a sign-magnitude byte. `-69` is stored as `0xBB`, and its magnitude `69 = 0x45`
expands to 168. Reading `0xBB` as sign-magnitude instead takes the low seven bits `0x3B` = 59
and yields 108 — an asymmetry that alone destroys the waveform (sine correlation falls to
0.465). **This was the trap that hid the format for four rounds.**

#### How it was finally settled `[C]`

The U-110's own Sound Check tone is the key, because the firmware plays a sample whose
correct output is *known*: a pure sine. Read linearly, wave-ROM sample 212 is a trapezoid.
Decoded as above, it is a sine.

| | sine corr | h3 | h5 |
|---|---|---|---|
| linear int8 | 0.98292 | **-15.3** | -24.2 |
| **float 3-4** | **0.99975** | **-66.7** | -59.4 |

Three independent confirmations:

- **The transfer curve is constant.** Because the target is a known sine, the decoder can be
  *read off* rather than guessed: plotting decoded value against the ideal sine at each
  instant, the ratio holds constant to **2.5% over a 14:1 amplitude range**. For the linear
  reading the same ratio varies by **32%**. A memoryless curve of exactly this shape is
  therefore the decoder, derived from data rather than assumed.
- **The residual ratio rises monotonically.** Fitting the fundamental to the raw bytes gives
  amplitude 79.3 while the data peaks at 69 — the stored peak is compressed by 13%, and the
  ideal/stored ratio climbs from 0.39 at small codes to 1.13 at the peak. That is an expander,
  visible directly in the waveform as steps that bunch together near the peaks.
- **Hardware's even harmonics prove the odd ones are not in the data.** The `Test10
  SoundCheck` recording measures h2 at **-47.6 dB** against an inter-harmonic floor of -89 dB.
  Symmetric data cannot produce even harmonics, so ~-48 dB is the **analog path's own
  distortion**. Measured h3 is -44.1 dB — the same order, therefore also analog. The linear
  reading predicts data h3 at -15.3 dB, which would swamp that by 29 dB. **It does not
  appear. Linear is refuted.** The float reading predicts -66.7 dB, safely beneath the analog
  floor, which is consistent with what the hardware shows.

#### What MAME had right, and the one character that was wrong

`roland_lp.cpp`'s `decode_sample()` is **correct** — it is Sarayan's 1-3-4 rule, sign applied
to the magnitude, byte-for-byte the function derived above. The defect was a single operator:

```c
chn.smpl_nxt += decode_sample(...);   // WRONG: treats the sample as a delta
chn.smpl_nxt  = decode_sample(...);   // right
```

The `std::clamp` to ±0x7FF beneath it, commented *"until the decoding is fixed"*, exists only
to contain drift the accumulator itself introduces. With `=` there is no drift, no clamp is
needed, and no scaling either: ±1984 already sits in the range the rest of the pipeline
(`sample * volume`, normalised by `32768 << 14`) expects. `roland_u110.cpp` selects it with
`set_pcm_mode(PCM_FLOAT8)`, leaving the CM-32P's historical accumulator untouched.

`[I]` The decode explains a long-standing listening complaint: linear-decoding a companded
signal adds distortion that **grows with amplitude**, heard as a brittle, gritty edge on loud
notes and as "built-in distortion" on sustained tones.

### Engine sample rate — **32,000 Hz** `[C]` `[S]`

`34,816,000 / 1088 = 32,000` exactly.

This is the value the first revision of this document derived, which corrections entry #13
then discarded in favour of MAME's `clock / 2 / 512`. **Real hardware says the original
derivation was right** — see #22. Measured by playing identical MIDI into a real U-110 and
into the emulator: with MAME's /1024 divisor the emulator ran **+104 cents** sharp, and
`1200 * log2(34000/32000) = +104.96`. With /1088 the two agree within a few cents across
five octaves.

The crystal itself is confirmed at **34.816 MHz** on the schematic (p.13, at IC15 pins
79/80), so it is the divisor that differs from the CM-32P, not the oscillator. Note the
consequence: both machines land on 32 kHz by different routes, and the circulating 32 kHz
WAVs are at the **native** rate rather than being resampled.

### Tuning — **A = 440 Hz** `[C]`

Measured on hardware, on patches without chorus, against equal temperament at A440:

| Patch | Note | cents |
|---|---|---|
| P-01 Ac.Piano | 48 / 60 / 72 / 84 | +0.6 / -2.0 / +1.9 / +1.4 |
| P-31 Strings | 48 / 60 | +2.2 / -0.2 |
| P-11 Vibraphone | 60 | +0.4 |

Within ±2.5 cents throughout; at A442 every reading would sit 6-8 cents flat. The Japanese
A442 convention does **not** apply to this unit. `P-46 Flute` appears to read 443 Hz only
because that patch has chorus: its A4 is a doublet at 439.79 and 443.12 Hz beating at
3.3 Hz.

### Known discrepancy: harmonic content `[I]`

With the decoder, rate and tuning all correct, the emulator still produces **too much
harmonic distortion**. Measured against hardware on the same patch and note, relative to
the fundamental:

```
VIB 1, note 60      harmonic:   2    3    4    5    6   10   11
                    hardware:  -8  -15  -20  -22  -33  -45  -46
                    emulator:  -1   -4   -5   -6  -17  -23  -19
```

Noise-plus-distortion is 8-19 dB worse than hardware across strings, vibes and piano, and
broadband noise between harmonics is 9-12 dB high.

`[C]` **The target is now measured, not assumed.** The service Sound Check (§8.5) plays a
pure tone on each voice, and on real hardware its harmonics sit at h2 -48, h3 -44, h4 -82,
h5 -67 dB. **The machine's own THD floor is therefore about -44 dB**, and all 31 voices are
identical to within 0.1 dB.

What has been ruled out:

| Candidate | Verdict |
|---|---|
| A companding / expansion curve | **No.** Fitting power-law and mu-law expansions against the hardware's harmonic profile improved the error only from 13.2 to 11.6 dB rms, with the residual keeping its shape. No memoryless curve accounts for it. |
| Missing reconstruction filter | **No.** The excess is uniform across 1-4 kHz, 4-8 kHz and 8-16 kHz; a filter would affect only the top. |
| Chorus splitting the measurement | **No.** The hardware's harmonics are single clean peaks. |
| Keymap / multisample selection | **No.** Piano note 60 correctly selects sample 4 (ref note 71, implied 59.92). |
| Stepped volume envelope | **No.** The firmware writes the volume register about once per note, so there is no staircase to smooth. |

`[I]` The leading remaining suspect is **voice allocation and release**. A single-part
strings patch was observed with six voices simultaneously enabled, which would inflate every
harmonic by adding correlated copies of the same waveform. This matches the open TODO in
`roland_cm32p.cpp`: *"figure out how 'freeing a voice' works - right now the firmware gets
stuck when playing the 32nd note."*

### 3.1 Output routing — IC26 `[C]`

`0xB721` loads all eight IC26 registers as one group, selected by a single byte in the
patch header:

```asm
b721  LDBZE RW50,0x280E     ; active patch header byte +0x0E  = output assign group
b726  MULUB RW50,#0x8       ; x 8 bytes per group
b729  LDB   R54,#0x1
b72c  STB   R54,0x1F08      ; enable
b733  LDB   R54,0xA8B6[RW50]  ; <-- ROM ROUTING TABLE at 0xA8B6
b738  STB   R54,0x1F00[RW52]  ; write registers 0x1F00..0x1F07
b741  CMPB  R52,#0x8
b744  JLT   0xB733
```

The table at **`0xA8B6`** holds 8 bytes per group; the values are small bit patterns
(`00 01 02 04 08`), which reads as a per-destination enable/level mask:

```
group 0: 00 01 00 00 00 01 00 00      group 5: 02 04 01 01 01 01 01 02
group 1: 01 02 01 01 01 01 01 01      group 6: 04 08 01 01 01 01 01 02
group 2: 00 02 00 01 00 01 00 01      group 7: 00 02 00 00 00 01 00 00
group 3: 02 04 01 01 01 01 01 01      group 8: 02 04 01 01 01 01 02 02
group 4: 02 02 01 01 01 01 01 02      group 9: 00 04 00 01 00 01 00 02
```

Loading this group is the firmware's **only** involvement in the analog output stage; the
multiplex timing comes from IC16, not the CPU (`SYSTEM-DESIGN.md` §5).

`[I]` The exact field meaning of the eight bytes is still not established — a listening
test that steps patch header byte `+0x0E` through its range while watching the six outputs
would settle it quickly.

### Sample address construction `[C]`

At `0x6672`, the address handed to the LSI is assembled from a byte stream via an
auto-incrementing pointer, into a 32-bit fixed-point value:

```asm
664e  0d0e40       SHLL RL40,#0x0E     ; 32-bit shift left 14 -> integer:fraction
6651  c7013e1454   STB  R54,0x143E
6656  c301141442   ST   RW42,0x1414    ; high word
665b  c301141442   ST   RW42,0x1414
6660  c7013e1454   STB  R54,0x143E
6665  c301101440   ST   RW40,0x1410    ; low word
666a  c301101440   ST   RW40,0x1410
...
6672  b23b44       LDB  R44,[RW3A]+    ; next 16-bit field from the tone record
6675  b23b45       LDB  R45,[RW3A]+
667a  0d0e44       SHLL RL44,#0x0E     ; same 14-bit scaling
667e  644440       ADD  RW40,RW44      ; accumulate into the 32-bit pointer
6681  a44642       ADDC RW42,RW46
```

The `SHLL #0x0E` scaling means the LSI consumes a **32-bit phase accumulator with a
14-bit fractional part** — the classic pitch-increment arrangement. Pitch is applied
before the write via a ROM tuning table:

```asm
66c7  af57692a42   LDBZE RW42,0x2A69[RW56]  ; fine-tune param, 7-bit
66cf  794042       SUBB  R42,#0x40          ; centre on 0
66d2  090142       SHL   RW42,#0x1          ; 16-bit table entries
66d5  6b4316a540   SUB   RW40,0xA516[RW42]  ; <-- PITCH TABLE at 0xA516
...
66ec  af01023c42   LDBZE RW42,0x3C02        ; master tune (global RAM)
66f4  674316a540   ADD   RW40,0xA516[RW42]
```

**`0xA516` is the pitch/tuning table**, 16-bit entries, indexed by a centred 7-bit
value. Retuning the instrument globally — stretched tuning, alternate temperaments —
means editing this table, and it is pure data with no checksum over it.

**What you cannot do from this ROM:** change the samples themselves, their loop
points as stored, or the wave ROM contents. Those live in the separate PCM ROMs.
What you *can* do is change every parameter that is fed to the LSI.

---

## 4. Patch table at `0xE000` — factory defaults vs user RAM `[C]`

> **Read §2.1 first.** `0xE000-0xFFFF` is bank-switched. The 64 records in *this EPROM*
> are the **factory defaults**, visible only while `P2.7 = 1` — which happens at exactly
> one place in the ROM, the "Mem Initialized" copy loop. Every other read of this address
> range, including all the indexing code quoted below, hits the **battery-backed user
> patch SRAM (IC11)** instead. The structure is identical in both; the contents are not.

The region is **64 patches x 128 bytes**, proven three ways:

```asm
81b0  b3014a2734   LDB   R34,0x274A    ; current patch number
81b5  7d8034       MULUB RW34,#0x80    ; x 128  -> record size
81b8  6500e034     ADD   RW34,#0xE000  ; -> table base
```
```asm
8322  a1400032     LD   RW32,#0x40     ; 64 iterations
8326  a100e030     LD   RW30,#0xE000
8331  65800030     ADD  RW30,#0x80     ; stride 128
8335  e032f2       DJNZ R32,0x832A
```
64 x 128 = 8192 = exactly `0xE000-0xFFFF`, and exactly the capacity of one 8K SRAM.
The names decoded out of the EPROM copy are the U-110's documented factory **patches** —
`'Ac.Piano  '`, `'Brt Piano '`, `'Wide Piano'` … `'Guit>Piano'`, `'Multi-Set5'`. See the
terminology note at the top of this document: several of them read like tone names, but
the split, layer and multi-set entries confirm these are patches.

This also explains cleanly why `0xE000-0xFFFF` is **byte-identical between v2.00 and
v2.03** despite 56% of the code changing: it is a factory default image, not code, and
the default patches did not change between firmware revisions.

At `0x81BC` the same offset is computed against `0x2800` (`SUB RW56,RW34,#0x2800`),
so **work RAM at `0x2800` holds the active patch in the identical 128-byte layout** — the
edit buffer, distinct from the 64-slot store at `0xE000`.

### Record layout

```
+0x00  4 bytes   zero / reserved
+0x04  10 bytes  ASCII patch name (same field width as a tone name, §6.6)
+0x0E  6 bytes   patch header params. +0x0E selects the output-routing
                 group (§3.1); chorus / tremolo settings live in this block.
                 Corroborated by the UI: the PATCH:COM page (0x9550) offers
                 exactly 'Patch Name', 'Output Mode' and 'Chorus/Tremolo'.
+0x14  6 x 16    SIX PART RECORDS, stride 0x10   (0x14,0x24,0x34,0x44,0x54,0x64)
+0x74  12 bytes  trailer
```

Part record fields confirmed by the channel-scan loop at `0x5638`:

| Off | Field |
|---|---|
| `+0x02` | MIDI receive channel — low nibble. Read as `0x2816` for part 0 |
| `+0x0B` | Flags. Read as `0x281F`. `(b & 0xE0) == 0xC0` ⇒ **part disabled** |

Verified against preset 0 `Ac.Piano`: part 0 flag `0x08` (enabled, channel 0),
parts 1-5 flag `0xC8` (disabled) — correct for a single-timbre piano patch.
Parts are otherwise byte-identical across the record, as expected.

---

## 5. MIDI

### Receive path `[C]`

Serial ISR at **`0x4154`** reads `SP_STAT`, then on the RX branch (`0x4198`):

```asm
4198  b00731       LDB  R31,SBUF
419e  99fe31       CMPB R31,#0xFE       ; Active Sensing -> flag only, not buffered
41a8  51f83132     ANDB R32,R31,#0xF8
41ac  99f832       CMPB R32,#0xF8       ; System Real-Time 0xF8-0xFF -> discarded
41af  df0a         JE   0x41BB
41b4  c733002131   STB  R31,0x2100[RW32]  ; -> MIDI IN ring buffer
41b9  17e2         INCB RE2               ; head++
41bb  b128e9       LDB  RE9,#0x28         ; reload active-sensing timeout (40 ticks)
```

Ring buffers: **IN at `0x2100`**, head `RE2` / tail `RE3`; **OUT at `0x2200`**,
head `REB` / tail `REC`, drained by the TX branch at `0x4171`. Both are 256 bytes
with byte-wide indices, so they wrap for free. Overflow sets bit 1 of `REA`, which
surfaces as the `MIDI Buffer Full` string.

### The MIDI command table — yes, fully exposed `[C]`

The parser at `0x56AD` dispatches on the status nibble through a compact,
contiguous jump table. This is the single most patch-friendly structure in the ROM:

```asm
56ad  51f0e530     ANDB R30,RE5,#0xF0   ; running-status byte -> status nibble
56b1  180430       SHRB R30,#0x4
56b4  710730       ANDB R30,#0x7        ; 0x8n..0xFn -> 0..7
56b7  ac3070       LDBZE RW70,R30
56ba  7d0370       MULUB RW70,#0x3      ; 3 bytes per slot
56bd  65c35670     ADD  RW70,#0x56C3    ; <-- TABLE BASE
56c1  e370         BR   [RW70]
```

**Table at `0x56C3` — 8 slots x 3 bytes, each a bare `LJMP` (`E7` + disp16):**

| Slot | File offset | Status | Message | Handler |
|---|---|---|---|---|
| 0 | `0x56C3` | `0x8n` | Note Off | `0x56DB` |
| 1 | `0x56C6` | `0x9n` | Note On | `0x5714` |
| 2 | `0x56C9` | `0xAn` | Poly Aftertouch | `0x5795` |
| 3 | `0x56CC` | `0xBn` | Control Change | `0x57D6` |
| 4 | `0x56CF` | `0xCn` | Program Change | `0x5A66` |
| 5 | `0x56D2` | `0xDn` | Channel Aftertouch | `0x5B12` |
| 6 | `0x56D5` | `0xEn` | Pitch Bend | `0x5B3E` |
| 7 | `0x56D8` | `0xFn` | System / SysEx | `0x5B7F` |

Repointing any message type is a **three-byte edit** at a known offset. System
messages are pre-filtered before the table: `0xF0` → `0x5B94` (SysEx start),
`0xF7` → `0x5B9F` (EOX), `0xFE` → active-sensing flag, `0xF8-0xFF` discarded.

Channel matching happens ahead of dispatch at `0x561F-0x565C`: the loop
(`LDB R35,#0x6`) walks all six parts, reads each part's channel from the active
patch at `0x2800`, and builds a six-entry match table in the register file at `0x43`.
Global MIDI settings come from `0x3C00`/`0x3C01` (the latter masked `& 0x0F`).

---

## 6. External PCM cards and wave ROM access `[C]`

Yes — there is substantial card-handling code, and the card format is largely recoverable
from it.

### 6.1 The CPU has no bus to the wave ROMs or cards

The schematic settles this: the cartridge ports hang off **IC15 (MH87419)**, not off the
CPU bus. `[S]` There is no chip-select, no window, no banking register that exposes card
memory to the CPU. Instead the firmware **borrows voice 0 of the tone generator as a read
engine**: it
parks that voice's phase accumulator at the address it wants, waits a fixed number of
loop iterations, and reads the byte back out of port **`0x1402`**.

The primitive is `0x7BB2` — read one byte from wave-ROM/card address `RW50`:

```asm
7bb2  PUSH  0x56
7bb6  CLR   RW54
7bb8  SUB   RW56,RW50,#0x2      ; address - 2  (LSI prefetch offset)
7bbd  SHRAL RL54,#0x2           ; 32-bit >>2  =>  addr << 14  (14-bit fraction, see §3)
7bc0  ANDB  INT_MASK,#0x7F      ; enter critical section
7bc3  STB   ZRlo,0x143E         ; select VOICE 0 as the read engine
7bc8  ST    RW54,0x1410         ; phase low
7bcd  ST    RW56,0x1414         ; phase high
7bd2  ORB   INT_MASK,#0x80
7bd5  LDB   R54,#0x10
7bd8  DJNZ  R54,0x7BD8          ; ~16-iteration busy wait for the fetch
7bdb  LDB   R54,0x1402          ; <-- THE DATA PORT
7be4  RET
```

`0x7B07` is the streaming variant: same mechanism, but with a 32-bit cursor in `RWF4`/`RWF6`
that auto-increments (`INC RWF4 ; ADDC RWF6,ZR`), backed by RAM at `0xF8`/`0xFA`.
Note the address is shifted left 14 — the same fixed-point format §3 identified, so
**card address = phase >> 14**.

**Consequence for patching:** any code that wants card data must go through this
sequence, must own voice 0 while doing so, and must keep the busy-wait. The wait is a
raw delay loop, not a status poll — there is no ready flag.

### 6.2 Bank / slot selection — `0x1404`

`0x7B74` writes the selector, with the slot number in `R52`:

```asm
7b87  ANDB R55,R52,#0x3      ; slot 0..3       -> bits 4-5 of the high byte
7b8b  SHLB R55,#0x4
7b8e  JBC  R52,0x2,0x7b96    ; R52 bit 2 = "card" rather than internal
7b91  ORB  R55,#0x8          ;   -> sets bit 3
7b9c  STB  ZRlo,0x143E       ; voice 0
7ba1  ST   RW54,0x1404       ; <-- BANK SELECT
```

So the high byte written to `0x1404` is `(slot & 3) << 4 | (card ? 8 : 0)`.
Test-mode scans banks 0-3 with the flag **clear** (internal wave ROMs); the runtime mount
loop scans 0-3 with `ADDB R52,R3C,#0x4` — flag **set** (card slots). The selector is
therefore 3 bits: 4 internal banks + 4 card banks.

`[C]` **A fourth bit is live.** Bit 10 of the 16-bit value (bit 2 of the high byte) is wave
address bit **18** — it picks the upper half of a 512 KB ROM. Confirmed under emulation: a
note whose sample lives at bank offset `0x59087` is set up with bank `0x3400`, and bit 10
supplies the `0x40000`. The chip forms its address as
`(phase >> 14) | ((bank & 0x3C00) << 8)`, so bank bits 10-13 land on address bits 18-21 and
the phase accumulator supplies only bits 0-17. That is exactly why the CPU's borrowed-voice
read port has to wrap at 18 bits (§6.1) — without it, a request for logical address 0 (which
the firmware makes by parking at `0 - 2`) lands a bank away.

`[S]` **There really are four card slots.** The service notes block diagram labels the
card board **"PCM CARD x4"**, and the schematic shows four sets of cartridge connectors.
An earlier revision of this document guessed the machine had fewer physical ports than the
firmware scanned and that the extra iterations were a harmless superset — that guess was
wrong. The 4-slot scan bound, the four `PORT1` presence bits and the four cartridge
connectors all agree. Wave ROM chip selects are decoded by **IC13 (HC02) and IC14 (HC32)**
from IC15 outputs.

### 6.3 Card presence detect — `PORT1` `[S]`

```asm
6eaf  ANDB R30,PORT1,0x9042[RW3C]     ; per-slot presence bit
```

`0x9042` holds `01 02 04 08 10 20 40 80` — one mask bit per slot, tested against 8097BH
`PORT1` (SFR `0x0F`). Slot index `R3C` runs 0..3 (`CMPB R3C,#0x4` at `0x6EE9`).

Polarity is **active low**: at `0x6EAF` a *set* bit branches to the next slot, so a clear
bit means a cartridge is present. `PORT1` is initialised to all-ones at `0x4378`
(`LDB PORT1,#0xFF`), which is the quasi-bidirectional idiom for using the pins as inputs.
Unpopulated slot lines therefore read high and are simply skipped — which is why the
firmware can scan four slots on a machine with fewer physical ports.

### 6.4 The two permutation tables — **RESOLVED** `[C]` `[S]`

`0x7CB5` reads the 48-byte ID header through two tables:

```asm
7cbc  LDBZE RW50,0x9357[RW5E]   ; 48-byte table -> the LSI address to request
7cc1  SCALL 0x7BB2              ; read that wave-ROM byte
7cc5  LDB   R50,0x9257[RW54]    ; 256-byte table applied to the byte
7cca  STB   R50,0x26B0[RW5E]    ; store
```

**Both tables are software compensation for physical wiring**, not obfuscation:

- **`0x9357`** (48 bytes) inverts the **address**-line permutation between IC15 and the
  wave ROM (`SYSTEM-DESIGN.md` §4.2).
- **`0x9257`** (256 bytes) inverts the **data**-line permutation along
  `ROM → IC16 → IC15 → CPU` (§4.3).

They compose to the identity, which is exactly why a plain linear read of a dump matches
the expected signature. Verified by simulation:

```
expected (program ROM 0x9387) : 'Roland\0*10 T-110   Ver'
perm[ inv[ dump[ g(addr[i]) ] ] ] : 'Roland\0*10 T-110   Ver'   MATCH
```

where `g` is the address map and `inv` the hardware data permutation.

> **The earlier contradiction was my own error.** A previous revision computed
> `perm[dump[addr[i]]]`, got garbage, and concluded something was unexplained. That
> omitted the hardware's own data permutation: the CPU never sees the raw chip byte, it
> sees `inv[chip_byte]`, and `perm[inv[x]] = x`. The dump and the firmware were consistent
> all along — the model was missing a stage.

#### How the address map was finished

The header table constrains only the six logical bits it exercises. The rest fell out from
**known plaintext**: the 99 preset tone names printed in the owner's manual (pp. 8-9).
Searching a real wave ROM for `"FRETLESS 1"`, `"MARIMBA"`, `"SHAKU 1"`, `"SLAP 1"` and
`"DRUMS"` under the predicted byte-scatter pattern produced five immediate hits, all in
`waverom0`, all under the same data transform. Fitting the remaining bits against the
resulting record positions completed the map for bits 0-13, after which **all 99 names
decode in order**.

### 6.5 Signature checks `[C]` + confirmed against real dumps

Two expected signatures sit adjacent in the program ROM, and they differ:

**`0x9387`, 27 bytes — internal wave ROM,** checked in test mode at `0x8989`:
```
"Roland" + 10 x NUL + "T-110   Ver"
```

**`0x93A2`, 16 bytes — PCM card,** checked on mount at `0x6EF2` and `0x7D17`:
```
52 6F 6C 61 6E 64 55 2D 31 31 30 20 4E B1 53 AC
"RolandU-110 N" + B1 53 AC
```

Mount failure jumps to `0x6E91`, which displays `"  Illegal CARD"` (`0x6EB9`) and stores
`0xFF` as that slot's ID. On success the ID byte is cached: `STB R35,0x2743[RW3C]` — a
4-byte table at RAM `0x2743`, one entry per slot.

**Real dumps confirm the header layout exactly**, including the two field offsets that
were predicted from the code before any dump was available:

| ROM | bytes `0x00-0x1A` | name `+0x10` | version `+0x1B` | **ID `+0x20`** |
|---|---|---|---|---|
| waverom0 | `Roland`+10 NUL+`T-110   Ver` | `T-110   Ver0.08 ` | `0.08` | `0x30` = `'0'` |
| waverom1 | same | same | `0.08` | `0x31` = `'1'` |
| waverom2 | same | same | `0.08` | `0x32` = `'2'` |
| waverom3 | same | same | `0.08` | `0x33` = `'3'` |
| waverom4 | `RolandU-110 N`+`B1 53 AC` | `SN-U110-08      ` | — | `0x08` |
| waverom5 | `RolandU-110 N`+`B1 53 AC` | `SN-U110-09 0.27 ` | `0.27` | `0x09` |

Two predictions land precisely:

- The internal banks carry ASCII `'0'`–`'3'`, which is why `0x89AC` does
  `SUBB R50,#0x30` before comparing against the bank index. **Four internal banks,
  numbered 0-3** — matching the `CMPB R30,#0x4` loop bound found in the firmware.
- The cards carry a **binary** ID equal to their SN-U110 part number: `0x08` for
  SN-U110-08, `0x09` for SN-U110-09. That is the value cached at `0x2743[slot]` and
  looked up by `0x7D40`, and it fits comfortably in the 5-bit part-record field of §6.7.
  So `PATCH:CARD ASGN` really does address cards by catalogue number.

### 6.6 Wave ROM / card data layout — **decoded** `[C]`

All addresses below are **logical** — what the firmware requests. The physical chip
address is `g(logical)` using the map in `SYSTEM-DESIGN.md` §4.2, and chip bytes arrive at
the CPU permuted per §4.3.

```
WAVE ROM / CARD LAYOUT  (logical addresses)
  0x0000-0x002F    48-byte ID header                              §6.5
  0x0100 + 10*n    tone name directory, 10 bytes per tone          [C from code]
  0x1000 + 0x50*n  TONE PARAMETER RECORDS, 80 bytes each          decoded below
  beyond           PCM sample data                                 [I]
```

**The tone name is the first 10 bytes of the parameter record.** That was deduced from the
`" No Card! "` placeholder and is now confirmed directly. The separate directory at
`0x0100` is a name-only index, cheaper to walk when scrolling a tone list.

#### All 99 internal tones decode

Using the solved map on `waverom0`, every record reads cleanly and in manual order:

```
  1 A.PIANO 1    23 MARIMBA      45 FINGERED 1   67 E.ORGAN 5    89 SAX 5
  2 A.PIANO 2    24 A.GUITAR 1   47 PICKED 1     75 E.ORGAN 13   90 BRASS 1
 11 E.PIANO 1    29 E.GUITAR 1   49 FRETLESS 1   76 SOFT TP 1    95 FLUTE 1
 16 VIB 1        33 SLAP 1       51 AC.BASS      79 TP / TRB 1   97 SHAKU 1
 19 BELL 1       44 SLAP 12      55 CHOIR 1      85 SAX 1        99 DRUMS
```

All 99 match the Preset Tones Chart in the owner's manual exactly. The same map applied to
the cartridge dumps yields their tone lists too — `FANTASIA`, `BELL PAD`, `SYN CHOIR`,
`BREATH VOX`, `L.CALLIOPE`, `METAL HIT`, `RICH BRASS`, `BRASTRINGS`, `PIZZAGOGO` … for
SN-U110-08, and `BRIGHT EP1`, `SYN.VOX 1`, `SYN.BASS 4`, `HEAVY.EG 1`, `JP.STRINGS` … for
SN-U110-09.

#### Record structure `[C]` / `[I]`

Tone 1, `A.PIANO 1` (a V-MIX tone, i.e. two velocity-mixed partials):

```
 +00  41 2E 50 49 41 4E 4F 20 31 20     "A.PIANO 1 "   10-byte name
 +0A  03 00 40 40 00 01                 header params
 +10  1E 27 2E 34 3D 47 54 60 FF FF FF  partial 1: 8 ascending split points
 +1B  00 01 02 03 04 05 06 07 08                   9 sample indices
 +24  FF FF FF 7F 7F 64 40 00 38 7F 67 C0          level / envelope
 +30  1E 27 2E 34 3D 47 54 60 FF FF FF  partial 2: same shape
 +3B  09 0A 0B 0C 0D 0E 0F 10 11                   next 9 samples
 +44  FF FF FF 7F 7F 64 41 00 39 7F 67 C0
```

The two 32-byte blocks at `+0x10` and `+0x30` are structurally identical — **two
partials**, matching the manual's SINGLE / DUAL / DETUNE / V-MIX / V-SW tone types.
`[C]` The ascending byte run `1E 27 2E 34 3D 47 54 60` reads as **key split points**
(MIDI notes 30, 39, 46, 52, 61, 71, 84, 96) and the run that follows as the **sample
index per zone** — a multisample keymap of up to nine zones per partial. That reading fits
the data everywhere it has been spot-checked but has not been proven against playback.

### 6.7 How a patch part selects a card tone

`0x7D56` resolves the reference:

```asm
7d56  MULUB RW50,#0x10        ; part index x 16
7d59  ADD   RW50,#0x2814      ; -> part record in the active patch (see §4)
7d5d  LDB   R50,[RW50]        ; part byte +0x00
7d60  ANDB  R50,#0x1F         ; 5-bit group selector
7d65  CMPB  ZRlo,0x50
7d6a  JE    0x7D71            ; 0 => internal wave ROM
7d6c  SCALL 0x7D40            ; else search 0x2743[0..3] for that card ID
7d6e  ORB   R51,#0x4          ; set the "card" flag for the bank selector
```

So the part record's first two bytes are the tone reference:

| Part offset | Field |
|---|---|
| `+0x00` | **card / group selector**, 5 bits. `0` = internal (the 99 ROM tones); otherwise a card ID |
| `+0x01` | **tone number** within that group — 1 byte, which comfortably holds 0-98 |
| `+0x02` | MIDI receive channel (low nibble) |
| `+0x0B` | flags — `(b & 0xE0) == 0xC0` means part disabled |

Because a patch names a card by **ID**, not by slot, the same patch works whichever slot
the card is in — `0x7D40` scans the four cached IDs and returns the slot, or `0xFF` if
that card is absent. That is exactly the `PATCH:CARD ASGN` feature (`0x9C10`).

### 6.8 Evidence from real wave ROM dumps `[C]`

Six dumps in `roland_u110_u220/` (512 KB each) plus U-110 and U-220 program ROMs let
several §6 claims be tested against reality rather than inferred from code.

| File | Identity | ID byte |
|---|---|---|
| `roland_t110_u110_u220_waverom0..3.bin` | `T-110   Ver0.08` | `'0'`,`'1'`,`'2'`,`'3'` |
| `roland_u220_waverom4_(sn-u110-08).bin` | `SN-U110-08` | `0x08` |
| `roland_u220_waverom5_(sn-u110-09).bin` | `SN-U110-09 0.27` | `0x09` |

**Confirmed**

- **Four internal wave ROM banks**, numbered `'0'`–`'3'`, exactly matching the
  `CMPB R30,#0x4` scan bound and the 2-bit bank field of §6.2. 4 x 512 KB = 2 MB of
  internal wave.
- **The internal ROMs identify as "T-110"** — resolved: *T-110 was the U-110's
  development codename*. The wave ROM masks kept the working name. Not an oddity, and not
  evidence of a shared part with some other product.
- **Both signature forms exist as predicted**, and the card form's trailing binary
  `B1 53 AC` is present verbatim.
- **Header field offsets `+0x1B` (version ASCII) and `+0x20` (ID)** are exactly where the
  code said they would be.
- **Card IDs are catalogue numbers** — `0x08`/`0x09` for SN-U110-08/-09 — confirming the
  §6.7 reading that patches address cards by ID rather than by slot.

**Refuted**

- **The header is not an anti-clone scramble.** Signatures sit as plain, linear ASCII at
  chip offset 0. The two firmware tables turned out to be compensation for physical bus
  wiring — see §6.4, now resolved.
- **Tone names are not visible in a naive linear dump.** They are nonetheless present:
  once the address permutation was solved, all 99 internal tone records and both
  cartridges' tone lists decoded cleanly (§6.6).

**On U-110 / U-220 sharing wave ROMs**

The sharing is real and total. `waverom0..3` are one set used by the T-110, U-110 and
U-220 alike, and the U-220 program ROM contains a **byte-identical copy of both
permutation tables** (`0x1B100` and `0x1B200` against the U-110's `0x9257` and `0x9357`)
and the **same two signature strings** at `0x1B2AA`. The U-220 therefore drives the same
wave hardware through the same interface — it is a U-110 wave engine in a new box.

What the U-220 adds is not a different wave set but **two more banks**: `waverom4` and
`waverom5` are the SN-U110-08 and SN-U110-09 card sets built onto the main board. The
dumper notes they "match perfectly with respective dumped card data", and their headers
confirm it — they still identify as `RolandU-110 N` cards, ID `0x08`/`0x09`, because from
the firmware's point of view they *are* cards that happen to be permanently fitted.

`[I]` The U-220 program ROM also carries a **second, different** bit-permutation table
(`0xEB7A`, mapping `D0→D3, D1→D2, D2→D0, D3→D4, D4→D5, D5→D1, D6→D7, D7→D6`) that has no
counterpart in the U-110. Presumably it serves a device the U-220 has and the U-110 does
not. Not investigated.

### 6.9 What this means for modification

- **Reading a card from patched code is straightforward** — call `0x7B74` to select the
  bank and `0x7BB2` / `0x7B07` to read. Both are clean, self-contained subroutines.
- **Relaxing card authentication is a small, well-localised edit.** The mount check is a
  16-byte `CMPB`/`DJNZ` loop at `0x7D2C` and `0x6E6E`; the length is an immediate
  (`LDB R51,#0x10`) and the branch target on mismatch is a single `JNE`. Shortening the
  compare or forcing the branch is a one- or two-byte change.
- **Authoring a card image is now within reach.** The ID header format (§6.5), the
  address and data permutations (§6.4) and the tone record layout (§6.6) are all known.
  What is still missing is wave ROM address bits A14-A18, which are needed only to place
  **sample data** — the directory structures are fully addressable without them.
- **The 80-byte tone parameter record is partly decoded** (§6.6): 10-byte name, six header
  bytes, then two structurally identical 32-byte partial blocks. The key-split / sample-index
  reading inside a partial is `[I]` and unproven against playback.
- `[I]` No code was found that *writes* to a card. The `Bulk Dump` / `Tone Bulk Rceiv.` /
  `Wave Bulk Rceiv.` paths are MIDI SysEx, targeting internal RAM. These cards appear to
  be read-only mask ROM.

---

## 7. Differences between v2.00 and v2.03

| Region | Status |
|---|---|
| `0x4002`, `0x4022` | 1 byte each — retargeted reset and EXTINT jumps |
| `0x417D-0xD6C6` | 38,218 bytes — code substantially rewritten |
| `0xE000-0xFFFF` | **byte-identical** — preset patch data unchanged |

36,893 bytes differ overall (56.3%). The `0xE000` region being untouched across a major
code revision is consistent with its role as a factory default patch image (§2.1, §4)
rather than as code or firmware-version-specific data.

---

## 8. Modification capability

### No integrity check stands in the way `[C]`

- **No program-ROM checksum runs at boot.** The service test menu covers LCD, RAM,
  battery, MIDI, `6.WAVE ROM CHECK` (the separate PCM ROMs) and `7.ROM CARD CHECK`.
  There is no self-test over this EPROM.
- The `Chk Sum Err [  ]` string belongs to SysEx/card handling, not a ROM self-test.
- Byte sums are not normalised: v2.00 `sum8=0xD3`, v2.03 `sum8=0x9A`.

### Toolchain is working `[C]`

Ghidra ships an MCS-96 module. Load with:

```
-processor MCS96:LE:16:default  -loader BinaryLoader  -loader-baseAddr 0x0000
```

Ghidra's SLEIGH **assembler round-trips this target**, verified against the ROM's
own bytes:

```
LJMP 0x4362  -> e7 5e 03     (matches ROM at 0x4001 exactly)
ORB RC4,#0x1 -> 91 01 c4     LDB R30,#0x5c -> b1 5c 30
```

So patches can be written as assembly in the listing and exported, not hand-assembled.

### Usable free space — small, and not where a naive scan suggests

| Region | Size | Usable? |
|---|---|---|
| `0x0000-0x00FF` | 256 | **No** — shadowed by the 8097BH register file |
| `0x0100-0x0FFF` | 3840 | **Yes** — `/CS7` decodes the EPROM here and it is blank |
| `0x1000-0x1FFF` | 4096 | **No** — I/O decode (`/CS1`-`/CS6`) |
| `0x2019-0x207F` | 103 | Yes — inside the `/CS7` window at `0x2000` |
| `0x2084-0x20FF` | 124 | Yes — inside the `/CS7` window at `0x2000` |
| `0x2100-0x3FFF` | 7936 | **No** — IC10 work RAM is decoded here (see §2.2) |
| `0xD6C7-0xDFFF` | 2361 | **Yes** — the main patch arena |
| `0xE000-0xFFFF` | 8192 | **No** — factory default patch image, and bank-switched away in normal operation (§2.1) |

**≈6.3 KB total (6,428 bytes)** — substantially more than the ≈2.5 KB this document
reported before the service notes were available.

The gain is `0x0100-0x0FFF`. IC8's published address map decodes `/CS7` — the program
EPROM — across `0x0000-0x0FFF`, and that whole region is `0xFF` in both images. Only the
bottom 256 bytes are lost to the 8097BH's internal register file shadow, leaving **3,840
contiguous bytes** of addressable, blank EPROM sitting below the I/O block. Nothing in the
firmware reads or writes there, which is why a code-flow scan found no references and this
document previously wrote the region off.

Two practical notes. The `0x0100-0x0FFF` arena is separated from the main code body by the
I/O block and the RAM window, so reaching it costs an `LJMP` either way — fine for
self-contained routines, awkward for inline extension. And adding code costs **stack** as
well as space: see the headroom warning in §2.2.

### Practical entry points, easiest first

1. **Text/UI strings** — plain ASCII, e.g. `" U-110  Ver2.03 "` at `0x795B`.
   Same-length overwrite, zero risk.
2. **Preset patch data** — `0xE000-0xFFFF`, structure fully mapped in §4.
   Pure data, no code changes.
3. **Tuning table** — `0xA516`, 16-bit entries. Alternate temperaments.
4. **MIDI message behaviour** — three-byte retarget in the `0x56C3` table, with new
   handler code placed in the `0xD6C7` gap.
5. **Factory default patches** — the EPROM image at `0xE000-0xFFFF`. Editing these
   changes what a "Mem Initialized" restores, *not* what the machine plays day to day
   (that lives in battery-backed SRAM). Users must run the init to see any change.
6. **Card authentication** — the 16-byte signature compare at `0x7D2C` / `0x6E6E`.
   Length is an immediate, mismatch is a single `JNE`; see §6.8.
7. **Tone generator behaviour** — `0x1400` window writes. Highest risk; preserve the
   double-write and the `INT_MASK` critical section.

### Hardware requirements

- A programmer that handles **27C512**, and a blank 27C512 or pin-compatible flash
  equivalent for faster iteration.
- `chip.txt` says DIP-28, so the part is likely socketed.
- **Burn to a new chip and keep the original.** These dumps are the only recovery path.

### Known limits of this analysis

- ~~Static analysis only.~~ **Superseded.** A MAME driver now runs this firmware against
  emulated IC8/IC15/IC26 — see `mame/src/mame/roland/roland_u110.cpp`. It boots to the
  play screen, navigates its menus and mounts PCM cards, so behavioural changes to
  everything except sound are now testable without burning an EPROM. Several entries in
  §9 (#16-#19) came out of it.
- The `0x1400` register semantics are inferred from usage patterns, not from a
  datasheet for IC15. Field-level meaning of most registers is unproven.
- The wave ROM **sample** data cannot yet be extracted: address bits A14-A18 are still
  unknown (§6.4). Everything above the sample data — headers, tone names, tone parameter
  records — is fully addressable.
- The key-split / sample-index reading of a partial block (§6.6) fits the data everywhere
  spot-checked but has not been verified against actual playback.
- The `0x2000-0x20FF` ROM window boundary is still `[I]`. It is inferred from access
  patterns plus the constraint that interrupt vectors must remain readable, and the
  scan behind it is blind to pointer stores (§2.2). IC8's internal decode would settle it.
- Leave the CCB at `0x2018` (`0xFB`) alone unless you intend to change bus timing
  and wait states.

---

## 8.5 Service test mode `[C]`

The firmware carries an eleven-entry service menu. It is reachable, scriptable, and its
Sound Check turns out to be the best distortion reference available for this machine.

### Entry and navigation

At reset the firmware reads the held-key mask once (`0x455B` calls `0xD650`, then
`0x4562` compares) and branches on it:

| Held at power-on | Effect |
|---|---|
| `0x30` = **DEC + INC** | service test menu |
| `0x03` = **PART + EDIT** | memory initialise |

`[C]` **The panel is not read at all until about 3.5 s into boot**, and from then it is
scanned at a steady **100 Hz** from the timer ISR. Anything that drives the keys must be
timed in scans, not in wall-clock time or video frames: a window that starts at reset has
already expired before the first read.

Menu keys come from the scan routine at `0x8F69`, which returns a code per press:

| Code | Keys | Meaning |
|---|---|---|
| 1-6 | single keys | PART, EDIT, LEFT, RIGHT, DEC, INC |
| **7** | LEFT + RIGHT | previous test |
| **8** | DEC + INC | next test |

A two-key code is only produced when both keys are present in the **same scan** — the
routine tests one key's press *event* against the other's *held* state. Frame-aligned input
cannot do this reliably.

### The menu

| # | Title | Notes |
|---|---|---|
| 1 | S-RAM CHECK | reports `RAM1 = OK` / `RAM2 = OK` |
| 2 | LCD CHECK | |
| 3 | KEY&LED CHECK | absorbs key presses as test input rather than advancing |
| 4 | BATTERY CHECK | `E = n.nV Good/Error`, window `0x85`-`0xCB` (§1.1) |
| 5 | MIDI CHECK | needs an OUT-to-IN loopback |
| 6 | WAVE ROM CHECK | **hardware and emulator both report `1.V0.08 2.V0.08 3.V0.08 4.V0.08`** |
| 7 | ROM CARD CHECK | |
| 8 | DAC OFFSET ADJ | toggles `0x1F08` to make a DC square for VR-2 |
| 9 | DAC MSB CHECK | VR-1 |
| 10 | **SOUND CHECK** | steps `VOICE-0`..`VOICE-31`, then CHORUS and TREMORO |
| 11 | OUTPUT CHECK | routes to each of the six jacks in turn |

### The Sound Check reference tone `[C]`

Recorded from a real U-110, every one of the 31 voices plays an **identical pure tone**:

```
fundamental 439.8 Hz          (A4 at A440, -0.8 cents)
h2  -48 dB    h3  -44 dB    h4  -82 dB    h5  -67 dB
```

identical across voices to within 0.1 dB and 0.1 Hz. Two consequences:

- **The machine's own THD floor is about -44 dB.** That is the figure an emulation has to
  reach; anything worse is emulation error, not the instrument.
- **Voice-to-voice variation is nil on hardware**, so any per-voice difference in emulation
  is a defect.

`[C]` **The test waveform is in the wave ROM** at bank 2, `0x56300`-`0x58300`: 8192 bytes of
a smooth sine-like wave, period 101.6 bytes, amplitude +/-34, giving 314.9 Hz at the 32 kHz
engine rate. Playing it at step `0x595F` produces the observed 439.8 Hz.

Decoding that region and comparing with the hardware (the figures below predate correction #30
and were computed with the **linear** reading, so the ROM-side harmonics are the trapezoid's,
not the true decode's):

| | h2 | h3 | h4 | h5 |
|---|---|---|---|---|
| ROM decoded | -47.8 | -40.8 | -24.7 | -54.9 |
| hardware | -48.0 | -44.0 | -82.0 | -67.0 |

**h2 agrees to 0.2 dB and h3 to 3 dB**, which is independent support for the linear decode
(§3). `[I]` h4 and h5 do not agree, but the comparison is not yet like-for-like: this
measures a windowed ROM region rather than the exact loop the firmware plays, and the wrong
loop boundary smears energy upward. Settling it needs the register values the firmware
actually writes during Sound Check.

### The test waveform family `[C]`

Sound Check plays a wave-ROM sample, not a synthesised tone, and the sample table names it.
A cluster of test waveforms sits at the very end of waverom3:

| Sample | start | len | loop | amp | h2 | h3 | h4 | h5 | shape |
|---|---|---|---|---|---|---|---|---|---|
| 213 | `0x7EBA1` | 272 | 268 | 126 | -25.7 | **-0.1** | -20.6 | -0.3 | square |
| **212** | **`0x7ECB1`** | 272 | 268 | 69 | -63.1 | -15.3 | -65.0 | -24.2 | **the one played** |
| 214, 215 | `0x7EEC1`, `0x7EF41` | 128 | — | ~190 | -4.1 | +0.2 | -9.1 | -8.0 | harsh, 128-byte period |
| 221, 216 | `0x7EDC1`, `0x7EE41` | 128 | — | 0 | — | — | — | — | silent |

Sample 212's start is exactly what the Sound Check registers specify, which **independently
confirms the wave-ROM address decode** — bank bits, the bit-10 -> address-18 mapping and the
descramble all reproduce an address the firmware's own table declares.

`[C]` The panel cannot select a different one: the step index chooses only the **voice**
(`STB R54,0x143E`), while the waveform comes from a sample-table lookup at `0xCD76`
(`MULUB R50,#0x0A` / `ADD R50,#0x0100`). All 32 steps play sample 212.

### Test 11 output routing — measured `[C]`

Recorded at the MIX output while stepping the six jacks, the L/R balance moves through a
small set of discrete positions:

| Step | L | R | R-L | L/R corr | reading |
|---|---|---|---|---|---|
| 1 | -83.7 | -17.9 | **+66 dB** | ~0 | hard right |
| 2 | -17.8 | -83.7 | **-66 dB** | ~0 | hard left |
| 3 | -23.9 | -24.0 | **0.0 dB** | **1.000** | centre |
| 4 | -27.6 | -20.9 | **+6.7 dB** | 1.000 | right of centre |
| 5 | -20.7 | -27.5 | **-6.8 dB** | 1.000 | left of centre |

Three things follow:

- The positions are **discrete and symmetric** (+/-66, +/-6.8, 0), which is what a
  bit-per-output select register produces, not a continuous pan law.
- A "hard" pan still leaks ~0.05% into the other channel, consistent with resistor-network
  summing (IC38) rather than a switch.
- Where a jack is centred or partly panned the two channels correlate at **1.000 with zero
  phase difference**, and broadband lag measures 0 samples. **There is no inter-channel
  delay**: the slot multiplexing performs routing and nothing else audible. An earlier
  reading of the whole-file 0.466 correlation as evidence of slot timing was wrong -- it is
  an artefact of averaging across differently-panned steps.

### Test 8 proves the analog path is clean `[C]`

Test 8's tone is not a sample: the CPU makes it by toggling the `0x1F08` output enable at
`0x8B5B`, so it enters at the analog switches and passes the filters and summing network.
Its harmonics land on the ideal square-wave 1/h law:

| harmonic | 3 | 5 | 7 | 9 | 11 |
|---|---|---|---|---|---|
| Hz | 392 | 653 | 914 | 1176 | 1437 |
| measured | -9.6 | -14.1 | -17.0 | -19.4 | -21.3 |
| ideal 1/h | -9.5 | -14.0 | -16.9 | -19.1 | -20.8 |

Within **0.5 dB up to 1437 Hz**, evens absent at -52 dB. **The analog output stage is flat
well past 1.4 kHz** -- which matches the Fig. 4 component values (a 5th-order anti-aliasing
filter cornering near 7 kHz, see `SYSTEM-DESIGN.md` §5).

### The transformation — **SOLVED: it was the decoder** `[C]`

For a long stretch this section recorded an unexplained gap: the ROM appeared to hold a
trapezoid while the hardware emitted a near-perfect sine, with odd harmonics suppressed by
29-50 dB, and the list of ruled-out causes kept growing:

| Candidate | Status |
|---|---|
| Wave-ROM address | Confirmed by the sample table (sample 212) |
| Sample rate | 26 ppm |
| Analog filter | Flat past 1.4 kHz, measured via test 8 |
| Slot multiplexing | Routing only — zero delay, unity correlation, measured via test 11 |
| Voice-status handshake | Modelled; no effect on the sample selected |
| **Data decode** | ~~"Emulator output matches the ROM bytes"~~ — **this was the answer** |

**There was no transformation.** The ROM never held a trapezoid; the trapezoid was an artefact
of reading a companded byte as linear PCM. Decoded correctly (§3) sample 212 *is* a sine, at
correlation 0.99975 with h3 at -66.7 dB, and the hardware's residual -44.1 dB h3 is its own
analog distortion — pinned by the even harmonic h2 at -47.6 dB, which symmetric data cannot
produce.

`[I]` The line in that table reading "emulator output matches the ROM bytes" was doing real
damage. It was **true and useless**: the emulator did match the bytes, and both were wrong in
the same way, so the check could never fail. It sat in the ruled-out column for weeks and
quietly protected the one candidate that mattered. A verification that compares a system
against itself excludes nothing.

The h^-3 curiosity previously logged here is likewise resolved and withdrawn: `n ~ 3` was
never physical, it was the numerical shadow of the missing expansion. Nothing about IC16 needs
to be invoked, and the suspicion recorded against it here is lifted.


---

## 9. Corrections log

This analysis was built in layers: first from the ROM images alone, then against real wave
ROM dumps, then against the schematic, then against the factory service notes. Several
early conclusions were wrong. They are recorded here because the *reasons* they were wrong
are reusable, and because anyone continuing this work should know which claims have been
stress-tested and which have not.

| # | Claim | Verdict | Why it was wrong |
|---|---|---|---|
| 1 | CPU is an "8098-class 48-pin" part | **Wrong** — it is an **8097BH** | Reasoned that one 64K x 8 EPROM implied an 8-bit bus, hence the 48-pin part. MCS-96 samples `BUSWIDTH` per cycle, so a 16-bit-bus CPU drives a byte-wide ROM happily. A valid observation led to an invalid narrowing. |
| 2 | ~10.5 KB of free space | **Wrong** | A naive `0xFF`-run scan counted `0x2100-0x3FFF`, which is RAM at runtime. |
| 3 | ~2.5 KB of free space | **Wrong** | Corrected #2 but missed that `/CS7` also decodes the EPROM at `0x0000-0x0FFF`. A code-flow scan cannot find a region no code references — and nothing references it because it is *blank*, not because it is unreachable. **Current figure: ~6.3 KB.** |
| 4 | The card ID header is deliberately scrambled as an anti-clone measure | **Wrong** | Real dumps show the signatures as plain, linear ASCII at offset 0. The two permutation tables are hardware compensation, not obfuscation. |
| 5 | Tone names are "stored plain" at card address `0x100` | **Withdrawn** | No readable tone names exist anywhere in any of the six wave ROM dumps. The *code* is unambiguous; where those addresses land physically is not. |
| 6 | The A/D converter is unused | **Wrong** — it reads **battery voltage** on channel 0 | Grepped for `AD_COMMAND`/`AD_RESULT`, names Ghidra's MCS-96 module does not define, and read the empty A/D vector as "idle". Polling is exactly *why* that vector is empty. |
| 7 | The machine has fewer than four card slots | **Wrong** — there are **four** | Assumed the firmware's 4-slot scan was a harmless superset. The block diagram says `PCM CARD x4`. |
| 8 | Wave ROM data goes to IC15 | **Wrong** — it goes to **IC16** | Assumed the chip that owns the register window also owns the data bus. IC15 generates the address; IC16 receives the data. |
| 9 | The bus is strapped 8-bit | **Wrong** — it is **dynamically sized** | `BUSWIDTH` is an output of IC8's decoder. Memory cycles run 8-bit, IC15 cycles 16-bit. |
| 10 | `0x1000`/`0x1010` are I/O accesses | **Wrong** | They are packed cursor arguments to the string-print routine at `0xD3A0`. Harvesting operand scalars to find hardware addresses produces false positives. |
| 11 | The card ID header path is an unresolved contradiction | **Wrong — resolved** (§6.4) | I computed `perm[dump[addr[i]]]`, got garbage, and reported an unexplained inconsistency. The model was missing a stage: the CPU never sees the raw chip byte, it sees `inv[chip_byte]` from the hardware data permutation, and `perm[inv[x]] = x`. I had *both* halves of the answer written down in adjacent sections and failed to compose them. |
| 12 | "No readable tone names exist in any dump" | **Wrong** | All 99 are there, and always were. The search was looking through the wrong address map. An absence of evidence claimed against an unvalidated model is not evidence of absence — the tone *parameter* records were equally invisible, and those provably had to exist. |
| 13 | "Engine sample rate is 32 kHz, confirmed" | ~~Wrong~~ — **this correction was itself wrong, see #22.  32 kHz was right.** | I derived 32 kHz from `34,816,000 / 1088` and treated the circulating 32 kHz WAVs as confirmation. MAME's hardware-derived formula is `clock / 2 / 512` = **34,000 Hz**. Both divisors give exact results from this crystal, which is what made the numerology persuasive. The WAVs are resampled, not native. |
| 14 | The published chip-bit → CPU-bit data table (SYSTEM-DESIGN §4.3) | **Wrong as printed** | Transcribed the `0x9257` table's forward values under inverse labels. The result had D2 as a target twice, so it was not even a permutation — a check I should have run on my own table. No computation was affected; they all used the array. |
| 15 | "The decode is stateful, not a lookup" | ~~Wrong~~ — **this correction was itself wrong, see #16** | I inferred statefulness from the WAVs showing 40,191 distinct values where a byte lookup allows 256, then talked myself out of it because resampling also explains dense values. The *reasoning* was indeed weak — but the conclusion it discarded was correct. |
| 16 | "The sample format is memoryless" (entry #15, and §3 as written) | **Wrong** — it is **differential** | I read MAME's decoder as a lookup and missed that `roland_lp.cpp` does `chn.smpl_nxt += decode_sample(...)`. One `+=`. Worse, I had already reached the right answer once and overturned it. When a correction discards a conclusion, the bar should be evidence *for* the replacement, not merely a flaw in the original argument. |
| 17 | "Sample encoding: **SOLVED**" (§10, and the emulator plan's headline) | **Overstated** | MAME implements a decoder and simultaneously documents it as broken — a `±0x7FF` clamp captioned *"until the decoding is fixed"*, ping-pong formulae marked *"probably incorrect"*, and a `MACHINE_IMPERFECT_SOUND` flag. I read the presence of an implementation as proof of a solution. Measured correlation against the reference WAVs is **0.32**, not ~1.0. |
| 18 | Empty card slots cache `0xFF` (§6.5) | **Wrong** — they read `0x00` | Emulated with two cards fitted, `0x2743` reads `08 00 00 09`. `0xFF` is written on **mount failure**, not on absence: an empty slot is skipped via its `PORT1` presence bit before the ID is ever stored, leaving the cleared value. |
| 19 | "The key-split / sample-index reading is unproven" (§6.6, `[I]`) | **Now proven** `[C]` | It needed no playback. The nine sample-table entries tone 1 references carry reference notes `48 55 64 66 71 79 89 94`, ascending in lockstep with the eight split points. Promoted to `[C]`. |
| 20 | "The reference WAVs are an oracle worth 0.32 correlation" (implementation plan §1.2) | **Wrong** | A 4,096-frame window of decoded sample 0 peaked at 0.318 against the WAV, ~9x the 0.037 noise floor, and I read that as a usable alignment. It is not. Using that alignment to solve for the byte->delta map directly gives incoherent ratios (-0.04 to +0.77), and the implied sample start reads -5075 where a zero-seeded accumulator must begin near zero. Sweeping all 226 samples against the WAV start finds nothing above 0.037. **Without knowing the extractor's method these files are not a byte-aligned oracle.** A correlation well above noise but far below unity is not "nearly aligned"; it is unexplained. |
| 21 | "Sample encoding is differential" (#16) | **Unconfirmed** | #16 correctly showed MAME *implements* an accumulator, and that remains a fact about MAME. Whether the **hardware** is differential is still open. A looping sample's deltas must sum to zero or the waveform steps every cycle, yet MAME's decoder closes only 22% of loops, and seven decoder variants all scored 21-33% — if the model were right, the correct decoder should close nearly all of them. Meanwhile loop-seam continuity under a *memoryless* reading beats a random-point null (66% vs 42%). The test is also confounded: it assumes MAME's loop handling is right, which MAME itself calls *"probably incorrect"*. Decoder and loop model are entangled and need solving together — the same trap as the address/data permutations in #11. |
| 22 | "The engine runs at 34 kHz" (#13) | **Wrong** — it is **32 kHz**, as first derived | #13 threw away a correct derivation (`34,816,000 / 1088`) because MAME's CM-32P formula (`clock / 2 / 512`) disagreed, and dismissed the 32 kHz reference WAVs as resampled. Playing identical MIDI into real hardware and into the emulator showed the emulator **+104 cents** sharp, and `1200*log2(34000/32000) = +104.96`. The crystal is confirmed at 34.816 MHz on the schematic, so the **divisor** differs from the CM-32P, not the oscillator. Two lessons: a formula that is right for a sibling machine is not evidence about this one, and the "resampled" dismissal of the WAVs was reasoning backwards from the conclusion. |
| 23 | "Sample encoding is differential" (#16, #21) | **Wrong** — it is **linear two's complement PCM** | Settled against a real U-110 recording: direct PCM matches at r = 0.756, the differential reading at 0.258.  #16 was right that *MAME* accumulates; wrong that the *hardware* does.  The distinction between "what an implementation does" and "what the machine does" is the whole of #17 restated, and I made it again. |
| 24 | "The format is 8-bit logarithmic / companded" | **Not supported** | A plausible external description, and it fit the 1-3-4 (mu-law-shaped) field layout attractively.  But fitting power-law and mu-law expansion curves against hardware harmonic profiles improved the error only from 13.2 to 11.6 dB rms.  No memoryless curve explains the residual, so the companding reading is unsupported by measurement. |
| 25 | "The emulator's noise floor now matches the reference" | **Withdrawn** | Claimed after comparing spectra -- but the emulator output being measured was **clipping** (a x64 scaling error put one voice at -1 dBFS).  Clipping generates broadband harmonics, so the measurement described my own bug.  Never characterise a signal before checking its headroom. |
| 26 | "The reference WAVs are resampled, so their low noise floor is an artefact" | **Half right** | They *are* interpolated (43,830 distinct values, gcd 1), so they are not a byte-aligned oracle -- #20 stands.  But the inference that their 32 kHz rate implied resampling was wrong: 32 kHz is the native engine rate (#22).  One true observation, one false conclusion, from the same sentence. |
| 27 | "The sample format is differential", reopened from outside — a website describing the U-110's audio as **"LDPCM"** | **Wrong again**; the *linear* half is right | Third-party spec text is not independent evidence. This is the fourth pass over the same ground (#16, #21, #23), and the term most plausibly expands to "Linear Differential PCM" — one word right, one wrong. Two new tests settle it without appeal to MAME or to the reference WAVs: sample 212's loop **sums to -268, not 0** (a differential loop must close), and FLUTE 1 under direct PCM yields a harmonic series **at its own declared reference note** while integration collapses it to DC. Lesson: when an external description conflicts with a measurement, the measurement wins — but *re-derive it from a fresh direction* rather than re-citing the old argument, because the value of the challenge is the new test it provokes. |
| 28 | "FLUTE 1's opening bytes are a smooth small oscillation" (§3, as the waveform-shape evidence) | **Bad argument, right conclusion** | `0, 0, 0, +13, -15, +3, +17, -21, +28, -32` is **64% sign-reversing** — near-Nyquist energy, the opposite of smooth — and the sustain still reverses 37% where a flute at `ref = 76` should reverse ~4%. The run is small-amplitude quantisation noise around zero crossings, which proves nothing about the decoder either way. §3's conclusion held up, but for years it rested partly on a bullet that any careful reader would have found unconvincing. **A correct conclusion does not sanctify the argument that reached it**; bad supporting evidence is a liability, because the next person to check it concludes the whole section is soft. Replaced with the harmonic-series test. |
| 29 | "One DAC serving **eight destinations**", and a planned demux built on a per-voice output-assignment register (§5.1, implementation plan) | **Wrong on both counts** | Prompted by a reader's objection that the unit has only six outputs. It does, and Fig. 3 never said otherwise: the "8" is the **address counter** `ADD 0-7`, and the figure draws a single representative switch. Eight *time slots*, six *outputs*. Worse, the planned model had voices individually assigned to outputs and listed the assignment register as the "blocking unknown" — **it does not exist**. `0xB721` block-copies eight bytes from a 50-entry table at `0xA8B6` indexed by patch byte `+0x0E`; routing is per-**slot**, and a voice reaches an output via its voice number. Two lessons. First, I had quoted Fig. 3's caption for months without reading the figure it captions — the drawing contradicts the sentence, and the Japanese column even flags the "0-8" as a typo for 0-7. **Quoting a caption is not reading a figure.** Second, the "blocking unknown" framing kept a search alive for a register that was never there; the answer came in twenty minutes from disassembling the code that writes the port, which should have been the first move, not the last. |
| 30 | "Sample format is linear 8-bit two's complement PCM" (#23, §3) | **Wrong** — it is an **8-bit float**, sign + 3-bit exponent + 4-bit mantissa, applied to the magnitude of the two's complement byte | Prompted by a reader looking at a plot of sample 212 and saying it was not a trapezoid at all but "a sine that needs to be decoded differently", noting the steps bunch together near the peaks. They were right. The decisive move was realising the target was *known*: the firmware plays this sample as a pure sine, so the transfer curve could be **read off** instead of guessed — decoded/ideal holds constant to 2.5% over a 14:1 range, against 32% for linear. Four rounds of this section were each half-right, and the reason they never converged is the same entanglement #11 and #21 both recorded and neither escaped: MAME's decoder combined a **correct** float table with an **incorrect** accumulator in one expression, so #23 tested the pair, found it worse than linear, and discarded both halves. **When a compound hypothesis fails, the failure does not distribute over its parts.** #24 then rejected "companded" — a *true* description — because it fitted mu-law and power curves to hardware harmonics rather than testing the one table already sitting in the source tree. The whole answer was one character: `+=` should have been `=`. |
| 31 | "Voice volume (regs `06`/`07`) is a linear 16-bit multiplier" (MAME's `smp_data * chn.volume`) | **Wrong** — reg 07 is **logarithmic, 16 units per octave**, and the two bytes are independent fields | Found by disassembling the writer rather than fitting curves. The firmware builds the value at `0x69F0`-`0x6A59` and stores it with a single `st 44, 140c`, but `6A27: ldb 45, 42` loads the high byte from a **different register**: the low byte (reg 06) is a 7-bit level clamped to 1..0x7F, the high byte (reg 07) is a separate log-domain level. The scale comes from the firmware's own table at `0xAEC6`, which reads 143, 159, 175, 191, 207, 223, 239, 255 at indices 1, 2, 4, 8, 16, 32, 64, 128 — **every doubling of amplitude adds exactly 16**, so 16 units per octave = 0.3763 dB per unit. Notes use a **voice pair** whose two partials are layered by velocity (at velocity 40 the second partial's reg 07 is `0x30` against the first's `0xC2`) and sum. Read linearly a velocity sweep spanning **21.3 dB** on hardware collapsed to **5.3 dB**; decoded this way the emulator now spans **20.1 dB**, within **1.3 dB** of hardware at every measured velocity. Two lessons: velocity touches **only** regs 06/07 — sample address, rate, loop and end are byte-identical, so measuring what does *not* change localised the field immediately; and after three failed attempts to fit a curve through three data points, **disassembling the code that writes the register took minutes and gave the exact scale**, the same shortcut that was available (and missed) for the `0x1F00` routing table. `[I]` The role of the low byte, and a residual ~1.3 dB, remain open. |

Claims that **survived** contact with the schematic and service notes, having originally
been derived from the ROM image alone: the MCS-96 identification, the `0x2000-0x20FF` ROM
window, work RAM starting at `0x2100`, the P2.7 bank switch and its meaning, the 12 MHz
crystal (from the baud divisor), MIDI on the hardware serial port, the MIDI activity LED
on P2.6, `PORT1` bits 0-3 as card presence, the four internal wave ROM banks, the
time-multiplexed single-DAC output stage, and every header field offset in §6.5.

### Recurring failure modes

**Inferring a specific part from a general constraint.** #1 and #7 are the same mistake:
a real observation ("one byte-wide ROM", "a 4-iteration loop") was narrowed to a specific
conclusion that a second mechanism (dynamic bus sizing, a card board with four slots)
invalidated.

**Reporting a contradiction instead of composing what I already knew.** #11 is the worst
of these. Both the address map and the data permutation had been derived and written down
in adjacent subsections; the "contradiction" existed only because the simulation applied
one and not the other. When a model produces an impossible result, the first suspect
should be a missing stage in the model, not the data.

**Concluding absence from a failed search.** #12 followed directly from #11. Several
increasingly elaborate searches — 10-byte stride, 0x50 stride, charset indices, brute-forced
address bits — all failed, and the failure was reported as evidence about the ROM rather
than about the search. The tell was available and noted at the time: the 80-byte parameter
records were equally unfindable, and those had to exist or the instrument could not sound a
note. **What broke the problem was neither a better search nor more schematic detail, but
known plaintext** — the tone names printed in the owner's manual.

**Treating someone else's derived output as raw evidence.** #13 and #15 both came from the
circulating WAVs. They are a resampled, possibly filtered rendering, and I drew conclusions
about the *hardware format* from properties that the resampling introduced. Their own
documentation said only "decompressed waves for your sampling delight" — no method, no
provenance. Derived data needs its derivation known before it can carry an argument.

**Overturning a right answer with a worse one.** #15 and #16 are the same event seen twice.
I concluded the decode was stateful, found the supporting argument weak, and retracted the
*conclusion* along with the argument — landing on "memoryless", which was flatly wrong. A
weak reason for a claim is not evidence against the claim. A correction needs positive
evidence for its replacement, and this one had none; one `+=` in the source I was already
citing would have settled it.

**Trusting a sibling machine's constant.** #22. MAME's `clock / 2 / 512` is correct for the
CM-32P, so I applied it to the U-110 and discarded a derivation made from this machine's own
crystal. Same chip, same family, different divisor. A formula verified elsewhere is a
hypothesis here, not evidence — and it cost a full semitone of tuning error that survived
several rounds of "verification" because everything downstream was measured against the same
wrong rate.

**Measuring my own bug and reporting it as a property of the system.** #25. I characterised
an emulator noise floor without checking headroom; the signal was clipping, so the spectrum I
described was my scaling error, not the machine. Check the level before believing the
spectrum.

**Iterating on plausible stories instead of getting ground truth.** The decode went
memoryless -> differential -> bit-permuted -> companded -> linear across many rounds of
metric-fitting, and *none* of the intermediate metrics settled it. What settled it was one
recording from a real U-110 and one stereo comparison file from its owner. Two of the three
decisive facts in this document (the sample format, 32 kHz) came from hardware, and both contradict
conclusions I had reached and defended from the data alone. When ground truth is available at
moderate cost, get it before the third hypothesis, not after the fifth.

**Reading an implementation as a solution.** #17. MAME had code for the sample decoder, so I
recorded the problem as solved — while the same file clamped its accumulator under the
comment *"until the decoding is fixed"*, called its loop maths *"probably incorrect"*, and
shipped flagged `MACHINE_IMPERFECT_SOUND`. Existing code answers "has someone attempted
this", not "is this correct". The caveats were in the same screenful as the algorithm.

**Trusting a tool's labels as if they were the architecture.** #6 and #10 both came from
grepping Ghidra output. Ghidra's MCS-96 module names dual-function SFRs by their *read*
side and does not define the A/D registers at all, and any scalar in an operand can look
like an address. Both are fixed by checking the disassembly at the byte level (§1.1).

---

## 10. Open questions

| # | Question | Status |
|---|---|---|
| 1 | ~~Where does the tone name / parameter table physically live?~~ | **SOLVED** (§6.4, §6.6). The address permutation is determined for bits 0-13 and all 99 tone records decode. Cracked with known plaintext from the owner's manual's Preset Tones Chart. |
| 1b | ~~Wave ROM address bits A14-A18~~ | **SOLVED** via MAME's CM-32P work: logical A14→chip A16, A15→A14, A16→A15, A17→A17, A18→A18. All 14 bits derived here matched MAME exactly. |
| 1c | ~~Sample encoding~~ | **SOLVED**: 8-bit float — sign + 3-bit exponent + 4-bit mantissa on the magnitude (§3). Companded after all; MAME's `decode_sample()` was right, its `+=` was not. |
| 2 | Field layout of the 80-byte tone parameter record | **Partly decoded** (§6.6): 10-byte name, header params, then two structurally identical 32-byte partial blocks. The key-split / sample-index reading is now **confirmed** `[C]` by the sample table's reference-note field (correction #19). Byte `+0x07` of a sample-table entry is a fine-tune centred on `0x40`; `+0x09` is still `[I]`. |
| 3 | Field meaning of the eight output-routing bytes at `0xA8B6` | A listening test stepping patch header byte `+0x0E` would settle it. |
| 4 | What `PORT1` bit 4 drives | Set/cleared around card operations (`0x6AFE`, `0xB759`, `0xC0EC`). |
| 5 | Why 16-bit writes to the `0x1400` window are duplicated | Documented as mandatory because the firmware does it universally; the mechanism is unexplained. IC15 has a true 16-bit port, so it is not byte-splitting. MAME's device may shed light. |
| 7 | Chip registers `0x10`, `0x19`-`0x1D` | Marked `??` in MAME too. `0x12`/`0x16` are now known to be the high halves of the 16-bit voice-enable registers (§3). `0x10` takes small ascending values as voices are allocated and is not a bitmask. |
| 8 | **Excess harmonic distortion in emulation** | Open (§3). 8-19 dB more noise+distortion than hardware, with upper harmonics 7-27 dB high. Companding, filtering, chorus, keymap and volume stepping all ruled out. Leading suspect: voice allocation/release. **Target measured**: hardware THD floor is -44 dB (§8.5). |
| 10 | **Test 8 does not advance in emulation** | Open (§8.5). Real hardware passes straight through *DAC OFFSET ADJ*; the emulator sticks there permanently. Its loop only toggles `0x1F08`, delays, and polls keys — so the output-enable write path or the delay differs. Blocks reaching test 10 in emulation. |
| 9 | Per-voice output assignment | Open. Needed for the six-bus output demultiplex. `0x00`/`0x01` and `0x1A` ruled out; `0x10`, `0x19`, `0x1B`, `0x1D` remain. |
| 6 | The second permutation table in the U-220 firmware (`0xEB7A`) | No U-110 counterpart. Presumably serves hardware the U-220 has and the U-110 does not. Not investigated. |

Nothing in this list blocks the modification paths in §8 — those are all reachable today.
