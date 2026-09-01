# The envelope decay investigation

State as of 2026-08-31.  What is settled, what is eliminated, and what to try next.

**The A. Piano 1 fault is SOLVED** -- see section 5.  The rest of the document is the
trail that led there and is kept because the eliminated hypotheses are the expensive part.

The audible symptom, in the user's words: *"Both clearly have some of those high frequency
partials hanging out longer than they should and longer than the hardware. All other
instruments that I tried sound great."*  Ac.Piano only.

---

## 1. The chain, as decoded from the firmware `[C]`

The U-110's amplitude envelope is **the CPU's**, not the chip's.  The chip provides a linear
ramp generator; the exponential shape is assembled by the firmware as a ladder of linear
segments.  That is a sensible 1988 design and it is what the code does.

**Registers 06/07 are one envelope SEGMENT** -- 07 is the target level (log, 16 units per
octave, 0.3763 dB per unit), 06 is a signed ramp rate.

**Phase machine.**  `3700[voice]` holds the phase in its low 3 bits; `0x41FF`-`0x4235`
dispatches to handlers at `B932`(1) `BAF4`(2) `BD42`(3) `BE7F`(4) `BEBD`(5, release).
Bit 4 routes to the ladder-continuation path at `0x42B3`.

**Phase 1's rate** (`B94A`-`B991`) is `(key - 0x45) x (tone[+0xAF] >> 4)`.  `B957` picks
between two parameter sets (`+0xA9/AA/AF` vs `+0xC9/CA/CF`) on bit 3 of the voice state,
which `C128`-`C134` sets when the key exceeds a per-tone threshold at `tone[+0xB0]` -- a
key split inside the tone.

**Ladder step** (`0x42BB`-`0x42EE`):

    rate_new = rate_stored
             + (reported_level - target) / 2        <- the only value from the chip
             + (3986[voice] x target) >> 9

`3986[voice]` is set at note-on (`0x76CF`-`0x76D5`) as `(new_level - old_level) / 2`.

**Rate byte is a float**: `C0C4` takes a signed exponent with `shrab #03`, keeps a 3-bit
mantissa with `andb #07`, bumps the exponent with `incb`, saturates at `0xFC`.  8 counts =
one doubling.  **The U-220 firmware contains this routine byte-for-byte identically** at
`0x0D636` (different registers, same algorithm, same `0xFC` saturation) -- independent
confirmation that the rate encoding is the chip's contract, not a U-110 quirk.

**Select 0x16 is a register read-back, not a computed level** `[C]`.  `0x7655` selects
`0x16`, reads it, **discards the value**, then reads the LINEAR level from `0x10` and `0x12`
and derives the logarithm itself (`0x7690`: `shlb #04` for the exponent, `shrb #03`/`andb
#0f` for the mantissa).  A routine holding a log level would not pay for that normalise.
The discarded read latches the level so the `0x10`/`0x12` pair cannot tear mid-ramp.
Committed as `roland_lp.cpp` returning `chn.volume` verbatim (mame `b7cdb70a`).

---

## 2. What is measured correct `[C]`

From `listen/hardware/env3` (the scratch-patch take, 76 trials, every parameter dictated
over SysEx so tone is the only variable).  Scored with `tools/scratch_analyse.py`:

| what it varies | mean error |
|---|---|
| `scratch_keys` -- rate, level fixed | **7%** |
| `scratch_env_attack` | **5%** |
| `scratch_slow_vib` | **5%** |
| `scratch_velocity_vib` | **7%** |
| `scratch_level_vib` | **9%** |

**The in-note fall is LINEAR IN AMPLITUDE**, settled by the PART LEVEL sweep: dropping the
level makes the decay *faster* (Vib 1 x2.67 across the sweep) against a linear-ramp
prediction of x2.5 and a log-domain prediction of x0.19 in the other direction.  This does
not contradict `../listen/hardware/env/ANALYSIS.md` section 1, which fitted the *release* --
that really is straight in dB.  The two are separate mechanisms, confirmed by
`scratch_env_release`: seven ENV RELEASE settings produce a byte-identical in-note ladder.

