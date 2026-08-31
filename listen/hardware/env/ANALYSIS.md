# What the envelope take measured

Take: `tools/capture_env.py --out-dir listen/env`, 15 sweeps, 172 trials, 860 s, peak
-15.7 dBFS, no clipped samples.  Measured with `tools/env_analyse.py`; per-trial numbers in
`measurements.csv`.  Rate bytes joined from `listen/env-emu/rates.txt`, an emulator render
of the identical sequence (the firmware computes the same byte; MAME just ignores it).

## 1. The ramp is a straight line in dB `[C]`

Fitting the release two ways over ~36 dB of fall, 1245 envelope frames:

| model | residual |
|---|---|
| straight in **dB** | **0.30 dB rms** |
| straight in linear amplitude | 66 dB rms |

So the chip ramps a **log-domain** level at a constant dB/s.  Not a linear amplitude ramp.

## 2. Rate byte: speed doubles every 8 counts `[C]`

Two independent routes agree.

**ENV RELEASE RATE**, whose step moves the rate byte by 8 (`shl 40, #03` at `0x64E0`) —
each setting exactly halves the time, on three unrelated tones:

| ENV RELEASE | -6 | -5 | -4 | -3 | -2 | -1 | 0 | +1 | +2 |
|---|---|---|---|---|---|---|---|---|---|
| Vib 1, dB/s | 1.9 | 3.5 | 7.1 | 14.6 | 29.7 | 60.7 | 124.3 | 252.9 | 512.0 |
| E. Organ 1 | 5.0 | 10.1 | 20.8 | 42.6 | 85.1 | 172.5 | 343.9 | 684.0 | — |
| Choir 3 | 1.1 | 2.6 | 5.4 | 11.3 | 24.4 | 49.3 | 101.0 | 206.1 | 403.9 |

With a 25 s tail (`slow_release_*` in `listen/env2`) the vib series extends down to
**-7: 1.5, -6: 2.3, -5: 4.0, -4: 7.5 dB/s**. Those ratios are 1.53, 1.74, 1.88 rather than
2.0 — subtract an additive ~0.7 dB/s, which is the tone's own decay still running underneath
the release, and all four return to exactly 2.0.

Successive ratios: **2.02, 2.05, 2.05, 2.00, 2.03, 1.99, 1.99** (organ) and 2.04, 2.05,
2.03, 2.04, 2.05, 2.03, 2.02 (vib).  Beyond about 700 dB/s the 10 ms RMS window smears the
ramp and the numbers become lower bounds, not measurements.

**ENV ATTACK RATE**, whose step moves the byte by 16 (`shl 40, #04` at `0x6A10`), on Vib 1
where the sample's own attack is ~20 ms and does not mask the envelope:

| rate byte | 8 | 24 | 40 |
|---|---|---|---|
| dB/s | 14.1 | 53.4 | 175.2 |

