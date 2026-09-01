# emu110 CURRENT issues

## Amplitude:
::IN PROGRESS::
The amplitude when run through the mame emulator is far too quiet. We need to increase it. We must preserve the available precision when we do this, ie, not post-attenuation scaling. 

## Envelope:
There might still be some discrepancies in the envelope, but they are quite small and not worth fussing over just yet. 

## Harmonic content: 
There is an unexplained decrease in legit harmonic content at around 10 KHz. Since I don't hear there that well, I'm ok to let it go. 

## Chrous and Tremelo: 
Chorus and tremelo are not implemented yet. We'll add these soon. 

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
