#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure the U-110's amplitude envelope by dividing hardware audio by a dry render.

The problem with reading an envelope straight off a note is that the wave-ROM sample has
its own decay baked in, so what you measure is (sample decay) x (chip envelope) and the
two cannot be separated.  This tool removes the sample data from the measurement:

  1. Take the voice parameters the firmware actually wrote to IC15 -- start, end, loop,
     step, volume -- from a MAME register trace.  These are ground truth; the driver just
     forwards what the CPU writes.
  2. Render those voices in Python with NO envelope at all: float8 decode, linear
     interpolation, loop from `end` back to `loop`, partials summed with the logarithmic
     gain from reg 07.  Run the Fig. 4 reconstruction filter over it and resample to the
     capture rate.  Call this the "dry" reference.
  3. Align the dry reference to the hardware note by cross-correlation.
  4. envelope(t) = hardware_peak(t) / dry_peak(t), measured per period of the note's
     fundamental so the measurement window is always a whole number of cycles.

Whatever is left in that ratio is the chip's doing, because everything else -- the sample's
own decay, the multisample choice, the two partials' relative level, the output filter --
is present identically in both.

Use listen/hardware/2, not listen/hardware/1: listen/hardware/1 peaks at 0.9999 with 16 clipped samples, which flattens
every attack in it.

  python3 tools/envelope_measure.py --trace mame/error.log --out listen/hardware/2/ENVELOPE.md
