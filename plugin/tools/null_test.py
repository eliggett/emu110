#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The null test: does U110Core produce exactly what MAME produces?

PLUGIN-PLAN.md section 4 calls for this to exist BEFORE the core does, and this is it.
The same MIDI goes through both; the requirement is bit-identical output, because there
is no resampler in the path -- the core renders at the chip's native 32 kHz and so does
MAME when asked.  "Close enough" is not the bar.  A one-LSB drift is a real emulation
difference and the whole point is to notice it.

Because the sources are SHARED (the plugin compiles MAME's own device files against
plugin/compat/emu.h -- see PLUGIN-PLAN.md section 3), this is not a one-time acceptance
gate.  It is a continuous regression check on every change to the emulation, and it
belongs in CI.

Modes:

    --self          render twice through MAME and require the two to match.
                    This tests the HARNESS and the precondition it rests on: if MAME's
                    own output is not reproducible, nothing can be null-tested against
                    it.  Run this first, and whenever the harness itself is touched.

    (default)       render through MAME, then through the core, and compare.
                    Not available until plugin/core is built -- reports that plainly
                    rather than pretending to pass.

Bit-identical is checked on the file bytes.  When they differ, the report is in terms an
audio person can act on: where the first difference is, how big the worst sample error is,
and the residual in dB relative to the reference.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
U110RUN = os.path.join(HERE, 'tools', 'u110run.sh')

# MAME's -min delivers the file about 10 s after its own timestamps, so the run has to be
# that much longer than the sequence.  capture_u110.py documents the same lag.
MIDI_DELIVERY_LAG_S = 10.0

# Dead time at the head of the sequence, so the machine has finished booting.
MIDI_LEAD_S = 2.0

# ALWAYS write an explicit tempo.  Without a set_tempo event MAME's MIDI file reader falls
# back to 60 BPM, not the 120 BPM the spec calls for, and the whole sequence plays at half
# speed -- which looks exactly like the emulator being slow, and cost an hour to spot.
MIDI_TEMPO_US = 500000


def build_test_midi(path):
    """A short sequence that exercises the parts a null test needs to cover.

    Deliberately not musical.  What matters is that it touches the tone generator, the
    envelope engine, voice allocation under a chord, velocity scaling, the pitch bender
    and a controller -- so that a difference anywhere in the emulation has a chance to
    show up in the output.
    """
    import mido
    mid = mido.MidiFile(type=0)
    tr = mido.MidiTrack()
    mid.tracks.append(tr)
    mid.ticks_per_beat = 480
    tr.append(mido.MetaMessage('set_tempo', tempo=MIDI_TEMPO_US, time=0))

    def tick(seconds):
        return int(mido.second2tick(seconds, 480, MIDI_TEMPO_US))

    ev = []                                   # (absolute seconds, message)
    t = MIDI_LEAD_S

    # single notes up the keyboard, so tuning and sample selection are both exercised
    for note in (36, 48, 60, 72, 84):
        ev.append((t, mido.Message('note_on', note=note, velocity=100)))
        ev.append((t + 0.8, mido.Message('note_off', note=note, velocity=0)))
        t += 1.0

    # velocity, which drives the envelope's attack level
    for vel in (1, 40, 80, 127):
        ev.append((t, mido.Message('note_on', note=60, velocity=vel)))
        ev.append((t + 0.6, mido.Message('note_off', note=60, velocity=0)))
        t += 0.8

    # a chord, so voice allocation and the release tail overlap
    for note in (48, 52, 55, 60, 64):
        ev.append((t, mido.Message('note_on', note=note, velocity=100)))
    for note in (48, 52, 55, 60, 64):
        ev.append((t + 1.5, mido.Message('note_off', note=note, velocity=0)))
    t += 2.5

    # bender and CC7, both of which reach the chip through different firmware paths
    ev.append((t, mido.Message('note_on', note=60, velocity=100)))
    ev.append((t + 0.3, mido.Message('pitchwheel', pitch=4000)))
    ev.append((t + 0.6, mido.Message('pitchwheel', pitch=-4000)))
    ev.append((t + 0.9, mido.Message('pitchwheel', pitch=0)))
    ev.append((t + 1.2, mido.Message('control_change', control=7, value=60)))
    ev.append((t + 1.5, mido.Message('control_change', control=7, value=127)))
    ev.append((t + 1.8, mido.Message('note_off', note=60, velocity=0)))
    t += 2.5

    ev.sort(key=lambda x: x[0])
    last = 0.0
    for when, msg in ev:
        msg.time = tick(when - last)
        msg.channel = 0
        tr.append(msg)
        last = when
    tr.append(mido.MetaMessage('end_of_track', time=480))
    mid.save(path)
    # The caller needs the RENDER length, which is the file length plus MAME's delivery lag.
    return t + 2.0 + MIDI_DELIVERY_LAG_S


