# Chorus and Tremolo — what the firmware and ROM say

`[C]` confirmed by measurement or by reading the code · `[I]` inferred · `[S]` from the
service notes.  Companion to `SYSTEM-DESIGN.md` §4.5, which had this as an open gap.

The short version: **the effects are not a separate DSP block.  They are two more slots on
the same envelope ramp generator the voices use**, numbered `0x20` and `0x21`, turned round
at each end by the same interrupt handler that walks a voice's envelope.  Everything about
their rate and depth is a pair of 16-entry tables in the program ROM, and both tables have
been read out.  Nothing here needed hardware.

---

## 1. The signal path `[S]` `[C]`

IC17 (CXK5814, 2K x 8) hangs off IC15's wave address bus.  Read/write is IC15 pin 59, chip
enable IC15 pin 65; both are dedicated.  The CPU cannot see it and no firmware access to it
exists anywhere in the image.  Eleven address bits, eight data bits: 2048 samples, which at
the 32 kHz engine rate is 64 ms — a delay line.

The effect is applied to **Voice Group 1 only**, and appears on Multi Outputs 1 and 2 as a
stereo pair.  The Owner's Manual footnote to the Output Mode table says it plainly: in modes
21-50 outputs 1 and 2 are one Voice Group, `M` is the dry centred version and `<L>/<R>` is
the wet stereo version.

---

## 2. The two LFOs are ramp-generator slots `[C]`

`roland_lp.h` already recorded that the chip has 34 slots, 32 voices plus two, and that
registers `0x19`/`0x1B`/`0x1D` belong to the effect subsystem.  This is what they are for.

**Patch load, at `0xB4C6`.**  Firmware register addresses are `0x1400 + 2*reg` — a word
store at `0x140C` is regs `06`/`07`, which is exactly one envelope segment.

```asm
B4C6: stb 0,    1432     ; reg 19 <- 0x00      scan range low   [I]
B4CB: ldb 50,   #21
B4CE: stb 50,   1436     ; reg 1B <- 0x21      scan range high  [I]  (34 slots)
B4D3: ldbze 50, 280e     ; the patch's OUTPUT MODE index
B4D8: mulub 50,  #08
B4DB: ldb 52,   a726[50] ; 8 bytes per output mode; byte 0 is the config byte
B4E0: stb 52,   143a     ; reg 1D <- config byte
B4F2: jbs 52, 1, b4f7    ; bit 1 = TREMOLO enabled
B4F7: ldbze 54, 2812     ; patch +0x12 = TREMOLO DEPTH
B4FF: cmpb 54, 0
B504: andb 52, #fd       ;   depth 0 -> clear the enable bit and skip entirely
B509: shlb 54, #01
B50C: ld 56,    aa86[54] ; depth -> the RISING segment word
B511: ld 58,    aaa6[54] ; depth -> the FALLING segment word
B516: ldb 54,   2811     ; patch +0x11 = TREMOLO RATE
B51E: add 56,   0054     ;   rising  segment += rate
B523: st 56,    3788
B528: sub 58,   0054     ;   falling segment -= rate
B52D: st 58,    378a
...   ldb 58, #21 / stb 58, 143e     ; reg 1F <- slot 0x21   (select)
      st 0,  1400                    ; regs 00/01 <- 0       (mode)
      ld 5a, #0200 / st 5a, 1404     ; regs 02/03 <- 0x0200  (initial level)
      ld 5a, 3788  / st 5a, 140c     ; regs 06/07 <- segment (start the ramp)
```

The chorus block at `0xB58B` is the same code against patch bytes `+0x0F` / `+0x10`, tables
`0xAA46` / `0xAA66`, RAM `0x378C` / `0x378E`, and **slot `0x20`**.  It also zeroes slot
`0x20`'s address registers `08`-`0B` before starting it `[I]` — plausibly the delay-line
write pointer.

**So: slot `0x20` is CHORUS, slot `0x21` is TREMOLO.**