x3.8 and x3.3 per +16, i.e. **~2x per +8**.  The attack route is biased slow at the low end
(the previous note's tail raises the floor), which is why the release figures are the ones
to trust.

So: **speed = k * 2^(rate/8)**, k ~ 7 dB/s at rate 0 from the Vib attack points.  The
absolute constant is the least certain number here.

## 3. Exponential, not float — and the ramp is LINEAR going up `[C]`

Settled by the follow-up take (`listen/env2`, `--set followup`), which used PART LEVEL to
land the rate byte on consecutive integers: **2..8**, **12..24**, plus 40 from the main
take.

**The attack is a straight line in AMPLITUDE, not in dB.** Vib 1, over 29 dB of rise:

| model | residual |
|---|---|
| straight in **amplitude** | **0.14 dB rms** |
| straight in dB | 2.36 dB rms |

Decay and release are the other way round — exponential (straight in dB): vib 0.24, marimba
0.70, fbass 0.48 dB rms against 67-76 dB rms for an amplitude-linear model. **Linear attack,
exponential decay and release.**

So the attack has to be read as a linear ramp: `T` = time from zero to the target =
`target / rate`. Regressing `log2(1/T)` on both registers over 41 trials:

| encoding | residual | coefficient |
|---|---|---|
| **exponential, `2^(rate/8)`** | **2.4 % in time** | 0.1249 = 1/**8.01** |
| float, 3-bit mantissa | 15.4 % in time | 0.80 (1.00 required) |

The float reading is dead. The rate byte is a **pure exponential with 8 counts per
doubling**, holding all the way down to byte 2.

There is a neat confirmation in why the ladder looked flat at first. The firmware sets
`reg06 = (reg07 * 127) >> 8`, and 127/256 is 1/2.016 — chosen so that 8 counts of `reg06`
exactly cancel 16 counts of `reg07`. **The attack time is deliberately independent of the
level**, which is only true if the base really is 8.

### Absolute calibration

    T = 2^(reg07/16 - reg06/8 - 10.886)  seconds

reproduces the fixed-target sweep at `reg06` = 8, 24, 40 (2.456 / 0.616 / 0.153 s) to under
1 %. That constant is what falls out of

> a **20-bit linear level**, incremented by **2^(reg06/8)** once per **engine sample
> (32 kHz)**

which predicts -10.904 against the measured -10.886, a 1.3 % difference. That is the
implementable form.

### Which makes the exponential release the CPU's doing `[I]`

If the chip only ramps linearly, the exponential decay and release cannot be the chip. They
do not have to be: the handler at `0x42B3` computes `56 = (3986[voice] * reg07) >> 9` and
adds it to the rate, with `reg07` refreshed from the chip's own level readback — **rate
proportional to current level, which is exactly an exponential**. That also explains why the
CPU needs a level readback at all.

The release residual is 0.23-0.27 dB rms, no larger than the tone's own amplitude ripple in
the sustain (0.21 dB), so if the rate is being stepped the steps are too fine to see. This
is the model to test once the interrupt is implemented: a linear ramp plus a working EXTINT
should come out exponential on its own, at the right rate, without anyone asking it to.

## 3b. `[--]` superseded: exponential or float?

Every rate byte this take produced is a multiple of 8.  A pure exponential `2^(rate/8)` and
a **float** encoding (3-bit mantissa, 4-bit exponent — the same shape the wave ROM uses)
agree **exactly** at multiples of 8 and differ everywhere else: across bytes 1..8 a float
is *linear* (ratio 2.0 from 1 to 2) where an exponential gives a flat 1.09 per step.

`capture_env.py --set followup` settles it.  PART LEVEL moves `reg07` a unit at a time and
the rate is `(reg07 * K) >> 8`, so parking ENV ATTACK/RELEASE low and sweeping the level
lands the byte on consecutive integers: **1..8**, **12..24** and **97..109** (confirmed
against an emulator render, `listen/env2-emu/rates.txt`).

## 4. The release rate scales with the target level `[C]`

Attack vs velocity on Brass 1 — same ENV settings, only velocity varies:

| velocity | 16 | 40 | 64 | 96 | 127 |
|---|---|---|---|---|---|
| `reg07` (target) | 171 | 194 | 211 | 226 | 239 |
| release dB/s | 254 | 376 | 566 | 796 | 1019 |

`8 * log2(ratio) / d(reg07)` ~ 0.24, so the release rate is about `(reg07 * 62) >> 8` —
the same shape as the note-on rate `(reg07 * K) >> 8` at `0x69F8`, with its own constant.

## 5. Two things the disassembly suggested that the hardware does NOT do

* **No hold-time correction.** `0x64AC` subtracts `f2 - 3680[voice]` from the release rate,
  which reads as "held longer, release slower".  E. Organ 1 (flat sustain, so nothing else
  changes) at holds of 0.2 / 0.5 / 1 / 2 / 4 / 8 s releases at **674, 698, 670, 677, 714,
  692 dB/s** — flat to +/-3%.  Vib 1 appears to speed up, but only because its own decay has
  taken it 46 dB down by the 4 s hold.
* **No key scaling of the release.** Organ at notes 36..84: **718, 704, 676, 713, 694,
  689 dB/s** — flat.  Vib again varies only through its own decay.

Both terms exist in the code; for these tones they evaluate to zero.

## 6. `[I]` The log level scale reads ~0.36 dB/unit, not 0.3763

PART LEVEL 127 -> 4, organ, 13 points spanning 26 dB, `reg07` 220 -> 149:

    0.3625 dB per unit, residual rms 0.08 dB

The velocity path over `reg07` 158..232 gives **0.3455** with rms 0.12.  Both sit below the
`0xAEC6` table's 6.0206/16 = **0.3763**, and they disagree with each other by 5% while each
fits its own points tightly — so a second variable is in play, not just noise.  Worth a
dedicated sweep before trusting either.

## 7. Natural decay over an 8 s hold, per tone

dB fallen at note-off, neutral ENV settings, note 60 velocity 100:

| tone | piano | vib | bell | marimba | slap | fbass | choir | strings | organ | brass | flute | shaku |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dB | -45.9 | -67.4 | -25.2 | -63.9 | -61.3 | -71.8 | -3.0 | -2.8 | -0.9 | -3.9 | -5.0 | -2.4 |

This is the decay-segment reference: no MIDI parameter reaches it, so it is the only view of
what the phase handlers do.