Overall the readback fix took the 76-trial mean absolute error from **28.5% to 14.1%**.

---

## 3. Ac.Piano: the remaining fault `[C]`

**A. Piano 1 is a V-MIX tone.**  The Owner's Manual (line 661) lists five tone structures:
Single and V-Sw use one voice; **Dual, Detune and V-MIX use two**.  The tone list marks
A. PIANO 1 as `V-MIX`.  This is invisible in the panel UI -- the part shows one Tone Select
and the second voice is internal to the tone.

**The two layers cannot be panned apart.**  `update_voice_routing()` assigns voices to
Voice Groups strictly in index order by the Output Mode's group sizes, and both of a tone's
voices belong to the part's group.  There is no per-partial output in the part block.

**But velocity separates them for free.**  Register dumps show both voices playing identical
sample start addresses (`0E82A` layer A, `04FF7` layer B) at every velocity -- V-MIX
crossfades *levels*, not samples -- and **layer B's envelope target is literally 0 below
velocity 71**.  So:

* velocities 1-57 give **layer A completely alone**;
* layer B is recovered by subtracting a scaled layer A from the vel-127 mix, the scale
  taken from the firmware's own targets (A at 212 vs 179 = +12.4 dB), not fitted.

Layer B is **12 dB brighter** than layer A in the raw samples (HF/LF -17.0 dB vs -28.8 dB),
so a slow layer B reads as sustained high partials.  Unlike marimba (body falls 2.2 dB, i.e.
flat), the piano samples carry real decay: **9.2 dB and 9.0 dB over 1.58 s**.

### The measurement

    LAYER A ALONE (vel 57)          hardware   emulator
      broadband                       4.07       3.81 dB/s     -6%   correct
    LAYER B (isolated, 2.5-8 kHz)     4.63       1.71 dB/s    -63%   WRONG

Spectral tilt of the full note (vel 127), emulator minus hardware, HF 2.5-8k vs LF 200-1200:
**+3.1 dB at the attack, widening to +5 to +7 dB by 2 s and staying there.**

### Why: layer B is abandoned after one segment

    layer A (v13):  attack 239 -> rate -50 target 212 -> [2.375 s] -> -32/196 -> -24/180
    layer B (v14):  attack 239 -> rate -51 target 212 -> rate -33 target 196 -> nothing
                                                                                for 7 s

Layer B's single segment needs **12.7 s** to traverse (delta 28.3 M at rate -33 = 2.235 M/s)
inside a 7 s note.  It never arrives, so it never raises an interrupt, so the firmware never
programs the next rung.  It creeps down and effectively holds.

**The emulator is executing this faithfully**: layer A's first segment predicts 2.37 s under
the same arithmetic and the log measures 2.375 s.  The ramp is not the fault.

The open question at this point was why the firmware writes layer B's second segment
0.34 ms after its first, with nothing having arrived in between.  **Section 5 answers it:
it does so because we asked it to.**

---

## 4. Eliminated, with evidence `[C]`

| hypothesis | how it died |
|---|---|
| Sample-playback / baked-in decay | Marimba's body falls 2.2 dB over 0.448 s -- flat.  The decay is the envelope. |
| Log-domain falling ramp | Fixes piano (+1%) but wrecks vib (-78%) and slap (-57%); PART LEVEL sweep says linear. |
| A corrected rate exponent or `ENV_FALL_DIVISOR` | Error is bimodal, not a function of rate: slap (rate 22.8) fine, fbass (24.9) 2x off.  No single divisor fits -- the code comment already said so. |
| Zero-length segment consuming a ladder rung | Suppressing the duplicate arrival changed nothing (45% -> 45%). |
| `0x16` low byte (rate) feeding `C0C4` | Returning the pre-zero-length rate changed nothing at all. |
| Ramp overshoot past target | Implemented properly (announce once, keep ramping): 45% -> 44%.  At a 32 kHz interrupt ceiling the CPU answers within a sample or two, so the term is ~0 on both sides. |
| Level readback / `3986[voice]` | **No `0x10`/`0x12` read happens anywhere near the note-on.**  The firmware does not consult it there. |
| Spurious ARRIVAL causing layer B's extra segment | Arrival log shows **no arrival between** layer B's two writes, 0.34 ms apart.  Correct as far as it went, and it pointed the wrong way: the extra segment came from a spurious **service**, not a spurious arrival -- an interrupt with nothing behind it.  See section 5. |
| Stale-object layout mismatch from editing `sound.h` | All 18 objects that depend on it were rebuilt; zero stale. |

