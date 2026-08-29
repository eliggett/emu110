# Working state — envelope; ping-pong / delta decode

Snapshot of the debugging context, so it survives a compaction. The settled findings live
in `analysis/ROM-ANALYSIS.md`; this file is the *scaffolding* around them.

## Current work: the missing decay envelope (2026-08-25)

**Found: the envelope is CPU firmware, and MAME never runs it.** `0x140C` is not a volume
pair — reg 07 is the envelope's **target level** (log, 16 units/octave) and reg 06 is the
**signed ramp rate** (`+1..+0x7F` up, `0x80..0xFF` = `-128..-1` down). The chip ramps toward
the target and raises `EXTINT`; the handler at **`0x41C4`** reads the voice index from chip
reg `0x00`, reads that voice's current level back through the status port, and writes the
next `(target, rate)` pair. Six phase handlers (`0xB932`, `0xBAF4`, `0xBD42`, `0xBE7F`,
`0xBEBD`, `0xBFAC`) are the envelope segments, with key scaling about key `0x45`.

`roland_lp.cpp` calls `m_int_callback(CLEAR_LINE)` at reset and never asserts it, and has no
per-voice level, ramp or status port. Measured: **490 writes to reg 06/07 for 228 note-ons**
over the whole 392 s reference session — the two identical stores at `0x6A59`/`0x6A5E` and
nothing else. Every voice holds its note-on target forever.

Full write-up in `analysis/ROM-ANALYSIS.md`, "The amplitude envelope lives in the CPU".

### Measured on hardware (2026-08-25) — see `listen/env/ANALYSIS.md`

* The ramp is a **straight line in dB**: 0.30 dB rms residual over 36 dB of fall, against
  66 dB rms for a linear-amplitude model. The chip ramps a log-domain level.
* **Speed doubles every 8 counts of the rate byte.** ENV RELEASE (which moves the byte by 8)
  halves the time at every step — ratios 2.02, 2.05, 2.05, 2.00, 2.03, 1.99, 1.99 on organ,
  and the same on vib and choir. ENV ATTACK (16 per step) gives ~4x per step. So
  `speed = k * 2^(rate/8)`, with k ~ 7 dB/s at rate 0.
* The **release rate scales with the target level** the same way the attack does:
  ~`(reg07 * 62) >> 8` on Brass 1.
* **No hold-time correction and no key scaling of the release** — both terms are in the
  code, both measure flat to +/-3% on a flat-sustain tone. They are zero for these tones.

### Settled by the follow-up take (`listen/env2`)

* **The attack is linear in AMPLITUDE** (0.14 dB rms over 29 dB), decay and release are
  exponential (0.24-0.70 dB rms in dB). Linear attack, exponential decay/release.
* The rate byte is a **pure exponential, 8.01 counts per doubling** — 2.4 % residual over 41
  trials spanning `reg06` 2..40 with consecutive integers. A 3-bit-mantissa float gives
  15.4 % and a coefficient of 0.80 where 1.00 is required: rejected.
* **Calibration**: `T = 2^(reg07/16 - reg06/8 - 10.886)` s, within 1 % at `reg06` = 8, 24,
  40. That is a **20-bit linear level incremented by `2^(reg06/8)` once per engine sample
  (32 kHz)** — which predicts -10.904 against the measured -10.886.
* So the exponential release is **the CPU's doing**, not the chip's: `0x42B3` adds
  `(3986[voice] * reg07) >> 9` to the rate with `reg07` refreshed from the level readback,
  i.e. rate proportional to current level. A linear ramp plus a working EXTINT should
  produce the measured exponential on its own — that is the test when it is implemented.

### Implementation status (2026-08-28)

In `roland_lp.cpp/h` behind `set_env_engine()`, **off by default** (the driver's `ENV_ENGINE`
constant switches it and `snd_r` together). With it off the render is **bit-identical** to
the pre-session build.

The firmware's own envelope now runs, and half the tones are right. Decay over an 8 s hold,
dB fallen at note-off, against `listen/env`:

