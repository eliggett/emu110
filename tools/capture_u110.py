#!/usr/bin/env python3
"""
capture_u110.py -- drive a real Roland U-110 over MIDI and record its audio.

Produces a reference recording for comparison against the emulator.

Run on the studio machine:

    pip3 install --user mido python-rtmidi sounddevice numpy
    python3 capture_u110.py

It lists MIDI output ports, asks you to pick one, plays a fixed test sequence, and
writes u110_capture.wav (2 channel, 48 kHz) plus u110_capture.txt describing exactly
what was played and when.

Notes
-----
* Audio uses the system default input device.  Override with  --device NAME_OR_INDEX
  or list what's available with  --list-devices.
* The U-110 must be listening on MIDI channel 1 (its default).  Override with --channel.
* This program sends only Program Change, Note On/Off and All Notes Off.  It deliberately
  does NOT send any Roland DT1 SysEx: the U-110's SysEx address map is not established
  here, and a wrong address would write to your unit's patch memory.  (For the record,
  the firmware expects F0 41 <dev> 23 ... -- manufacturer 0x41, model ID 0x23.)
* Program Change on a part's channel selects that part's **TONE** (0-98, the names in
  reference/U-110.ins), NOT a patch.  The U-110's patches are panel-only; sending a
  program change leaves the display on the same patch name with a "TEMP:" prefix.
  The numbers below are therefore tone numbers.
"""

import argparse, sys, time, threading, queue, wave

SR = 48000
CH = 2
OUT_WAV = 'u110_capture.wav'
OUT_LOG = 'u110_capture.txt'

# ---------------------------------------------------------------- test sequence
# (program, patch name, [(midi_note, velocity, hold_seconds, gap_after), ...])
# Kept deliberately sparse: single notes with silence between them, so each can be
# analysed in isolation for pitch, envelope and noise floor.
SEQUENCE = [
    (0,  'A. Piano 1',    [(36, 100, 3.0, 1.0), (48, 100, 3.0, 1.0),
                            (60, 100, 3.0, 1.0), (72, 100, 3.0, 1.0),
                            (84, 100, 3.0, 1.5)]),
    (0,  'A. Piano 1',    [(60,  40, 3.0, 1.0), (60, 127, 3.0, 1.5)]),   # velocity pair
    (94, 'Flute 1',     [(69, 100, 4.0, 1.0),                          # A4 = tuning ref
                            (60, 100, 3.0, 1.0), (72, 100, 3.0, 1.5)]),
    (58, 'Strings 1',     [(48, 100, 4.0, 1.0), (60, 100, 4.0, 1.5)]),
    (32, 'Slap 1',      [(36, 110, 3.0, 1.0), (43, 110, 3.0, 1.0),
                            (48, 110, 3.0, 1.5)]),
    (15, 'Vib 1',      [(60, 100, 4.0, 1.5)]),
    (98, 'Drums',           [(36, 110, 1.5, 0.5), (38, 110, 1.5, 0.5),
                            (42, 110, 1.5, 1.5)]),
]

LEAD_SILENCE  = 3.0     # noise-floor reference before anything plays
TAIL_SILENCE  = 3.0


def total_duration():
    t = LEAD_SILENCE + TAIL_SILENCE
    for _, _, notes in SEQUENCE:
        t += 0.4                                  # settle after program change
        for _, _, hold, gap in notes:
            t += hold + gap
    return t


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--device', default=None, help='audio input device name or index')
    ap.add_argument('--list-devices', action='store_true')
    ap.add_argument('--channel', type=int, default=1, help='U-110 MIDI channel (1-16)')
    ap.add_argument('--out', default=OUT_WAV)
    args = ap.parse_args()

    try:
        import mido, sounddevice as sd, numpy as np
    except ImportError as e:
        sys.exit("Missing dependency: %s\n\n  pip3 install --user mido python-rtmidi "
                 "sounddevice numpy\n" % e.name)

    if args.list_devices:
        print(sd.query_devices()); return

    dev = args.device
    if dev is not None and dev.isdigit():
        dev = int(dev)

    port_name = pick_midi_port()
    dur = total_duration()
    print("\nAudio in : %s" % (dev if dev is not None else 'system default'))
    print("MIDI out : %s   (channel %d)" % (port_name, args.channel))
    print("Duration : %.1f s" % dur)
    print("Output   : %s\n" % args.out)
    input("Set the U-110 volume to a normal level, then press Enter to start...")

    frames = int(dur * SR) + SR
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
    log = []
    t0 = None

    with sd.InputStream(samplerate=SR, channels=CH, dtype='float32',
                        device=dev, callback=cb, blocksize=1024):
        with mido.open_output(port_name) as out:
            out.send(mido.Message('control_change', channel=ch, control=123, value=0))
            time.sleep(0.2)
            t0 = time.time()
            print("recording...\n")
            time.sleep(LEAD_SILENCE)
            for prog, pname, notes in SEQUENCE:
                out.send(mido.Message('program_change', channel=ch, program=prog))
                time.sleep(0.4)
                for note, vel, hold, gap in notes:
                    ts = time.time() - t0
                    line = "%8.3f  %-16s note %3d vel %3d hold %.1fs" % (ts, pname, note, vel, hold)
                    print("  " + line); log.append(line)
                    out.send(mido.Message('note_on', channel=ch, note=note, velocity=vel))
                    time.sleep(hold)
                    out.send(mido.Message('note_off', channel=ch, note=note, velocity=0))
                    time.sleep(gap)
            out.send(mido.Message('control_change', channel=ch, control=123, value=0))
            time.sleep(TAIL_SILENCE)

    n = written[0]
    audio = buf[:n]
    peak = float(np.abs(audio).max()) if n else 0.0
    print("\ncaptured %.2f s, peak %.1f dBFS%s" %
          (n / SR, 20 * np.log10(peak + 1e-12),
           "   *** CLIPPING ***" if peak >= 0.999 else ""))
    if peak < 0.02:
        print("  WARNING: level very low - check the audio interface input and U-110 volume")
    if overflows[0]:
        print("  WARNING: %d input overflows (recording may have gaps)" % overflows[0])

    pcm = (np.clip(audio, -1.0, 1.0) * 32767.0).astype('<i2')
    with wave.open(args.out, 'wb') as w:
        w.setnchannels(CH); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())

    with open(OUT_LOG, 'w') as f:
        f.write("U-110 capture\n")
        f.write("sample rate %d, channels %d, duration %.2f s\n" % (SR, CH, n / SR))
        f.write("MIDI port: %s   channel %d\n" % (port_name, args.channel))
        f.write("peak %.1f dBFS\n" % (20 * np.log10(peak + 1e-12)))
        f.write("lead silence %.1f s (noise-floor reference)\n\n" % LEAD_SILENCE)
        f.write("time      patch             event\n")
        f.write("\n".join(log) + "\n")

    print("\nwrote %s and %s" % (args.out, OUT_LOG))


if __name__ == '__main__':
    main()
