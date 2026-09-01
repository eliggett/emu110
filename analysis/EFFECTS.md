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

## 4. What that comes out as `[C]`

The level is linear amplitude on a 2^26 scale, `level(L) = 2^26 * 2^((L-255)/16)`, and the
ramp advances by `2^(rate/8) * 4` per 32 kHz sample.

`[C]` **That scale is 4 in BOTH directions, which is not what the voices do.**  A voice's
rising ramp adds `2^(rate/8) << 6` and its falling one subtracts `2^(rate/8) << 2`, a
measured 16:1 asymmetry (`ENV_FALL_DIVISOR`).  Carried over to these slots it predicts a
sawtooth of duty 1/17.  **The hardware makes a symmetric triangle** -- measured duty 0.47 to
0.54 over the whole tremolo sweep -- and fitting the scale to the measured half-periods gives
3.95 to 4.29 across eighteen trials spanning a 44:1 range of period.  So the LFO slots ramp
at the FALLING constant whichever way they are going.  Section 8 has the measurement.

`[I]` Why the two differ is open.  The obvious lead is the mode register: the firmware writes
`00`/`01` = 0 for these slots and something quite different for a voice, so a bit there may
select the rate scale.  It is also possible the voices' asymmetry is not really about
direction at all and only correlates with it, in which case this is the counter-example that
says so.

```
LFO frequency, Hz                        rate ->
                    0     1     2     3     4     5     6     7     8     9    10    11    12    13    14    15
  chorus  depth  1   0.42  0.46  0.50  0.54  0.59  0.65  0.70  0.77  0.84  0.91  0.99  1.09  1.18  1.29  1.41  1.53
  chorus  depth  4   0.46  0.50  0.54  0.59  0.65  0.70  0.77  0.84  0.91  1.00  1.09  1.18  1.29  1.41  1.54  1.67
  chorus  depth  7   0.47  0.52  0.56  0.61  0.67  0.73  0.79  0.87  0.95  1.03  1.12  1.23  1.34  1.46  1.59  1.73
  chorus  depth 11   0.46  0.50  0.54  0.59  0.65  0.71  0.77  0.84  0.91  1.00  1.09  1.19  1.29  1.41  1.54  1.68
  chorus  depth 15   0.46  0.50  0.55  0.60  0.65  0.71  0.78  0.85  0.92  1.01  1.10  1.20  1.31  1.43  1.55  1.70
  tremolo depth  1   1.67  1.82  1.99  2.17  2.37  2.58  2.81  3.07  3.35  3.65  3.98  4.34  4.73  5.16  5.63  6.14
  tremolo depth  4   1.83  1.99  2.17  2.37  2.58  2.82  3.07  3.35  3.65  3.98  4.34  4.74  5.17  5.63  6.14  6.70
  tremolo depth  7   1.89  2.06  2.25  2.45  2.67  2.91  3.18  3.47  3.78  4.12  4.50  4.90  5.35  5.83  6.36  6.93
  tremolo depth 11   1.83  2.00  2.18  2.37  2.59  2.82  3.08  3.36  3.66  3.99  4.35  4.75  5.17  5.64  6.15  6.71
  tremolo depth 15   1.85  2.02  2.20  2.40  2.61  2.85  3.11  3.39  3.70  4.03  4.40  4.80  5.23  5.70  6.22  6.78

depth ->                0      1      2      3      4      5      6      7      8      9     10     11     12     13     14     15
tremolo pan, dB       0.4    1.5    2.6    3.8    4.9    6.0    7.1    8.3    9.4   11.3   13.2   15.1   16.9   20.3   23.7   30.1
chorus delay, ms
   shortest         16.00  14.67  13.45  12.34  11.31  10.37   9.51   8.72   8.00   6.73   5.66   4.76   4.00   2.83   2.00   1.00
   longest          16.71  17.45  18.22  19.03  19.87  20.75  21.67  22.63  23.63  24.68  25.77  26.91  28.10  29.34  30.64  32.00
```

`[C]` **The chorus delay tap is the level shifted right by 14** -- `2^26` maps to 4096
samples, so the reachable range is 0 to 128 ms and the tables use 1 to 32 ms of it.  Measured
from the pitch sidebands at five different depth and rate settings, which give 13.90 to 13.96.
The targets are chosen to land on round sample counts: log level 143 is `2^19` -> 32 samples,
207 is `2^23` -> 512, and 223 is `2^24` -> 1024.

**Why the tables are more than a guess.**  At a fixed RATE the frequency barely moves as
DEPTH changes -- tremolo at rate 7 runs 3.36 to 3.47 Hz across depths 4-15, chorus at rate 7
runs 0.84 to 0.87 Hz.  That is a designer choosing rate bytes so that changing the excursion
does not change the speed, and it only comes out flat if the level law and the rate law are
both right.  Substituting a log-domain ramp spreads the same numbers over 1.75:1.

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

## 8. What the hardware says `[C]`