**Turnaround, in the interrupt handler at `0x41D5`.**  The handler special-cases the two
slots before the voice path:

```asm
41D5: cmpb 54, #20  / je 4241        slot 0x20 -> chorus turnaround
41DA: cmpb 54, #21  / je 427a        slot 0x21 -> tremolo turnaround

4241: st 54,  142c        ; reg 16 <- slot: select the status field
4246: ld 56,  1404        ; read regs 02/03
424B: jbc 56, 7, 4264     ; bit 7 -> which way was it going?
424E: stb 54, 143e        ; reg 1F <- slot
4253: ld 56,  378c        ; the RISING segment
4258: st 56,  140c        ; regs 06/07 -- written TWICE
425D: st 56,  140c
4264: ...     378e        ; else the FALLING segment
```

Two segments, alternating on arrival: **the LFO is a ramp generator run back and forth
between two levels by the CPU**, one interrupt per half cycle.  A 6 Hz tremolo therefore
costs the CPU 12 interrupts a second, which is nothing next to a piano attack.

`[C]` **What bit 7 of register `02` means.**  Both LFOs live entirely above log level 128,
so it cannot be the level's own bit 7 or the test would never alternate.  It is the **sign of
the current rate byte** -- "which segment am I finishing".  Nothing had to be added to make
that work: select `0x16` already hands the segment register pair back verbatim, target in the
high byte and rate in the low, so the sign bit was already sitting where the handler looks.
The firmware turns both LFOs round correctly against the device exactly as it stood.

---

## 3. The tables, read out of the ROM `[C]`

Segment word = `target << 8 | rate`, the same layout the voices use: high byte a log level
(16 units per octave, 0.3763 dB per unit), low byte a signed rate.

```
depth           0     1     2     3     4     5     6     7     8     9    10    11    12    13    14    15
CHORUS  rising  0xAA46
   target    208   209   210   211   212   213   214   215   216   217   218   219   220   221   222   223
   rate        8    26    33    35    40    42    44    46    48    49    50    51    52    53    54    55
CHORUS  falling 0xAA66
   target    207   205   203   201   199   197   195   193   191   187   183   179   175   167   159   143
   rate       -8   -26   -33   -37   -40   -42   -44   -46   -48   -49   -50   -51   -52   -53   -54   -55
TREMOLO rising  0xAA86
   target    240   241   242   243   244   245   246   247   248   249   250   251   252   253   254   255
   rate       40    58    65    67    72    74    76    78    80    81    82    83    84    85    86    87
TREMOLO falling 0xAAA6
   target    239   237   235   233   231   229   227   225   223   219   215   211   207   199   191   175
   rate      -40   -58   -65   -69   -72   -74   -76   -78   -80   -81   -82   -83   -84   -85   -86   -87
```

The RATE parameter 0-15 is then added to the rising rate and subtracted from the
falling one, so it makes both ramps steeper by the same amount.  DEPTH picks the two
target levels; RATE picks the two speeds.  Depth 0 does not mean a small effect --
the firmware clears the enable bit and never starts the LFO at all.

---

## 4. What that comes out as `[I]`

Applying the ramp law the voices already use — level is **linear amplitude** in a 2^26 scale,
`level(L) = 2^26 * 2^((L-255)/16)`, a rising ramp adds `2^(rate/8) << 6` per 32 kHz sample and
a falling one subtracts `2^(rate/8) << 2` (`ENV_FALL_DIVISOR`, measured at 15.0-15.7x on the
release sweep) — the tables give this:

