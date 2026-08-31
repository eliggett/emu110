#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
capture_u110.py -- drive a real Roland U-110 over MIDI and record its audio.

Produces reference recordings for comparison against the emulator.  Each test segment is
written to its own WAV, so no splitting is needed afterwards.

    pip3 install --user mido python-rtmidi sounddevice numpy
    python3 capture_u110.py --out-dir listen/hardware/3                     # the baseline set
    python3 capture_u110.py --set followup --out-dir listen/hardware/4      # the follow-up set

Two sets of segments.  --set baseline is the original seventeen and is unchanged.
--set followup adds four that the baseline cannot answer: a ten-velocity sweep on one
piano note, P-52 Fantasy and P-48 Shakuhachi with chorus and tremolo switched off, and a
four-octave C stack on strings for broadband response.  --set all takes both.

The follow-up set switches chorus and tremolo off over SYSEX -- no panel work, nothing to
remember mid-take.  It is not optional bookkeeping: P-48 Shakuhachi and P-31 Strings both
load with CHORUS DEPTH = 6, and P-52 Fantasy with tremolo as well, none of which the
emulator models, so without this the two sides are not comparable at all.  Verified in the
emulator by watching the edit buffer: 06 -> 00 about 0.2 s after each patch change.

ONE THING STILL HAS TO BE ON: 'SETUP:MIDI:EXCLUSIVE'.  It gates every exclusive message
(firmware 0x5BAF, bit 5 of the receive-switch mask at 0x3C00) and cannot be set over MIDI
-- the switch that would enable it is itself gated by it.  The script checks by asking the
machine to read a parameter back, and refuses to record if it gets no answer, rather than
producing a take with the effects silently still on.  See verify_sysex_link().

It lists MIDI output ports, asks you to pick one, plays the sequence below, and writes

    listen/hardware/3/session.wav        the whole take, for reference
    listen/hardware/3/NN_<segment>.wav   one file per segment
    listen/hardware/3/session.txt        what was played and when

How the U-110 is told what to play
----------------------------------
Two different Program Changes, on two different channels.  This is the thing that used to
be got wrong here, and it is worth stating plainly:

  * **Program Change on the CONTROL channel selects a PATCH** (P-01..P-64).  The control
    channel defaults to **16**, and program number 0 selects P-01.  Verified against the
    emulator: a program change on channel 16 switches the output-mode index, which is a
    patch property, and the same message on channels 1-15 does not.
  * **Program Change on a PART's channel selects that part's TONE** (0-98, the names in
    reference/U-110.ins).

