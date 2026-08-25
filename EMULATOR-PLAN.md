# Roland U-110 — Emulator / LV2 Plugin Feasibility Outline

Scoping notes for a software U-110 that runs the real ROM dumps and loads as an LV2 plugin,
primarily as a development platform for ROM patching and custom cartridge authoring.

Companions: [`ROM-ANALYSIS.md`](ROM-ANALYSIS.md) (firmware), [`SYSTEM-DESIGN.md`](SYSTEM-DESIGN.md) (hardware).

---

## 0. This plan changed substantially

**MAME already emulates a close sibling of this machine, and the parts I had scoped as
months of open-ended research are done.**

- `src/mame/roland/roland_cm32p.cpp` — driver for the **Roland CM-32P**, built on the same
  PCM engine. Driver by Valley Bell.
- `src/devices/sound/roland_lp.cpp` — `mb87419_mb87420_device`, **our exact gate array
  pair**, with register map, phase accumulator, interpolation and sample decoding
  implemented.
- `src/mame/roland/bu3905.cpp` — the output control chip (§3.1).
- MAME's MCS-96 core covers the CPU.

Every finding in this analysis that overlaps MAME's agrees with it — including all 14
address-scramble bits and all 8 data-scramble bits, derived here independently. See
`SYSTEM-DESIGN.md` §4.6 for the full comparison.

**What that leaves is a porting and adaptation job, not a research project.**

---

## 1. Feasibility summary

| Component | Status | Where |
|---|---|---|
| CPU (8097BH) | Known; MAME core exists | `SYSTEM-DESIGN.md` §1 |
| Address decode, chip selects | Known | §2, service notes Fig. 1-b |
| P2.7 bank switch | Known — **U-110 specific, MAME's CM-32P has none** | §3 |
| I/O map | Known | §2.1 |
| MIDI | Known | `ROM-ANALYSIS.md` §5 |
| Patch / part / tone data formats | Known | §4, §6.6 |
| Wave ROM address permutation, all 19 bits | **Solved** | §4.2 |
| Wave ROM data permutation | **Solved** | §4.3 |
| **Sample encoding** | **Partly solved** — 1-3-4 float *deltas*; MAME's decoder is documented by MAME as unfixed | `ROM-ANALYSIS.md` §3 |
| **IC15 register map** | **Solved** | §3 |
| Engine sample rate | **32,000 Hz** = `34,816,000 / 1088`. `[C]` Confirmed against hardware; MAME's `clock/2/512` made the emulator +104 cents sharp | §5 |
| Voice allocation | 32 voices, voice 0 reserved for ROM reads, allocated 31 first. `[C]` Confirmed in emulation: playing voices are 1-31, voice 0 never allocated, and its regs `08`-`0B` step an address while reg `01` is read back | §3 |
| Chip regs `0x10`-`0x12`, `0x19`-`0x1D` | Still mostly unknown. `[C]` `0x10`/`0x12` select the voice for status reads; `0x1D` varies per patch (a candidate for the effect settings); `0x19`/`0x1B` are constant | — |
| IC16 output multiplex detail | Partially modelled in MAME | §5 |

### The remaining U-110-specific work

MAME's driver is for the **CM-32P**, not the U-110. The differences that matter:

| | U-110 | CM-32P |
|---|---|---|
| CPU | 8097BH, 16-bit bus to IC15 | P8098, 8-bit bus |
| Chip register addressing | `0x1400 + 2*reg` | `0x1400 + reg` |
| IC15 crystal | 34.816 MHz ÷ **1088** → 32 kHz | 32.768 MHz ÷ 1024 → 32 kHz |
| `0xE000-0xFFFF` bank switch on P2.7 | **yes** | absent |
| Cartridge slots | **four** | one |
| DSP at `0x1080-0x10FF` | absent | present |
| Panel / LCD | 16x2, own switch matrix | different |

None of that is research. The register-address doubling is a bus-width detail, the bank
switch is documented in `SYSTEM-DESIGN.md` §3, and the four-slot cartridge handling is
documented in `ROM-ANALYSIS.md` §6.

---

## 2. Architecture choice

**Low-level (LLE): emulate the 8097BH running the real firmware, model IC15/IC16 as devices.**
Faithful; every firmware quirk, timing behaviour and bug comes free; directly serves the goal
of testing modified ROMs. This is the Munt/MT-32 model.