| tone | hardware | emulator | | tone | hardware | emulator |
|---|---|---|---|---|---|---|
| flute | -5.0 | **-5.1** | | fbass | -71.8 | -0.9 |
| brass | -3.9 | **-4.1** | | vib | -67.4 | -7.4 |
| choir | -3.0 | **-2.9** | | marimba | -63.9 | -4.5 |
| strings | -2.8 | **-2.6** | | slap | -61.3 | -4.6 |
| shaku | -2.4 | **-2.3** | | piano | -45.9 | -26.2 |
| organ | -0.9 | **-1.1** | | bell | -25.2 | -8.5 |

Every sustaining tone is within 0.2 dB. Before this work all twelve read 0.0.

Three things had to be found to get there:

* **The scanner was being switched off by the chorus.** Registers 0x19/0x1B/0x1D looked like
  scanner configuration -- 0x00/0x21/0x64 written together at 0x43DA, and 0x21 is
  suggestively the top slot number -- but the same trio is rewritten at 0xB4C6 from the
  patch's CHORUS/TREMOLO byte (0x280E) through a table at 0xA726, beside code writing
  0x378C/0x378E, which are exactly the RAM the handler's 0x20/0x21 branches read. Driving
  the scanner from 0x1D killed the envelope whenever the firmware retuned the chorus.
* **The interrupt is arrival-driven, not round-robin.** The phase advance at 0xBAD4 is an
  unconditional `incb`, so one interrupt is one envelope step. A round-robin over 34 slots
  would need ~11000/s to give a piano its attack, which the CPU cannot service; arrival
  driven, a note needs about four. The "interrupt storm" that made this look wrong the first
  time was the MCS-96 level-7 bug, since fixed.
* **Register 0x1A is a fourth status select, and it reads the PLAY ADDRESS, not a level.**
  0x71AA selects a voice there, reads 0x1404 and compares it with 3720[voice] -- and
  3720[voice] is written at 0x66BA from the same word just sent to register 0x0E, the sample
  LOOP address. The question is "has this voice reached its loop point", which gates several
  phases. Implementing it as a level (the first guess) left every sustaining tone 1-2 dB
  short; as an address they land within 0.2 dB.

### Why it is still off

**The release stalls part way, so notes hang audibly between notes.** After note-off the
level drops 20-30 dB in the first quarter second and then sits there: piano goes -15 -> -41
and creeps only to -51 over the next 3 s; vib flattens at -37 and stays. Hardware reaches
60 dB down in 0.07 s (organ, brass), 0.09 s (piano), 0.88-1.16 s (strings, choir).

Voices are NOT leaked -- an earlier note here said they were, wrongly. The level just before
every new note-on reads -180 dB, digital silence: the firmware force-silences the voice
(0x7217 writes target 0, rate -128) when it needs it again, so nothing accumulates, no notes
are lost, and the decay figures above are not contaminated by earlier notes.

**Half of this was a MAME CPU bug, now fixed.** Opcode 5F, `mulub dst, src, offset[reg]`,
read the indexed operand and then overwrote it with the register instead of multiplying --
`TMP = reg_r8(OP2)` where every sibling has `TMP *= reg_r8(OP2)`.  The U-110 computes its
release rate with exactly that instruction at 0x64A0, `mulub 4a, 44, afc6[48]`: 96 * 66 came
back as 96, the rate clamped to its minimum, and every release stalled.  With the multiply
restored the same note-off produces a rate of **-59** instead of -2, and the percussive
tones decay further (vib -7.4 -> -10.5, slap -4.6 -> -7.5, fbass -0.9 -> -2.9).

**The status word carries the RATE in its low byte, and returning 0 there stalled every
release.** Select 0x16 hands back the voice's whole current segment: the level reached in the
high byte, the rate it is running at in the low byte. The release handler at 0xBEBD reads
that word, scales the rate down through 0xC0C4 (`shrab 42, #01`) and writes it back -- which
is how an exponential release is built out of linear segments. The proof that the low byte is
meaningful is in the sustain handler, which has to `clrb` it at 0xBE99 to get a rate of zero.
With it fixed, **every tone's release now completes**: all twelve reach 60 dB down, in
0.08-0.17 s against hardware's 0.07-1.16 s, and voices are freed instead of hanging.

The decay chain is now visible and its SHAPE is right. Piano steps its target down in 16-unit
(6.02 dB) increments with the rate halved each time -- a piecewise-linear exponential --
totalling about 48 dB against hardware's 45.9 dB.