If a part's channel happens to equal the control channel, the U-110 does both -- patch
first, then tone (Owner's Manual, MIDI section).  Keeping notes on channel 1 and patch
changes on channel 16 avoids that entirely.

Why not SysEx
-------------
The U-110 does accept Roland DT1 (F0 41 <dev> 23 12 <addr> <data> <sum> F7, model ID
0x23), and section 4.2.2 of the Owner's Manual gives the address map -- part parameters at
00 1n xx, so tone number is 00 1n 03.  It is deliberately not used here for two reasons.
First, the U-110 ignores exclusive messages entirely unless SETUP:MIDI:EXCLUSIVE is ON,
which is a front-panel setting this script cannot check or set; a run that silently did
nothing would look identical to a run that worked.  Second, a DT1 with a wrong address
writes to your unit's memory.  Program Change achieves the same thing, needs no setup, and
cannot damage anything.

Notes
-----
* Audio uses the system default input device.  Override with --device, or list what is
  available with --list-devices.
* Note-on reaches the machine about 0.2 s after this script sends it.  The timestamps in
  session.txt are send times; measure the real onset from the audio (see
  tools/envelope_measure.py, which cross-correlates against a dry render).
"""

import argparse, os, sys, time, wave

SR = 48000
CH = 2

LEAD_SILENCE  = 3.0     # noise-floor reference before anything plays
PATCH_SETTLE  = 1.0     # after a patch or tone change, before the first note
SEG_GAP       = 2.5     # between segments, so tails do not bleed across the split
TAIL_SILENCE  = 3.0
CONTROL_CH    = 16      # U-110 default; Program Change here selects a PATCH

# ---------------------------------------------------------------- the sequence
#
# A segment is  dict(name=, label=, patch=, tone=, notes=[...] or score=(bpm, [...]))
#   patch  program number on the control channel: 0 -> P-01, 3 -> P-04 Wide Piano
#   tone   program number on the part channel:    0 -> A. Piano 1, 56 -> Choir 3
#   notes  [(note, velocity, hold_s, gap_s), ...]        one note at a time
#   score  (bpm, [(beat, note, velocity, beats), ...])   polyphonic

def _jazz():
    """Eight bars of ii-V-I in Bb for Wide Piano: shell voicings under a simple line.

    Wide Piano splits the keyboard across six parts by note range, so a two-handed piece
    exercises several parts -- and therefore several Multi Outputs and pan positions -- at
    once, which single notes never do."""
    ev = []
    def chord(bar, notes, vel=72, beats=3.6):
        for n in notes:
            ev.append((bar * 4.0, n, vel, beats))
    def line(bar, beat, notes, vel=96, beats=0.9):
        for i, n in enumerate(notes):
            if n is not None:
                ev.append((bar * 4.0 + beat + i, n, vel, beats))
    #      bar   voicing (LH shell + guide tones)
    chord(0, [48, 55, 58, 63])      # Cm7
    chord(1, [41, 51, 57, 60])      # F7
    chord(2, [46, 53, 57, 62])      # BbMaj7
    chord(3, [43, 53, 58, 62])      # Gm7
    chord(4, [48, 55, 58, 63])      # Cm7
    chord(5, [41, 51, 57, 60])      # F7
    chord(6, [46, 53, 57, 62], beats=7.6)   # BbMaj7, let it ring over two bars
    line(0, 0, [70, 72, 75, 74])
    line(1, 0, [72, 70, 69, 67])
    line(2, 0, [70, None, 74, 72])
    line(3, 0, [74, 72, 70, 69])
    line(4, 0, [67, 70, 72, 75])
    line(5, 0, [74, 72, 70, 69])
    line(6, 0, [70, None, None, None], beats=7.6)
    return (92, ev)


SEGMENTS = [
    # --- the original test notes, unchanged: taken every session as a baseline ---
    dict(name='piano_scale', patch=0, label='A. Piano 1', tone=0,
         notes=[(36, 100, 3.0, 1.0), (48, 100, 3.0, 1.0), (60, 100, 3.0, 1.0),
                (72, 100, 3.0, 1.0), (84, 100, 3.0, 1.5)]),
    dict(name='piano_velocity', patch=0, label='A. Piano 1', tone=0,
         notes=[(60, 40, 3.0, 1.0), (60, 127, 3.0, 1.5)]),
    dict(name='flute', patch=0, label='Flute 1', tone=94,
         notes=[(69, 100, 4.0, 1.0), (60, 100, 3.0, 1.0), (72, 100, 3.0, 1.5)]),
    # Strings 1 is a ping-pong tone too, so this baseline segment covers loop mode 2
    # as well -- kept where it was so the take stays comparable with listen/hardware/1 and /2.
    dict(name='strings1', patch=0, label='Strings 1 (ping-pong loop)', tone=58,
         notes=[(48, 100, 4.0, 1.0), (60, 100, 4.0, 1.5)]),
    dict(name='slap', patch=0, label='Slap 1', tone=32,
         notes=[(36, 110, 3.0, 1.0), (43, 110, 3.0, 1.0), (48, 110, 3.0, 1.5)]),
    dict(name='vibes', patch=0, label='Vib 1', tone=15,
         notes=[(60, 100, 4.0, 1.5)]),
    dict(name='drums', patch=0, label='Drums', tone=98,
         notes=[(36, 110, 1.5, 0.5), (38, 110, 1.5, 0.5), (42, 110, 1.5, 1.5)]),

    # --- ping-pong loops.  Choir 3 and Strings 3 are DUAL tones whose samples use loop
    #     mode 2, the one code path in MAME's roland_lp.cpp that has never been tested and
    #     that its own comment calls "probably incorrect ... cause some DC offset in most
    #     samples".  Held long on purpose: a DC offset accumulates over seconds.
    #
    #     Sweeping a note through all 99 internal tones in the emulator shows only two loop
    #     modes in use: 87 tones normal, 12 ping-pong (54-61, 89-92), and *none* one-shot.
    #     So mode 2 is the only untested path these captures can reach.
    dict(name='choir3_pingpong', patch=0, label='Choir 3 (ping-pong loop)', tone=56,
         notes=[(48, 100, 8.0, 1.5), (60, 100, 8.0, 1.5), (72, 100, 8.0, 2.0)]),
    dict(name='strings3_pingpong', patch=0, label='Strings 3 (ping-pong loop)', tone=60,
         notes=[(48, 100, 8.0, 1.5), (60, 100, 8.0, 1.5), (72, 100, 8.0, 2.0)]),

    # --- elongated ping-pong holds.  The 8 s notes above already show a click in the
    #     emulator; these run long enough to see whether the artefact grows with time,
    #     which is what a DC offset accumulating on every loop reversal would do.
    dict(name='choir3_sustain', patch=0, label='Choir 3 (long hold)', tone=56,
         notes=[(55, 100, 15.0, 2.0), (67, 100, 15.0, 2.5)]),

    # --- a wider spread of patches, each with short notes then long ones ---
    dict(name='fing_bass', patch=22, label='P-23 Fing Bass',
         notes=[(28, 100, 0.35, 0.45), (33, 100, 0.35, 0.45), (38, 100, 0.35, 0.45),
                (43, 100, 0.35, 0.9), (28, 100, 5.0, 1.2), (40, 100, 5.0, 1.5)]),
    dict(name='fless_bass', patch=24, label='P-25 Fless Bass',
         notes=[(28, 100, 0.35, 0.45), (33, 100, 0.35, 0.45), (38, 100, 0.35, 0.45),
                (43, 100, 0.35, 0.9), (28, 100, 5.0, 1.2), (40, 100, 5.0, 1.5)]),
    dict(name='shakuhachi', patch=47, label='P-48 Shakuhachi',
         notes=[(62, 100, 0.35, 0.45), (67, 100, 0.35, 0.45), (74, 100, 0.35, 0.9),
                (62, 100, 6.0, 1.2), (74, 100, 6.0, 1.5)]),
    # Fantasy carries chorus and tremolo, which the emulator does not model at all -- a
    # long hold here is the reference for that work when it comes.
    dict(name='fantasy', patch=51, label='P-52 Fantasy',
         notes=[(48, 100, 0.35, 0.45), (60, 100, 0.35, 0.9),
                (48, 100, 8.0, 1.2), (60, 100, 8.0, 1.2), (72, 100, 8.0, 1.5)]),

    # --- Wide Piano: one note in each of the six key zones, then a spread chord ---
    dict(name='wide_piano_zones', label='Wide Piano (P-04)', patch=3,
         notes=[(30, 100, 2.5, 0.8), (40, 100, 2.5, 0.8), (52, 100, 2.5, 0.8),
                (64, 100, 2.5, 0.8), (76, 100, 2.5, 0.8), (90, 100, 2.5, 1.5)]),
    dict(name='wide_piano_chord', label='Wide Piano (P-04)', patch=3,
         score=(60, [(0, n, 100, 6.0) for n in (30, 45, 57, 64, 76, 88)])),

    # --- a short jazz piece, Wide Piano ---
    dict(name='wide_piano_jazz', label='Wide Piano (P-04)', patch=3, score=_jazz()),
]


# ---------------------------------------------------------------- follow-up set
#
# Run with --set followup (or --set all to take the baseline as well).  Four segments,
# each answering something the baseline set cannot.
#
# EFFECTS ARE SWITCHED OFF OVER SYSEX, not by hand.  Chorus and tremolo are patch
# parameters and no Program Change or CC reaches them, but the U-110's individual-parameter
# DT1 does: see tools/u110_sysex.py, whose framing comes from the firmware's own parser
# rather than the manual's badly-OCR'd address table.  Every message used here has been
# verified against the emulator by dumping the patch edit buffer before and after --
# chorus depth writes 0x2810, tremolo depth writes 0x2812, a part's tone number writes
# part+0x01.
#
# The addresses are all TEMPORARY, i.e. the edit buffer.  They change what is playing now
# and are discarded by the next program change.  Nothing here can touch the 64 stored
# patches; that needs a WRITE from the panel, which this script never asks for.
#
# ORDER MATTERS: the program change reloads the patch from memory, so the sysex must go out
# AFTER it, or the reload wipes it.  The record loop does that.
#
# THE ONE THING SYSEX CANNOT DO IS ENABLE ITSELF.  'SETUP:MIDI:EXCLUSIVE' gates every
# exclusive message -- firmware 0x5BAF tests bit 5 of the receive-switch mask at 0x3C00 --
# and a message arriving with it off is discarded in silence.  So the script asks the
# machine whether it is listening before recording anything: verify_sysex_link() below.
#
# The emulator needs no equivalent: it models neither chorus nor tremolo, so a render of
# these segments is already "effects off".  That is the point -- these are the segments
# where hardware and emulator are finally comparable.

def _effects_off(dev):
    import u110_sysex as sx
    return sx.effects_off(dev)


FOLLOWUP = [
    # Ten velocities on one note, one patch, with room for the whole decay.  The baseline
    # set has two velocities on note 60 and nothing systematic; this is the segment that
    # says how the envelope and the tone's velocity switching actually behave, and note 43
    # is where the audible artefact was reported.  7 s hold: at velocity 100 this note is
    # still sounding at 8 s, so anything shorter clips the tail being measured.
    dict(name='piano_vel_43', patch=0, tone=0, label='A. Piano 1 note 43, velocity sweep',
         notes=[(43, v, 7.0, 3.0) for v in
                (1, 15, 29, 43, 57, 71, 85, 99, 113, 127)]),

    # The same notes as the baseline 'fantasy' and 'shakuhachi' segments, so the two takes
    # subtract directly and any difference IS the effect.  Both patches can carry chorus and
    # tremolo, which makes the baseline versions unusable as a response reference -- and
    # gives us, for free, a measurement of the effects for when they get implemented.
    dict(name='fantasy_dry', patch=51, label='P-52 Fantasy (effects off)',
         sysex=_effects_off,
         notes=[(48, 100, 0.35, 0.45), (60, 100, 0.35, 0.9),
                (48, 100, 8.0, 1.2), (60, 100, 8.0, 1.2), (72, 100, 8.0, 1.5)]),
    dict(name='shakuhachi_dry', patch=47, label='P-48 Shakuhachi (effects off)',
         sysex=_effects_off,
         notes=[(62, 100, 0.35, 0.45), (67, 100, 0.35, 0.45), (74, 100, 0.35, 0.9),
                (62, 100, 6.0, 1.2), (74, 100, 6.0, 1.5)]),

    # Four C's an octave apart, held together on strings, effects off.  Strings sustain
    # without decaying, and stacking octaves fills in between the harmonics, so this gives a
    # long stationary broadband spectrum -- which is exactly what the response measurement
    # lacks.  In listen/hardware/3 nothing but the two effect-carrying patches reaches past 9 kHz at
    # all, so tools/fit_output_eq.py has no usable data for the top two octaves.  This
    # segment is meant to be that data.
    #
    # P-31 Strings, not P-32 Double Str: "Double" patches layer two detuned copies, which
    # would smear the very spectrum this is here to measure.
    dict(name='strings_c_stack', patch=30, label='P-31 Strings, C stack (effects off)',
         sysex=_effects_off,
         score=(60, [(0.0, n, 100, 12.0) for n in (36, 48, 60, 72)])),
]

SETS = {
    'baseline': SEGMENTS,
    'followup': FOLLOWUP,
    'all':      SEGMENTS + FOLLOWUP,
}


# ---------------------------------------------------------------- timing
def score_events(bpm, ev):
    """(beat, note, vel, beats) -> sorted (seconds, on/off, note, vel), and the length."""
    spb = 60.0 / bpm
    out = []
    for beat, note, vel, beats in ev:
        out.append((beat * spb, 1, note, vel))
        out.append(((beat + beats) * spb, 0, note, 0))
    out.sort(key=lambda x: (x[0], x[1]))
    return out, (max(e[0] for e in out) if out else 0.0)


def segment_duration(seg):
    t = PATCH_SETTLE
    if 'notes' in seg:
        for _, _, hold, gap in seg['notes']:
            t += hold + gap
    else:
        _, length = score_events(*seg['score'])
        t += length + 2.0                     # let the last chord ring
    return t


def total_duration():
    return (LEAD_SILENCE + TAIL_SILENCE
            + sum(segment_duration(s) + SEG_GAP for s in SEGMENTS))


def pick_midi_port(ports=None, kind='output'):
    import mido
    if ports is None:
        ports = mido.get_output_names()
    if not ports:
        sys.exit("No MIDI %s ports found.  Is the interface connected?" % kind)
    print("\nMIDI %s ports:" % kind)
    for i, p in enumerate(ports):
        print("  [%d] %s" % (i, p))
    while True:
        s = input("\nSelect MIDI %s port number: " % kind).strip()
        if s.isdigit() and 0 <= int(s) < len(ports):
            return ports[int(s)]
        print("  ...not a valid choice")


def verify_sysex_link(port_name, in_name, device_id, timeout=1.5):
    """Ask the U-110 to read a parameter back, and see whether it answers.

    This exists because of the one failure mode that looks exactly like success:
    'SETUP:MIDI:EXCLUSIVE' is a front-panel switch, the firmware tests it before it looks
    at anything else in the message (0x5BAF, bit 5 of the receive-switch mask at 0x3C00),
    and a message that arrives with it off is dropped without a sound or a display change.
    A whole take would come back with the effects still on and nothing to show for it.

    Returns (ok, detail).  A device-ID mismatch fails the same way, so on failure this
    sweeps 0x00-0x1F and reports any ID that does answer."""
    import mido
    import u110_sysex as sx

    def ask(dev):
        with mido.open_input(in_name) as inp:
            for _ in inp.iter_pending():
                pass
            with mido.open_output(port_name) as o:
                o.send(mido.Message('sysex',
                                    data=list(sx.rq1(sx.PROBE_ADDR, sx.PROBE_SIZE,
                                                     dev)[1:-1])))
            end = time.time() + timeout
            while time.time() < end:
                for m in inp.iter_pending():
                    d = list(m.bytes())
                    if len(d) > 4 and d[1] == sx.ROLAND and d[3] == sx.MODEL_ID:
                        return d
                time.sleep(0.02)
        return None

    reply = ask(device_id)
    if reply is not None:
        # The reply carries the parameter's current value, so say what it was -- a
        # non-zero chorus depth here is exactly what the followup set exists to remove.
        val = reply[8] if len(reply) > 8 else None
        return True, ("device ID 0x%02X answered; chorus depth currently %s"
                      % (device_id, "%d" % val if val is not None else "?"))
    found = [d for d in range(0x20) if ask(d) is not None]
    if found:
        return False, ("device ID 0x%02X did not answer, but 0x%02X did -- "
                       "re-run with --device-id 0x%02X"
                       % (device_id, found[0], found[0]))
    return False, ("nothing answered on any device ID 0x00-0x1F.  Almost certainly "
                   "SETUP:MIDI:EXCLUSIVE is OFF on the U-110 -- that switch gates every "
                   "exclusive message and cannot itself be set over MIDI, so it is the "
                   "one thing that has to be done on the panel.  Turn it ON and re-run.")


def write_wav(path, a):
    import numpy as np
    pcm = (np.clip(a, -1.0, 1.0) * 32767.0).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(CH); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def write_dry_run(path, segments, ch, cch, midi_delay=10.0):
    """The same sequence as a Standard MIDI File, for running through the emulator.

    MAME's -min delivers events about 10 s after their file timestamps, so everything is
    shifted by that much; the offsets between events are unchanged."""
    def vlq(n):
        b = [n & 0x7f]; n >>= 7
        while n:
            b.append((n & 0x7f) | 0x80); n >>= 7
        return bytes(reversed(b))
    import struct
    ev = [(0.0, b'\xff\x51\x03' + struct.pack('>I', 500000)[1:])]
    t = midi_delay + LEAD_SILENCE
    marks = []
    for seg in segments:
        start = t
        if 'patch' in seg:
            ev.append((t, bytes([0xC0 | cch, seg['patch']])))
        if 'tone' in seg:
            if 'patch' in seg:
                t += 0.3
            ev.append((t, bytes([0xC0 | ch, seg['tone']])))
        if seg.get('sysex'):
            # Same order as the live take: after the program change, before the notes.
            # The emulator models neither chorus nor tremolo, so these are inert there --
            # but carrying them makes the render a faithful mirror of the hardware run,
            # and it is how the messages were verified in the first place (dump the patch
            # edit buffer at 0x2800 before and after, see FOLLOWUP above).
            import u110_sysex as _sx
            for _m in seg['sysex'](0x0F):
                t += 0.06
                ev.append((t, b'\xf0' + vlq(len(_m) - 1) + _m[1:]))
            t += 0.2
        t += PATCH_SETTLE
        if 'notes' in seg:
            for note, vel, hold, gap in seg['notes']:
                ev.append((t, bytes([0x90 | ch, note, vel])))
                ev.append((t + hold, bytes([0x80 | ch, note, 0])))
                t += hold + gap
        else:
            events, length = score_events(*seg['score'])
            for when, on, note, vel in events:
                ev.append((t + when, bytes([(0x90 if on else 0x80) | ch, note, vel])))
            t += length + 2.0
        marks.append((seg['name'], start, t))
        t += SEG_GAP
    ev.sort(key=lambda x: x[0])
    data = b''
    prev = 0.0
    for when, msg in ev:
        data += vlq(int(round((when - prev) * 960))) + msg
        prev = when
    data += vlq(480) + b'\xff\x2f\x00'
    open(path, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) +
                           b'MTrk' + struct.pack('>I', len(data)) + data)
    # MAME's -min delivers the file a further ~10 s after its timestamps, ON TOP of the
    # dead time already built in here.  Print the times the audio actually lands at, not
    # the file's own -- comparing a render against the file timestamps is 10 s out, which
    # makes the gap between two segments look like a dropout inside one of them.
    print("wrote %s  (%.0f s of file; the render runs ~%.0f s longer)"
          % (path, t + TAIL_SILENCE, midi_delay))
    print("   %-22s %-18s %s" % ("segment", "file t", "RENDER t  <- use this"))
    for name, a, b in marks:
        print("   %-22s %7.2f..%-7.2f  %7.2f..%-7.2f"
              % (name, a, b, a + midi_delay, b + midi_delay))
    # (name, start, end) in RENDER time, plus the length to give MAME's -seconds_to_run.
    # tools/render_u110.py splits the render on these, so they must stay in render time.
    return ([(name, a + midi_delay, b + midi_delay) for name, a, b in marks],
            t + TAIL_SILENCE + midi_delay)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--device', default=None, help='audio input device name or index')
    ap.add_argument('--list-devices', action='store_true')
    ap.add_argument('--channel', type=int, default=1, help='part MIDI channel (1-16)')
    ap.add_argument('--control-channel', type=int, default=CONTROL_CH,
                    help='channel the U-110 receives PATCH program changes on (default 16)')
    ap.add_argument('--out-dir', default='listen/hardware/3')
    ap.add_argument('--dry-run-midi', default=None, metavar='FILE',
                    help='write the sequence as a MIDI file instead of playing it, so it '
                         'can be run through the emulator first')
    ap.add_argument('--set', default='baseline', choices=sorted(SETS),
                    help="which group of segments to take: 'baseline' (the original "
                         "seventeen, default), 'followup' (the velocity sweep, the two "
                         "effects-off patches and the strings C stack), or 'all'")
    ap.add_argument('--only', default=None,
                    help='comma-separated segment names, to re-take just those')
    ap.add_argument('--device-id', type=lambda x: int(x, 0), default=0x0F,
                    help='U-110 exclusive device ID (default 0x0F).  It is one byte of '
                         'battery-backed RAM at 0x3C01; read it out of a MAME nvram with '
                         '\'open("nvram/u110/workram","rb").read()[0x3C01-0x2100]\', or '
                         'let --verify-sysex find it.')
    ap.add_argument('--midi-in', default=None,
                    help='MIDI input port for the sysex readback check; omit to be asked')
    ap.add_argument('--sysex-selftest', action='store_true',
                    help='ask the U-110 to read a parameter back, print what came out, '
                         'and stop.  Use this to check the rig before committing to a take')
    ap.add_argument('--no-verify', action='store_true',
                    help='skip the sysex readback check.  Only for a machine that cannot '
                         'send MIDI back -- a followup take that silently failed to turn '
                         'the effects off is worthless, and this is what catches that.')
    args = ap.parse_args()

    if args.dry_run_midi:
        segs = SETS[args.set]
        if args.only:
            want = [x.strip() for x in args.only.split(',')]
            segs = [x for x in SETS[args.set] if x['name'] in want]
        write_dry_run(args.dry_run_midi, segs, args.channel - 1,
                      args.control_channel - 1)
        return

    try:
        import mido, sounddevice as sd, numpy as np
    except ImportError as e:
        sys.exit("Missing dependency: %s\n\n  pip3 install --user mido python-rtmidi "
                 "sounddevice numpy\n" % e.name)

    if args.sysex_selftest:
        import u110_sysex as sx
        out_name = pick_midi_port()
        in_name = args.midi_in or pick_midi_port(mido.get_input_names(), 'input')
        print("\nprobing %s -> U-110 -> %s" % (out_name, in_name))
        print("request : %s"
              % ' '.join('%02X' % b for b in sx.rq1(sx.PROBE_ADDR, sx.PROBE_SIZE,
                                                    args.device_id)))
        ok, detail = verify_sysex_link(out_name, in_name, args.device_id)
        print("result  : %s" % detail)
        if not ok:
            print("\nThings to check, in the order they go wrong:")
            print("  1. Is the U-110's MIDI OUT cabled back to your interface's MIDI IN?")
            print("     The baseline take never needs a return path, so this is the first")
            print("     time it matters and it is the usual answer.")
            print("  2. Is the input port above the one that cable lands on?")
            print("  3. SETUP:MIDI:EXCLUSIVE ON (it gates every exclusive message).")
            print("  4. Device ID: --device-id; the sweep above tried 0x00-0x1F.")
            print("\nIf you are confident the writes work but cannot get a reply back,")
            print("--no-verify records anyway.")
        sys.exit(0 if ok else 1)

    if args.list_devices:
        print(sd.query_devices()); return

    segments = SETS[args.set]
    if args.only:
        want = [s.strip() for s in args.only.split(',')]
        segments = [s for s in SETS[args.set] if s['name'] in want]
        if not segments:
            sys.exit("no segment matched --only (names: %s)"
                     % ", ".join(s['name'] for s in SETS[args.set]))

    dev = args.device
    if dev is not None and dev.isdigit():
        dev = int(dev)
    os.makedirs(args.out_dir, exist_ok=True)

    port_name = pick_midi_port()
    dur = (LEAD_SILENCE + TAIL_SILENCE
           + sum(segment_duration(s) + SEG_GAP for s in segments))
    print("\nAudio in : %s" % (dev if dev is not None else 'system default'))
    print("MIDI out : %s" % port_name)
    print("Channels : notes and tone changes on %d, patch changes on %d"
          % (args.channel, args.control_channel))
    print("Segments : %d" % len(segments))
    print("Duration : %.0f s (%.1f min)" % (dur, dur / 60.0))
    print("Output   : %s/\n" % args.out_dir)
    if any('sysex' in sg for sg in segments):
        if args.no_verify:
            print("\n!! --no-verify: not checking that the U-110 accepts exclusive "
                  "messages.\n   If SETUP:MIDI:EXCLUSIVE is OFF this take will have the "
                  "effects still on.\n")
        else:
            in_name = args.midi_in
            if in_name is None:
                in_name = pick_midi_port(mido.get_input_names(), 'input')
            ok, detail = verify_sysex_link(port_name, in_name, args.device_id)
            print("Sysex   : %s" % detail)
            if not ok:
                sys.exit("\nAborting: the effects-off segments would silently record "
                         "with the effects ON.")

    print("Set the U-110 volume so the loudest note peaks around -6 dBFS.  Do NOT let it")
    print("clip: listen/hardware/1 has 16 clipped samples and every attack in it is unusable.")
    input("\nPress Enter to start...")

    frames = int(dur * SR) + 2 * SR
    buf = np.zeros((frames, CH), dtype='float32')
    written = [0]
    overflows = [0]

    def cb(indata, n, t, status):
        if status:
            overflows[0] += 1
        w = written[0]
        m = min(n, frames - w)
        if m > 0:
            buf[w:w+m] = indata[:m]
            written[0] = w + m

    ch = args.channel - 1
    cch = args.control_channel - 1
    log, marks = [], []

    with sd.InputStream(samplerate=SR, channels=CH, dtype='float32',
                        device=dev, callback=cb, blocksize=1024):
        with mido.open_output(port_name) as out:
            out.send(mido.Message('control_change', channel=ch, control=123, value=0))
            time.sleep(0.2)
            t0 = time.time()
            print("recording...\n")
            time.sleep(LEAD_SILENCE)

            for seg in segments:
                seg_start = time.time() - t0
                if 'patch' in seg:
                    out.send(mido.Message('program_change', channel=cch,
                                          program=seg['patch']))
                    log.append("%8.3f  -- patch P-%02d on ch %d --"
                               % (seg_start, seg['patch'] + 1, args.control_channel))
                if 'tone' in seg:
                    if 'patch' in seg:
                        time.sleep(0.3)      # let the patch load before overriding its tone
                    out.send(mido.Message('program_change', channel=ch, program=seg['tone']))
                    log.append("%8.3f  -- tone %d on ch %d --"
                               % (seg_start, seg['tone'], args.channel))
                if seg.get('sysex'):
                    # AFTER the program change, which has just reloaded the patch and
                    # would otherwise wipe these, and before the notes.
                    import u110_sysex as sx
                    for msg in seg['sysex'](args.device_id):
                        out.send(mido.Message('sysex', data=list(msg[1:-1])))
                        time.sleep(0.06)      # the U-110 parses these one at a time
                        log.append("%8.3f  -- sysex %s --"
                                   % (time.time() - t0,
                                      ' '.join('%02X' % b for b in msg)))
                    time.sleep(0.2)
                    seg_start = time.time() - t0

                time.sleep(PATCH_SETTLE)
                print("  [%s] %s" % (seg['name'], seg['label']))

                if 'notes' in seg:
                    for note, vel, hold, gap in seg['notes']:
                        ts = time.time() - t0
                        line = ("%8.3f  %-24s note %3d vel %3d hold %.1fs"
                                % (ts, seg['label'], note, vel, hold))
                        print("     " + line); log.append(line)
                        out.send(mido.Message('note_on', channel=ch, note=note, velocity=vel))
                        time.sleep(hold)
                        out.send(mido.Message('note_off', channel=ch, note=note, velocity=0))
                        time.sleep(gap)
                else:
                    events, length = score_events(*seg['score'])
                    base = time.time()
                    log.append("%8.3f  %-24s score, %d events, %.1fs"
                               % (time.time() - t0, seg['label'], len(events), length))
                    for when, on, note, vel in events:
                        d = base + when - time.time()
                        if d > 0:
                            time.sleep(d)
                        out.send(mido.Message('note_on' if on else 'note_off',
                                              channel=ch, note=note, velocity=vel))
                    time.sleep(2.0)

                out.send(mido.Message('control_change', channel=ch, control=123, value=0))
                marks.append((seg['name'], seg_start, time.time() - t0))
                time.sleep(SEG_GAP)

            out.send(mido.Message('control_change', channel=ch, control=123, value=0))
            time.sleep(TAIL_SILENCE)

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

    write_wav(os.path.join(args.out_dir, 'session.wav'), audio)
    # Split on the recorded marks.  Each segment keeps SEG_GAP of tail so a release or a
    # ping-pong DC drift is still visible after the last note-off.
    for i, (name, a, b) in enumerate(marks):
        s, e = int(a * SR), min(int((b + SEG_GAP) * SR), n)
        if e - s < SR // 4:
            continue
        write_wav(os.path.join(args.out_dir, "%02d_%s.wav" % (i + 1, name)), audio[s:e])

    with open(os.path.join(args.out_dir, 'session.txt'), 'w') as f:
        f.write("U-110 capture\n")
        f.write("sample rate %d, channels %d, duration %.2f s\n" % (SR, CH, n / SR))
        f.write("MIDI port: %s\n" % port_name)
        f.write("part channel %d, control channel %d\n" % (args.channel, args.control_channel))
        f.write("peak %.1f dBFS, %d clipped samples\n" % (20 * np.log10(peak + 1e-12), clipped))
        f.write("lead silence %.1f s (noise-floor reference)\n" % LEAD_SILENCE)
        f.write("NOTE: times below are when this script SENT each message; the U-110 acts\n"
                "      on it about 0.2 s later.  Measure onsets from the audio.\n\n")
        f.write("segments\n")
        for i, (name, a, b) in enumerate(marks):
            f.write("  %02d_%-22s %8.3f .. %8.3f\n" % (i + 1, name + '.wav', a, b + SEG_GAP))
        f.write("\ntime      event\n")
        f.write("\n".join(log) + "\n")

    print("\nwrote %s/session.wav and %d segment files" % (args.out_dir, len(marks)))


if __name__ == '__main__':
    main()