`listen/hardware/effects`, captured 2026-09-01 with `capture_env.py --set effects`.  The
first recording of either effect that exists.  Everything below is a ratio, a frequency or a
duty cycle, so the session's higher interface gain does not enter into any of it.

Three questions went in.  All three are answered, and the first one answered against me.

### The LFO is a symmetric TRIANGLE, not a sawtooth

Tremolo rate 7 depth 15, 71 cycles averaged: period 275 ms, trough to peak in 134 ms.  **Duty
0.487.**  Across the whole tremolo sweep the duty runs 0.47 to 0.54.  The predicted sawtooth
was 0.059.

The shape in dB rises fast and flattens, which is what a ramp linear in AMPLITUDE looks like
plotted in dB -- so the level law is confirmed even as the rate law is corrected.

Fitting `scale` in `dLevel / (halfPeriod * 32000) / 2^(rate/8)` to each trial:

```
  tremolo rate sweep    3.956  4.029  3.973  3.954  4.194
  tremolo depth sweep          4.112  3.973  4.105  4.288      (depths 4,7,11,15)
  chorus  rate sweep    3.954  4.026  3.954  4.026
  chorus  depth sweep   4.085  4.026  4.187  4.287
```

Constant at **4.0**, which is `ENV_RATE_SCALE / ENV_FALL_DIVISOR` = 64/16 exactly.  With that
one change the frequency table above lands within 1.5% of measurement:

```
  rate            0      3      7     11     15
  chorus  meas    -   0.606  0.872  1.211  1.744 Hz      (rate 0 too slow for a 10 s note)
          pred  0.473  0.613  0.867  1.226  1.733
  tremolo meas  1.869  2.469  3.443  4.845  7.269 Hz
          pred  1.890  2.451  3.466  4.902  6.933
```

### The tremolo is an AUTO-PAN

L and R modulate in antiphase: their envelope cross-correlation peaks at half the LFO period,
and the SUM does not move with depth at all -- 8.9 dB of the organ's own ripple at depth 7 and
the same 8.9 dB at depth 15, while each channel alone goes from 14.9 to 30.9 dB.

So one channel takes the slot's level `g` and the other takes `A_hi + A_lo - g`, which for a
symmetric triangle is the same waveform half a period later and sums to a constant.  The
"spacious stereo effects" of the manual is literal.

Measuring the pan ratio `L/(L+R)` cancels the tone's own level exactly, and it lands on the
target table almost dead on:

```
  depth   targets    measured g/C      predicted g/C     swing meas  pred
    0    240/239   0.4991..0.5012   0.4892..0.5108        0.0 dB    0.4
    1    241/237   0.4696..0.5464   0.4568..0.5432        1.3       1.5
    4    244/231   0.3775..0.6384   0.3628..0.6372        4.6       4.9
    7    247/225   0.2854..0.7303   0.2783..0.7217        8.2       8.3
   11    251/211   0.1626..0.8452   0.1502..0.8498       14.3      15.1
   15    255/175   0.0396..0.9617   0.0303..0.9697       27.7      30.1
```

The shortfall at the ends is a percentile reading a momentary peak.  Depth 0 sits dead centre
and does not move at all, confirming that the firmware really does switch the effect off
rather than run a tiny one.

### The chorus delay tap is `level >> 14`

A sustained partial splits into a symmetric **triplet** -- the dry line plus two shifted
copies, one from each ramp.  Harmonic 8 of note 60, chorus rate 7 depth 7: lines at -44.5,
+0.3 and +43.9 cents.

That the two sidebands are not equal in cents is itself the proof this is a delay and not a
pair of detuned voices.  A delay slope `s` gives ratios `1+s` and `1-s`, which in cents are
`+1200*log2(1+s)` and `-1200*log2(1-s)` -- unequal by exactly the curvature of the log.  At
depth 15 the measured pair is +92.6 / -96.9 cents, which solves to s = 0.05506 and 0.05481.

Converting the slope to a delay swing and dividing into the level swing:

```
  trial              cents    half period    swing, samples      k
  depth 7  rate 7    43.9        577 ms          474.9        13.91
  depth 7  rate 0    23.7       1043 ms          457.7        13.96
  depth 4  rate 7    25.0        604 ms          280.9        13.96
  depth 11 rate 7    67.5        596 ms          757.7        13.90
  depth 15 rate 7    94.7        590 ms         1060.6        13.90
```

**k = 14** across a 3.6:1 range of depth and 1.8:1 of rate.  The residual 0.06 is inside the
4% spread of the scale fit.  It is corroborated by the targets themselves being powers of
two once shifted: 32, 512 and 1024 samples.

### The firmware pre-compensates for the pan `[C]`

With the tremolo enabled the firmware asks the chip for a **different voice level**: volume
word `F278` instead of `E270`, i.e. log target 242 against 226.  Sixteen units is exactly one
octave, 6.02 dB.

