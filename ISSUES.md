# emu110 CURRENT issues

## Amplitude:
::IN PROGRESS::
The amplitude when run through the mame emulator is far too quiet. We need to increase it. We must preserve the available precision when we do this, ie, not post-attenuation scaling. 

## Envelope:
There might still be some discrepancies in the envelope, but they are quite small and not worth fussing over just yet. 

## Harmonic content: 
There is an unexplained decrease in legit harmonic content at around 10 KHz. Since I don't hear there that well, I'm ok to let it go. 

## Chorus and Tremolo (see analysis/EFFECTS.md)
Not implemented yet.  The firmware side is now fully decoded: both effects are LFOs run on
ramp-generator slots 0x20 and 0x21, and the rate/depth tables are read out of the ROM.  Two
things stand in the way of building it:

- **No hardware recording of the effects exists.**  Every factory patch selects a dry output
  mode, so nothing in `listen/hardware/` has chorus or tremolo in it.  Hearing them at all
  needs `PATCH:COM:OUT #` set to an odd mode in 21-49.  Section 5.
- **Two unknowns need one capture each**: whether the LFO is the predicted 1/17-duty sawtooth,
  and the shift that turns the chorus LFO level into a delay-line tap.  Section 8.

The capture set is written and validated against the emulator:
`python3 tools/capture_env.py --set effects`, 52 trials, about 15 minutes.  It dictates every
parameter it depends on, including the OUTPUT MODE (SysEx patch-common offset 0x18), rather
than inheriting anything.  Section 7.

Done: the device now carries the two LFO slots and runs them (they were aliasing onto voices
0 and 1 through a five-bit slot mask).  Section 6.

## MIDI output:
There is not any MIDI output right now. MIDI through need not be implemented -- the OS or DAW can handle that.

## Spectral / reconstruction (see analysis/RECONSTRUCTION.md)

- **Marimba is 6-12 dB too dark at 6-9 kHz.**  Genuine recorded content, not images, so no
  interpolation kernel touches it.  Largest single spectral error left.  Section 7.
- **The output EQ correction is unexplained.**  A fitted -7.2 dB bell at 5.9 kHz that
  measurably helps but is not derived from the circuit.  Leading candidate is the modelled
  Sallen-Key resonance being +4.17 dB where the service notes say +2.17.  Fix the circuit
  model first, then re-fit or delete.  Section 7.
- **Samples played above their stored rate get no anti-aliasing.**  Structurally true,
  checked against the hardware by ear and inaudible on both machines.  Documented, not
  scheduled.  Section 2.