### The decay: sustain level and rate bytes are right, the falling RAMP is not

The sustain level and the transition were the obvious suspects and both turned out innocent:

* `0xBD61` reads the sustain level from `36e0[voice]` and `0xBD76` transitions to the hold
  at `0xBDB4` when the decay reaches it.  For A. Piano 1 that level is **0**, computed at
  `0xBC0E` from tone parameter `0x28AB`, which really is `0x00` in the cached tone record.
  A piano decaying to nothing is correct, so there is no missing transition.
* The rate bytes were checked instruction by instruction with `db@`: key 36, key-scale
  nibble 12, tone parameter `0x28AC` = 56, key-scale term -13, giving **43** -- exactly the
  -43 the decay uses.  The firmware's arithmetic is right.

What is wrong is the ramp itself, and only downwards.  The RISING ramp is pinned hard by the
hardware attack times -- emulator 2.142 / 0.518 / 0.116 s against 2.164 / 0.540 / 0.122
measured -- but the same scale applied downwards runs every decay about **4x too fast**:

    piano, dB below peak    0.1    0.2    0.4    0.8    1.6    3.2    7.9 s
    hardware               -2.3   -2.9   -6.3  -12.9  -23.4  -31.6  -45.4
    emulator               -7.1  -11.1  -16.6  -29.6  (silent from 0.8 s)

Hardware also DECELERATES -- 17 dB/s early, 3 dB/s late -- where the emulator holds a
constant ~60 dB/s.  That is the signature of a multiplicative fall, and it is exactly what
the original hardware analysis concluded: **rising ramps are linear in amplitude, falling
ones are exponential** (0.14 dB rms vs 2.36 for the rise; 0.30 vs 66 for the fall).  Both are
currently implemented as linear, which is right for one direction and wrong for the other.

A single divisor does not rescue it: 4 puts Bell almost exactly on hardware (settles -24.3 vs
-24.2) but leaves piano and vib 2-3x fast and does nothing for marimba, which barely decays.
`ENV_FALL_DIVISOR` is left at 1 rather than baking in a number that fits one tone.

### Solved: the envelope was quantised to MAME's sound-stream period

Two faults, found by calibrating against the ENV RELEASE sweep.

**1. Segments were quantised to 20 ms.** The level only advanced inside
`sound_stream_update`, so the arrive -> interrupt -> next-segment cycle could complete at
most once per stream update.  Every release segment took a flat 0.0200 s whatever rate the
firmware asked for, capping the chain at fifty segments a second.  The release saturated at
~316 dB/s against 5 to 1200 on hardware.  Fixed by updating the stream from the envelope
timer before offering a slot.

**2. A falling ramp moves 16x slower than a rising one** at the same rate byte -- the rise
adds `2^(rate/8) << 6` to the level, the fall subtracts `2^(rate/8) << 2`.  Measured, not
fitted: with the quantisation gone the emulator/hardware ratio was a flat 15.0-15.7x across
five ENV RELEASE settings spanning a 15x range of rate.

Against the hardware sweeps the release now tracks across its whole range:

| ENV RELEASE | -7 | -6 | -5 | -4 | -3 | -2 | -1 | 0 |
|---|---|---|---|---|---|---|---|---|
| hardware dB/s | 5.5 | 10.4 | 20.8 | 42.6 | 85.1 | 172.5 | 343.9 | 684.0 |
| emulator | 4.8 | 9.6 | 19.4 | 39.0 | 78.4 | 159.3 | 319.5 | 627.5 |

A constant 0.92x over 250x of range -- one small scale offset left, well inside the 5%
uncertainty already noted on `ENV_RATE_SCALE` (the regression gave 60.6, rounded to 64).

And 8 of 12 tones now match on both decay and release:

| tone | decay hw/emu | release hw/emu | | tone | decay hw/emu |
|---|---|---|---|---|---|
| shaku | -2.4 / **-2.4** | 0.22 / **0.23 s** | | organ | -0.9 / -1.7 |
| brass | -3.9 / **-4.0** | 0.07 / **0.07 s** | | flute | -5.0 / -5.4 |
| choir | -3.0 / **-2.9** | 1.16 / **1.18 s** | | bell | -25.2 / -26.0 |
| strings | -2.8 / **-2.7** | 0.88 / **0.91 s** | | piano | -45.9 / **-46.5** |