---

## 5. SOLVED: the chip was interrupting the CPU more often than it meant to `[C]`

Not the ramp, not the rate law, not the readback.  **The offer on the INT line was being
made more than once per arrival**, and the firmware answers every offer it is given.

### What the firmware does

    41C4: orb int_mask, #40      handler entry -- and it RE-ENABLES EXTINT
    41C7: ei
    ...
    41D0: ldbze 54, 1400         read register 00: which slot am I being offered?
    41D5: cmpb  54, #20          0x20 / 0x21 are the two chorus-tremolo slots
    ...
    41F5: ldb   56, 3700[54]     that voice's phase
    41FF: andb  57, 56, #07      -> lcall B932 / BAF4 / BD42 / BE7F / BEBD

Register 00 is read **exactly once per interrupt**, and nothing in the handler checks that
the named voice has actually arrived.  It trusts the offer and steps that voice's phase
machine.  So an interrupt the chip did not mean to give **is** an envelope segment the
voice did not ask for.

### What our chip was doing

`env_scan()` ran on a timer and **toggled** INT on every tick for as long as *any* voice
was pending.  The tick is 62.5 us; the firmware's handler measures about 300 us.  So four
or five rising edges landed while the CPU was still inside the handler, the 8x9x latched
one of them, and it was answered *after* the read of register 00 that acknowledged the real
offer -- by which time `m_env_pending` was empty and `m_env_service` still named the voice
just serviced.

Traced directly (`U110_ENVTRACE=1`, note 43 velocity 127 on A. Piano 1):

    ENVARR 21.303969 v03                    both voices arrive at the attack target
    ENVARR 21.303969 v04
    ENVACK 21.303983 v03 pending=00000018 real     -> BAB8: rate -50 target 212
    ENVACK 21.304283 v04 pending=00000010 real     -> BAB8: rate -51 target 212
    ENVACK 21.304582 v04 pending=00000000 STALE    -> BD06: rate -33 target 196
    ENVRD  21.304582 -> v04  @':maincpu' (41D5)    ...from the handler, so a real interrupt

That third line is the whole fault.  All three reads come from 41D5, so the CPU really did
take three interrupts for two arrivals.  The spare one lands on the **last voice serviced**,
which on a two-voice tone is always the second layer.  Layer B is handed its phase-3 rung
0.3 ms after its phase-2 rung, while its level is still at the attack peak rather than at
212, so the rung spans 43 log units instead of 16 and needs **12.7 s** inside a 7 s note.
It never arrives, never interrupts, and the ladder stops there.

That also answers the three questions this investigation kept circling:

* **Is the decay commanded per layer?**  Yes -- each voice has its own segment registers and
  its own phase in `3700[voice]`, and the two layers of a V-MIX are two independent voices.
* **Should layer B decay at the same rate as layer A?**  Nearly, not exactly.  The firmware
  gives them rates one count apart (-50 / -51, then -32 / -33) because their targets differ.
  What it does not do is give one of them four times the duration of the other.
* **Was the CPU reading a bad number out of our output stage?**  No.  The level readback is
  fine; the fault was in the interrupt *handshake*, one register earlier.

### The fix

One rising edge per offer, and no further edge until the CPU has taken it: `env_scan()`
asserts, drops the line on the next tick, and does not assert again until the read of
register 00 clears `m_env_offered`.  A re-pulse after `ENV_OFFER_RETRY` ticks (~2 ms) covers
an edge lost while the firmware had EXTINT masked -- safe, because the pending bit is still
set, so a re-offer names the same voice.