def render_mame(midi, wav, seconds, samplerate=32000, patch=1, log=None):
    """One deterministic MAME render at the chip's native rate.

    Dither is switched OFF.  It is deterministic (a seeded xorshift32, which is why two
    MAME runs match each other), but the core cannot reproduce MAME's exact sequence of
    calls into it and there is no reason it should have to.  emu/sound.h says as much.
    """
    env = dict(os.environ, U110_DITHER='0')
    cmd = [U110RUN, '-t', str(int(seconds)), '-m', midi, '-w', wav,
           '-samplerate', str(samplerate)]
    if log:
        cmd.append('-log')
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('MAME render failed')
    if log:
        src = os.path.join(HERE, 'mame', 'error.log')
        if os.path.exists(src):
            os.replace(src, log)
    return wav


def midi_arrival_times(logfile, out_csv):
    """Pull "MIDI IN <t> <byte>" out of MAME's log.

    The core is then fed the SAME bytes at the SAME emulated instants.  Without this the
    test would be measuring MAME's -min delivery lag and bit clock rather than the
    emulation, and those are transport details the plugin deliberately does not copy.
    """
    rows = []
    with open(logfile, errors='ignore') as f:
        for line in f:
            m = re.search(r'MIDI IN ([\d.]+) ([0-9A-F]{2})', line)
            if m:
                rows.append((float(m.group(1)), int(m.group(2), 16)))
    with open(out_csv, 'w') as f:
        for t, b in rows:
            f.write('%.6f,%02x\n' % (t, b))
    return len(rows)


def render_core(midi_csv, wav, seconds, block=512):
    """Render through U110Core.  Returns None if it has not been built."""
    exe = os.path.join(HERE, 'plugin', 'build', 'u110_render')
    if not os.path.exists(exe):
        return None
    cmd = [exe, '--roms', os.path.join(HERE, 'roms'),
           '--seconds', str(seconds), '--block', str(block),
           '--out', wav]
    if midi_csv:
        cmd += ['--midi-at', midi_csv]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('core render failed')
    return wav


def best_alignment(a, b, search=512):
    """The lag at which the two line up best, and the count of differing frames there.

    An offset is expected and is not an emulation error: MAME's sound manager has its own
    output phase, which the core has no reason to reproduce.  What matters is whether the
    SAMPLES agree once that constant offset is taken out.
    """
    best = None
    n0 = min(len(a), len(b))
    for lag in range(-search, search + 1):
        if lag < 0:
            x, y = a[-lag:n0], b[0:n0 + lag]
        else:
            x, y = a[0:n0 - lag], b[lag:n0]
        n = min(len(x), len(y))
        if n < 1000:
            continue
        d = x[:n].astype(np.int64) - y[:n].astype(np.int64)
        diff = int((np.abs(d).sum(axis=1) > 0).sum())
        if best is None or diff < best[1]:
            best = (lag, diff, d, n)
    return best


def read_wav(path):
    with wave.open(path) as w:
        n, ch = w.getnframes(), w.getnchannels()
        data = np.frombuffer(w.readframes(n), dtype='<i2')
    return data.reshape(-1, ch)


def check_not_silent(path):
    """A null test against silence passes trivially and proves nothing.

    This is not a hypothetical: the first run of this harness reported a clean PASS on two
    completely silent files, because the sequence was landing past the end of the render.
    Nothing downstream can catch that, so it is checked here before any comparison.
    """
    a = read_wav(path)
    peak = int(np.abs(a).max())
    if peak == 0:
        print('ERROR  the reference render is digital silence.')
        print('       A null test against silence passes for the wrong reason.  Check that '
              'the sequence lands inside the run: MAME delivers -min about %.0f s after the '
              'file timestamps.' % MIDI_DELIVERY_LAG_S)
        return False
    sounding = int((np.abs(a).sum(axis=1) > 8).sum())
    print('reference: peak %d (%.1f dBFS), %d of %d frames sounding (%.0f%%)'
          % (peak, 20 * np.log10(peak / 32768.0), sounding, len(a),
             100.0 * sounding / max(len(a), 1)))
    if sounding < len(a) // 20:
        print('WARNING  under 5%% of the render has any signal in it; the test is weak.')
    return True