That is an independent confirmation of the pan model, and it fixes the gain law.  The pan
gains are `g/C` and `(C-g)/C` -- they add to one, so each channel sits at HALF the signal in
the middle of the sweep -- and the firmware puts the missing 6 dB back by raising the voice.
Normalising the pan to unity at its centre instead (gains `2g/C`) double-counts the
correction and makes every tremolo patch 6 dB loud, which is exactly the error the first
build of this made.  Nothing is done for the chorus, whose 1 dB the firmware ignores.

### The chorus is two taps, not a polarity flip

`L - R` kills the unshifted carrier by 32 dB, so the dry signal is common to both channels.
But `L + R` only reduces the sidebands by 4 dB, so the wet signal is NOT simply inverted in
one channel -- an inversion would have cancelled it just as completely.  Both channels carry
a delayed copy, and the two taps are half an LFO period apart, which is the same relationship
the tremolo has.  One LFO, and the right channel uses its complement.

Sideband energy relative to the carrier, chorus rate 7 depth 7, gain-matched (the capture's
own channel imbalance is -0.13 dB and the channels null to -57 dB on a dry trial):

```
                      L      L+R     L-R
  chorus wet       -2.9     -6.9    21.0 dB
  chorus dry      -15.5    -15.5     0.7 dB
```

At -2.9 dB relative to the carrier the wet copy is roughly as loud as the dry one, so the mix
is near enough 50/50.

`[open]` Nothing here measures the delay line's 8-bit width or any feedback path.  The
sideband clusters are clean and no second-order lines appear at 2x the offset, so there is no
obvious regeneration.

## 9. What the emulator does now `[C]`

`roland_lp.cpp`, `fx_render()`.  It runs whenever register `0x1D` says an effect is on, on
Voice Group 1 only -- exactly the voices the driver has given mask `0x03` -- which are summed
onto one bus instead of reaching Multi Outputs 1 and 2 directly.

```
  delay line   2048 samples of the group sum, IC17's size at the engine rate
  chorus       tap_L = level >> 14, tap_R = (endpoints - level) >> 14, integer, no
               interpolation; out = dry + 0.5 * tap
  tremolo      gain_L = level / endpoints, gain_R = 1 - gain_L
  LFO          the ramp generator's own slots, ramping at 2^(rate/8) * 4 per sample in
               both directions
```

`endpoints` is the sum of the segment's two ends, captured in `env_segment()` as the ramp is
programmed: the chip has both in front of it, this device sees one at a time.

Rendered against the same capture (`tools/fx_analyse.py --hw ... --emu ...`):

```
                          hardware            emulator
  chorus rate 3/7/11/15   0.606 0.872 1.211 1.744    0.614 0.872 1.223 1.721 Hz
  tremolo rate 0/3/7/11   1.869 2.469 3.443 4.845    1.869 2.469 3.443 4.845 Hz
  pan ratio, depth 7      0.2823 .. 0.7273           0.2827 .. 0.7170
  pan ratio, depth 15     0.0390 .. 0.9611           0.0401 .. 0.9598
  L alone / sum, depth 15   30.9 / 8.9 dB              30.8 / 8.9 dB
  chorus sideband/carrier  -2.9 dB                    -3.2 dB
  wet level vs dry         chorus +1.1, trem +0.5     chorus +0.7, trem +0.2 dB
  chorus L-vs-R sideband    -1.000                     -0.993
```

`[I]` **Not modelled, and not measured either.** The delay line holds floats, where IC17 is
eight bits wide; the tap does not interpolate, which is what an eleven-wire address does but
means the sweep steps; the wet/dry mix is a flat 0.5 from three readings that bracket
0.45-0.55; and the order of the two effects when both are on is a guess -- chorus first,
then the pan.  The last of those is the only one likely to be audible, and `both_wet` in the
capture can probably settle it.

## 10. Open questions, in the order they should be settled

1. **`[open]` Why the LFO slots ramp symmetrically and the voices do not.**  Section 4.  The
   mode register is the lead.  This matters beyond the effects: if the voices' 16:1 asymmetry
   is not really about direction, `ENV_FALL_DIVISOR` is modelling the wrong thing.
2. **`[open]` Whether the delay line's 8-bit width is audible.**  IC17 is 8 bits wide, so the
   delayed copy is stored at 8 bits, very likely through the same companding curve the wave
   ROM uses -- IC15 already has a decoder for it.  Worth modelling only if a measurement shows
   the wet path noisier than the dry one; nothing in this capture says it is.
3. **`[open]` What the other three effect config bits do.**  Bit 4 is set on every effect mode
   and bits 2, 5 and 6 vary with the group sizes, so they are probably routing rather than
   effect parameters.
4. **`[open]` Registers `0x19` and `0x1B`.**  Written `0x00` and `0x21` here and at the
   `0x43DA` init, never anything else.  Reading them as the slot scan range is a guess that
   fits `m_env_slots = 0x22` but has no evidence.
