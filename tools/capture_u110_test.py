#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
capture_u110_test.py -- record a real U-110 while you walk its built-in service tests.

You drive the front panel; this records continuously and lets you mark each step, so the
result can be split into labelled per-step WAVs for comparison against the emulator.

    pip3 install --user sounddevice numpy
    python3 capture_u110_test.py --list
    python3 capture_u110_test.py --test 10

Writes  u110_test<N>.wav        2 channel, 48 kHz, the whole session
        u110_test<N>.txt        marks: step number, label, time
        u110_test<N>_XX_*.wav   one file per step (unless --nosplit)


ENTERING TEST MODE
------------------
Hold [DEC] and [INC] while switching the unit on.  The display shows

        == Test Mode ===
         U-110  Ver2.03

Then, anywhere in the menu:

        [DEC]+[INC]   next test
        [ < ]+[ > ]   previous test

(Both derived from the firmware: it samples the held-key mask at reset and looks for
0x30 = DEC+INC; the menu key codes come from the scan routine at 0x8F69.)
"""

import argparse, sys, time, wave

SR = 48000
CH = 2

# ---------------------------------------------------------------- test procedures
# steps: (label, ...) -- what you advance through inside the test
# audio: does this test produce sound worth recording?
TESTS = {
    1:  dict(name="S-RAM CHECK",      audio=False, steps=[],
             note="Reports RAM1/RAM2 OK. Nothing to record - just tell me what it says."),
    2:  dict(name="LCD CHECK",        audio=False, steps=[],
             note="PLACEHOLDER - contrast/character sweep, nothing useful to capture."),
    3:  dict(name="KEY&LED CHECK",    audio=False, steps=[],
             note="PLACEHOLDER - press each key; useful only to confirm the panel works."),
    4:  dict(name="BATTERY CHECK",    audio=False, steps=[],
             note="Reports 'E = n.nV Good/Error'. Tell me the voltage - the emulator "
                  "reports 3.2V and the firmware accepts 0x85..0xCB."),
    5:  dict(name="MIDI CHECK",       audio=False, steps=[],
             note="Needs a MIDI cable from OUT back to IN. Tell me pass/fail."),
    6:  dict(name="WAVE ROM CHECK",   audio=False, steps=[],
             note="Shows the four internal wave ROM versions. Emulator shows "
                  "1.V0.08 2.V0.08 3.V0.08 4.V0.08 - confirm yours matches."),
    7:  dict(name="ROM CARD CHECK",   audio=False, steps=[],
             note="Insert a card first. Shows the card IDs."),
    8:  dict(name="DAC OFFSET ADJ",   audio=True,  steps=[("DC square wave", 6.0)],
             note="Toggles the output enable to make a DC square wave for VR-2. "
                  "Worth 6 s of audio: it exercises the DAC and output stage with a "
                  "known signal."),
    9:  dict(name="DAC MSB CHECK",    audio=True,  steps=[("MSB test tone", 6.0)],
             note="Adjust VR-1 signal. Another known-waveform reference."),
    10: dict(name="SOUND CHECK",      audio=True,
             steps=[("VOICE-%d" % i, 2.5) for i in range(32)] +
                   [("CHORUS", 4.0), ("TREMORO", 4.0)],
             note="THE IMPORTANT ONE. Steps through all 32 voices plus CHORUS and "
                  "TREMORO. Use [ > ] to advance a step, [ < ] to go back. Each voice "
                  "should sound on its own - this isolates voice allocation, which is "
                  "the emulator's leading suspect for excess harmonic distortion."),
    11: dict(name="OUTPUT CHECK",     audio=True,
             steps=[("Jack %d" % j, 3.0) for j in range(1, 7)] +
                   [("VOICE", 3.0), ("CHORUS", 3.0), ("TREMORO", 3.0)],
             note="Routes sound to each of the six individual jacks in turn. Record the "
                  "MIX output (not the individual jacks) so we can see how each output "
                  "contributes to the stereo mix - this is exactly the data needed to "
                  "model the six-way output demultiplex."),
}


def show_list():
    print(__doc__.split("ENTERING TEST MODE")[0])
    print("Available tests:\n")
    for n in sorted(TESTS):
        t = TESTS[n]
        mark = "audio" if t["audio"] else "  -  "
        print("  %2d  %-16s [%s]  %d step(s)" % (n, t["name"], mark, len(t["steps"])))
        for line in _wrap(t["note"], 68):
            print("      " + line)
        print()


def _wrap(s, w):
    out, cur = [], ""
    for word in s.split():
        if len(cur) + len(word) + 1 > w:
            out.append(cur); cur = word
        else:
            cur = (cur + " " + word).strip()
    if cur: out.append(cur)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--test', type=int, help='test number (see --list)')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--device', default=None, help='audio input device name or index')
    ap.add_argument('--list-devices', action='store_true')
    ap.add_argument('--nosplit', action='store_true', help='do not write per-step WAVs')
    ap.add_argument('--prefix', default=None)
    args = ap.parse_args()

    if args.list:
        show_list(); return

    try:
        import sounddevice as sd, numpy as np
    except ImportError as e:
        sys.exit("Missing dependency: %s\n\n  pip3 install --user sounddevice numpy\n" % e.name)

    if args.list_devices:
        print(sd.query_devices()); return
    if args.test not in TESTS:
        sys.exit("Pick a test with --test N (see --list)")

    t = TESTS[args.test]
    prefix = args.prefix or ("u110_test%d" % args.test)
    dev = args.device
    if dev is not None and dev.isdigit():
        dev = int(dev)

    print("=" * 70)
    print("  Test %d: %s" % (args.test, t["name"]))
    print("=" * 70)
    for line in _wrap(t["note"], 68):
        print("  " + line)
    print()
    print("  1. Switch the U-110 OFF.")
    print("  2. Hold [DEC] and [INC], switch it ON, then release.")
    print("     Display should read '== Test Mode ==='.")
    print("  3. Press [DEC]+[INC] together %d time(s) to reach '%d.%s'."
          % (args.test, args.test, t["name"]))
    print("     ([ < ]+[ > ] steps back if you overshoot.)")
    if t["audio"]:
        print("  4. Connect the MIX OUT (either jack) to your audio interface.")
    print()

    if not t["audio"]:
        print("  This test produces no audio worth recording.")
        print("  Just run it on the unit and tell me what the display shows.")
        return

    input("Press Enter here when the display shows '%d.%s' and you're ready... "
          % (args.test, t["name"]))

    frames = int((sum(s[1] for s in t["steps"]) + 60) * SR)
    buf = np.zeros((frames, CH), dtype='float32')
    written = [0]; overflows = [0]

    def cb(indata, n, tinfo, status):
        if status: overflows[0] += 1
        w = written[0]; m = min(n, frames - w)
        if m > 0:
            buf[w:w+m] = indata[:m]; written[0] = w + m

    marks = []
    with sd.InputStream(samplerate=SR, channels=CH, dtype='float32',
                        device=dev, callback=cb, blocksize=1024):
        t0 = time.time()
        print("\nRECORDING. For each step: set it on the unit, then press Enter here.\n")
        for i, (label, _dur) in enumerate(t["steps"]):
            if args.test == 10:
                hint = "press [ > ] to reach %s" % label
            elif args.test == 11:
                hint = "press [ > ] to reach %s" % label
            else:
                hint = "let it run"
            s = input("  [%2d/%2d] %-10s  (%s), then Enter (or 'q' to stop): "
                      % (i + 1, len(t["steps"]), label, hint))
            if s.strip().lower() == 'q':
                print("  stopping early"); break
            marks.append((time.time() - t0, i, label))
        print("\n  letting the last step settle...")
        time.sleep(2.5)

    n = written[0]
    audio = buf[:n]
    peak = float(np.abs(audio).max()) if n else 0.0
    print("\ncaptured %.1f s, peak %.1f dBFS%s"
          % (n / SR, 20 * np.log10(peak + 1e-12),
             "   *** CLIPPING ***" if peak >= 0.999 else ""))
    if peak < 0.02:
        print("  WARNING: very low level - check the interface input and U-110 volume")
    if overflows[0]:
        print("  WARNING: %d input overflows" % overflows[0])

    def write_wav(path, a):
        pcm = (np.clip(a, -1.0, 1.0) * 32767.0).astype('<i2')
        with wave.open(path, 'wb') as w:
            w.setnchannels(CH); w.setsampwidth(2); w.setframerate(SR)
            w.writeframes(pcm.tobytes())

    write_wav(prefix + ".wav", audio)
    with open(prefix + ".txt", 'w') as f:
        f.write("U-110 test %d: %s\n" % (args.test, t["name"]))
        f.write("sample rate %d, channels %d, duration %.2f s, peak %.1f dBFS\n\n"
                % (SR, CH, n / SR, 20 * np.log10(peak + 1e-12)))
        f.write("mark_time  step  label\n")
        for mt, i, label in marks:
            f.write("%9.3f  %4d  %s\n" % (mt, i, label))

    if not args.nosplit and marks:
        for k, (mt, i, label) in enumerate(marks):
            a = int(mt * SR)
            b = int(marks[k + 1][0] * SR) if k + 1 < len(marks) else n
            if b - a < SR // 4:
                continue
            safe = label.replace(" ", "").replace("/", "")
            write_wav("%s_%02d_%s.wav" % (prefix, i, safe), audio[a:b])
        print("wrote %d per-step files" % len(marks))

    print("wrote %s.wav and %s.txt" % (prefix, prefix))


if __name__ == '__main__':
    main()