### What it bought

Note 43 at velocity 127, decay fitted 0.4-5.5 s after the onset:

| | hardware | before | after |
|---|---|---|---|
| broadband | 4.05 dB/s | 2.42 (-40%) | 4.56 (+13%) |
| 2.5-8 kHz | 4.17 dB/s | 1.89 (-55%) | **4.80 (+15%)** |

Across the 76-trial scratch set, `tools/scratch_analyse.py --hw listen/hardware/env3 --emu
listen/emulated/env3-emu-D`:

| | before | after |
|---|---|---|
| mean absolute error, 76 trials | 15.7% | **8.6%** |
| worst segment | 34% | 24% |
| piano (`scratch_tones`) | 1.98 vs 17.79 dB/s, **-89%** | 17.02, **-4%** |
| marimba | -25% | -9% |
| fretless bass | -10% | -4% |

**The attack scale is untouched**, which was the constraint: the six-setting ENV ATTACK
sweep measures 0.120 / 0.035 / 0.040 / 0.045 / 0.050 / 0.050 s against hardware's 0.115 /
0.040 / 0.045 / 0.045 / 0.045 / 0.050, the same as before the change to within the 5 ms
analysis step.  Nothing in the ramp arithmetic moved; only the delivery of the interrupt.

Listen: `listen/comparisons/piano-offer-fix/`.

### Tracing it again

`U110_ENVTRACE=1` on the emulator binary logs `ENVARR` (a ramp arrived), `ENVACK` (the CPU
read register 00, with the pending mask and whether the offer was real or `STALE`) and
`ENVRD` (the same read from the driver side, with the CPU's PC).  Costs one predictable
branch when unset.

### Also still open

**Bell 1 decays 62% too FAST** (5.56 dB/s on hardware, 9.14 here), has no zero-length
segment, and is **completely untouched by the interrupt fix** -- 62% before, 64% after.  It
is the only tone wrong in the other direction and is a separate fault.

**VELO SENS** is the worst segment left at 24%, almost all of it one trial (`s0/v100`,
+94%).  Also unmoved by the fix.

**Layer A's high partials at low velocity.**  At velocity 57, where layer A plays alone,
broadband matches (hardware 3.71 vs 3.60 dB/s) but 2.5-8 kHz does not (2.18 vs 3.60): our
layer A sheds its top end faster than the hardware does.  That is a spectral-tilt question,
not an envelope one, and belongs with the output-filter fit rather than here.

---

## 6. See also

`RECONSTRUCTION.md` covers the spectral side: pitch-shift images, the interpolation kernel,
and the fact that samples played ABOVE their stored rate still get no anti-aliasing at all.
It also records the measurement trap that bit this investigation twice -- comparing spectra
of decaying notes over a one-second window mixes decay rate and noise floor into the answer.

---

## 7. Where the data and tools are

    listen/hardware/env3/          the scratch-patch take, 76 trials + ANALYSIS.md
    listen/hardware/env/           release/attack sweeps, 15_decay_hold.wav
    listen/hardware/4/             velocity sweep on note 43 (the layer separation)
    tools/scratch_analyse.py       scores a scratch take against its render
    tools/capture_env.py --set scratch    the sweep that produced env3
    tools/u110_sysex.py            SysEx, framing taken from the firmware's parser
    analysis/readback-target-hypothesis.patch   superseded by mame b7cdb70a

Render a comparison without disturbing the interactive setup:

    SDL_VIDEODRIVER=dummy ./roland u110 -min in.mid -wavwrite out.wav \
      -seconds_to_run <midi_len + 20> -nothrottle -video none \
      -cfg_directory "$(mktemp -d)" -nvram_directory "$(mktemp -d)"

`-min` delivers the file ~10 s after its own timestamps; add that to `-seconds_to_run`.
`-wavwrite` is the pre-effects record buffer and **never exercises the live output path**.
