#!/usr/bin/env python3
"""
capture_u110.py -- drive a real Roland U-110 over MIDI and record its audio.

Produces reference recordings for comparison against the emulator.  Each test segment is
written to its own WAV, so no splitting is needed afterwards.

    pip3 install --user mido python-rtmidi sounddevice numpy
    python3 capture_u110.py --out-dir listen/3

It lists MIDI output ports, asks you to pick one, plays the sequence below, and writes

    listen/3/session.wav        the whole take, for reference
    listen/3/NN_<segment>.wav   one file per segment
    listen/3/session.txt        what was played and when

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
    # as well -- kept where it was so the take stays comparable with listen/1 and /2.
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


def pick_midi_port():
    import mido
    ports = mido.get_output_names()
    if not ports:
        sys.exit("No MIDI output ports found.  Is the interface connected?")
    print("\nMIDI output ports:")
    for i, p in enumerate(ports):
        print("  [%d] %s" % (i, p))
    while True:
        s = input("\nSelect MIDI output port number: ").strip()
        if s.isdigit() and 0 <= int(s) < len(ports):
            return ports[int(s)]
        print("  ...not a valid choice")


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
    ap.add_argument('--out-dir', default='listen/3')
    ap.add_argument('--dry-run-midi', default=None, metavar='FILE',
                    help='write the sequence as a MIDI file instead of playing it, so it '
                         'can be run through the emulator first')
    ap.add_argument('--only', default=None,
                    help='comma-separated segment names, to re-take just those')
    args = ap.parse_args()

    if args.dry_run_midi:
        segs = SEGMENTS
        if args.only:
            want = [x.strip() for x in args.only.split(',')]
            segs = [x for x in SEGMENTS if x['name'] in want]
        write_dry_run(args.dry_run_midi, segs, args.channel - 1,
                      args.control_channel - 1)
        return

    try:
        import mido, sounddevice as sd, numpy as np
    except ImportError as e:
        sys.exit("Missing dependency: %s\n\n  pip3 install --user mido python-rtmidi "
                 "sounddevice numpy\n" % e.name)

    if args.list_devices:
        print(sd.query_devices()); return

    segments = SEGMENTS
    if args.only:
        want = [s.strip() for s in args.only.split(',')]
        segments = [s for s in SEGMENTS if s['name'] in want]
        if not segments:
            sys.exit("no segment matched --only (names: %s)"
                     % ", ".join(s['name'] for s in SEGMENTS))

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
    print("Set the U-110 volume so the loudest note peaks around -6 dBFS.  Do NOT let it")
    print("clip: listen/1 has 16 clipped samples and every attack in it is unusable.")
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