```
LFO frequency, Hz          rate ->
                 0     1     2     3     4     5     6     7     8     9    10    11    12    13    14    15
chorus   d= 1  0.79  0.86  0.94  1.02  1.11  1.21  1.32  1.44  1.57  1.72  1.87  2.04  2.23  2.43  2.65  2.89
chorus   d= 4  0.86  0.94  1.02  1.11  1.22  1.33  1.45  1.58  1.72  1.87  2.04  2.23  2.43  2.65  2.89  3.15
chorus   d= 7  0.89  0.97  1.06  1.15  1.26  1.37  1.50  1.63  1.78  1.94  2.12  2.31  2.52  2.74  2.99  3.26
chorus   d=11  0.86  0.94  1.02  1.12  1.22  1.33  1.45  1.58  1.72  1.88  2.05  2.23  2.44  2.66  2.90  3.16
chorus   d=15  0.87  0.95  1.03  1.13  1.23  1.34  1.46  1.60  1.74  1.90  2.07  2.26  2.46  2.68  2.93  3.19
tremolo  d= 1  3.15  3.43  3.75  4.08  4.45  4.86  5.30  5.78  6.30  6.87  7.49  8.17  8.91  9.72 10.59 11.55
tremolo  d= 4  3.44  3.75  4.09  4.46  4.86  5.30  5.78  6.30  6.88  7.50  8.18  8.92  9.72 10.60 11.56 12.61
tremolo  d= 7  3.56  3.88  4.23  4.61  5.03  5.49  5.98  6.52  7.12  7.76  8.46  9.23 10.06 10.97 11.97 13.05
tremolo  d=11  3.44  3.76  4.10  4.47  4.87  5.31  5.79  6.32  6.89  7.51  8.19  8.93  9.74 10.62 11.58 12.63
tremolo  d=15  3.48  3.80  4.14  4.51  4.92  5.37  5.85  6.38  6.96  7.59  8.28  9.03  9.84 10.73 11.71 12.77

depth ->                  0      1      2      3      4      5      6      7      8      9     10     11     12     13     14     15
tremolo swing, dB       0.4    1.5    2.6    3.8    4.9    6.0    7.1    8.3    9.4   11.3   13.2   15.1   16.9   20.3   23.7   30.1
chorus delay, ms    
   shortest            8.00   7.34   6.73   6.17   5.66   5.19   4.76   4.36   4.00   3.36   2.83   2.38   2.00   1.41   1.00   0.50
   longest             8.35   8.72   9.11   9.51   9.93  10.37  10.83  11.31  11.81  12.34  12.88  13.45  14.05  14.67  15.32  16.00
```

`[C]` **Confirmed in the emulator against all four tables.**  With the output mode set to 21
the firmware programs both slots and turns them round on its own; timing the interval between
arrivals gives, for the five rate settings of each sweep:

```
                 measured  predicted                    measured  predicted
  chorus rate 0    0.889 Hz    0.889     tremolo rate 0   3.557 Hz    3.558
              3    1.153       1.153                  3   4.614       4.614
              7    1.631       1.631                  7   6.523       6.525
             11    2.307       2.307                 11   9.225       9.228
             15    3.262       3.262                 15  13.040      13.050
```

Those frequencies come out of the same ramp law the prediction uses, so that much is
consistency, not proof.  What is not circular, and is now settled: which slot each effect
uses, which of the four tables each reads, that the segment words are indexed by DEPTH and
offset by RATE, that the firmware alternates them into a two-level cycle, that changing depth
mid-note retargets the running LFO, and that the config byte reaches register `0x1D` carrying
exactly the bits the `0xA726` table predicts -- `0x1E` for mode 21, `0x00` for mode 22, `0x1C`
when tremolo depth is zeroed (chorus only) and `0x16` when chorus depth is (tremolo only).
What hardware still has to settle is the LFO's shape in time, and through it the real
frequency scale.

**Why this is more than a guess.**  At a fixed RATE the frequency barely moves as DEPTH
changes — tremolo at rate 7 runs 6.15 to 6.90 Hz across depths 2-15, chorus at rate 7 runs
1.54 to 1.73 Hz.  That is a designer choosing rate bytes so that changing the excursion does
not change the speed, and it only comes out flat if the level law and the rate law are both
right.  Substituting a log-domain ramp instead spreads the same numbers over 1.75:1.  The
resulting ranges are also the right ones: **chorus 0.65-3.2 Hz, tremolo 2.6-12.8 Hz,
tremolo swing 0.4-30 dB.**