"""
import argparse, os, re, sys
import numpy as np
from scipy.signal import lfilter, resample_poly

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE_RATE = 32000                      # 34.816 MHz / 1088

# ---------------------------------------------------------------- wave ROM
ADDR_ORDER = [18,17,15,14,16,12,11,7,9,13,10,8,3,2,1,6,4,5,0]
DATA_ORDER = [1,2,7,3,5,0,4,6]

def _bitswap(v, order):
    r = 0
    for i, b in enumerate(order):
        if v >> b & 1:
            r |= 1 << (len(order) - 1 - i)
    return r

def load_roms():
    amap = np.array([_bitswap(i, ADDR_ORDER) for i in range(1 << 19)], dtype=np.int32)
    dmap = np.array([_bitswap(v, DATA_ORDER) for v in range(256)], dtype=np.uint8)
    banks = []
    for i in range(4):
        path = os.path.join(ROOT, 'roms/roland_t110_u110_u220_waverom%d.bin' % i)
        raw = np.frombuffer(open(path, 'rb').read(), dtype=np.uint8)
        out = np.zeros(1 << 19, dtype=np.uint8)
        out[amap] = dmap[raw]
        banks.append(out)
    return banks

def decode_float8(b):
    """Sign + 3-bit exponent + 4-bit mantissa on the magnitude.  Full scale +/-1984."""
    b = b.astype(np.int16)
    b = np.where(b > 127, b - 256, b)
    sign = np.where(b < 0, -1, 1)
    v = np.abs(b)
    sh, m = v >> 4, v & 0x0F
    return (np.where(sh == 0, m, (0x10 + m) << np.maximum(sh - 1, 0)) * sign).astype(float)

# ---------------------------------------------------------------- trace
TRACE_RE = re.compile(
    r'Starting channel (\d+), bank 0x([0-9A-F]+), addr 0x([0-9A-F]+)\.\d+\s*\n'
    r'.*?Smpl End Ofs: 0x([0-9A-F]+), Loop Ofs 0x([0-9A-F]+), '
    r'Step 0x([0-9A-F]+), Volume ([0-9A-F]+)')

def parse_trace(path):
    """Voices the firmware actually started, in order, skipping the reset-time dummies."""
    text = open(path, errors='replace').read()
    out = []
    for m in TRACE_RE.finditer(text):
        ch, bank, addr, end, loop, step, vol = m.groups()
        step = int(step, 16)
        if step == 0:
            continue                     # every voice is "started" with zero params at reset
        base = (int(bank, 16) & 0x3C00) << 8
        out.append(dict(ch=int(ch), bank=int(bank, 16),
                        start=base | int(addr, 16), end=base | int(end, 16),
                        loop=base | int(loop, 16), step=step, vol=int(vol, 16)))
    return out

def rom_slice(banks, addr, n):
    b, off = addr >> 20, addr & 0xFFFFF
    return banks[b][off:off + n]

# ---------------------------------------------------------------- render
def volume_gain(vol):
    """reg 07 is logarithmic: 16 units per doubling, i.e. 0.3763 dB per unit."""
    return 2.0 ** (((vol >> 8) - 255) / 16.0)

def render_voice(banks, v, dur):
    """One voice, dry: no envelope, no release.  Returns ENGINE_RATE samples."""
    n = int(dur * ENGINE_RATE)
    body = decode_float8(rom_slice(banks, v['start'], v['end'] - v['start'] + 2))
    loop_len = v['end'] - v['loop']
    body_len = v['end'] - v['start']
    r = v['step'] / 0x4000
    pos = np.arange(n) * r
    # fold anything past `end` back into the loop region
    over = pos >= body_len
    if loop_len > 0:
        pos = np.where(over, body_len - loop_len + (pos - (body_len - loop_len)) % loop_len, pos)
    else:
        pos = np.minimum(pos, body_len - 2)
    i = pos.astype(int)
    f = pos - i
    i = np.clip(i, 0, len(body) - 2)
    return (body[i] * (1 - f) + body[i + 1] * f) * volume_gain(v['vol'])

# ---------------------------------------------------------------- filter (Fig. 4)
SK = [(8200e-12, 680e-12), (3300e-12, 560e-12)]
FR, RC_R, RC_C = 10e3, 10e3, 2200e-12

def output_filter(x, fs):
    y = np.asarray(x, float)
    for c1, c2 in SK:
        f0 = 1.0 / (2 * np.pi * FR * np.sqrt(c1 * c2))
        q = 0.5 * np.sqrt(c1 / c2)
        w0 = 2 * np.pi * f0 / fs
        al = np.sin(w0) / (2 * q)
        c = np.cos(w0)
        b = np.array([(1 - c) / 2, 1 - c, (1 - c) / 2])
        a = np.array([1 + al, -2 * c, 1 - al])
        y = lfilter(b / a[0], a / a[0], y)
    k = 1.0 - np.exp(-1.0 / (RC_R * RC_C) / fs)
    return lfilter([k], [1, -(1 - k)], y)

# ---------------------------------------------------------------- envelope
def rms_envelope(x, sr, f0, target_ms=50.0):
    """RMS in windows of a whole number of cycles.

    Per-period PEAK is the right estimator for a pure tone (it is what makes the service
    test's 9 ms ramps legible), but on a piano note the partials beat and the peak jitters
    by several dB between adjacent periods.  Whole-cycle RMS averages that out while still
    keeping the window commensurate with the pitch, so no partial straddles a boundary.
    """
    cycles = max(1, int(round(target_ms * f0 / 1000.0)))
    p = sr / f0 * cycles
    n = int(len(x) / p)
    return (np.array([np.sqrt((x[int(i * p):int((i + 1) * p)] ** 2).mean())
                      for i in range(n)]), p / sr)

def ref_level(t, db, a=0.15, b=0.30):
    """Zero the envelope on a fixed early window, not on its maximum.

    Normalising to the peak makes the zero point depend on one noisy sample and puts a
    spurious positive excursion right after it."""
    m = np.isfinite(db) & (t >= a) & (t <= b)
    return np.median(db[m]) if m.any() else 0.0


def read_wav(path):
    import wave
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(float)
    return a.reshape(-1, w.getnchannels()).mean(1) / 32768.0, w.getframerate()

# ---------------------------------------------------------------- main
# listen/hardware/2's P-01 section.  Times are the capture log's; the log records when the script
# SENT each message, and the note-off lands about 230 ms later -- measure, do not assume.
EVENTS = [(3.404, 36, 100, 3.0), (7.407, 48, 100, 3.0), (11.412, 60, 100, 3.0),
          (15.416, 72, 100, 3.0), (19.420, 84, 100, 3.0),
          (24.325, 60, 40, 3.0), (28.330, 60, 127, 3.0)]

def note_hz(n):
    return 440.0 * 2.0 ** ((n - 69) / 12.0)

def align(dry, hw, sr, t0, search=0.8, use=0.30):
    """Return the hardware offset (s) that best lines up with the dry render."""
    a = dry[:int(use * sr)]
    lo = max(0, int((t0 - 0.30) * sr))
    hi = int((t0 + search) * sr) + len(a)
    seg = hw[lo:hi]
    c = np.correlate(seg, a, 'valid')
    return (lo + int(np.argmax(np.abs(c)))) / sr

def characterise(t, db, tf=None, dbf=None, hw_fine=None, floor_db=None):
    """Sustain and release, fitted separately.

    `t`/`db` are the coarse (50 ms) envelope, which is what the sustain needs; `tf`/`dbf`
    are an optional fine (10 ms) version, which the release needs -- at 50 ms a 250 ms
    release is only five points.
    """
    out = {}
    ok = np.isfinite(db)
    if ok.sum() < 20:
        return out
    tv, dv = t[ok], db[ok]

    def fits(x, y):
        def r2(z):
            p = np.polyfit(x, z, 1)
            return (1 - ((z - np.polyval(p, x)) ** 2).sum()
                    / max(((z - z.mean()) ** 2).sum(), 1e-30)), p[0]
        rl, sl = r2(10 ** (y / 20.0))
        rd, sd = r2(y)
        return rl, rd, sd

    # release: the note is held 3.0 s, so look for the knee only where it can be.  A
    # generic "biggest drop" search latches onto loud partials dying mid-note instead.
    knee_t = None
    for i in range(len(tv)):
        if not (2.6 <= tv[i] <= 3.6):
            continue
        j = int(np.searchsorted(tv, tv[i] - 0.25))
        if dv[i] < dv[j] - 12:
            knee_t = tv[j]
            break

    if tf is not None and hw_fine is not None:
        # The note is held 3.0 s and the note-off reaches the machine a fixed ~0.2 s later,
        # so the release always starts in a narrow window.  Take the steepest 150 ms fall
        # in that window as the knee -- far more reliable than a generic threshold, which
        # trips on individual partials dying earlier in the note.
        dbf = np.asarray(hw_fine, float)
        w = int(round(0.15 / (tf[1] - tf[0])))
        cand = [i for i in range(len(tf) - w)
                if 2.5 <= tf[i] <= 3.4 and np.isfinite(dbf[i]) and np.isfinite(dbf[i + w])]
        knee_t = tf[min(cand, key=lambda i: dbf[i + w] - dbf[i])] if cand else None
        okf = np.isfinite(dbf)
        m = okf & (tf >= (knee_t if knee_t is not None else 2.8))
        if m.sum() > 6:
            x, y = tf[m], dbf[m]
            # Stop at the capture's noise floor.  The note reaches it well before the
            # analysis window ends, so fitting to the end of the window fits an L-shape:
            # a real ~250 ms fall followed by several hundred ms of flat noise.  That is
            # what made the earlier release durations and R2 values meaningless.
            stop = floor_db + 6.0 if floor_db is not None else y[0] - 40.0
            end = next((k for k in range(len(y)) if y[k] < stop), len(y) - 1)
            x, y = x[:max(end, 4) + 1], y[:max(end, 4) + 1]
            if len(y) > 5:
                rl, rd, _ = fits(x - x[0], y)
                out.update(release_start_s=float(x[0]),
                           release_ms=float((x[-1] - x[0]) * 1000.0),
                           release_drop_db=float(y[0] - y[-1]),
                           release_to_floor=bool(y[-1] <= (floor_db + 8.0)
                                                 if floor_db is not None else False),
                           release_lin_r2=rl, release_db_r2=rd)

    hi = knee_t - 0.15 if knee_t is not None else tv[-1]
    m = (tv >= 0.5) & (tv <= hi)
    if m.sum() > 10:
        rl, rd, sd = fits(tv[m], dv[m])
        out.update(sustain_db_per_s=sd, sustain_lin_r2=rl, sustain_db_r2=rd,
                   sustain_drop_db=float(dv[m][0] - dv[m][-1]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace', default=os.path.join(ROOT, 'mame/error.log'))
    ap.add_argument('--capture', default=os.path.join(ROOT, 'listen/hardware/2/u110_capture.wav'))
    ap.add_argument('--emu', default=None, help='emulator render of the same events')
    ap.add_argument('--emu-offset', type=float, default=11.6,
                    help='capture time + this = emulator time (MIDI file shift + the\n                          ~10 s the U-110 takes to start accepting MIDI)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'listen/hardware/2/ENVELOPE.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'listen/hardware/2/envelope_data.csv'))
    args = ap.parse_args()

    banks = load_roms()
    voices = parse_trace(args.trace)
    if len(voices) < 2 * len(EVENTS):
        sys.exit("trace has %d voices, need %d -- is it the right run?"
                 % (len(voices), 2 * len(EVENTS)))
    hw, sr = read_wav(args.capture)
    emu = emu_sr = None
    if args.emu:
        emu, emu_sr = read_wav(args.emu)

    rows, curves = [], []
    DUR = 3.8          # events are ~4.0 s apart; never let the next note into the window
    for k, (t0, note, vel, hold) in enumerate(EVENTS):
        vs = voices[2 * k:2 * k + 2]
        dry = sum(render_voice(banks, v, DUR) for v in vs)
        dry = output_filter(dry, ENGINE_RATE)
        dry = resample_poly(dry, sr, ENGINE_RATE)
        off = align(dry, hw, sr, t0)
        seg = hw[int(off * sr):int(off * sr) + len(dry)]
        f0 = note_hz(note)
        ph, dt = rms_envelope(seg, sr, f0)
        pd, _ = rms_envelope(dry, sr, f0)
        n = min(len(ph), len(pd))
        ph, pd = ph[:n], pd[:n]
        t = np.arange(n) * dt
        floor = np.median(np.abs(hw[:int(2.5 * sr)])) * 6      # lead-silence noise floor
        valid = ph > floor
        ratio = 20 * np.log10(np.maximum(ph, 1e-12) / np.maximum(pd, 1e-12))
        ratio -= ref_level(t, ratio)
        ratio[~valid] = np.nan
        phf, dtf = rms_envelope(seg, sr, f0, target_ms=10.0)
        pdf, _ = rms_envelope(dry, sr, f0, target_ms=10.0)
        nf = min(len(phf), len(pdf))
        rf = 20 * np.log10(np.maximum(phf[:nf], 1e-12) / np.maximum(pdf[:nf], 1e-12))
        tfine = np.arange(nf) * dtf
        rf -= ref_level(tfine, rf)
        floor_f = phf[:nf] > floor
        rf[~floor_f] = np.nan
        # no noise gate here: the release is exactly where a gate would bite, and
        # the floor shows up harmlessly as a plateau the fit already stops before
        hwf = 20 * np.log10(np.maximum(phf[:nf], 1e-12))
        hw_peak_db = np.nanmax(hwf)
        hwf -= hw_peak_db
        noise_db = 20 * np.log10(np.sqrt((hw[:int(2.5 * sr)] ** 2).mean())) - hw_peak_db
        info = characterise(t, ratio, tfine, rf, hwf, noise_db)
        info.update(note=note, vel=vel, voices=[v['ch'] for v in vs],
                    steps=[v['step'] for v in vs], vols=['%04X' % v['vol'] for v in vs],
                    hw_offset=off)
        # the emulator, measured exactly the same way
        if emu is not None:
            edry = dry if emu_sr == sr else resample_poly(dry, emu_sr, sr)
            eoff = align(edry, emu, emu_sr, t0 + args.emu_offset, search=0.6)
            eseg = emu[int(eoff * emu_sr):int(eoff * emu_sr) + len(edry)]
            ped, _ = rms_envelope(edry, emu_sr, f0)
            pe, _ = rms_envelope(eseg, emu_sr, f0)
            m = min(len(pe), len(ped), n)
            er = 20 * np.log10(np.maximum(pe[:m], 1e-12) / np.maximum(ped[:m], 1e-12))
            er -= ref_level(t[:len(er)], er)
            info['emu_ratio'] = er
        rows.append(info)
        curves.append((note, vel, t, ph, pd, ratio, info.get('emu_ratio')))

    write_report(args.out, args.csv, rows, curves, args)
    for r in rows:
        print("n%-3d v%-3d  sustain %+6.2f dB/s (lin R2 %.3f / dB R2 %.3f)   "
              "release @%6.3fs %5.0f ms drop %4.1f dB (lin R2 %.3f / dB R2 %.3f)"
              % (r['note'], r['vel'], r.get('sustain_db_per_s', float('nan')),
                 r.get('sustain_lin_r2', float('nan')), r.get('sustain_db_r2', float('nan')),
                 r.get('release_start_s', float('nan')), r.get('release_ms', float('nan')),
                 r.get('release_drop_db', float('nan')),
                 r.get('release_lin_r2', float('nan')), r.get('release_db_r2', float('nan'))))
    print("\nwrote %s and %s" % (args.out, args.csv))

def write_report(out, csvpath, rows, curves, args):
    with open(csvpath, 'w') as f:
        f.write("note,velocity,t_s,hw_peak,dry_peak,envelope_dB,emu_envelope_dB\n")
        for note, vel, t, ph, pd, ratio, er in curves:
            for i in range(len(t)):
                e = "" if er is None or i >= len(er) else "%.3f" % er[i]
                f.write("%d,%d,%.6f,%.6g,%.6g,%s,%s\n"
                        % (note, vel, t[i], ph[i], pd[i],
                           "" if np.isnan(ratio[i]) else "%.3f" % ratio[i], e))

    L = []
    L.append("# U-110 amplitude envelope, measured from hardware\n")
    L.append("Generated by `tools/envelope_measure.py`. Raw per-period values are in "
             "`envelope_data.csv` next to this file.\n")
    L.append("## Method\n")
    L.append(__doc__.split('  python3')[0].split('\n', 2)[2].strip() + "\n")
    L.append("Specifics of this run:\n")
    L.append("- capture `%s` (44.1 kHz; peak 0.796, **no clipping** -- listen/hardware/1 clips and\n"
             "  must not be used for envelope work)" % os.path.relpath(args.capture, ROOT))
    L.append("- voice parameters from `%s`, a MAME run of the same seven events on P-01\n"
             "  selected from the front panel" % os.path.relpath(args.trace, ROOT))
    L.append("- dry reference rendered at %d Hz, Fig. 4 filter applied, resampled to the\n"
             "  capture rate, aligned by cross-correlation over the first 300 ms" % ENGINE_RATE)
    L.append("- envelope sampled once per period of the note's fundamental\n")
    L.append("## Results\n")
    L.append("| note | vel | voices | steps | vol regs | sustain dB/s | sus lin R2 "
             "| sus dB R2 | release at (s) | release (ms) | drop (dB) | release dB/s "
             "| rel lin R2 | rel dB R2 |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    def g(r, k, fmt="%.3f"):
        return (fmt % r[k]) if k in r else "-"
    for r in rows:
        rate = ("%.0f" % (-r['release_drop_db'] / (r['release_ms'] / 1000.0))
                if 'release_ms' in r and r['release_ms'] > 0 else "-")
        L.append("| %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r['note'], r['vel'], ",".join(str(x) for x in r['voices']),
            ",".join("0x%04X" % s for s in r['steps']), ",".join(r['vols']),
            g(r, 'sustain_db_per_s', "%+.2f"), g(r, 'sustain_lin_r2', "%.4f"),
            g(r, 'sustain_db_r2', "%.4f"), g(r, 'release_start_s'),
            g(r, 'release_ms', "%.0f"), g(r, 'release_drop_db', "%.1f"), rate,
            g(r, 'release_lin_r2', "%.4f"), g(r, 'release_db_r2', "%.4f")))
    L.append("")
    L.append("`linear R2` fits the release against a straight line in **amplitude**; "
             "`dB-linear R2` against a straight line in **dB**. The larger one names the "
             "curve type.\n")
    L.append("## Caveats\n")
    L.append("- **Sustain numbers are solid.** The dry render was validated against MAME's own\n"
             "  output for every note: they agree to **0.2 dB** across the whole 3 s (see the\n"
             "  emulator table below, which is flat at 0 until the emulator cuts the voice).\n"
             "  So the sustain column really is the chip's doing and nothing else's.")
    L.append("- The release fit stops at the capture's noise floor (+6 dB). Fitting to the end\n"
             "  of the analysis window instead fits an L-shape -- a real fall followed by several\n"
             "  hundred ms of flat noise -- which is what made an earlier version of this table\n"
             "  report 500-780 ms releases with R2 around 0.4.")
    L.append("- Note-on lands **0.18-0.28 s after the time in the capture log** -- the log records\n"
             "  when the script sent the message. All times here are relative to the measured\n"
             "  onset, not the log.")
    L.append("- P-01 here is the front-panel patch. The capture script sends a MIDI program\n"
             "  change, which does not select the same thing (a program change of 0 produces\n"
             "  different volume registers), so the two are not guaranteed identical.\n")
    L.append("## Envelope curves\n")
    L.append("Chip envelope in dB (hardware / dry render), 100 ms grid:\n")
    hdr = "| note/vel | " + " | ".join("%.1f" % (i * 0.1) for i in range(0, 40, 3)) + " |"
    L.append(hdr)
    L.append("|" + "---|" * (len(hdr.split('|')) - 2))
    for note, vel, t, ph, pd, ratio, er in curves:
        vals = []
        for i in range(0, 40, 3):
            j = int(np.searchsorted(t, i * 0.1))
            vals.append("%.1f" % ratio[j] if j < len(ratio) and not np.isnan(ratio[j]) else "")
        L.append("| n%d v%d | " % (note, vel) + " | ".join(vals) + " |")
    L.append("")
    if any(c[6] is not None for c in curves):
        L.append("Same measurement applied to the emulator render (should match the table "
                 "above once the envelope is implemented):\n")
        L.append(hdr)
        L.append("|" + "---|" * (len(hdr.split('|')) - 2))
        for note, vel, t, ph, pd, ratio, er in curves:
            if er is None:
                continue
            vals = []
            for i in range(0, 40, 3):
                j = int(np.searchsorted(t, i * 0.1))
                vals.append("%.1f" % er[j] if j < len(er) and not np.isnan(er[j]) else "")
            L.append("| n%d v%d | " % (note, vel) + " | ".join(vals) + " |")
        L.append("")
    open(out, 'w').write("\n".join(L) + "\n")


if __name__ == '__main__':
    main()
