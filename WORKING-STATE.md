# Working state — ping-pong / delta decode

Snapshot of the debugging context, so it survives a compaction. The settled findings live
in `analysis/ROM-ANALYSIS.md` §3a; this file is the *scaffolding* around them.

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
