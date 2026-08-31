# What the scratch-patch take measured

Take: `tools/capture_env.py --set scratch`, 12 sweeps, 76 trials, 981 s, peak -18.5 dBFS.
Compared against `tools/render_u110.py --sequence capture_env --set scratch` with
`tools/scratch_analyse.py`. Every parameter is dictated over SysEx — 27 writes per segment,
parts 1-5 parked on MIDI channel 14 — so **tone is the only variable left**, and every
trial has exactly one voice except piano, whose two layers are inside the tone.

## 1. The in-note fall is LINEAR IN AMPLITUDE `[C]`

The decisive sweep. Dropping PART LEVEL lowers the level *and* the rate together, so the two
candidate ramp models predict opposite directions. Measured, decay speed **rises** as level
falls:

| PART LEVEL | 127 | 96 | 72 | 52 | 36 | 24 | 14 | 8 | ratio |
|---|---|---|---|---|---|---|---|---|---|
| Vib 1, dB/s | 11.80 | 12.69 | 13.36 | 16.01 | 16.89 | 21.73 | 25.21 | 31.44 | **x2.67** |
| Fretless Bass | 9.40 | 10.40 | 10.97 | 11.29 | 12.36 | 14.43 | 17.58 | 19.85 | x2.11 |
| Marimba | 52.54 | 58.68 | 58.55 | 60.85 | 62.18 | 66.24 | 71.85 | 78.20 | x1.49 |

Predictions were **up x2.5 for a linear-amplitude ramp, down x5.2 for a log-domain one**.
Vib lands on the linear prediction. The log-domain reading is dead for the in-note decay.

`[I]` This does **not** contradict `../env/ANALYSIS.md` §1, which fitted the *release* and
found it straight in dB. The two are separate mechanisms, and §5 below shows it directly.
The device's linear ramp is the right shape; `ENV_FALL_DIVISOR` is compensating for
something else.

## 2. The rate exponent is right `[C]`

Note number moves the rate while leaving register 07 and every target identical — rate law
at constant level. Hardware 7.49 -> 24.00 dB/s across six octaves (x3.20), emulator
7.22 -> 22.76 (x3.15). Mean error **7%**.

## 3. Level, velocity and attack all track `[C]`

| sweep | mean error |
|---|---|
| `scratch_level_vib` | 11% |
| `scratch_velocity_vib` | 10% |
| `scratch_env_attack` | 5% |
| `scratch_slow_vib` | 5% |

ENV ATTACK does not move the decay at all, on either side — 11.79 dB/s at every setting.

## 4. What is actually wrong: a per-tone rate offset, in whole rungs `[C]`

Six tones, byte-identical patch. The error is a **constant factor per tone**, the same at
every level (fbass -52% to -57% across the whole level sweep), so the ramp is right and only
the starting rung of the ladder is wrong:

| tone | emulator's first decay rate | hw dB/s | emu dB/s | counts short | rungs |
|---|---|---|---|---|---|
| Vib 1 | -43 | 11.80 | 11.17 | +0.6 | — |
| Slap 1 | -44 | 9.12 | 8.22 | +1.2 | — |
| Bell 1 | -56 | 5.56 | 9.18 | -5.8 | **-1** (too fast) |
| Fretless Bass | -42 | 9.40 | 4.51 | +8.5 | **+1** |
| Marimba | -59 | 52.33 | 20.64 | +10.7 | **+1** |
| A. Piano 1 | -27 | 17.79 | 4.24 | +16.6 | **+2** |

One rung is 8 counts, which is exactly 2x. The error is **discrete**, not continuous — a
control-flow fault in which phase the ladder starts from, not an arithmetic one.

Piano's figure is the aggregate of two layers and is the least clean of the six.

## 5. ENV RELEASE does not touch the in-note decay `[C]`

Negative control, confirmed on both sides: seven ENV RELEASE settings, mean error 9%, and
the decay slope does not move with the setting. Only the fall after note-off does. Any model
of one mechanism has to leave the other alone.

## Ranking

    scratch_level_marimba      +65%     scratch_velo_sens          +21%
    scratch_velocity_marimba   +59%     scratch_level_vib          +11%
    scratch_level_fbass        +53%     scratch_velocity_vib       +10%
    scratch_slow_fbass         +52%     scratch_env_release         +9%
    scratch_tones              +45%     scratch_keys                +7%
                                        scratch_slow_vib            +5%
                                        scratch_env_attack          +5%

Everything above 40% is marimba, fretless bass or the tones sweep that contains them.
Everything Vib 1 touches is within 11%.
