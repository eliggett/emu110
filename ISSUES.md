# emu110 CURRENT issues

## Amplitude:
::IN PROGRESS::
The amplitude when run through the mame emulator is far too quiet. We need to increase it. We must preserve the available precision when we do this, ie, not post-attenuation scaling. 

## Envelope:
There might still be some discrepancies in the envelope, but they are quite small and not worth fussing over just yet. 

## Harmonic content: 
There is an unexplained decrease in legit harmonic content at around 10 KHz. Since I don't hear there that well, I'm ok to let it go. 

## Chorus and Tremolo (see analysis/EFFECTS.md)
**Implemented.**  The device now carries the two LFO slots, runs them against the real
firmware, and renders both effects on Voice Group 1.
`listen/hardware/effects` is the first recording of either effect that exists -- no factory
patch enables them, so hearing one at all needs `PATCH:COM:OUT #` set to an odd mode 21-49.
The specification is now complete and measured:

- **LFO**: a symmetric triangle on ramp slots 0x20 (chorus) and 0x21 (tremolo), advancing
  `2^(rate/8) * 4` per 32 kHz sample in BOTH directions.  Chorus 0.42-1.73 Hz, tremolo
  1.67-6.93 Hz.  Depth 0 means the effect is off, not shallow.
- **Tremolo** is an auto-pan: one channel gets the slot's level, the other its complement,
  so the sum does not move.  0.4 to 30.1 dB of pan across the depth range.
- **Chorus** is a delay tapped at `level >> 14` samples, 1 to 32 ms, with a tap in EACH
  channel half an LFO period apart and a roughly 50/50 wet/dry mix.

Rendered against the hardware capture the two agree closely -- LFO rates within 1%, the pan
ratio at depth 15 measuring 0.040..0.960 against the hardware's 0.039..0.961, and the wet
level within 0.4 dB.  Sections 8 and 9.

The order of the two is now measured too -- delay first, pan last, which is also the only
arrangement one 2K x 8 SRAM can support.

Left undone, and neither part measured: the delay line holds floats where IC17 is eight bits
wide, and the wet/dry mix is a flat 0.5 fitted from three readings that bracket 0.45-0.55.

One open discrepancy: switching the tremolo on UNDER a sounding note drops it 6.02 dB in the
emulator, because the firmware's compensation is written at note-on and never again; the
owner measures 3 dB on hardware.  New notes are not affected and match hardware to 0.01 dB.
Section 10.

Still open: why these slots ramp symmetrically when the voices are 16:1 asymmetric.  That is
not just an effects question -- if the voices' asymmetry is not really about ramp direction,
`ENV_FALL_DIVISOR` is modelling the wrong thing.  Section 10.

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