Median error 0.7 dB, against every tone reading 0.0 before this work started.

### 10 of 12 tones are correct -- two of the four "failures" were the metric

`decay at note-off` is measured against the recording's noise floor, and for a tone that
decays to nothing that is all it measures.  Hardware's vib peaks at -33.5 dBFS over a -101 dB
floor, so its "-67.4" just means "gone".  The emulator reaching -162 (digital zero) is the
same outcome, not a 95 dB error.

Timed instead against thresholds above the floor, vib and slap were never broken:

| time to fall | 10 dB | 20 dB | 40 dB | | | 10 dB | 20 dB | 40 dB |
|---|---|---|---|---|---|---|---|---|
| piano hw | 0.64 | 1.14 | 5.07 | | vib hw | 1.04 | 1.88 | 3.58 |
| piano emu | **0.63** | **1.17** | **5.71** | | vib emu | **1.07** | **2.00** | **3.82** |
| bell hw | 0.28 | 0.78 | -- | | slap hw | 0.97 | 2.09 | 4.20 |
| bell emu | **0.34** | **0.78** | -- | | slap emu | **1.11** | **2.27** | **4.51** |

vib's segment chain also matches directly: 0.583 s per 6.02 dB step against 0.53 measured.

`[I]` **Two tones left: marimba and fbass.**  Both fail the same way and it is not the ramp.
Their phase-1 handler writes a decay segment whose TARGET equals the level just reached --
marimba gets `rate=-75 target=227` immediately after an attack to 227 -- so the ramp has
nowhere to go and the note sits flat for the whole 8 s hold.

Not a tone-data problem: the records are read per tone and the phase-1 parameter differs
correctly (piano `0x64`, vib `0x5C`, bell `0x7D`, marimba `0x7F`), and the per-voice tone
pointer `3810[voice]` is 0 for every tone including the ones that work.  Note bell reads
`0x7D`, nearly as high as marimba's `0x7F`, and decays correctly -- so a high value is not
itself the trigger.  The fault is in how that parameter and the rate byte (`0x44`: bell
`0x7F`, marimba `0x5C`) combine into the target, in the chain at 0xB9A4-0xBAAE.

### Superseded: a multiplicative falling ramp

Recorded because it looked convincing.  Before the 20 ms quantisation was found, making the
fall shed a fraction of the level per sample brought seven tones onto hardware -- but it
made the release decelerate 16x (52 -> 26 -> 13 -> 6.5 -> 3.3 dB/s) where hardware's
releases fit a straight line in dB to 0.23-0.27 dB rms.  Both directions are additive; the
firmware builds every curve out of straight segments by halving the rate every 6.02 dB at
`0xC0C4`, and the ramp itself is not curved.  The rate floor of -4 in that routine
(`cmpb 42,#fc` / `ldb 42,#fc`) is deliberate, not a defect.

### Reading the CPU's registers

The MCS-96 register file is on **AS_DATA**, not the program space, so the debugger reaches
it with the **`db@`** prefix -- `db@0x4b` is register 0x4B.  `b@0x4b` reads ROM instead and
looks plausible, which wasted time.  Combined with `tracelog` inside an active trace this
gives direct readings of the firmware's own arithmetic:

    bpset 64a0,1,{tracelog "44=%02X 48=%02X afc6=%02X\n",db@0x44,db@0x48,b@(0xafc6+db@0x48);g}

That is how the MULUB bug was caught: the operands were right and the product was not.

**And run `mame/u110`, not `mame/roland`.** Debugger invocations call `./u110` directly,
which `u110run.sh` refreshes but a bare `./u110` does not -- several measurements here were
taken against a stale binary before that was noticed.

`[I]` **Nothing increments `f2`.** The only `inc f2` in the ROM, at 0x96E3, is inside ASCII
menu text, and 0x439F's `ld f2, #ffff` is the only other write -- yet 0xC167 stores `f2` per
voice at 0x36C0 as a timestamp, so it must be a running counter on real hardware. Either it
is written through a pointer the search missed, or a timer this emulation does not deliver
drives it. That is the next thread: with `f2` static the release rate is computed from a
frozen clock.

### What is still unknown
### What is still unknown