**High-level (HLE): skip the CPU, parse ROM structures natively, implement a sampler.**
Much faster to first sound, but tests nothing about a modified ROM — which is the entire point.

**Recommendation: LLE core, with an HLE sampler built first as a throwaway.** The HLE path is
the cheapest way to validate the sample decode and the A14-A18 choice in isolation, and it
throws away cleanly once IC15 is modelled.

---

## 3. Phase 1 — system emulation (achievable now)

Everything here is specified by the existing documents.

**CPU core.** MAME has a mature MCS-96 / `i8x9x` implementation covering this part; reusing it
is the lowest-risk path and is BSD-licensed. Writing one is also tractable — the ISA is small
and Ghidra's SLEIGH definition is an executable reference — but is a few weeks of work plus a
long tail of flag-behaviour bugs.

**Memory map** exactly as `SYSTEM-DESIGN.md` §2.1, including the three EPROM windows, the
`0x2100` RAM boundary, and the P2.7 bank switch over `0xE000-0xFFFF`.

**Peripherals to model:**

| Device | Notes |
|---|---|
| Serial port | 31250 baud; feed from the host's MIDI input. `SBUF`/`SP_STAT`/`SP_CON` |
| HSO software timers 0/1/2 | All periodic ticks hang off these plus `TIMER1` |
| `PORT1` bits 0-3 | Cartridge presence, **active low** — drive from which card images are loaded |
| `PORT2` bit 6 | MIDI activity LED — expose as a UI blinker |
| `PORT2` bit 7 | Bank select — must actually switch the `0xE000` mapping |
| A/D channel 0 | Battery voltage. Return a healthy constant |
| LCD (`0x1100`/`0x1102`) | Model an HD44780-style controller; surface the 16x2 text to the UI |
| LEDs (`0x1200`), switches (`0x1300`) | Panel; switches let the UI drive real parameter editing |
| IC15 window (`0x1400-0x143F`) | **Stub initially — log every write with a timestamp** |
| Output control (`0x1F00-0x1F08`) | Log; wire up in Phase 2 |

**Milestone 1: the firmware boots. — DONE.** It boots to `P-01:Ac.Piano / MIDI 1 * * * * *`,
navigates its menus, lights the EDIT lamp, passes the battery check, persists its patch RAM
and mounts all four PCM cards, with no ROM patches. See
[`IMPLEMENTATION-PLAN.md`](IMPLEMENTATION-PLAN.md) §3 and
`mame/src/mame/roland/roland_u110.cpp`. No audio yet.

That milestone is worth reaching on its own merits — the timestamped IC15 register trace it
produces is the primary dataset for Phase 2, and it already validates modified ROMs for
everything except sound.

`[C]` **Engine sample rate: 32,000 Hz** = `34,816,000 / 1088`.

> This line has flipped twice. It first said 32,000 Hz, was overturned to 34,000 Hz by
> correction #13 on the strength of MAME's `clock / 2 / 512`, and is now back to 32,000 —
> settled against a real U-110 by playing identical MIDI into both. With MAME's `/1024` the
> emulator ran **+104 cents** sharp, and `1200 * log2(34000/32000) = +104.96`. The original
> derivation was right; #13 discarded a correct result because an implementation disagreed
> with it. See `ROM-ANALYSIS.md` correction #22.

---

## 4. Phase 2 — the sound engine

> **Superseded.** This section was written before MAME's CM-32P work was found, and items
> 1 and 2 below are solved (§0). The live plan is
> [`IMPLEMENTATION-PLAN.md`](IMPLEMENTATION-PLAN.md) §4; what remains is repairing the
> **differential** decoder, not discovering the format. Kept for the reasoning.

**Inputs available:** the register trace from Phase 1; the tone parameter records already
decoded (`ROM-ANALYSIS.md` §6.6); the real hardware, if you have a unit, for A/B comparison.

**Work items, roughly in order:**

1. **Resolve A14-A18** — 120 candidates. Best discriminated *after* the sample format is
   known: decode a known tone under each permutation and check which yields coherent audio.
   `DRUMS` (tone 99) or a sustained `FLUTE 1` are good targets. Note that this and item 2 are
   entangled — solving either makes the other easy, and attacking both at once is what
   defeated the WAV-alignment attempts in §1.1.
2. **Decode the sample format.** Known: 8 bits per sample, stateful/adaptive expansion to
   16-bit at 32 kHz (§1.1). Validate any candidate decoder by cross-correlating its output
   against the reference WAVs — a correct decoder matches unambiguously.
