# A. Piano 1, note 43 vel 127 -- linear against quadratic reconstruction

Each normalised to its own peak; 4.6 s from the onset.  Both emulator takes have the
output EQ correction ON, which is the setting that measured best.

    hardware_note43_vel127.wav       listen/hardware/4/01_piano_vel_43.wav
    emu-linear_note43_vel127.wav     U110_RECON=0  (the current default)
    emu-quadratic_note43_vel127.wav  U110_RECON=1

This note plays sample 2 of the tone, whose reference is note 64, at step 0.2952 -- so
the sample runs at 9450 Hz and its Nyquist is 4725 Hz.  EVERYTHING above 4.7 kHz that
you hear is reconstruction, on both machines: the spectrum repeats every 9450 Hz and the
interpolation kernel is the only thing suppressing the copies.  Linear interpolation
rejects an image of 1 kHz content by 37 dB; one more order rejects it by 55 dB.

Emulator minus hardware, level-matched 150-1000 Hz:

              3-4k   5-6k   6-7k   7-8k   8-9k  9-10k   mean
    linear    +1.9   +3.3   +6.7  +11.5  +11.3  +11.0    6.2
    quadratic +0.9   +0.7   +2.2   +4.1   +1.9   +0.7    1.5

Run it live with U110_RECON=1 (and U110_RECON=0 for the A side).