* exactly **when `EXTINT` fires** (on arrival at the target is the working assumption);
* the **`0x142C` / reg `0x16` collision** — the envelope handler writes a voice index to the
  register MAME decodes as voice-enable bits 24-31, so the U-110's 16-bit bus cannot map
  that register at `addr/2` like the rest;
* `[I]` the log level scale measures **0.36 dB/unit**, not the table's 0.3763, and the level
  and velocity paths disagree with each other by 5%.

### The measurement: tools/capture_env.py

`tools/capture_env.py` drives a real U-110 through its envelope parameters over SysEx and
records the result -- 15 sweeps, 172 trials, 14 min. It pairs with the emulator: the
emulator runs the same firmware, so it computes the same rate byte and `-log` prints it,
even though it then ignores it. Hardware gives the dB/s that byte produces.

    python3 tools/capture_env.py --list
    python3 tools/render_u110.py --sequence capture_env --log --out-dir listen/env-emu
    python3 tools/capture_env.py --emu-rates listen/env-emu    # -> the rate per trial
    python3 tools/capture_env.py --out-dir listen/env          # the hardware take

The emulator half is already done and checked in as `listen/env-emu/rates.txt`. SysEx
reaches the emulated U-110 and works: DT1 `F0 41 <dev> 23 12 <addr> <data> <sum> F7` with
**device ID = control channel - 1** (firmware at `0x5624` masks `0x3C01` to a nibble) and
**model ID `0x23`** (`0x5BD4`); part parameters at `00 1n xx`. PART LEVEL 127 -> 0 moves
`reg07` 220 -> 48, and 48 = `0x30`, the mute constant.

The three attack sweeps between them cover the rate byte densely:

| ENV ATTACK | -7 | -6 | -5 | -4 | -3 | -2 | -1 | 0 | +1 | +2 |
|---|---|---|---|---|---|---|---|---|---|---|
| Vib 1 (`reg07` 211) | 1 | 8 | 24 | 40 | 56 | 72 | 88 | **104** | 120 | 127 |
| Strings 1 (227) | 1 | 1 | 1 | 1 | 8 | 24 | 40 | **56** | 72 | 88 |
| Brass 1 (227) | 1 | 16 | 32 | 48 | 64 | 80 | 96 | **112** | 127 | 127 |

Steps of exactly 16 -- the `16 * (nibble - 8)` term at `0x6A0C` -- around the unmodified
base `(reg07 * 127) >> 8`, clamped to 1 below and to the `0xB0C6` ceiling above. Seventeen
distinct known rate values from three sweeps.

**The release sweeps do not pair.** The emulator's note-off writes no volume at all: the
release path at `0x64FF` is gated on the envelope phase, the phase only advances from the
EXTINT handler, and that handler never runs. MAME cuts the enable bit at `0x1422` and
substitutes its own fixed fade. `release_by_hold` is the sweep that tests the disassembly's
claim directly -- same note, same settings, holds from 0.2 s to 8 s, and the slope should
get shallower with hold time.

### The status port reports a 26-bit LINEAR level `[C]`

Found while chasing the missing note-off. The routine at `0x7655` reads the same voice
three times, once per select:

```
7658: st 54, 142c ; ld 3e, 1404      ; select 0x16 -> one field
7662: stb 54, 1420 ; ld 44, 1404     ; select 0x10 -> the HIGH 16 bits
766C: stb 54, 1424 ; ld 46, 1404     ; select 0x12 -> the LOW 10 bits
7679: and 46, #03ff
767D: shll 44, #06                   ; normalise the 32-bit pair...
7683: (loop) shll 44, #01 until bit 15 of byte 47 is set, counting 15 down
7690: 40 = (count << 4) | (bits 6..3 of byte 47)
```

That is a normalise-and-take-the-exponent: the result is `exponent * 16 + mantissa nibble`
-- **the same 16-units-per-octave log scale as `reg07`**. So the chip reports a voice's
current level as a 26-bit *linear* value split across two selects, and the firmware converts
it to the log domain itself. That is a precise specification for what the device has to
implement, and it is the piece the note-off path is waiting on.

### Anchors for calibrating the rate

* note-off writes a rate built from `0xAFC6[]`, reduced by how long the note was held;
  measured hardware release is **94-166 dB/s, exponential** (`listen/2/ENVELOPE.md`).