3. **Confirm the partial/keymap reading.** §6.6 reads `1E 27 2E 34 3D 47 54 60` as eight key
   split points and the following run as per-zone sample indices. Verify by playing across
   the splits and watching the sample index change.
4. **Model the phase accumulator.** Format is known — 32-bit with a 14-bit fraction — so pitch
   is an increment. Calibrate against the tuning table at `0xA516`.
5. **Model envelopes and levels** by correlating register writes against expected amplitude
   over time. This is where the trace pays off.
6. **Model IC16** — voice summing, and the output multiplex driven by `INH`/`MXA`/`MXD`.
7. **Optionally model the analog stage** — six LPFs. Low value early; skip until it sounds
   right digitally.

**Risk:** there is no datasheet for either gate array. If a behaviour cannot be inferred from
traces plus listening, the fallback is approximation, and the result stops being an emulator
and becomes an emulation *inspired by* the hardware. Worth deciding up front how much fidelity
the ROM-patching use case actually needs — for validating a modified MIDI dispatch table or a
new cartridge directory, quite a lot less than for authentic sound.

---

## 5. Phase 3 — LV2 packaging

Straightforward once the engine exists.

**Ports**

| Port | Type |
|---|---|
| MIDI in | `atom:AtomPort`, `atom:supports midi:MidiEvent` |
| Audio out | Either stereo mix, or **8 outputs** (6 individual + L/R) mirroring the real jacks |
| Controls | ROM image path, cartridge slots 1-4, plus panel-equivalent controls |

ROM and cartridge paths are best handled as LV2 `state:interface` properties rather than
control ports, since they are file references and change rarely.

**Realtime design.** `run()` must be allocation-free, lock-free and syscall-free. Structure
each call as: drain the incoming MIDI atom sequence into the emulated UART; step the CPU and
IC15 forward by exactly the frame count requested, interleaved so register writes land at the
right sample offsets; write output.

Do **not** run the CPU on a worker thread — it destroys determinism, which is precisely what
makes the emulator useful for testing ROM changes.

**Load.** A 12 MHz 8097BH is roughly 1-2 MIPS, and 31 voices at 32 kHz is trivial arithmetic.
Both together should sit far below realtime on any modern core, leaving headroom for the
sample-accurate interleaving above.

**Latency.** Nothing inherent beyond the host buffer — 64 frames at 48 kHz is 1.33 ms. The
emulated firmware's own MIDI-to-note delay is reproduced rather than added, which is authentic
behaviour, not a defect. Two things to get right: resample 32 kHz engine output to host rate
with a decent kernel, and keep MIDI event timestamps within the buffer rather than quantising
to buffer boundaries.

---

## 6. What this unlocks

- Load a modified program EPROM image instantly; no burning, no desoldering.
- Load synthetic cartridge images and iterate on directory structures — the format is
  documented in §6.5-6.6.
- Deterministic regression testing of ROM patches.
- The register trace is a permanent reverse-engineering instrument, useful well beyond the
  emulator itself.

---

## 7. Effort and sequencing

| Phase | Nature | Rough scale |
|---|---|---|
| 1. System emulation | Engineering, fully specified | Weeks; **days** if MAME's CM-32P driver is used as the starting point |
| 2. Sound engine | **Mostly** implemented in MAME, decoder imperfect | Adaptation done; decoder repair remains |
| 3. LV2 packaging | Engineering | A week or two |

> The middle row was the project risk. It shrank a great deal, but it is not the smallest
> item — MAME's decoder drifts and MAME says so. See `IMPLEMENTATION-PLAN.md` §4.3.

**Recommended route: write a U-110 driver for MAME first.** It is a genuine sibling of an
existing driver, the sound device already exists, and the deltas above are small and
documented. That yields a working, debuggable U-110 with far less effort than a standalone
build — and it is the natural place to contribute the findings back.

Extract to LV2 only afterwards, reusing MAME's CPU core and `roland_lp` device as libraries
(both BSD-3) behind a thin realtime wrapper.

**Sequencing advice:** get Milestone 1 (firmware boots, traces register writes) before
committing to anything else. It is the cheap part, it de-risks everything after it, and it
already delivers most of the ROM-modding value even with silence at the output.



**Licensing.** LV2 is permissive and MAME's CPU cores are BSD-3; both are compatible with an
open plugin. **Ship no ROM images** — the plugin must load user-supplied dumps, exactly as
Munt does for the MT-32.
