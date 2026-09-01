# A. Piano 1, note 43, velocity 127 -- before and after the one-shot interrupt offer

Three takes of the same note, each normalised to its own peak so only the shape is being
compared.  4.6 s from the onset.

    hardware_note43_vel127.wav    listen/hardware/4/01_piano_vel_43.wav, the vel-127 note
    emu-before_note43_vel127.wav  emulator with the toggling INT line
    emu-after_note43_vel127.wav   emulator with one rising edge per offer

What changed is in `env_scan()`: the chip was putting an edge on INT every other tick for
as long as any voice was waiting, and the firmware's handler is about five ticks long, so
a spare edge got latched and answered after the read that acknowledged the real offer.
Register 00 still named the voice just serviced, and the handler does not check that a
voice has arrived -- it advances that voice's phase machine on trust.  On this patch the
spare service landed on the second voice of the V-MIX every time, handing it its next
envelope rung 0.3 ms after the previous one, while its level was still at the attack peak.
The rung then needed 12.7 s inside a 7 s note, so it never arrived and the ladder stopped.

Measured decay, 2.5-8 kHz, fitted from 0.4 to 5.5 s after the onset:

    hardware   4.17 dB/s
    before     1.89 dB/s   -55%
    after      4.80 dB/s   +15%

Across the 76-trial scratch set (`tools/scratch_analyse.py`) the mean absolute error goes
from 15.7% to 8.6%.  See analysis/ENVELOPE-DECAY.md.