* voice kill and power-up both write `0x0080`: target 0, rate `-128` — the steepest fall.
* `listen/3` (hardware) and `listen/emu2` (emulator) are segment-for-segment aligned by
  `tools/render_u110.py`, so any candidate rate law can be A/B'd directly.

### Tooling added

`tools/mcs96dasm.py` — a standalone MCS-96 disassembler for the program ROM
(`--xref`, `--all`). MAME's `-debugscript` only runs when the debugger actually stops the
machine, which under `-debugger none` it usually does not: the `dasm` command silently does
nothing. This reads the same generated tables MAME's own disassembler is built from and
its output matches the debugger's byte for byte.

## Where things stand — RESOLVED (2026-08-25)

The offline model (`tools/stitch_pingpong.py`) is correct and approved by ear:

    python3 tools/stitch_pingpong.py --index 121 122 --invert --no-repeat-endpoint --cycles 8

The C++ port used to sound clearly worse. **Cause found: the reflection was done in
address space, but the interpolator's lag is direction-dependent, so the OUTPUT reflected
about `pivot - 0.5` instead of `pivot`.** Fixed in `roland_lp.cpp` by reflecting the
output position: `addr_new = 2*pivot + 0x4000 - addr` at both turns.

Verification, in two independent steps:

1. A bit-exact Python transcription of the C++ inner loop, compared against an ideal
   continuous-path integrator (reflect the position analytically, lerp the integral):

   | reflection | s121 | s122 |
   |---|---|---|
   | `2*p - addr` (old) | -13.1 dB | -6.2 dB |
   | `2*p + 1 - addr` (new) | **exactly 0** | **exactly 0** |

   Zero over four full ping-pong cycles, with the same quantised step on both sides.

2. The *built* emulator against that same transcription, Wiener-equalised to fit out the
   analog filter chain: **-48.2 dB (s121), -43.7 dB (s122)**, against -20.5 / -17.2 dB for
   the old model. So the binary really does implement the corrected rule.

Renders for listening: `listen/renders/{strings1,choir3_pingpong,strings3_pingpong}_v19.wav`.

## The bug, stated exactly

`sample_interpolate(a, b, f)` computes `a*(1-f) + b*f`. After a fetch at byte `b`,
`smpl_nxt = w[b]`; `smpl_cur` is whichever byte was fetched *before* it, which depends on
which way the address was moving:

| direction | smpl_cur | smpl_nxt | expression | output position |
|---|---|---|---|---|
| forward  | `w[b-1]` | `w[b]` | `interp(cur, nxt, f)` | **addr - 1** |
| backward | `w[b+1]` | `w[b]` | `interp(nxt, cur, f)` | **addr** |

A constant one-byte lag is inaudible in a forward loop. At a ping-pong turn the lag
*changes sign*. Reflecting `addr` about the pivot then gives
`q_new = (2*pivot - 1) - q_cont` — a reflection about `pivot - 0.5`. Worse, at the `lo`
turn the output position keeps *descending* through the pivot before it climbs again:
with step 0.7 the sequence runs `lo+0.4, lo-0.7, lo+0.0, lo+0.7` instead of turning.
Reflecting the output position, `q_new = 2*pivot - q_cont`, is `addr_new = 2*pivot + 1 - addr`
for both turns.

## Resolved: every byte on the path must be integrated `[C]`

Found by running the hardware sequence through the emulator (`tools/render_u110.py`,
2026-08-25): `fantasy` sat +21.9 dB and `choir3_sustain` +10.4 dB above the hardware in RMS,
and the cause was a DC ramp, not extra signal. Only the *second, higher* note of each pair
walked. The register trace said why -- that voice steps **0x42CF = 1.0439 bytes/sample**
while the clean ones run 0.56, 0.88, 0.89.

The device fetched at most ONE byte per output sample. That is right for ordinary PCM,
where a skipped byte is just aliasing, and wrong for delta data, where a skipped delta is
never applied: the loop stops summing to zero and the accumulator ramps. Two places had it:

  * the fetch itself, now a `walk()` over every byte crossed;
  * the **forward-loop wrap**, which the generic `reachedEnd` handler did *after* the read.
    Below one byte per sample the index lands exactly ON `end`, and since every loop sums
    to zero, `W[end] == W[loop]` and the wrap is seamless -- which is why this never showed
    up before. Above one byte per sample it overshoots to `end+1` and applies a delta from
    beyond the loop, once per loop, forever. It now folds before the read like ping-pong.

