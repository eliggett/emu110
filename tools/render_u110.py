#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
render_u110.py -- run the emulator through the SAME sequence tools/capture_u110.py plays
on the hardware, and write the results in the same shape.

    python3 tools/render_u110.py --out-dir listen/emulated/emu
    python3 tools/render_u110.py --only strings1,choir3_pingpong
    python3 tools/render_u110.py --sequence capture_env --out-dir listen/emulated/env-emu

This is the emulator-side counterpart of capture_u110.py.  Where that script drives a real
U-110 over MIDI and records an interface, this one renders the identical note sequence
through MAME and splits it the same way, so a segment file here lines up with the segment
file of the same name under listen/3.

    <out-dir>/session.wav        the whole render
    <out-dir>/NN_<segment>.wav   one file per segment
    <out-dir>/session.txt        what was played, when, and how it was rendered

Two things about the timing, both of which have cost real debugging time before:

  * capture_u110.py's MIDI file already carries a 10 s lead, and MAME's -min delivers the
    file about 10 s AFTER its own timestamps on top of that.  So audio lands ~20 s in.
    write_dry_run() returns the marks already converted to RENDER time; use them as they
    come and do not add an offset of your own.
  * The marks are exact, not approximate.  Predicted note onsets have been checked against
    a render and matched to the sample (strings1 note 1 at 24.3000 s).

