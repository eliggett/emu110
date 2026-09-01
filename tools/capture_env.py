#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
capture_env.py -- drive a real U-110 through its ENVELOPE parameters and record the result.

    python3 tools/capture_env.py --list
    python3 tools/capture_env.py --out-dir listen/hardware/env
    python3 tools/capture_env.py --only release_sweep,release_by_hold

This exists to pin down one number that cannot be got any other way: **how fast one unit of
the chip's ramp-rate byte actually moves the level**.

What we know (analysis/ROM-ANALYSIS.md, "The amplitude envelope lives in the CPU"): the
volume pair at chip register 0x06/0x07 is an envelope SEGMENT -- register 07 is the target
level (log, 16 units per octave = 0.3763 dB/unit) and register 06 is a SIGNED ramp rate.
The chip ramps toward the target and raises EXTINT; the firmware handler at 0x41C4 writes
the next segment.  What we do not know is the ramp clock.

The trick this script sets up is a pairing.  The *emulator* runs the same firmware, so it
computes the same rate byte and logs it -- even though it then ignores it.  The *hardware*
gives the dB/s that byte produces.  Run the identical sequence through both:

    python3 tools/render_u110.py --sequence capture_env --log --out-dir listen/emulated/env-emu

and pair `reg06` from `listen/emulated/env-emu/error.log` against the slope measured here.  Two
unknowns, two measurements.

That pairing is already verified for the note-on rate.  Rendering `attack_sweep_vib`
through the emulator gives, for ENV ATTACK RATE -7..+7 at a fixed target of reg07 = 211:

    reg06 = 1, 8, 24, 40, 56, 72, 88, 104, 120, 127, 127, 127, 127, 127, 127

Steps of exactly 16 -- the `16 * (nibble - 8)` term at 0x6A0C -- around the unmodified base
(211 * 127) >> 8 = 104, clamped to 1 below and to the 0xB0C6 ceiling above.  So one sweep
yields nine distinct known rate bytes, and the hardware take gives the dB/s each produces.

The RELEASE sweeps do not pair this way, and it is worth knowing why before running them.
The emulator's note-off writes no volume at all: the firmware's release path at 0x64FF is
gated on the envelope phase, the phase only advances from the EXTINT handler, and that
handler never runs.  MAME cuts the voice's enable bit and substitutes its own fixed fade.
So for the release sweeps the rate byte has to come from the formula at 0x649A rather than
from a log -- which is exactly what release_by_hold is there to test.

Sweeps
------
The parameters reachable from outside are ENV ATTACK RATE and ENV RELEASE RATE (-7..+7 per
part), PART LEVEL and VELOCITY SENS.  Attack and release rates move register 06 directly;
level and velocity move register 07, which is already decoded, so those sweeps are controls
-- if they come out wrong, the level scale is wrong and nothing else can be trusted.

`release_by_hold` is the one that tests a specific claim rather than fitting a curve: the
note-off code at 0x649A reduces the release rate by how long the note was held
(`f2 - 3680[voice]`).  Same note, same settings, holds from 0.2 s to 8 s.  If the release
slope does not change with hold time, that reading is wrong.

SysEx
-----
capture_u110.py deliberately avoids SysEx.  Here there is no choice: the envelope rates are
not reachable by Program Change or Control Change.  So:

  * Everything written goes to `00 1n xx`, the TEMPORARY patch.  Writes to stored patches
    (`02 xx xx`) and to setup (`01 xx xx`) are refused by send_param() outright.
  * The run ends by re-sending the patch Program Change, which reloads the temporary area
    from the stored patch and undoes everything this script touched.  That works with no
    MIDI cable back from the U-110.
  * If the U-110's MIDI OUT *is* connected, the script first reads the parameters back
    (RQ1) and checks the reply.  No reply means SETUP:MIDI:EXCLUSIVE is OFF or the device
    ID is wrong -- and it says so and stops, rather than recording 12 minutes of a sweep
    that never happened.