`[I]` **The chorus delay tap.**  The delay column above assumes the tap is the slot's linear
level shifted right by 15, i.e. `2^26 -> 2048` samples.  That is one free constant and it is
the one thing here with no evidence behind it.  It is an attractive guess: it puts depth 0 at
8.00-8.35 ms and depth 15 at exactly 0.50-16.00 ms, which are 256/267 and 16/512 samples,
round numbers falling out of targets that are exact powers of two (`2^-7` and `2^-2` of full
scale).  A shift of 14 or 13 would give 1-32 ms or 2-64 ms and use more of the 2048-byte
SRAM.  **Section 7 has a one-measurement way to settle it.**

`[I]` **The LFO shape is a sawtooth, not a triangle.**  Rising and falling ramps use the same
rate byte magnitude, but the hardware's falling ramps move 16x slower, so the cycle is 1/17
rise and 16/17 fall — for chorus at depth 7 rate 7, 36 ms up and 577 ms down.  In delay terms
that is a slow steady sweep giving a near-constant pitch offset (about +21 cents), punctuated
by a fast return.  A steady pitch offset over most of the cycle would show up as a **doublet**
in the spectrum rather than a smear, which is a sharp, falsifiable prediction.  This is the
biggest single unknown and the first thing a hardware capture should look at.

---

## 5. No factory patch turns them on `[C]`

The config byte at `0xA726 + 8*mode_index` carries the enable bits: **bit 1 tremolo, bit 3
chorus** (the two `jbs` tests above), and bit 4 the effect section as a whole.  Reading the
table out:

- effects on: modes **21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49** — exactly
  the odd `<L>/<R>` rows of the Owner's Manual table, and nothing else.
- effects off: every other mode, including all of 1-20, and every even `M` row.

Every one of the 64 factory patches stores an output mode index of 21, 19, 7, 12 or 49 at
`+0x0E`, i.e. **mode 22, 20, 8, 13 or 50 — all of them dry**.  The stored chorus/tremolo
bytes are real (eleven distinct combinations, `Ac.Piano 07 07 07 07`) but nothing reads them
into the chip, because the config byte's enable bits are clear.

Three independent confirmations:

1. **The recordings are mono.**  P-01, P-46, P-31, P-11, P-20 in `listen/hardware/1` all have
   L/R correlation 1.00000 — which is what mode 22, `M31` "centred", means.
2. **Turning the effects off changed nothing.**  `listen/hardware/4` re-recorded P-52 Fantasy
   and P-48 Shakuhachi after zeroing chorus and tremolo depth over SysEx.  Against the wet
   takes in `listen/hardware/3`: Shakuhachi RMS -30.48 dB both, Fantasy -40.41 / -40.36 dB;
   the 6.16 Hz amplitude wobble in Fantasy is present in *both* takes, so it belongs to the
   tone, not to a tremolo.  Fantasy is mode 13, config byte `0x60`, effects off.
3. **The tables agree with the manual's own footnote** about which rows carry the effect.

`[C]` **Correction to `ROM-ANALYSIS.md`.**  That document attributes the flute's 443 Hz
reading to "that patch has chorus".  It does not — P-46 is mode 22.  Measured on
`listen/hardware/1`, note 69: two partials of nearly equal level at **439.71 and 443.24 Hz**,
3.53 Hz apart (13.8 cents), beating at 3.24 Hz.  Two equal, stable, discrete partials is a
**Detune-type tone** — two voices a fixed interval apart — not a delay-based chorus.  The
tuning conclusion (A = 440) is unaffected; only the explanation for the doublet changes.

The practical consequence: **hearing the U-110's chorus at all requires setting the Output
Mode to an odd number in 21-49.**  On P-01, `PATCH:COM:OUT # = 21` should give chorus at
1.63 Hz over a 4.4-11.3 ms delay and tremolo at 6.5 Hz swinging 8.3 dB.