Verified in the bit-exact transcription before building:

| case | old | new |
|---|---|---|
| ping-pong, step 0.74 | good | **bit-identical** |
| ping-pong, step 1.04 / 1.88 | broken | **exactly zero residual vs ideal path** |
| forward loop, step 0.74 | DC +/-1 | **DC +/-1, unchanged** |
| forward loop, step 1.04 | DC walks to -13479 | **DC +/-1** |

And in the render: `choir3_sustain` max|DC| 6468 -> 5.1, `fantasy` 9834 -> 11.8 (hardware
~1), with the ping-pong segments unchanged to -66 dB, which is the 16-bit requantisation
floor of the comparison itself.

8 of 228 voices in the reference capture exceed one byte per sample, worst 1.880.

## All 226 wave ROM loops sum to exactly zero `[C]`

An offline survey of the whole sample table: the deltas over one loop traversal sum to
**exactly zero** for every real sample -- all 30 ping-pong, all 26 one-shot, all 170
forward. Roland DC-balanced every loop on purpose, which is what makes a leak-free
integrator safe.

An earlier pass reported 12 exceptions. That was wrong -- an artefact of slicing with a
negative loop start. Those 12 are exactly the entries with `looplen > length`, which is
impossible for a real loop, and they are not samples at all:

  * 214-221: 127 bytes of `0x80` filler, `looplen` 57556 -- padding.
  * 222-225: demo song data. The bytes read as ASCII: **"T-Jazz #1"**, **"Swing High"**,
    **"Cloud 9"**, **"NoOne Home"**.

So the sample table's tail points at the demo sequences and padding. Treat `looplen > length`
as the "not a sample" test.

## Tried and rejected (do not redo)

* Storing interpolation taps pre-transformed by sign/offset: 0.2116 -> 0.2110, a no-op.
* Sub-byte offset sweep in the reference (±1.5 bytes): flat.
* Integrator leak: worse at every value swept (turn value +15 at 0 Hz -> +62 at 20 Hz).
* Inserting a zero sample at the turn: only correct for an *inverted* join, and the
  reflection-about-turn-value construction already makes it unnecessary.
* A separate de-emphasis filter stage: superseded — the integration is the de-emphasis.

## Method notes, learned the hard way

* **`make SUBTARGET=roland` links `mame/roland`, but `tools/u110run.sh` runs `mame/u110`,
  a copy.** A stale copy makes a rebuild silently do nothing, and the render then measures
  the *old* code — this happened, and cost a full analysis cycle chasing a phantom.
  `u110run.sh` now refreshes the copy itself when `roland` is newer.
* Compare against the model, not against detectors. Every glitch detector built in this
  investigation (LPC residual, spectral flux, HF bursts, level dips, per-sample |dv| and
  curvature) reported *zero* clusters on both the good and the bad build.
* Align before you measure. A coarse FFT cross-correlation locked onto lag 219 when the
  true lag was 5, turning a perfect match into an apparent 0.59 residual. Verify a lag by
  checking it is stable across windows.
* Two things in the emulator render are not in the offline reference: the firmware's TVA
  volume ramp (time-varying, so an LTI fit cannot absorb it) and the analog filter chain
  (LTI, so a Wiener fit can). Normalise the envelope, Wiener out the filter.
* `stitch_pingpong.py` quantises its turn to a whole byte; the emulator turns at a
  fractional position. That alone makes the two drift by ~1 sample per few turns, so
  compare against the *continuous* model for sample-accurate work.
* Change **one** thing per render and A/B it.
* Renders go in `listen/renders/`; `tools/u110run.sh` puts a bare `-w` name there.
* `--dry-run-midi` prints file time *and* render time; use the render column.
* Native-rate renders (`-samplerate 32000`) for anything sample-accurate.
* `-log` writes `mame/error.log`; grep `Starting channel` / `Smpl End Ofs` for the exact
  start/end/loop/step a note actually used. That is how sample 121/122 and their native
  rates (step 0x2F65, 0x2CD1) were confirmed rather than assumed.