The device ID is the CONTROL CHANNEL minus 1: the firmware at 0x5624 loads 0x3C01, masks
the low nibble, and compares that with the message's DEV byte.  Model ID is 0x23 (0x5BD4).
"""

import argparse, os, sys, time, wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_u110 as cap

SR, CH        = cap.SR, cap.CH
LEAD_SILENCE  = cap.LEAD_SILENCE
TAIL_SILENCE  = cap.TAIL_SILENCE
SEG_GAP       = cap.SEG_GAP
CONTROL_CH    = cap.CONTROL_CH
PATCH_SETTLE  = cap.PATCH_SETTLE
PARAM_SETTLE  = 0.15        # after a DT1 burst, before the note

MODEL_ID      = 0x23
DT1, RQ1      = 0x12, 0x11

# Part parameters, from the Owner's Manual section 4.2.2 (address 00 1n <off>).
P_TONE_MEDIA  = 0x02        # 0 = internal
P_TONE        = 0x03        # 0..98
P_LEVEL       = 0x07        # 0..127
P_VELO_SENS   = 0x08        # 0..15
P_ENV_ATTACK  = 0x0A        # 1..15  ->  -7..+7   (8 = 0, the neutral setting)
P_ENV_RELEASE = 0x0B        # 1..15  ->  -7..+7

# The rest of the part block (Owner's Manual 4.2.2), needed to dictate a patch outright
# rather than inherit whatever the factory patch happens to hold.  Verified one at a time
# against the emulator by writing a distinctive value and dumping the edit buffer at
# 0x2814: tone lands at part+0x01, bend/channel share part+0x02 (high nibble / low nibble),
# key range at +0x03 and +0x04, level at +0x05, output assign at +0x0B.
P_OUT_ASSIGN   = 0x00       # 0..6
P_RX_CHANNEL   = 0x01       # 0..15
P_BEND_RANGE   = 0x04       # 0..12
P_KEY_LO       = 0x05       # 0..127
P_KEY_HI       = 0x06       # 0..127
P_PITCH_COARSE = 0x0C       # 52..76  -> -12..+12 semitones, 64 = 0
P_PITCH_FINE   = 0x0D       # 14..114 -> -50..+50 cents,     64 = 0
P_LFO_RATE     = 0x0E
P_LFO_DELAY    = 0x0F
P_LFO_RISE     = 0x10
P_LFO_DEPTH    = 0x11
P_LFO_MRISE    = 0x12
P_LFO_MDEPTH   = 0x13
P_LFO_CHPRESS  = 0x14

# Patch COMMON parameters, temporary area 00 01 xx.  Chorus and tremolo are here, not in
# the part block, which is why they need their own writer.
C_CHORUS_RATE, C_CHORUS_DEPTH   = 0x19, 0x1A
C_TREMOLO_RATE, C_TREMOLO_DEPTH = 0x1B, 0x1C

# OUTPUT MODE, and it is the parameter that decides whether the effects exist at all: the
# enable bits are not in the chorus/tremolo bytes but in a config byte the firmware selects
# with this one (0xA726 + 8*index), and only the odd L/R modes 21-49 set them.  Every
# factory patch picks a dry mode, so nothing recorded before this existed has an effect in
# it.  See analysis/EFFECTS.md.
#
# The Owner's Manual address map OCRs too badly to read here, so the offset was found the
# way the part offsets were: write a distinctive value to each candidate and see where it
# lands in the edit buffer.  0x14..0x18 -> 0x2800..0x2803 and 0x280E, so 0x18 is the one.
# On the WIRE it is the mode number minus one, exactly as stored: 20 selects mode 21.
C_OUTPUT_MODE = 0x18
FX_MODE_WET, FX_MODE_DRY = 20, 21       # mode 21 <L31><R31>, mode 22 M31 -- the manual's pair

PARAM_NAMES = {P_TONE_MEDIA: 'tone_media', P_TONE: 'tone', P_LEVEL: 'level',
               P_VELO_SENS: 'velo_sens', P_ENV_ATTACK: 'env_attack',
               P_ENV_RELEASE: 'env_release'}

# The parameters a run may disturb, read back before and restored after.
TOUCHED = [P_LEVEL, P_VELO_SENS, P_ENV_ATTACK, P_ENV_RELEASE]


def env_val(display):
    """ENV ATTACK/RELEASE RATE: the front panel shows -7..+7, the wire carries 1..15."""
    if not -7 <= display <= 7:
        raise ValueError("env rate %d out of range -7..+7" % display)
    return display + 8


# ---------------------------------------------------------------- the sequence
#
# A segment is  dict(name=, label=, patch=, trials=[...])
# and a trial is  dict(params={off: value}, note=, vel=, hold=, gap=, tag=)
# `params` is written as DT1 to the part just before the note.  `tag` is what ends up in
# trials.csv, so make it say what the trial varies.

def _trial(note, vel, hold, gap, tag, params, common=None):
    return dict(note=note, vel=vel, hold=hold, gap=gap, tag=tag, params=params,
                common=common or {})


def common_order(d):
    """Patch-common writes in the order they must be sent.

    Writing ANY of the five effect parameters makes the firmware re-run its effect setup
    (0xB4C6) against whatever the other four currently hold -- confirmed in the emulator,
    where changing tremolo depth alone retargeted the running LFO mid-note.  So OUTPUT MODE
    goes LAST: whatever order the rest arrive in, the final setup is then run against a
    complete, intended set rather than a half-updated one."""
    rest = [(o, d[o]) for o in sorted(d) if o != C_OUTPUT_MODE]
    return rest + ([(C_OUTPUT_MODE, d[C_OUTPUT_MODE])] if C_OUTPUT_MODE in d else [])


# Tones chosen for different envelope characters, not for variety's sake:
#   0  A. Piano 1    strong decay, layered two voices per note
#  15  Vib 1         medium decay, clean single partial
#  18  Bell 1        the manual's "long release" entry
#  22  Marimba       fast decay, short sample
#  32  Slap 1        very fast attack and decay
#  44  Fingered 1    plucked bass, medium decay
#  56  Choir 3       flat sustain, ping-pong loop
#  62  E. Organ 1    flat sustain, no decay at all
#  58  Strings 1     slow attack, flat sustain, ping-pong loop
#  94  Flute 1       flat sustain, breathy attack
#  89  Brass 1       moderate attack, flat sustain
#  96  Shakuhachi 1  slow attack, flat sustain
TONE = dict(piano=0, vib=15, bell=18, marimba=22, slap=32, fbass=44,
            choir=56, strings=58, organ=62, brass=89, flute=94, shaku=96)

RELEASE_TONES = [('vib', 15), ('organ', 62), ('choir', 56)]
ATTACK_TONES  = [('vib', 15), ('strings', 58), ('brass', 89)]

SEGMENTS = []


def _seg(name, label, trials, patch=0):
    SEGMENTS.append(dict(name=name, label=label, patch=patch, trials=trials))


# 1. ENV RELEASE RATE, all fifteen settings.  The release is the cleanest measurement in
#    the machine: one segment, one rate byte, a straight line in dB.  Held short so the
#    hold-time correction (see release_by_hold) stays small and roughly constant.
for _tn, _t in RELEASE_TONES:
    _seg('release_sweep_' + _tn, 'ENV RELEASE -7..+7, %s' % _tn,
         [_trial(60, 100, 1.0, 3.2, 'env_release=%+d' % d,
                 {P_TONE: _t, P_ENV_RELEASE: env_val(d)})
          for d in range(-7, 8)])

# 2. ENV ATTACK RATE, all fifteen settings.  Held long enough that the attack has finished
#    well before note-off, so the two do not overlap in the measurement.
for _tn, _t in ATTACK_TONES:
    _seg('attack_sweep_' + _tn, 'ENV ATTACK -7..+7, %s' % _tn,
         [_trial(60, 100, 2.5, 1.8, 'env_attack=%+d' % d,
                 {P_TONE: _t, P_ENV_ATTACK: env_val(d)})
          for d in range(-7, 8)])

# 3. PART LEVEL.  A control: register 07 is supposed to be logarithmic at 0.3763 dB per
#    unit, and this is the cleanest way to see the whole curve, including the bottom end
#    where the 0xB0C6 ceiling table reads zero and the firmware substitutes silence.
_seg('level_sweep', 'PART LEVEL 0..127, organ (flat sustain)',
     [_trial(60, 100, 1.2, 1.3, 'level=%d' % v,
             {P_TONE: TONE['organ'], P_LEVEL: v})
      for v in (127, 112, 96, 80, 64, 56, 48, 40, 32, 24, 16, 8, 4, 0)])

# 4. VELOCITY SENS x velocity.  The other handle on register 07, and the one that exposes
#    how velocity is folded in -- at sens 0 the velocity should stop mattering entirely.
_seg('velocity_sens', 'VELO SENS x velocity, organ',
     [_trial(60, v, 1.2, 1.3, 'velo_sens=%d/vel=%d' % (s, v),
             {P_TONE: TONE['organ'], P_VELO_SENS: s})
      for s in (0, 4, 8, 12, 15) for v in (1, 40, 80, 127)])

# 5. Release vs hold time.  This tests a specific claim from the disassembly rather than
#    fitting anything: 0x649A subtracts a term proportional to (now - note-on time) from
#    the release rate.  If that is right the slope gets shallower the longer the note was
#    held, monotonically, with everything else identical.
for _tn, _t in (('vib', 15), ('organ', 62)):
    _seg('release_by_hold_' + _tn, 'release vs hold time, %s' % _tn,
         [_trial(60, 100, h, 3.5, 'hold=%.2f' % h,
                 {P_TONE: _t, P_ENV_RELEASE: env_val(0)})
          for h in (0.2, 0.5, 1.0, 2.0, 4.0, 8.0)])

# 6. Release across the keyboard.  The phase handlers scale rates by (key - 0x45), so the
#    same note-off should release at different speeds at different pitches.
for _tn, _t in (('vib', 15), ('organ', 62)):
    _seg('release_by_key_' + _tn, 'release across the key range, %s' % _tn,
         [_trial(n, 100, 1.0, 3.2, 'note=%d' % n,
                 {P_TONE: _t, P_ENV_RELEASE: env_val(0)})
          for n in (36, 48, 60, 69, 72, 84, 96)])

# 7. Attack vs velocity.  The note-on rate is (reg07 * K) >> 8 -- proportional to the
#    target level -- so a louder note should reach its (higher) target in about the same
#    time.  If time-to-peak is flat across velocity, that reading is right.
for _tn, _t in (('strings', 58), ('brass', 89)):
    _seg('attack_by_velocity_' + _tn, 'attack vs velocity, %s' % _tn,
         [_trial(60, v, 2.5, 1.8, 'vel=%d' % v,
                 {P_TONE: _t, P_ENV_ATTACK: env_val(0)})
          for v in (16, 40, 64, 96, 127)])

# 8. Long holds at neutral settings, one per envelope character.  The decay segments are
#    tone data and no MIDI parameter reaches them, so this is the only way to see them:
#    the reference for whatever the phase handlers turn out to do.
_seg('decay_hold', 'long holds at neutral settings, twelve tones',
     [_trial(60, 100, 8.0, 3.0, 'tone=%s' % n,
             {P_TONE: t, P_ENV_ATTACK: env_val(0), P_ENV_RELEASE: env_val(0)})
      for n, t in sorted(TONE.items(), key=lambda kv: kv[1])])



# ---------------------------------------------------------------- scratch-patch sweeps
#
# Run with --set scratch.  Everything else in this file leans on a factory patch and
# changes one or two parameters; this set dictates the WHOLE patch first, so nothing is
# inherited and nothing else can move.  That matters because the open question -- what the
# falling ramp actually does -- has so far been answered differently by different tones,
# and a tone is not a controlled variable if the patch around it is not pinned.
#
# THE SCRATCH PATCH
#
#   * part 0 gets every parameter written explicitly: tone, level, velocity sens, both ENV
#     rates, full key range, no pitch shift, ALL LFO depths zero;
#   * parts 1-5 are moved to MIDI channel 14, so they never see a note and never allocate
#     a voice.  Not level 0 -- a silent part still costs polyphony and can still be given a
#     segment, and the point is to have exactly one voice in flight;
#   * chorus and tremolo depth are set to 0 in the patch common block.
#
# It is written after the program change (which reloads the patch and would wipe it) and
# lives only in the edit buffer -- nothing here can touch the 64 stored patches.
#
# WHAT EACH SEGMENT ASKS
#
# The decisive one is scratch_level, and it is sharper than it first looks.  Dropping PART
# LEVEL does not only lower the level: the firmware scales the decay RATE with it too.
# Rendered through the emulator, PART LEVEL 127 -> 8 walks register 07 from 217 to 158 --
# 59 units, 22.2 dB of level -- while the first decay segment goes from rate -43 to -24,
# 19 counts.  The two candidate models then predict OPPOSITE directions:
#
#     linear-amplitude ramp   t = (level/2) / speed
#                             level x0.077, speed x0.192  ->  dB/s goes UP about 2.5x
#     log-domain ramp         dB/s = k * 2^(rate/8)
#                             rate -19 counts             ->  dB/s goes DOWN about 5.2x
#
# A factor of thirteen apart, in opposite directions, from one sweep.  Whichever way the
# hardware moves settles it, and neither answer needs the rate ladder to be known.
#
# scratch_velocity is the same question through a different lever, and must agree.
# scratch_keys is the complement: note number moves the rate (-38 at note 24 to -51 at 96,
# verified in the emulator) while register 07 and every target stay identical, so it
# measures the rate exponent at CONSTANT level -- the calibration the release sweep gives at note-off, now
# during a note.  Both models predict the same thing there, which is what makes it a clean
# control rather than a second opinion.
#
# Every one of these runs on a tone that currently measures RIGHT (Vib 1) and on one that
# measures 2x SLOW (Fretless Bass, Marimba).  If the answer differs between those two
# families, that difference is the bug.

SCRATCH_MUTE_CH = 13            # parts 1-5 parked on MIDI channel 14

SCRATCH_PART0 = {
    P_OUT_ASSIGN: 0, P_RX_CHANNEL: 0, P_TONE_MEDIA: 0,
    P_BEND_RANGE: 2, P_KEY_LO: 0, P_KEY_HI: 127,
    P_LEVEL: 127, P_VELO_SENS: 8,
    P_ENV_ATTACK: 8, P_ENV_RELEASE: 8,          # 8 on the wire = 0 on the panel
    P_PITCH_COARSE: 64, P_PITCH_FINE: 64,
    P_LFO_RATE: 0, P_LFO_DELAY: 0, P_LFO_RISE: 0, P_LFO_DEPTH: 0,
    P_LFO_MRISE: 0, P_LFO_MDEPTH: 0, P_LFO_CHPRESS: 0,
}


def _scratch_setup(tone):
    """Every write that builds the scratch patch, in the order it must happen."""
    out = [('common', C_CHORUS_DEPTH, 0), ('common', C_TREMOLO_DEPTH, 0)]
    for off, val in sorted(SCRATCH_PART0.items()):
        out.append(('part', 0, off, val))
    out.append(('part', 0, P_TONE, tone))
    for part in range(1, 6):
        out.append(('part', part, P_RX_CHANNEL, SCRATCH_MUTE_CH))
    return out


def _sseg(name, label, tone, trials):
    SCRATCH.append(dict(name=name, label=label, patch=0,
                        setup=_scratch_setup(tone), trials=trials))


SCRATCH = []

# 1. THE DECISIVE ONE.  PART LEVEL sweep, everything else pinned.
#    linear-amplitude ramp -> dB/s doubles for each halving of level
#    log-domain ramp       -> dB/s flat across the whole sweep
for _tn, _tone in (('vib', 15), ('fbass', 44), ('marimba', 22)):
    _sseg('scratch_level_' + _tn, 'PART LEVEL sweep, %s, scratch patch' % _tn, _tone,
          [_trial(60, 100, 8.0, 3.0, 'level=%d' % v, {P_LEVEL: v})
           for v in (127, 96, 72, 52, 36, 24, 14, 8)])

# 2. The same question through velocity, which moves register 07 and leaves the rate alone.
for _tn, _tone in (('vib', 15), ('marimba', 22)):
    _sseg('scratch_velocity_' + _tn, 'velocity sweep, %s, scratch patch' % _tn, _tone,
          [_trial(60, v, 8.0, 3.0, 'vel=%d' % v, {})
           for v in (127, 110, 90, 70, 50, 30, 15)])

# 3. NEGATIVE CONTROL.  Rendered through the emulator, ENV RELEASE does not touch the
#    in-note decay at all -- all seven settings produce the identical ladder, r-50 -> 198,
#    r-43 -> 182, r-35 -> 166.  Only the fall after note-off moves.  Worth confirming on
#    hardware, because it says the in-note decay and the release are separate mechanisms,
#    and every model of one has to leave the other alone.  The 6 s gap catches the release
#    as well, so this doubles as a check that the release law still holds with the whole
#    patch dictated rather than inherited.
_sseg('scratch_env_release', 'ENV RELEASE -4..+2, vib, scratch patch', 15,
      [_trial(60, 100, 8.0, 6.0, 'env_release=%+d' % d, {P_ENV_RELEASE: env_val(d)})
       for d in (-4, -3, -2, -1, 0, 1, 2)])

# 4. ENV ATTACK moves the byte by 16 instead of 8 -- a coarser lever on the same law.
_sseg('scratch_env_attack', 'ENV ATTACK -3..+3, vib, scratch patch', 15,
      [_trial(60, 100, 8.0, 3.0, 'env_attack=%+d' % d, {P_ENV_ATTACK: env_val(d)})
       for d in (-4, -3, -2, -1, 0, 1)])       # +2 and beyond saturate the byte at 0x7F

# 5. Six tones under IDENTICAL dictated settings.  Three of these (marimba, fbass, piano)
#    write a zero-length segment and currently render 2x slow; three do not and render
#    right.  With the patch pinned, tone is finally the only variable.
_sseg('scratch_tones', 'six tones, identical scratch patch', 15,
      [_trial(60, 100, 8.0, 3.0, 'tone=%s' % n, {P_TONE: t})
       for n, t in (('vib', 15), ('bell', 18), ('slap', 32),
                    ('marimba', 22), ('fbass', 44), ('piano', 0))])

# 6. Key scaling: the phase handlers scale rates by (key - 0x45), so the note number is a
#    lever on the rate that does not touch the level.  The complement of segment 1.
_sseg('scratch_keys', 'vib across the keyboard, scratch patch', 15,
      [_trial(n, 100, 8.0, 3.0, 'note=%d' % n, {})
       for n in (24, 36, 48, 60, 72, 84, 96)])

# 7. SHAPE, not speed.  Long holds so the decay can be fitted frame by frame rather than
#    averaged: a linear-amplitude ramp is concave in dB within each 6 dB segment and a
#    log-domain one is dead straight.  Slowest ENV RELEASE in case it reaches the decay.
_sseg('scratch_slow_vib', 'vib, 20 s hold, slow envelope', 15,
      [_trial(60, 100, 20.0, 4.0, 'env_release=%+d' % d, {P_ENV_RELEASE: env_val(d)})
       for d in (0, -4, -7)])
_sseg('scratch_slow_fbass', 'fretless bass, 20 s hold, slow envelope', 44,
      [_trial(60, 100, 20.0, 4.0, 'env_release=%+d' % d, {P_ENV_RELEASE: env_val(d)})
       for d in (0, -4, -7)])

# 8. VELOCITY SENS against velocity: separates "how hard it was hit" from "how much this
#    part cares", which are the two things that feed register 07 at note-on.
_sseg('scratch_velo_sens', 'VELO SENS x velocity, vib, scratch patch', 15,
      [_trial(60, v, 6.0, 2.5, 'sens=%d/vel=%d' % (sv, v), {P_VELO_SENS: sv})
       for sv in (0, 8, 15) for v in (100, 40)])


# ---------------------------------------------------------------- effects sweeps
#
# Run with --set effects.  This is the FIRST capture of the U-110's chorus and tremolo:
# nothing recorded before it contains either, because the enable bits live in a config byte
# selected by the OUTPUT MODE and all 64 factory patches choose a dry mode.  See
# analysis/EFFECTS.md for the decode this is built to test.
#
# NOTHING IS INHERITED.  Every trial writes all five effect parameters -- chorus rate and
# depth, tremolo rate and depth, output mode -- even when a value is unchanged from the
# trial before, and every segment writes the whole part block first.  The effects are
# reached only by changing the output mode, which is exactly the parameter most likely to
# be left somewhere unexpected by a previous session or a panel edit, so there is no value
# here that is allowed to be whatever the machine happened to hold.
#
# WHAT EACH SWEEP ASKS
#
#   fx_chorus_rate    predicted 0.65 -> 3.2 Hz across rate 0-15, at essentially constant
#                     depth.  Measured as the modulation period of a sustained partial.
#   fx_chorus_depth   predicted delay swing 11 -> 496 samples (0.35 -> 15.5 ms), the LFO
#                     period staying put.  Depth 0 is the dry reference: the firmware
#                     clears the enable bit rather than running a small effect.
#   fx_tremolo_rate   predicted 2.6 -> 12.8 Hz.
#   fx_tremolo_depth  predicted 0.4 -> 30.1 dB peak to peak.  Depth 0 is again dry.
#   fx_shape          the decisive one, and it needs no sweep.  The rising and falling
#                     ramps carry the same rate byte but the hardware's falling ramps move
#                     16x slower, so the LFO should be a SAWTOOTH of duty 1/17, not a
#                     triangle -- 36 ms up and 577 ms down at chorus rate 7 depth 7.  In
#                     the chorus that means a near-constant pitch offset for 94% of the
#                     cycle, which puts a DOUBLET in the spectrum rather than a smear.  A
#                     triangle would smear.  Long holds on a flat-sustain tone, so there is
#                     plenty of steady material to transform.
#   fx_stereo         the same settings played wet (mode 21, <L31><R31>) and dry (mode 22,
#                     M31).  L+R and L-R from the wet takes answer how the wet signal is
#                     spread -- if it is dry+wet against dry-wet, L+R comes out comb-free
#                     -- and whether the tremolo is antiphase, i.e. an auto-pan.
#   fx_tones          three tones through the same settings, for listening rather than
#                     measuring.
#   fx_factory        three factory patches with their OWN stored effect values, written
#                     explicitly, dry then wet.  What Roland's settings actually sound like
#                     when the output mode lets them through.
#
# E. Organ 1 carries the measurements: flat sustain, no decay at all, so the only thing
# moving in the recording is the effect.

EFFECTS = []

FX_NOTE, FX_VEL = 60, 100
FX_HOLD, FX_GAP = 10.0, 2.5


def _fx_common(cr, cd, tr, td, mode=FX_MODE_WET):
    return {C_CHORUS_RATE: cr, C_CHORUS_DEPTH: cd,
            C_TREMOLO_RATE: tr, C_TREMOLO_DEPTH: td, C_OUTPUT_MODE: mode}


def _fx_setup(tone):
    """The whole patch, dictated -- the scratch patch plus the effect block."""
    out = []
    for off, val in sorted(SCRATCH_PART0.items()):
        out.append(('part', 0, off, val))
    out.append(('part', 0, P_TONE, tone))
    for part in range(1, 6):
        out.append(('part', part, P_RX_CHANNEL, SCRATCH_MUTE_CH))
    for off, val in common_order(_fx_common(0, 0, 0, 0)):
        out.append(('common', off, val))
    return out


def _fxseg(name, label, tone, trials):
    EFFECTS.append(dict(name=name, label=label, patch=0, setup=_fx_setup(tone),
                        trials=trials))


def _fxtrial(tag, cr, cd, tr, td, mode=FX_MODE_WET, hold=FX_HOLD, note=FX_NOTE):
    return _trial(note, FX_VEL, hold, FX_GAP, tag, {},
                  common=_fx_common(cr, cd, tr, td, mode))


# 1. CHORUS RATE, depth held at 7, tremolo off.
_fxseg('fx_chorus_rate', 'CHORUS RATE 0-15, depth 7, tremolo off, organ', TONE['organ'],
       [_fxtrial('crate=%d' % r, r, 7, 0, 0) for r in (0, 3, 7, 11, 15)])

# 2. CHORUS DEPTH, rate held at 7, tremolo off.  Depth 0 is the dry control.
_fxseg('fx_chorus_depth', 'CHORUS DEPTH 0-15, rate 7, tremolo off, organ', TONE['organ'],
       [_fxtrial('cdepth=%d' % d, 7, d, 0, 0) for d in (0, 1, 4, 7, 11, 15)])

# 3. TREMOLO RATE, depth held at 7, chorus off.
_fxseg('fx_tremolo_rate', 'TREMO. RATE 0-15, depth 7, chorus off, organ', TONE['organ'],
       [_fxtrial('trate=%d' % r, 0, 0, r, 7) for r in (0, 3, 7, 11, 15)])

# 4. TREMOLO DEPTH, rate held at 7, chorus off.  Depth 0 is the dry control.
_fxseg('fx_tremolo_depth', 'TREMO. DEPTH 0-15, rate 7, chorus off, organ', TONE['organ'],
       [_fxtrial('tdepth=%d' % d, 0, 0, 7, d) for d in (0, 1, 4, 7, 11, 15)])

# 5. SHAPE.  Long holds, one effect at a time, at the settings the prediction is sharpest
#    for.  25 s is about 27 chorus cycles at rate 7 -- enough steady material to resolve a
#    doublet 21 cents wide, which needs roughly 2 s of window on its own.
_fxseg('fx_shape', 'sawtooth-or-triangle: long holds, one effect at a time, organ',
       TONE['organ'],
       [_fxtrial('shape/chorus_7_7',  7, 7, 0, 0, hold=25.0),
        _fxtrial('shape/chorus_7_15', 7, 15, 0, 0, hold=25.0),
        _fxtrial('shape/chorus_0_7',  0, 7, 0, 0, hold=25.0),
        _fxtrial('shape/tremolo_7_7', 0, 0, 7, 7, hold=25.0),
        _fxtrial('shape/tremolo_7_15', 0, 0, 7, 15, hold=25.0),
        _fxtrial('shape/dry', 0, 0, 0, 0, hold=25.0)])

# 6. STEREO.  Identical settings, wet mode against dry mode, on a tone with no decay.
_fxseg('fx_stereo', 'mode 21 <L31><R31> against mode 22 M31, same settings, organ',
       TONE['organ'],
       [_fxtrial('stereo/chorus_wet',  7, 7, 0, 0, FX_MODE_WET, hold=15.0),
        _fxtrial('stereo/chorus_dry',  7, 7, 0, 0, FX_MODE_DRY, hold=15.0),
        _fxtrial('stereo/tremolo_wet', 0, 0, 7, 7, FX_MODE_WET, hold=15.0),
        _fxtrial('stereo/tremolo_dry', 0, 0, 7, 7, FX_MODE_DRY, hold=15.0),
        _fxtrial('stereo/both_wet',    7, 7, 7, 7, FX_MODE_WET, hold=15.0),
        _fxtrial('stereo/both_dry',    7, 7, 7, 7, FX_MODE_DRY, hold=15.0)])

# 7. THREE TONES, wet and dry, for the ear rather than the analyser.  Strings and Choir
#    are ping-pong loops with a flat sustain; Vib has a decay, so the effect has to be
#    heard against a moving envelope.
for _tn in ('strings', 'choir', 'vib'):
    _fxseg('fx_tones_' + _tn, 'wet against dry at 7/7/7/7, %s' % _tn, TONE[_tn],
           [_fxtrial('%s/dry' % _tn, 7, 7, 7, 7, FX_MODE_DRY, hold=12.0),
            _fxtrial('%s/wet' % _tn, 7, 7, 7, 7, FX_MODE_WET, hold=12.0)])

# 8. FACTORY PATCHES with the settings Roland stored for them, written out explicitly so
#    the take does not depend on the patch memory being intact.  Read from the firmware's
#    own patch table at 0xE000 + 0x80*n + 0x0F.  These segments deliberately have NO part
#    setup: the point is the patch as shipped, with only the output mode moved.
FACTORY_FX = [(1,  'Ac.Piano',   7, 7, 7, 7),
              (31, 'Strings',    4, 6, 5, 0),
              (14, 'Marimba',    4, 3, 5, 4)]

for _pn, _nm, _cr, _cd, _tr, _td in FACTORY_FX:
    EFFECTS.append(dict(
        name='fx_factory_p%02d' % _pn,
        label='P-%02d %s, stored %d/%d/%d/%d, dry then wet' % (_pn, _nm, _cr, _cd, _tr, _td),
        patch=_pn - 1, setup=[],
        trials=[_fxtrial('P-%02d/n%d/dry' % (_pn, n), _cr, _cd, _tr, _td, FX_MODE_DRY,
                         hold=12.0, note=n)
                for n in (48, 60)]
               + [_fxtrial('P-%02d/n%d/wet' % (_pn, n), _cr, _cd, _tr, _td, FX_MODE_WET,
                           hold=12.0, note=n)
                  for n in (48, 60)]))

# All three of these store output mode index 21, i.e. mode 22 `M31` -- so the dry trial is
# the patch exactly as Roland shipped it, and the wet trial is mode 21, the SAME voice
# grouping with the effect switched in.  Nothing else moves between the two.


# ---------------------------------------------------------------- follow-up sweeps
#
# Run with --set followup.  These exist because the first take (listen/hardware/env) settled the
# shape of the rate law but left two things open.
#
#  1. Every rate byte the first take produced was a multiple of 8, because ENV ATTACK and
#     ENV RELEASE move the byte in steps of 16 and 8.  A pure exponential 2^(rate/8) and a
#     float encoding (3-bit mantissa, 4-bit exponent, the same shape the wave ROM uses)
#     agree EXACTLY at multiples of 8 and differ in between.  PART LEVEL moves reg07 a
#     unit or two at a time, and the note-on rate is (reg07 * K) >> 8, so parking ENV
#     ATTACK/RELEASE low and sweeping the level lands the rate byte on consecutive
#     integers -- in the slow region where it is measurable.  That is the decisive test.
#
#  2. The slowest releases (ENV RELEASE -7, -6) are slower than the 3.2 s gap allows, so
#     they came out unmeasurable.  Same trials, 25 s of tail.
#
# `rate_ladder_*` deliberately uses Vib 1: its own attack is ~20 ms, so the envelope, not
# the sample, is what the measurement sees.

FOLLOWUP = []


def _fseg(name, label, trials, patch=0):
    FOLLOWUP.append(dict(name=name, label=label, patch=patch, trials=trials))


# A fine ladder in the ATTACK rate byte.  Two ENV ATTACK settings so the two ladders
# overlap: -5 subtracts 80 from the base, -6 subtracts 96, and the level sweep moves the
# base itself by about a unit per two steps of PART LEVEL.
for _d in (-5, -6):
    _fseg('rate_ladder_attack_%d' % abs(_d), 'attack rate ladder, ENV ATTACK %+d, vib' % _d,
          [_trial(60, 100, 2.5, 1.6, 'env_attack=%+d/level=%d' % (_d, v),
                  {P_TONE: TONE['vib'], P_ENV_ATTACK: env_val(_d), P_LEVEL: v})
           for v in range(127, 39, -4)])

# The same idea on the release, which measures far more cleanly -- no sample transient to
# fight.  ENV RELEASE -4 puts organ near 40 dB/s, slow enough to resolve and fast enough
# to finish inside the gap.
_fseg('rate_ladder_release', 'release rate ladder, ENV RELEASE -4, organ',
      [_trial(60, 100, 1.0, 3.2, 'env_release=-4/level=%d' % v,
              {P_TONE: TONE['organ'], P_ENV_RELEASE: env_val(-4), P_LEVEL: v})
       for v in range(127, 39, -4)])

# The slow tail the first take could not reach.  25 s of gap: at ENV RELEASE -7 the fall
# is under 1 dB/s, so it needs the room.
for _tn, _t in (('vib', 15), ('choir', 56)):
    _fseg('slow_release_' + _tn, 'ENV RELEASE -7..-4 with a long tail, %s' % _tn,
          [_trial(60, 100, 1.0, 25.0, 'env_release=%+d' % d,
                  {P_TONE: _t, P_ENV_RELEASE: env_val(d)})
           for d in (-7, -6, -5, -4)])


# ---------------------------------------------------------------- SysEx
def checksum(body):
    """Roland: address + data + checksum must sum to 0 mod 128 (firmware at 0x5BE8)."""
    return (128 - (sum(body) & 0x7F)) & 0x7F


def dt1(dev, addr, data):
    body = list(addr) + list(data)
    return [0x41, dev, MODEL_ID, DT1] + body + [checksum(body)]


def rq1(dev, addr, size):
    body = list(addr) + list(size)
    return [0x41, dev, MODEL_ID, RQ1] + body + [checksum(body)]


def part_addr(part, off):
    return (0x00, 0x10 | (part & 0x0F), off)


def common_addr(off):
    return (0x00, 0x01, off)


def send_common(out, mido, dev, off, value):
    """Write one patch-COMMON parameter to the TEMPORARY patch."""
    addr = common_addr(off)
    if addr[0] != 0x00 or addr[1] != 0x01:
        raise ValueError("refusing to write outside the temporary patch: %r" % (addr,))
    out.send(mido.Message('sysex', data=dt1(dev, addr, [value & 0x7F])))


def send_param(out, mido, dev, part, off, value):
    """Write one part parameter to the TEMPORARY patch.  Refuses anything else."""
    addr = part_addr(part, off)
    if addr[0] != 0x00 or (addr[1] & 0xF0) != 0x10:
        raise ValueError("refusing to write outside the temporary patch: %r" % (addr,))
    out.send(mido.Message('sysex', data=dt1(dev, addr, [value & 0x7F])))


def read_params(mido, out, inp, dev, part, offs, timeout=0.6):
    """RQ1 each parameter and collect the DT1 replies.  {} if the U-110 does not answer."""
    got = {}
    for off in offs:
        while inp.poll():
            pass
        out.send(mido.Message('sysex', data=rq1(dev, part_addr(part, off), (0, 0, 1))))
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = inp.poll()
            if msg is None:
                time.sleep(0.005)
                continue
            d = list(msg.data)
            if len(d) >= 8 and d[0] == 0x41 and d[2] == MODEL_ID and d[3] == DT1 \
                    and tuple(d[4:7]) == part_addr(part, off):
                got[off] = d[7]
                break
    return got


# ---------------------------------------------------------------- timing
def segment_duration(seg):
    return PATCH_SETTLE + sum(PARAM_SETTLE + t['hold'] + t['gap'] for t in seg['trials'])


def total_duration(segments):
    return (LEAD_SILENCE + TAIL_SILENCE
            + sum(segment_duration(s) + SEG_GAP for s in segments))


def write_dry_run(path, segments, ch, cch, part=0, midi_delay=10.0):
    """The same sequence as a Standard MIDI File, for running through the emulator.

    The SysEx goes into the file too.  The emulator ignores register 06 when it plays, but
    the firmware still computes it, and `-log` prints it at every note-on -- which is the
    whole point of rendering this sequence: it hands us the rate byte for each trial.
    """
    import struct

    def vlq(n):
        b = [n & 0x7f]; n >>= 7
        while n:
            b.append((n & 0x7f) | 0x80); n >>= 7
        return bytes(reversed(b))

    def sysex(payload):
        # In an SMF, F0 is followed by a length and the payload ending in F7.
        body = bytes(payload) + b'\xf7'
        return b'\xf0' + vlq(len(body)) + body

    dev = cch & 0x0F
    ev = [(0.0, b'\xff\x51\x03' + struct.pack('>I', 500000)[1:])]
    t = midi_delay + LEAD_SILENCE
    marks, rows = [], []
    for seg in segments:
        start = t
        ev.append((t, bytes([0xC0 | cch, seg['patch']])))
        t += 0.30
        # The scratch patch, if this segment has one.  AFTER the program change, which has
        # just reloaded the patch from memory and would otherwise wipe every write.
        for item in seg.get('setup', ()):
            if item[0] == 'common':
                ev.append((t, sysex(dt1(dev, common_addr(item[1]), [item[2]]))))
            else:
                ev.append((t, sysex(dt1(dev, part_addr(item[1], item[2]), [item[3]]))))
            t += 0.006
        if seg.get('setup'):
            t += 0.25
        t += PATCH_SETTLE
        for i, tr in enumerate(seg['trials']):
            for off in sorted(tr['params']):
                ev.append((t, sysex(dt1(dev, part_addr(part, off), [tr['params'][off]]))))
                t += 0.006                    # ~3.5 ms on the wire at 31250 baud
            for off, val in common_order(tr.get('common', {})):
                ev.append((t, sysex(dt1(dev, common_addr(off), [val]))))
                t += 0.006
            t += PARAM_SETTLE
            rows.append((seg['name'], i, tr['tag'], tr['note'], tr['vel'], tr['hold'], t))
            ev.append((t, bytes([0x90 | ch, tr['note'], tr['vel']])))
            ev.append((t + tr['hold'], bytes([0x80 | ch, tr['note'], 0])))
            t += tr['hold'] + tr['gap']
        marks.append((seg['name'], start, t))
        t += SEG_GAP
    ev.sort(key=lambda x: x[0])

    data, prev = b'', 0.0
    for when, msg in ev:
        data += vlq(int(round((when - prev) * 960))) + msg
        prev = when
    data += vlq(480) + b'\xff\x2f\x00'
    open(path, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) +
                           b'MTrk' + struct.pack('>I', len(data)) + data)

    print("wrote %s  (%.0f s of file; the render runs ~%.0f s longer)"
          % (path, t + TAIL_SILENCE, midi_delay))
    print("   %-26s %-18s %s" % ("segment", "file t", "RENDER t  <- use this"))
    for name, a, b in marks:
        print("   %-26s %7.2f..%-7.2f  %7.2f..%-7.2f"
              % (name, a, b, a + midi_delay, b + midi_delay))
    # Note onsets in RENDER time, so a log line can be matched to the trial that caused it.
    write_trials(os.path.splitext(path)[0] + '-trials.csv',
                 [(s, i, tag, n, v, h, on + midi_delay)
                  for s, i, tag, n, v, h, on in rows])
    return ([(name, a + midi_delay, b + midi_delay) for name, a, b in marks],
            t + TAIL_SILENCE + midi_delay)


def write_trials(path, rows):
    with open(path, 'w') as f:
        f.write("segment,trial,tag,note,velocity,hold_s,onset_s\n")
        for seg, i, tag, note, vel, hold, onset in rows:
            f.write("%s,%d,%s,%d,%d,%.3f,%.4f\n" % (seg, i, tag, note, vel, hold, onset))
    print("wrote %s  (%d trials)" % (path, len(rows)))



# ---------------------------------------------------------------- emulator pairing
def emu_rates(out_dir):
    """Print the rate byte the firmware computed for each trial, from an emulator render.

        python3 tools/render_u110.py --sequence capture_env --log --out-dir listen/emulated/env-emu
        python3 tools/capture_env.py --emu-rates listen/emulated/env-emu

    Matching is by TIME, not by index: layered tones start two voices per note, so the
    n-th note-on is not the n-th trial.  MAME's TG trace timestamps every register write,
    and trials.csv carries each note's onset in the same render clock.
    """
    import csv, re
    log = os.path.join(out_dir, 'error.log')
    idx = os.path.join(out_dir, 'trials.csv')
    for f in (log, idx):
        if not os.path.exists(f):
            sys.exit("%s not found -- render with --sequence capture_env --log" % f)

    # (time, reg06, reg07) for every volume word the firmware wrote.
    pat = re.compile(r"TG (\d+\.\d+) v([0-9A-F]{2}) reg (0[67]) = ([0-9A-F]{2})")
    pending, writes = {}, []
    for line in open(log):
        m = pat.search(line)
        if not m:
            continue
        t, v, reg, val = float(m.group(1)), m.group(2), m.group(3), int(m.group(4), 16)
        if reg == '06':
            pending[v] = (t, val)
        elif v in pending:
            t6, lo = pending.pop(v)
            writes.append((t6, v, lo, val))
    writes.sort()

    rows = list(csv.DictReader(open(idx)))
    # Whitespace-columned, and read back by tools/env_analyse.py -- trial tags must not
    # contain spaces or every column after them shifts.  They must not contain commas
    # either: trials.csv is written unquoted.
    print("%-26s %-26s %5s %5s   %s" % ("sweep", "trial", "reg07", "reg06", "voices"))
    for i, r in enumerate(rows):
        on = float(r['onset_s'])
        nxt = float(rows[i + 1]['onset_s']) if i + 1 < len(rows) else on + 30.0
        # everything written between this note-on and the next belongs to this trial
        w = [x for x in writes if on - 0.05 <= x[0] < nxt - 0.05]
        seen, uniq = set(), []
        for _, v, lo, hi in w:
            if (v, lo, hi) not in seen:
                seen.add((v, lo, hi))
                uniq.append((lo, hi))
        if not uniq:
            print("%-26s %-26s %5s %5s   -" % (r['segment'], r['tag'], '-', '-'))
            continue
        lo, hi = uniq[0]
        extra = "" if len(uniq) == 1 else \
            "  + " + " ".join("%d/%d" % (h, l) for l, h in uniq[1:])
        print("%-26s %-26s %5d %5d   %d%s"
              % (r['segment'], r['tag'], hi, lo, len(uniq), extra))

# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', default='listen/hardware/env')
    ap.add_argument('--device', default=None, help='audio input device name or index')
    ap.add_argument('--list-devices', action='store_true')
    ap.add_argument('--list', action='store_true', help='show the sweeps and stop')
    ap.add_argument('--channel', type=int, default=1, help='part MIDI channel (1-16)')
    ap.add_argument('--control-channel', type=int, default=CONTROL_CH)
    ap.add_argument('--part', type=int, default=0, help='part index the notes reach (0-5)')
    ap.add_argument('--patch', type=int, default=0,
                    help='patch to work in, as a program number (0 = P-01)')
    ap.add_argument('--only', default=None, help='comma-separated sweep names')
    ap.add_argument('--set', default='main',
                    choices=('main', 'followup', 'scratch', 'effects', 'all'),
                    help="'main' is the original 15 sweeps; 'followup' is the shorter set "
                         "the first take showed was needed (see FOLLOWUP above); "
                         "'effects' is the chorus/tremolo set, which needs the OUTPUT MODE "
                         "moved to a wet mode and is the only set that ever hears them")
    ap.add_argument('--dry-run-midi', default=None, metavar='FILE',
                    help='write the sequence as a MIDI file instead of playing it')
    ap.add_argument('--emu-rates', default=None, metavar='DIR',
                    help='read an emulator render made with --sequence capture_env --log '
                         'and print the rate byte the firmware computed for each trial')
    ap.add_argument('--no-readback', action='store_true',
                    help='skip the SysEx readback check (no MIDI cable back from the U-110)')
    args = ap.parse_args()

    if args.emu_rates:
        emu_rates(args.emu_rates)
        return

    segments = {'main': SEGMENTS, 'followup': FOLLOWUP, 'scratch': SCRATCH,
                'effects': EFFECTS,
                'all': SEGMENTS + FOLLOWUP + SCRATCH + EFFECTS}[args.set]
    if args.only:
        want = [x.strip() for x in args.only.split(',')]
        pool = segments
        segments = [s for s in pool if s['name'] in want]
        if not segments:
            sys.exit("no sweep matched --only (names: %s)"
                     % ", ".join(s['name'] for s in pool))
    for s in segments:
        s['patch'] = args.patch

    if args.list:
        print("%-26s %6s  %8s  %s" % ("sweep", "trials", "duration", "what it varies"))
        for s in segments:
            print("%-26s %6d  %7.1fs  %s"
                  % (s['name'], len(s['trials']), segment_duration(s), s['label']))
        print("%-26s %6d  %7.1fs  (%.1f min)"
              % ("TOTAL", sum(len(s['trials']) for s in segments),
                 total_duration(segments), total_duration(segments) / 60.0))
        return

    if args.dry_run_midi:
        write_dry_run(args.dry_run_midi, segments, args.channel - 1,
                      args.control_channel - 1, args.part)
        return

    try:
        import mido, sounddevice as sd, numpy as np
    except ImportError as e:
        sys.exit("Missing dependency: %s\n\n  pip3 install --user mido python-rtmidi "
                 "sounddevice numpy\n" % e.name)

    if args.list_devices:
        print(sd.query_devices()); return

    dev_audio = args.device
    if dev_audio is not None and dev_audio.isdigit():
        dev_audio = int(dev_audio)
    out_dir = args.out_dir if os.path.isabs(args.out_dir) \
        else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    port_name = cap.pick_midi_port()
    in_name = None
    if not args.no_readback:
        ins = mido.get_input_names()
        if ins:
            print("\nMIDI input ports (the U-110's MIDI OUT, for the readback check):")
            for i, p in enumerate(ins):
                print("  [%d] %s" % (i, p))
            s = input("Select MIDI input port number, or Enter to skip: ").strip()
            if s.isdigit() and 0 <= int(s) < len(ins):
                in_name = ins[int(s)]

    ch, cch = args.channel - 1, args.control_channel - 1
    dev = cch & 0x0F
    dur = total_duration(segments)

    print("\nAudio in : %s" % (dev_audio if dev_audio is not None else 'system default'))
    print("MIDI out : %s" % port_name)
    print("MIDI in  : %s" % (in_name or 'not used -- no readback or restore-by-readback'))
    print("Channels : notes on %d, patch changes on %d" % (args.channel, args.control_channel))
    print("SysEx    : device ID %d (control channel - 1), model 0x%02X, part %d"
          % (dev, MODEL_ID, args.part))
    print("Sweeps   : %d, %d trials" % (len(segments),
                                        sum(len(s['trials']) for s in segments)))
    print("Duration : %.0f s (%.1f min)" % (dur, dur / 60.0))
    print("Output   : %s/\n" % out_dir)
    print("On the U-110, SETUP:MIDI:EXCLUSIVE must be ON, or every SysEx here is ignored")
    print("and the sweeps all come out identical.  Nothing is written to stored patches:")
    print("only the temporary patch is touched, and the run ends by reselecting the patch,")
    print("which reloads it.  Do not press WRITE afterwards.")
    print("\nKeep the input gain where it was for listen/hardware/3 if you want the takes to be")
    print("comparable -- but the measurements here are slopes in dB/s, so gain does not")
    print("matter as long as nothing clips.")
    input("\nPress Enter to start...")

    saved = {}
    with mido.open_output(port_name) as out:
        inp = mido.open_input(in_name) if in_name else None
        try:
            # Establish a known state, then check that SysEx is actually getting through.
            out.send(mido.Message('program_change', channel=cch, program=args.patch))
            time.sleep(PATCH_SETTLE)
            if inp is not None:
                saved = read_params(mido, out, inp, dev, args.part, TOUCHED)
                if not saved:
                    sys.exit("\nThe U-110 did not answer an RQ1.\n"
                             "  * SETUP:MIDI:EXCLUSIVE is probably OFF -- turn it ON, or\n"
                             "  * the control channel is not %d (the device ID is that\n"
                             "    channel minus 1), or\n"
                             "  * the U-110's MIDI OUT is not connected to this input.\n"
                             "Re-run with --no-readback to go ahead without the check."
                             % args.control_channel)
                print("readback OK: %s"
                      % ", ".join("%s=%d" % (PARAM_NAMES[k], v) for k, v in sorted(saved.items())))

            frames = int(dur * SR) + 2 * SR
            buf = np.zeros((frames, CH), dtype='float32')
            written, overflows = [0], [0]

            def cb(indata, n, t, status):
                if status:
                    overflows[0] += 1
                w = written[0]
                m = min(n, frames - w)
                if m > 0:
                    buf[w:w + m] = indata[:m]
                    written[0] = w + m

            log, marks, rows = [], [], []
            with sd.InputStream(samplerate=SR, channels=CH, dtype='float32',
                                device=dev_audio, callback=cb, blocksize=1024):
                out.send(mido.Message('control_change', channel=ch, control=123, value=0))
                time.sleep(0.2)
                t0 = time.time()
                print("\nrecording...\n")
                time.sleep(LEAD_SILENCE)

                for seg in segments:
                    seg_start = time.time() - t0
                    out.send(mido.Message('program_change', channel=cch, program=seg['patch']))
                    time.sleep(0.30)
                    for item in seg.get('setup', ()):
                        if item[0] == 'common':
                            send_common(out, mido, dev, item[1], item[2])
                        else:
                            send_param(out, mido, dev, item[1], item[2], item[3])
                        time.sleep(0.012)
                    if seg.get('setup'):
                        log.append("%8.3f  -- scratch patch written (%d parameters) --"
                                   % (time.time() - t0, len(seg['setup'])))
                        time.sleep(0.25)
                    time.sleep(PATCH_SETTLE)
                    print("  [%s] %s" % (seg['name'], seg['label']))
                    log.append("%8.3f  == %s : %s ==" % (seg_start, seg['name'], seg['label']))

                    for i, tr in enumerate(seg['trials']):
                        for off in sorted(tr['params']):
                            send_param(out, mido, dev, args.part, off, tr['params'][off])
                        for off, val in common_order(tr.get('common', {})):
                            send_common(out, mido, dev, off, val)
                            time.sleep(0.012)
                        time.sleep(PARAM_SETTLE)
                        ts = time.time() - t0
                        line = ("%8.3f  %-28s note %3d vel %3d hold %.2fs"
                                % (ts, tr['tag'], tr['note'], tr['vel'], tr['hold']))
                        print("     " + line)
                        log.append(line)
                        rows.append((seg['name'], i, tr['tag'], tr['note'], tr['vel'],
                                     tr['hold'], ts))
                        out.send(mido.Message('note_on', channel=ch,
                                              note=tr['note'], velocity=tr['vel']))
                        time.sleep(tr['hold'])
                        out.send(mido.Message('note_off', channel=ch,
                                              note=tr['note'], velocity=0))
                        time.sleep(tr['gap'])

                    out.send(mido.Message('control_change', channel=ch, control=123, value=0))
                    marks.append((seg['name'], seg_start, time.time() - t0))
                    time.sleep(SEG_GAP)

                out.send(mido.Message('control_change', channel=ch, control=123, value=0))
                time.sleep(TAIL_SILENCE)
        finally:
            # Restore, twice over: write back anything we read, then reselect the patch,
            # which reloads the temporary area from the stored patch regardless.
            for off, v in saved.items():
                send_param(out, mido, dev, args.part, off, v)
                time.sleep(0.02)
            out.send(mido.Message('control_change', channel=ch, control=123, value=0))
            out.send(mido.Message('program_change', channel=cch, program=args.patch))
            if inp is not None:
                inp.close()

    n = written[0]
    audio = buf[:n]
    peak = float(np.abs(audio).max()) if n else 0.0
    clipped = int((np.abs(audio) >= 0.999).sum())
    print("\ncaptured %.2f s, peak %.1f dBFS" % (n / SR, 20 * np.log10(peak + 1e-12)))
    if clipped:
        print("  *** %d CLIPPED SAMPLES - turn the level down and re-take ***" % clipped)
    if peak < 0.02:
        print("  WARNING: level very low - check the interface input and U-110 volume")
    if overflows[0]:
        print("  WARNING: %d input overflows (recording may have gaps)" % overflows[0])

    cap.write_wav(os.path.join(out_dir, 'session.wav'), audio)
    for i, (name, a, b) in enumerate(marks):
        s, e = int(a * SR), min(int((b + SEG_GAP) * SR), n)
        if e - s > SR // 4:
            cap.write_wav(os.path.join(out_dir, "%02d_%s.wav" % (i + 1, name)), audio[s:e])
    write_trials(os.path.join(out_dir, 'trials.csv'), rows)

    with open(os.path.join(out_dir, 'session.txt'), 'w') as f:
        f.write("U-110 ENVELOPE capture (tools/capture_env.py)\n")
        f.write("sample rate %d, channels %d, duration %.2f s, peak %.1f dBFS\n"
                % (SR, CH, n / SR, 20 * np.log10(peak + 1e-12)))
        f.write("notes on ch %d, patch changes on ch %d, part %d, device ID %d\n"
                % (args.channel, args.control_channel, args.part, dev))
        f.write("parameters saved before the run: %s\n"
                % (", ".join("%s=%d" % (PARAM_NAMES[k], v) for k, v in sorted(saved.items()))
                   or "none (no readback)"))
        f.write("\nTimes are send times.  Measure real onsets from the audio.\n\n")
        for line in log:
            f.write(line + "\n")
        f.write("\nsegments\n")
        for i, (name, a, b) in enumerate(marks):
            f.write("  %02d_%-26s %8.3f .. %8.3f\n" % (i + 1, name + '.wav', a, b))

    print("\nwrote %s/session.wav, %d sweep files, trials.csv and session.txt"
          % (out_dir, len(marks)))


if __name__ == '__main__':
    main()