Level: the U-110's own output is quiet (a loud note peaks near -23 dBFS here), so by
default ONE gain is computed over the whole session and applied to every file, which makes
it audible while leaving the relative levels between segments untouched.  --raw keeps the
emulator's own scaling.
"""

import argparse, os, subprocess, sys, tempfile, wave
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib
import capture_u110 as cap

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_wav(path):
    with wave.open(path, 'rb') as w:
        n, ch, sr = w.getnframes(), w.getnchannels(), w.getframerate()
        a = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch)
    return a, sr, ch


def write_wav(path, a, sr, ch):
    with wave.open(path, 'wb') as w:
        w.setnchannels(ch); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(np.clip(a, -32768, 32767).astype('<i2').tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', default='listen/emulated/emu')
    ap.add_argument('--only', default=None,
                    help='comma-separated segment names, to render just those')
    ap.add_argument('--channel', type=int, default=1, help='part MIDI channel (1-16)')
    ap.add_argument('--control-channel', type=int, default=cap.CONTROL_CH)
    ap.add_argument('--samplerate', type=int, default=48000,
                    help='48000 matches the hardware captures under listen/hardware/3; '
                         'use 32000 (the engine rate) for sample-accurate work')
    ap.add_argument('--patch', type=int, default=1,
                    help='front-panel patch to boot into (the sequence sets patches by '
                         'program change anyway)')
    ap.add_argument('--raw', action='store_true',
                    help='do not apply the session gain; keep the emulator scaling')
    ap.add_argument('--keep-midi', default=None, metavar='FILE',
                    help='also keep the generated MIDI file here')
    ap.add_argument('--mame-arg', action='append', default=[], metavar='ARG',
                    help='extra argument passed straight through to MAME; repeatable. '
                         'u110run.sh forwards these AFTER its own options, so a later '
                         '-autoboot_script wins over the built-in select_patch.lua '
                         '(which is a no-op at --patch 1 anyway).  Used to switch the '
                         'output EQ correction off while measuring the raw chain.')
    ap.add_argument('--log', action='store_true',
                    help="also run MAME with -log and keep mame/error.log as "
                         "<out-dir>/error.log -- that is where the reg 06/07 values are")
    ap.add_argument('--set', default=None,
                    help="for --sequence capture_env: 'followup' or 'all' instead of the "
                         "main sweep set")
    ap.add_argument('--sequence', default='capture_u110',
                    help='module in tools/ that defines the sequence: capture_u110 (the '
                         'reference take, the default) or capture_env (the envelope sweeps)')
    args = ap.parse_args()

    # Any module exposing SEGMENTS and write_dry_run() will do; capture_env.py is the
    # other one, and pairs its trials.csv with the reg 06/07 values MAME's -log prints.
    seq = importlib.import_module(args.sequence)

    segs = seq.SEGMENTS
    if args.set:
        # 'baseline' is capture_u110.py's name for the same thing; accept both so the
        # two tools take the same --set value.
        sets = {'main': seq.SEGMENTS, 'baseline': seq.SEGMENTS,
                'followup': seq.FOLLOWUP,
                'all': seq.SEGMENTS + seq.FOLLOWUP}
        if hasattr(seq, 'SCRATCH'):
            sets['scratch'] = seq.SCRATCH
            sets['all'] = sets['all'] + seq.SCRATCH
        segs = sets[args.set]
    if args.only:
        # Filter what --set already chose, NOT seq.SEGMENTS: filtering the module's default
        # list threw --set away silently and rendered the wrong sequence's names.
        want = [x.strip() for x in args.only.split(',')]
        pool = segs
        segs = [s for s in pool if s['name'] in want]
        if not segs:
            sys.exit("no segment matched --only (names: %s)"
                     % ", ".join(s['name'] for s in pool))

    out_dir = args.out_dir if os.path.isabs(args.out_dir) else os.path.join(HERE, args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    tmp = tempfile.mkdtemp(prefix='u110render.')
    midi = args.keep_midi or os.path.join(tmp, 'sequence.mid')
    marks, total = seq.write_dry_run(midi, segs, args.channel - 1, args.control_channel - 1)

    session = os.path.join(out_dir, 'session.wav')
    secs = int(total) + 2
    print("\nrendering %.0f s (%.1f min) at %d Hz -> %s"
          % (secs, secs / 60.0, args.samplerate, session))
    cmd = [os.path.join(HERE, 'tools', 'u110run.sh'),
           '-p', str(args.patch), '-t', str(secs), '-m', midi, '-w', session,
           '-samplerate', str(args.samplerate)]
    if args.log:
        cmd.append('-log')
        try:
            os.remove(os.path.join(HERE, 'mame', 'error.log'))
        except OSError:
            pass
    cmd += args.mame_arg
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("u110run.sh failed (exit %d)" % r.returncode)
    if not os.path.exists(session):
        sys.exit("no audio was written -- did MAME start?")

    # capture_env's write_dry_run() drops a per-trial index beside the MIDI file; it is
    # what pairs a log line with the sweep setting that produced it, so keep it.
    trials = os.path.splitext(midi)[0] + '-trials.csv'
    if os.path.exists(trials):
        with open(trials) as f:
            open(os.path.join(out_dir, 'trials.csv'), 'w').write(f.read())

    if args.log:
        src = os.path.join(HERE, 'mame', 'error.log')
        if os.path.exists(src):
            os.replace(src, os.path.join(out_dir, 'error.log'))
            print("kept the MAME log as %s/error.log" % out_dir)

    audio, sr, ch = read_wav(session)
    peak = int(np.abs(audio).max()) if len(audio) else 0
    gain = 1.0
    if not args.raw and peak > 0:
        gain = (10 ** (-3.0 / 20) * 32767) / peak      # one gain for the whole session
        audio = audio.astype(np.float64) * gain
        write_wav(session, audio, sr, ch)
    print("captured %.2f s, peak %.1f dBFS%s"
          % (len(audio) / sr, 20 * np.log10(max(peak, 1) / 32767.0),
             "" if args.raw else "  (session gain x%.2f applied)" % gain))

    # Split on the render marks.  Each segment keeps SEG_GAP of tail, exactly as the
    # hardware capture does, so a release or a ping-pong drift is still visible.
    written = []
    for i, (name, a, b) in enumerate(marks):
        s, e = int(a * sr), min(int((b + seq.SEG_GAP) * sr), len(audio))
        if e - s < sr // 4:
            continue
        p = os.path.join(out_dir, "%02d_%s.wav" % (i + 1, name))
        write_wav(p, audio[s:e], sr, ch)
        written.append((i + 1, name, a, b + seq.SEG_GAP,
                        20 * np.log10(max(int(np.abs(audio[s:e]).max()), 1) / 32767.0)))

    with open(os.path.join(out_dir, 'session.txt'), 'w') as f:
        f.write("U-110 EMULATOR render (tools/render_u110.py)\n")
        f.write("sample rate %d, channels %d, duration %.2f s\n" % (sr, ch, len(audio) / sr))
        f.write("part channel %d, control channel %d\n"
                % (args.channel, args.control_channel))
        f.write("emulator peak %.1f dBFS%s\n"
                % (20 * np.log10(max(peak, 1) / 32767.0),
                   "" if args.raw else ", session gain x%.2f applied to every file" % gain))
        f.write("command: %s\n" % " ".join(cmd))
        f.write("\nTimes below are RENDER times and are exact: capture_u110.py's MIDI file\n"
                "carries a 10 s lead and MAME's -min adds ~10 s more, both already folded in.\n\n")
        f.write("segments\n")
        for n, name, a, b, pk in written:
            f.write("  %02d_%-22s %8.3f .. %8.3f   peak %6.1f dBFS\n"
                    % (n, name + '.wav', a, b, pk))
    print("\nwrote %s/session.wav and %d segment files" % (out_dir, len(written)))
    for n, name, a, b, pk in written:
        print("   %02d_%-24s %7.2f..%-7.2f  peak %6.1f dBFS" % (n, name, a, b, pk))


if __name__ == '__main__':
    main()
