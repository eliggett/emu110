# Sample reconstruction, pitch shifting, and the treble excess

State as of 2026-08-31.  Companion to `ENVELOPE-DECAY.md`, which covers amplitude; this
one covers spectrum.

The symptom, in the user's words: *"the emulator sounds bright. There's a definite increase
in the 7k-8k band when I compare them."*

**Resolved.**  It was reconstruction images from pitch-shifting, and one more order of
interpolation kernel removes most of them.  `U110_RECON=1` (quadratic B-spline) is now the
default; `U110_RECON=0` restores the old linear path for A/B.

    A. PIANO 1 note 43, emulator minus hardware, level-matched 150-1000 Hz

                   3-4k   5-6k   6-7k   7-8k   8-9k  9-10k   mean
      linear       +1.9   +3.3   +6.7  +11.5  +11.3  +11.0    6.2
      quadratic    +0.9   +0.7   +2.2   +4.1   +1.9   +0.7    1.5

Six scratch tones at note 60: **7.3 -> 3.9 dB** mean absolute error.  The 76-trial envelope
score is unchanged at 8.6%, as it must be -- this changes spectrum, not timing.

---

## 1. What the machine is actually doing `[C]`

**Samples are stored decimated, and played back well below their stored rate.**  Every split
of A. PIANO 1 plays its sample 5 to 24 semitones below that sample's reference note.  That
is deliberate ROM economy, and it is the root of everything here.

    A. PIANO 1   splits [30, 39, 46, 52, 61, 71, 84, 96], samples 0-8
    note 43   -> sample 2, reference note 64, i.e. 21 semitones down
              -> step 2^(-21/12) = 0.2973 predicted
              -> firmware wrote 0x12E5 = 0.2952  (the 0.7% is the sample's `fine` byte, 65)

So for that note the sample runs at **9450 Hz** and its Nyquist is at **4725 Hz**.
Everything above 4.7 kHz in the output is reconstruction -- on both machines.

**Our sample choice and rate match the firmware exactly**, so the hardware plays the same
data at the same rate.  Whatever differs is in how the two reconstruct it.

Playing a 9450 Hz source out of a 32 kHz engine is a 3.39x upsample.  Doing it correctly
needs a brick wall at 4725 Hz; linear interpolation is a crude approximation of one, so
copies of the baseband centred at 9450 Hz, 18900 Hz... survive.  The first copy occupies
**4725-14175 Hz -- inside the audio band** -- and is baked into the 32 kHz stream before the
DAC ever sees it.

### These are IMAGES, not aliasing

Worth keeping straight, because it decides what can fix them:

* **Aliasing** happens at record time, or when decimating.  Content above Nyquist folds
  down and is unrecoverable.
* **Images** happen at reconstruction time.  The spectrum repeats; the copies are removable
  by filtering, and the interpolation kernel IS that filter.

Ours are images.  The kernel's frequency response is just the Fourier transform of its
kernel, so rejection follows directly:

| reconstruction | response | rejection of an image of 1 kHz content, fs = 9450 |
|---|---|---|
| drop-sample | sinc | ~-13 dB |
| linear (what we had) | sinc^2 | -37 dB |
| **quadratic B-spline (now)** | sinc^3 | **-55 dB** |

The gain is strongly frequency dependent, and that asymmetry is the fingerprint that
confirmed the diagnosis: one more order buys **18 dB** on an image of 1 kHz content (landing
near 9450 Hz) but only **2.6 dB** on an image of 4 kHz content (landing at 5450, just above
the source Nyquist).  The measured error had exactly that shape -- +11 dB at 7-10 kHz,
+3 dB at 5-6 kHz.

### It scales with each tone's own step, and that is measurable

Predicted extra attenuation at 6-9 kHz from each tone's step, against what actually happened
when the kernel changed:

| tone | ref | step | source rate | predicted | measured |
|---|---|---|---|---|---|
| bell (BELL 1) | 93 | 0.149 | 4757 Hz | -14.7 .. -25.0 | **-14.4** |
| vib (VIB 1) | 71 | 0.530 | 16951 Hz | -1.9 .. -4.5 | **-2.5** |
| piano (A.PIANO 1) | 71 | 0.530 | 16951 Hz | -1.9 .. -4.5 | **-2.4** |
| fbass (FINGERED 1) | 61 | 0.944 | 30204 Hz | -0.6 .. -1.3 | **-1.0** |
| marimba (MARIMBA) | 63 | 0.841 | 26909 Hz | -0.7 .. -1.7 | **0.0** |
| slap (SLAP 1) | 49 | 1.888 | 60408 Hz | -0.1 .. -0.3 | n/a, gated off |

The model predicting its own effect tone by tone is the strongest evidence here.

---

## 2. STEP > 1 GETS NO ANTI-ALIASING AT ALL `[known, not audible, deprioritised]`

**Checked by ear against the hardware on 2026-08-31 and closed for now.**  The user played
the worst-offending patches on the real U-110 and on the emulator: *"I did not hear any
noticable image artifact sounds on the patches indicated, emulated or hardware."*  Both
machines behave the same way, and neither is objectionable.

The likely reason is content rather than filtering: the offenders are bass and synth tones
whose source material carries little energy near its own Nyquist, so what folds down is
low-level and masked by the tone itself.  Aliasing is only as loud as the content that
folds.

So this is documented, not scheduled.  What follows is what it IS, so that if a tone ever
does sound gritty in a way that moves DOWN the keyboard as you play UP, the mechanism is
already written down.

When a sample plays *faster* than its stored rate we are **decimating**, and there is no
anti-alias filter anywhere in the path.  Source content above the 32 kHz engine's Nyquist
folds down into the audible band, and unlike images it is **unrecoverable** once folded --
no downstream filter can separate it from real music.

Tones known to be there:

    SLAP 1              step 1.888   source 60408 Hz   content to 30 kHz folds
    VIB 1 at note 96    step 2.000   source 64000 Hz
    P-27 Synth Bass note 57, both voices:
       v01  bank 3400  addr 2E7C9  step 0x482F = 1.1279  loop 123 bytes
       v02  bank 2400  addr 06417  step 0x5573 = 1.3351  loop  97 bytes

The three-tap kernels are **gated off above step 1** (see section 5), so those tones get
plain linear interpolation, exactly as before.  They are not made worse -- but they are not
helped either.

**If it ever needs fixing, the fix is oversampling, not interpolation.**  Run the voice
engine at a multiple of 32 kHz so the folded content lands above the audio band, apply the
output filter there, then decimate.  This is the one place the suggestion in section 4 is
right.  It would need `set_rate_divider()` lowered with the envelope rate scaled to
compensate, since `env_rate` is applied per output sample.

**How to find the offenders again.**  A patch scan measured 77 of 182 patch/note
combinations above step 1, concentrated in P-15 to P-30.  Worst: P-27 at note 72 (step
2.683), P-25 at 84 (2.488), P-26 at 60 (2.236), P-19 at 84 (2.229, across four voices).
P-26, P-21 and P-20 are already past 1.8 at middle C.  Render a MIDI that program-changes
through the presets playing a few notes each, trace with `-log`, and read register 04/05 at
PC 67FF -- see section 8.

---

## 3. What is measured correct `[C]`

`-wavwrite` **does** include the full analog chain.  The tap is the speaker's *input* buffer
(`sound.cpp:332`), and the two Sallen-Key sections, the RC and the EQ correction are all
sound devices upstream of it.  Only MAME's own mixer effects rack is excluded.  Verified
against the rendered files: the rolloff is unmistakably present.

The modelled analog chain, dB re 1 kHz, computed independently and agreeing with the
driver's own comment:

    SK1 f0 6740 Hz Q 1.736   SK2 f0 11708 Hz Q 1.214   RC fc 7234 Hz
    peak +4.17 dB at 6087 Hz  -- the service notes' own simulation says +2.17 dB max

That excess resonance is real but small: correcting Q1 to 1.315 changes 7 kHz by only
+2.4 dB, 8 kHz by +1.9.  It is not the treble excess and never was.

---

## 4. Eliminated, with evidence `[C]`

| hypothesis | how it died |
|---|---|
| Interpolator truncation (`>> 14` to 12 bits before the envelope multiply) | Plausible -- the error is signal-scaled, so it follows the note down. Tested with `U110_INTERP` 0/1/2 (floor / round / full precision): moves 6-9 kHz by **0.1 to 0.4 dB** on all six tones. |
| "Multiply-add": add `delta * step` per output sample instead of interpolating | **Algebraically identical.** Within one byte the value is `s[n] + d[n+1]*frac` and frac grows by `step` each output sample, so it reproduces the same piecewise-linear curve. Convolution and integration commute. Checked numerically: max difference 7e-15. |
| Oversample the engine to 192 kHz, filter there, decimate (Gemini's Method A) | Tested numerically on a 9450 Hz source: images at 6450 / 7950 / 8950 Hz come out at -16.5 / -29.9 / -49.9 dB against -16.5 / -29.7 / -50.4 for the 32 kHz path. **Unchanged within 0.5 dB.** The premise is "stop the images folding down", but nothing folds -- they are at multiples of the SOURCE rate and are born in-band. Correct medicine for step > 1 (section 2), wrong disease here. |
| The demux / sample-and-hold / fast DAC suppress them in hardware | Cannot. A 9450 Hz image is indistinguishable from a genuine 9450 Hz partial to anything downstream of the DAC. The per-channel S/H is still 1/32000 long: `sinc(f/32000)` = **-1.3 dB at 9.45 kHz**. What the fast DAC and demux buy is suppression of the 32 kHz+ family, which is why a gentle analog filter sufficed. The simple filter is evidence the chip never MADE these images, not that it removed them. |
| Hardware plays a different, higher-rate multisample | Keymap says no. Note 43 takes sample 2, reference note 64; predicted step 0.2973 against the firmware's 0.2952. Our selection and rate match the firmware, and the firmware is the hardware. |
| The output EQ correction's shape is the fault | `[RETRACTED]` This was argued from long-window scores that showed the EQ helping at note 43 and over-darkening at note 60, i.e. varying with step, which no filter can do. Re-measured in the corrected short window the EQ helps in **both** cases (note 43 mean 6.9 -> 2.7 dB, six tones 6.8 -> 3.9), so there is no step dependence and the argument is withdrawn. See section 7. |
| Sallen-Key resonance too high | Real (+4.17 vs +2.17 dB) but worth only ~2 dB at 7-8 kHz. Not the excess. |
| A *smoother* kernel is wrong; a *sharper* one is right | Followed from a measurement artefact -- see section 6. The 16-tap windowed sinc overshoots (-12.5 dB at 8-9 kHz on note 43) and Catmull-Rom undershoots badly (+15.5 dB at 7-8 kHz, worse than linear, because an interpolating cubic peaks near Nyquist). Quadratic sits between them, where the hardware is. |

---

## 5. The bug that got shipped, and how it was caught `[C]`

The quadratic went in **ungated**, and `U110_RECON=2` was the only mode checking
`step <= 0x4000`.  Found by ear within an evening: *"note 57 played through patch P-27:Synth
Bass sounds very different... fuzzy with the fix enabled."*

`smpl_cur` and `smpl_nxt` are always adjacent bytes, but the third tap comes from the
previous **output** sample, which sits `step` bytes back -- one byte or two, depending where
the fraction falls.  Above step 1 the three taps are irregularly spaced while the weights
assume unit spacing, and the spacing pattern beats against the fractional part of the step.
On Synth Bass (steps 1.1279 and 1.3351, loops of only 123 and 97 bytes) that is a beat every
~8 output samples on a strongly periodic tone.  Plainly audible.

With the gate, that note renders **bit-identical** between `RECON=0` and `RECON=1`.

Linear uses only the adjacent pair and was never affected -- which is exactly why the
fault appeared only when the new kernel was switched on.

---

## 6. The measurement trap `[C]`

**Do not compare spectra of decaying notes over long windows.**  It conflates spectrum with
decay rate, and it cost two wrong kernels.

Scoring the six scratch tones at 6-9 kHz relative to 200-1200 Hz, emulator minus hardware:

    window            vib   bell   slap  marimba  fbass  piano
    0.05-0.25 s      +9.3  +26.6   +3.5    -7.5   +4.9   +9.3
    0.2 -1.2  s      -3.0  +14.6   +3.1   -17.7   +0.5   +7.3

The long window says vib, marimba and fbass are too *dark* and invites a "two opposite
faults" story -- passband droop plus images.  The short window says the emulator is simply
too **bright** on five of six tones, which is one fault.  Marimba decays at 52 dB/s, so most
of a one-second window is tail and noise floor, and the two machines' floors differ (theirs
is analog noise, ours is dither at -93 dBFS).

Modes 2 and 3 were both built on the long-window reading.  Use **0.05-0.25 s after the
onset**, and check the window before believing a spectral comparison.

---

## 7. Still open

**Step > 1 has no anti-aliasing.**  Section 2.  Structurally real, but checked against the
hardware by ear and inaudible on both machines, so it is documented rather than scheduled.

### Marimba is 6-12 dB too dark at 6-9 kHz `[open]`

The one tone where the emulator has LESS high-frequency energy than the hardware, and the
largest single spectral error left.

    MARIMBA, scratch patch tone 22, note 60
      sample reference note 63, step 0.8409 -> source rate 26909 Hz, Nyquist 13454 Hz

so 6-9 kHz is **genuine recorded content, not images** -- which is why no kernel touches it.
Measured at 6-9 kHz against 200-1200 Hz, emulator minus hardware, 0.05-0.25 s window:

    linear + EQ   -12.1        quadratic + EQ   -11.7
    linear no EQ   -7.5        quadratic no EQ   -6.7

Two things worth noting.  The EQ correction accounts for about 4-5 dB of it, and marimba is
the only tone that measurably wants the EQ **off** -- which ties it to the open question
below.  The remaining ~7 dB is unexplained.

The envelope is not the cause: the decay fits at 52.33 dB/s on hardware against 47.52 here,
-9%, in line with the rest of the set.

Not yet checked, in the order I would check them: whether the keymap picks the same sample
the firmware does (the check that settled A. PIANO 1 in section 1, and it is cheap); whether
the loop length and loop mode are handled correctly for this sample; and whether the raw
decoded ROM data has the high-frequency content at all, by rendering the sample directly
with `tools/render_note.py` and comparing against the hardware capture.  If the raw sample
is already dark, the fault is in the decoder, not the player.

### The output EQ correction is unexplained `[open]`

    FILTER_BIQUAD PEAK, fc 5933.3 Hz, Q 1.9306, gain 0.436787 (-7.195 dB)
    roland_u110.cpp, switchable at runtime and with U110_EQ

It is a fitted correction layer, not derived from the circuit, and it **helps consistently**:

                          note 43      six tones
      quadratic, no EQ      6.9 dB        6.8 dB
      quadratic + EQ        2.7 dB        3.9 dB

`[RETRACTED]` An earlier reading of this document claimed the EQ's required amount varies
with playback step -- which would have meant it could not be a filter response at all.  That
came from long-window scores; in the corrected short window (section 6) it helps in both
cases, and the claim is withdrawn.  Do not build on it.

What remains genuinely open is **why it is needed**, since we now know the treble excess it
was originally fitted against was largely reconstruction images, which the kernel has since
removed -- and yet the correction still earns its place.  The leading candidate is the known
error in the analog model: the Sallen-Key chain as modelled peaks **+4.17 dB at 6087 Hz**
where the service notes' own simulation of the same circuit says **+2.17 dB max** (section 3).
Twice the resonance, in the same place.  Correcting Q1 from 1.736 to 1.315 is worth about
+2.4 dB at 7 kHz, so it plausibly accounts for a good part of a -7.2 dB bell centred at
5.9 kHz, but not obviously all of it.

The principled repair is to fix the circuit model first and then re-fit or delete the
correction -- in that order, because re-fitting the EQ against a wrong Sallen-Key Q just
re-encodes the error.  Until that is done the emulator carries a correction it cannot
justify from the hardware, and the spectral side is not finished.

**We may now be quieter than the hardware above 10 kHz.**  Hardware's peak-to-median there
is 8.0 dB -- its own noise floor -- while ours is dither.  After A/B-ing the two the user's
verdict was *"I think the Piano might sound better on the emulator, which I'm just fine
with"*, and that is probably what is happening: no analog hiss, and the images that used to
sit on top of the piano are gone.  Recorded as a fidelity question rather than a defect --
if strict hardware fidelity ever matters more than sounding good, this is the knob.

---

## 8. Switches, data and tools

    U110_RECON=0|1|2|3   reconstruction kernel: linear / quadratic B-spline (default) /
                         16-tap windowed sinc / Catmull-Rom.  Modes 1-3 apply at step <= 1
                         only; above that all fall back to linear.
    U110_INTERP=0|1|2    interpolator precision: truncate (default) / round / full.
                         Kept for the record; measured irrelevant.
    U110_EQ=0|1          force the output EQ correction off or on, bypassing cfg.
    U110_ENVTRACE=1      the envelope handshake trace, see ENVELOPE-DECAY.md.

    listen/hardware/4/01_piano_vel_43.wav      the velocity sweep on note 43
    listen/hardware/env3/08_scratch_tones.wav  six tones, dictated scratch patch, note 60
    listen/hardware/env3/09_scratch_keys.wav   VIB 1 at notes 24..96, i.e. 7 steps
    listen/comparisons/reconstruction-kernel/  hardware / linear / quadratic on note 43

Tracing which sample a note actually plays, and at what rate:

    ./roland u110 -min <file>.mid -wavwrite /dev/null -seconds_to_run <n> \
      -nothrottle -video none -log -cfg_directory "$(mktemp -d)" -nvram_directory "$(mktemp -d)"
    awk '$3+0>=<t> && $3+0<=<t+0.002> && $6!="1F"' error.log

Registers: 02/03 bank, 04/05 step (2.14, 0x4000 = unity), 08-0B start address (18.14),
0C/0D end, 0E/0F loop, 06/07 the envelope segment.  Resolve a start address against the
wave ROM sample table with `tools/render_note.py`'s `load()` and `tone_rec()`.
