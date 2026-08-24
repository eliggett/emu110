#!/usr/bin/env python3
"""Render a U-110 patch in stereo, using its per-part Output Assign.

The U-110 is one mono DAC time-multiplexed to six outputs, which IC38 sums to the MIX
L/R pair through a resistor network.  A patch assigns each of its six parts to one of
those outputs (Owner's Manual p.5 "Patch Setting Chart"), and each output lands at a
fixed place in the stereo image.  So the stereo picture follows from two tables:

    part  -> output   : patch part byte +0x0B >> 5   (verified against the OM chart)
    output -> pan     : measured on real hardware, service test 11

MAME's device is mono, so this renders each part on its own (feeding the emulator only
the notes in that part's key range, which is exact because the zones are disjoint) and
pans the results together.

    python3 tools/render_stereo.py --patch 4 --out wide_piano.wav
"""
import argparse, os, struct, subprocess, sys, tempfile, wave
import numpy as np

ROM  = 'roms/U110v203.BIN'
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Service test 11, MIX output, measured on real hardware.  The firmware presents the
# steps as "Jack 1 2 3 4 5 6 VOICE- CHORUS TREMORO" (string at 0x08F18), and the capture
# runs in that order, ~1.8 s per step:
#
#     jack     1       2      3     4      5      6
#     R-L    +65.9  -65.9   0.0   0.0   +6.7   -6.8   dB
#
# Correlation is 1.000 with zero inter-channel delay, so this is pure routing.
#
# `[I]` The capture reads jack 1 as hard RIGHT, but Roland labels OUTPUT 1 as MIX L, so
# the recording is most likely channel-swapped at the interface.  MIRROR flips it to the
# convention.  Either way Wide Piano sweeps monotonically across the image, which is what
# confirms the jack->position assignment; only the absolute L/R sense is in question.
MIRROR = True
_MEASURED = {1: +65.9, 2: -65.9, 3: 0.0, 4: 0.0, 5: +6.7, 6: -6.8}
PAN_DB = {k: (-v if MIRROR else v) for k, v in _MEASURED.items()}


def gains(db):
    """balance in dB (20log10(R/L)) -> (gL, gR) at constant power"""
    r = 10 ** (db / 20.0)
    gl = 1.0 / np.sqrt(1 + r * r)
    return gl, gl * r


def parts_of(patch):
    d = open(os.path.join(ROOT, ROM), 'rb').read()
    rec = d[0xE000 + patch * 0x80: 0xE000 + (patch + 1) * 0x80]
    name = rec[4:14].decode('ascii', 'replace').strip()
    out = []
    for p in range(6):
        q = rec[0x14 + p * 16: 0x14 + p * 16 + 16]
        out.append(dict(part=p + 1, tone=q[2], lo=q[3], hi=q[4], assign=(q[11] >> 5) + 1))
    return name, out, rec[0x0E] + 1


def vlq(n):
    b = [n & 0x7f]; n >>= 7
    while n:
        b.append((n & 0x7f) | 0x80); n >>= 7
    return bytes(reversed(b))


def write_midi(path, notes, t0=5.0, hold=1.6, step=2.0):
    msgs = []
    t = t0
    for n in notes:
        msgs += [(t, bytes([0x90, n, 100])), (t + hold, bytes([0x80, n, 0]))]
        t += step
    msgs.sort()
    ev = vlq(0) + b'\xff\x51\x03' + struct.pack('>I', 500000)[1:]
    prev = 0.0
    for tt, m in msgs:
        ev += vlq(int(round((tt - prev) * 960))) + m; prev = tt
    ev += vlq(480) + b'\xff\x2f\x00'
    open(path, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) +
                           b'MTrk' + struct.pack('>I', len(ev)) + ev)
    return t + 4.0


def read_wav(p):
    w = wave.open(p, 'rb')
    a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(float)
    return a.reshape(-1, w.getnchannels()).mean(1) / 32768., w.getframerate()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--patch', type=int, default=4)
    ap.add_argument('--out', default='stereo.wav')
    ap.add_argument('--notes', default='')
    args = ap.parse_args()

    name, parts, mode = parts_of(args.patch - 1)
    print("P-%02d %s   (Output Mode %d)\n" % (args.patch, name, mode))
    notes = [int(x) for x in args.notes.split(',')] if args.notes else \
            [30, 40, 52, 64, 76, 90]

    tmp = tempfile.mkdtemp()
    mono = []
    for p in parts:
        mine = [n for n in notes if p['lo'] <= n <= p['hi']]
        print("  part %d  keys %3d-%3d  output %d  pan %+.1f dB  notes %s"
              % (p['part'], p['lo'], p['hi'], p['assign'], PAN_DB[p['assign']], mine or '-'))
        if not mine:
            continue
        mid = os.path.join(tmp, "p%d.mid" % p['part'])
        wav = os.path.join(tmp, "p%d.wav" % p['part'])
        # Keep every part on ONE shared timing grid: each part's notes are written at
        # the position they occupy in the full sequence, so the six mono renders line up
        # sample-for-sample and can simply be summed.
        keep = [n for n in notes if n in mine]
        end = write_midi(mid, keep, t0=5.0 + 2.0 * notes.index(keep[0]))
        subprocess.run([os.path.join(ROOT, 'tools/u110run.sh'),
                        '-p', str(args.patch), '-t', str(int(end + 12)),
                        '-m', mid, '-w', wav], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        x, sr = read_wav(wav)
        mono.append((p, x, sr))

    if not mono:
        sys.exit("no part produced audio")
    sr = mono[0][2]
    n = max(len(x) for _, x, _ in mono)
    out = np.zeros((n, 2))
    for p, x, _ in mono:
        gl, gr = gains(PAN_DB[p['assign']])
        out[:len(x), 0] += x * gl
        out[:len(x), 1] += x * gr
    peak = np.abs(out).max()
    if peak > 0:
        out *= 0.89 / peak
    pcm = (np.clip(out, -1, 1) * 32767).astype('<i2')
    with wave.open(args.out, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(pcm.tobytes())
    print("\nwrote %s  (%.1f s, %d parts)" % (args.out, n / sr, len(mono)))


if __name__ == '__main__':
    main()
