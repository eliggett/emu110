# `listen/` — recordings

Split by **where the audio came from**, because that is the distinction it is easiest
to get wrong and most expensive to get wrong:

    hardware/     a real Roland U-110, recorded through an interface
    emulated/     MAME renders
    comparisons/  Audacity projects and composites that hold both at once

Before this split the two sat side by side under names like `env` and `env-dh`, and only
the first line of each `session.txt` said which was which. That is a trap — an emulator
render compared against another emulator render looks like a passing test.

Every capture and render still carries its own provenance in `session.txt`:

    U-110 capture (tools/capture_u110.py)          <- hardware
    U-110 ENVELOPE capture (tools/capture_env.py)  <- hardware
    U-110 EMULATOR render (tools/render_u110.py)   <- emulated

## hardware/

| directory | what it is |
| --- | --- |
| `1`, `2` | the first two takes; `2` carries `ENVELOPE.md` and `envelope_data.csv` |
| `3` | the reference session — 17 segments, the basis of the output-response fit |
| `4` | the follow-up set: velocity sweep, effects-off Fantasy and Shakuhachi, strings C stack |
| `env` | envelope sweeps: release, attack, level, velocity sens, decay holds (`15_decay_hold.wav`) |
| `env2` | the rate-ladder follow-ups that pinned the rate law between multiples of 8 |
| `service-tests` | the unit's own service tests 8–11, recorded off the machine |

`env` and `env2` are the **only** hardware envelope data. Everything under
`emulated/env*` is a render, however much the name looks like a capture.

## emulated/

Renders from `tools/render_u110.py`, which plays the same sequence a hardware capture used,
so segment files of the same name line up and subtract directly.

| pairs with | render |
| --- | --- |
| `hardware/3` | `emu`, `emu2`, `emu3`, `emu4`, `emu-off`, `fit-eqon`, `fit-eqoff` |
| `hardware/4` | `4-emu` (EQ on), `4-emu-eqoff` (raw chain) |
| `hardware/env` | `env-emu`, `env-dh-emu`, and the single-sweep `env-*` directories |
| `hardware/env2` | `env2-emu` |

`fit-eqon` / `fit-eqoff` and `4-emu` / `4-emu-eqoff` come in pairs on purpose: the `-eqoff`
render has the output EQ correction switched off, which is what
`tools/fit_output_eq.py` needs in order to fit the whole correction rather than a residual
on top of the existing one.

`scratch/` is ad-hoc `u110run.sh -w` output — working files, not a matched set.

## Nothing here is committed

The wave files are large and reproducible: renders from the tree, captures from the
machine. `session.txt`, `trials.csv` and the `ANALYSIS.md` files are the parts worth
keeping, and they travel with their audio.