---

## 6. A bug this uncovered in our device `[C]` -- fixed

`roland_lp.cpp` masked the slot select to five bits:

```cpp
case 0x1F:  m_sel_chn = data & 0x1F;   break;      // and the same in the 0x10/0x12/0x16/0x1A
                                                   // status selects, m_status_voice = data & 0x1F
```

Slots `0x20` and `0x21` therefore alias onto voices **0 and 1**.  Every time the firmware
sets up the effects it writes the tremolo LFO's mode, level and segment into voice 1 — a real
voice — and the chorus LFO's into voice 0, the wave-ROM read port.  It was harmless only
because the patch-load path bails out before the slot writes when the config byte says the
effects are off, which for every factory patch it does -- but selecting output mode 21 would
have corrupted voice 1 at once.

Fixed: the mask is `0x3F`, `NUM_SLOTS` is 34, `m_env_pending` is 64 bits wide, and the two
extra slots get their ramps advanced by `env_advance_slot()`, since they never reach the audio
loop -- nothing ever sets their enable bit.  That is the whole of what it took to make both
LFOs run.

---

## 7. Getting the hardware to make one `[C]`

Two things are needed and neither is a default.

**The output mode has to be moved.**  Over SysEx it is patch-common offset **`0x18`**, one
below chorus rate -- found the way the part offsets were, by writing a distinctive value to
every candidate and watching where it landed in the edit buffer (`0x14`..`0x18` reach
`0x2800`..`0x2803` and `0x280E`).  On the wire it is the mode number **minus one**, exactly as
stored: send 20 to select mode 21.  From the panel it is `PATCH:COM:OUT #`.

**Nothing may be inherited.**  `tools/capture_env.py --set effects` writes the whole part
block, parks parts 1-5 on an unused MIDI channel, and re-writes all five effect parameters
before every trial even where the value has not changed -- the output mode is precisely the
parameter most likely to have been left somewhere unexpected by an earlier session or a panel
edit.  Order matters: a write to any of the five makes the firmware re-run its setup against
whatever the other four currently hold, so OUTPUT MODE is always sent last.

The set was rendered through the emulator before being taken near the hardware, and the
config byte reaches the intended value at every one of its trials.

`tools/dump_patch.lua` prints the edit buffer during an emulator run, which is how both of
those were checked.

## 8. Open questions, in the order they should be settled

1. **`[open]` LFO shape — sawtooth or triangle?**  Predicted 1/17 duty.  A sustained tone
   through mode 21 with chorus depth 7 should show a *doublet* about 21 cents wide if the
   sawtooth reading is right, and a smeared, symmetrically swept partial if it is a triangle.
   One capture answers it.
2. **`[open]` The chorus delay tap shift.**  During the slow phase the delay changes at a
   constant rate, so the pitch ratio is constant.  Measure that ratio r on a sustained tone;
   the swing in samples is `(r-1) * 32000 * t_fall`, and `t_fall` is known from the LFO period
   in the same recording.  That pins the shift with no other assumption.
3. **`[open]` How the wet signal becomes stereo.**  The manual promises "spacious stereo".
   The likely arrangement is `L = dry + wet, R = dry - wet` — check whether `L+R` from a
   chorused capture is comb-free.  Whether the tremolo is also antiphase between L and R
   (i.e. an auto-pan) is the same measurement on the envelope.
4. **`[open]` Whether the delay line's 8-bit width is audible.**  IC17 is 8 bits wide, so the
   delayed copy is stored at 8 bits — very likely through the same companding curve the wave
   ROM uses, which IC15 already has a decoder for.  Worth modelling only if a capture shows
   the wet path is noisier than the dry one.
5. **`[open]` Registers `0x19` and `0x1B`.**  Written `0x00` and `0x21` here and at the
   `0x43DA` init, never anything else.  Reading them as the slot scan range is a guess that
   fits `m_env_slots = 0x22` but has no evidence.