def compare(ref_path, test_path, ref_name='reference', test_name='test'):
    """Bit-identical or bust.  Returns True on a pass, and explains any failure."""
    with open(ref_path, 'rb') as f:
        ref_bytes = f.read()
    with open(test_path, 'rb') as f:
        test_bytes = f.read()

    if ref_bytes == test_bytes:
        a = read_wav(ref_path)
        print('PASS  bit-identical: %d frames x %d ch' % (a.shape[0], a.shape[1]))
        return True

    print('FAIL  outputs differ')
    a, b = read_wav(ref_path), read_wav(test_path)
    if a.shape != b.shape:
        print('  shape: %s (%s) vs %s (%s)' % (a.shape, ref_name, b.shape, test_name))
        n = min(len(a), len(b))
        a, b = a[:n], b[:n]
        if n == 0:
            return False

    d = a.astype(np.int64) - b.astype(np.int64)
    nz = np.flatnonzero(np.abs(d).sum(axis=1))
    print('  frames differing : %d of %d (%.4f%%)'
          % (len(nz), len(a), 100.0 * len(nz) / max(len(a), 1)))
    if len(nz):
        print('  first difference : frame %d (%.3f s)' % (nz[0], nz[0] / 32000.0))
    print('  worst sample err : %d LSB' % np.abs(d).max())

    ref_rms = np.sqrt(np.mean(a.astype(np.float64) ** 2))
    err_rms = np.sqrt(np.mean(d.astype(np.float64) ** 2))
    if err_rms > 0 and ref_rms > 0:
        print('  residual         : %.1f dB below the reference'
              % (20 * np.log10(ref_rms / err_rms)))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--self', dest='self_test', action='store_true',
                    help='render twice through MAME and require a match; tests the harness')
    ap.add_argument('--midi', default=None, help='MIDI file to use instead of the built-in')
    ap.add_argument('--seconds', type=float, default=None)
    ap.add_argument('--patch', type=int, default=1)
    ap.add_argument('--block', type=int, default=512,
                    help='core render block size; the output must not depend on it')
    ap.add_argument('--check-blocks', action='store_true',
                    help='also render at a second block size and require identical output')
    ap.add_argument('--keep', default=None, metavar='DIR',
                    help='keep the rendered wavs in DIR instead of a temp dir')
    args = ap.parse_args()

    out = args.keep or tempfile.mkdtemp(prefix='u110null.')
    os.makedirs(out, exist_ok=True)

    if args.midi:
        midi = args.midi
        seconds = args.seconds or 60.0
    else:
        # Always build the sequence: --seconds overrides how long to RENDER it, it does
        # not mean "the file already exists".
        midi = os.path.join(out, 'null_test.mid')
        needed = build_test_midi(midi)
        seconds = args.seconds or needed

    print('null test: %s, %.0f s at %d Hz' % (os.path.basename(midi), seconds, 32000))
    print('  sequence : %s' % midi)
    print('  outputs  : %s\n' % out)

    log = os.path.join(out, 'mame.log')
    ref = render_mame(midi, os.path.join(out, 'mame_a.wav'), seconds,
                      patch=args.patch, log=log)
    if not check_not_silent(ref):
        return 3

    if args.self_test:
        b = render_mame(midi, os.path.join(out, 'mame_b.wav'), seconds, patch=args.patch)
        print('\n-- MAME vs MAME (determinism) --')
        ok = compare(ref, b, 'mame run A', 'mame run B')
        if ok:
            print('\nMAME renders reproducibly, so it can serve as the null-test oracle.')
        else:
            print('\nMAME is NOT reproducible run to run.  Nothing can be null-tested '
                  'against it until that is fixed -- do not trust a later PASS.')
        return 0 if ok else 1

    csv = os.path.join(out, 'midi_at.csv')
    nbytes = midi_arrival_times(log, csv)
    print('MIDI: %d bytes, replayed into the core at MAME\'s own arrival times' % nbytes)

    core = render_core(csv if nbytes else None, os.path.join(out, 'core.wav'),
                       seconds, block=args.block)
    if core is None:
        print('\nThe core renderer (plugin/build/u110_render) does not exist yet.')
        print('Run plugin/tools/build_core.sh.  Built the reference only.')
        return 2

    # Section 4 requires the core to render identically at any block size.  It is checked
    # here rather than by hand because it is easy to lose and invisible by ear: the failure
    # is a fraction of a sample of drift per block, which only shows up against a reference.
    if args.check_blocks:
        print('\n-- block-size independence --')
        alt = render_core(csv if nbytes else None,
                          os.path.join(out, 'core_alt.wav'), seconds, block=64)
        x, y = read_wav(core), read_wav(alt)
        n = min(len(x), len(y))
        d = x[:n].astype(np.int64) - y[:n].astype(np.int64)
        nbad = int((np.abs(d).sum(axis=1) > 0).sum())
        print('  block %d vs 64 : %s (%d of %d frames differ)'
              % (args.block, 'identical' if nbad == 0 else 'DIFFERS', nbad, n))
        if nbad:
            print('  The core must be independent of block size; it is not.')
            return 1

    print('\n-- MAME vs U110Core --')
    a, b = read_wav(ref), read_wav(core)
    lag, diff, d, n = best_alignment(a, b)

    print('  constant offset  : %+d samples (%.3f ms)' % (lag, lag / 32.0))
    print('  frames identical : %d of %d (%.4f%%)'
          % (n - diff, n, 100.0 * (n - diff) / n))
    if diff == 0:
        print('\nPASS  bit-identical once the constant output offset is removed.')
        return 0

    nz = np.flatnonzero(np.abs(d).sum(axis=1))
    print('  worst error      : %d LSB' % np.abs(d).max())
    print('  first difference : %.4f s' % (nz[0] / 32000.0))
    ref_rms = np.sqrt(np.mean(a[:n].astype(np.float64) ** 2))
    err_rms = np.sqrt(np.mean(d.astype(np.float64) ** 2))
    if err_rms > 0:
        print('  residual         : %.1f dB below the reference'
              % (20 * np.log10(ref_rms / err_rms)))
    print('\nNOT YET BIT-IDENTICAL.  See PLUGIN-PLAN.md section 4 for what is known '
          'about the remaining difference.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
