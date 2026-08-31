# Roland U-110 — Implementation Plan

Working plan for building a U-110 emulator, starting as a MAME driver and extracting to LV2.

Companions: [`analysis/ROM-ANALYSIS.md`](analysis/ROM-ANALYSIS.md) (firmware),
[`analysis/SYSTEM-DESIGN.md`](analysis/SYSTEM-DESIGN.md) (hardware),
[`EMULATOR-PLAN.md`](EMULATOR-PLAN.md) (feasibility outline this supersedes).

> **Confidence key** — `[C]` confirmed by running code against real data during planning,
> `[S]` from service notes / MAME source, `[I]` inferred, still open.

---

## 0. Corrections folded back into the earlier documents — **applied**

Three claims in the existing documents were wrong or stale. All three have now been
corrected at source: `ROM-ANALYSIS.md` §3 and corrections #15-#19, and `EMULATOR-PLAN.md`
§1, §3, §4 and §7. The reasoning is kept here because it is what set the schedule.

### 0.1 The sample decoder is *differential*, not memoryless `[C]`

`ROM-ANALYSIS.md` §3 states the 1-3-4 float format is *"memoryless, not adaptive"*, and
corrections-log entry **#15** retires the earlier (correct) stateful reading. That is a
misreading of MAME. `src/devices/sound/roland_lp.cpp` accumulates:

```c
chn.smpl_cur = chn.smpl_nxt;
chn.smpl_nxt += decode_sample((int8_t)read_byte(addr));   //  +=  , not  =
```

seeded at zero on note start. The 1-3-4 float is the **increment**, not the sample. All
three reference WAVs in `waves/` begin with a literal `0x0000` frame, which is the
fingerprint of exactly this: an accumulator starting from silence.

So entry #15 reversed a conclusion that was right. The original evidence (40,191 distinct
values where a byte lookup allows 256) was weak — resampling explains density too — but the
conclusion it overturned was correct.

**Entry #15 should be re-reversed, and §3 should read "differential 1-3-4 float".**

### 0.2 MAME's decoder is known-broken, in MAME's own words `[S]`

`EMULATOR-PLAN.md` §1 lists sample encoding as **Solved** and §7 calls Phase 2 *"the
smallest item"*. MAME disagrees with itself twice in the same file:

```c
// until the decoding is fixed, we prevent overflow bugs (due to DC offsets when looping) this way
chn.smpl_nxt = std::clamp<int16_t>(chn.smpl_nxt, -0x7FF, +0x7FF);
```
```c
// Note: These formulae are probably incorrect, as they cause some DC offset in most samples.
```

The `±0x7FF` clamp is a band-aid over an accumulator that drifts, and `cm32p` ships
`MACHINE_IMPERFECT_SOUND` because of it. Reclassify from **Solved** to
**implemented-but-known-wrong**; it is §4's single real work item.

### 0.3 The stale 32 kHz line — **inverted; the "stale" line was the correct one** `[C]`

> This item read: *"`EMULATOR-PLAN.md:119` still asserts 32,000 Hz from `34,816,000 / 1088`.
> Corrections entry #13 retired that reasoning; §1 of the same file says 34,000 Hz. Line 119
> is a leftover."* **Backwards.** Real hardware settled it at **32,000 Hz**: with MAME's
> `/1024` the emulator ran +104 cents sharp and `1200 * log2(34000/32000) = +104.96`. Line 119
> was not a leftover, it was right, and #13 had discarded a correct derivation because an
> implementation disagreed with it. Both files now say 32 kHz. See correction #22.

`EMULATOR-PLAN.md` §4 as a whole is stale — it is written as "the research phase" with work
items 1 and 2 being the two things §0 of that file declares solved.

---

## 0.5. Phase 1 status — **complete**

The driver is `mame/src/mame/roland/roland_u110.cpp` (~650 lines). Build it with
`make SOURCES=src/mame/roland/roland_u110.cpp SUBTARGET=u110`.

Verified running, with **no ROM patches**:

| Item | Result |
|---|---|
| Boot | `Roland / PCM Sound Module` -> `Mem Initialized` -> `P-01:Ac.Piano / MIDI 1 * * * * *` |
| Panel | Edit/Exit -> `Select Mode` -> `UTILITY` -> `UTILITY:BULK`, three levels deep |
| LEDs | EDIT lamp lights on entering edit mode (`0xFFFD` = `NOT 0x0002`) |
| Battery check | Passes; accepted window measured as `AD_RESULT_HI` in `[0x85, 0xCB]` |
| NVRAM | Cold boot initialises, warm boot does not — both SRAMs persist |
| Cards | All four slots mount; ID cache at `0x2743` reads `08 00 00 09` with SN-U110-08 in slot 1 and SN-U110-09 in slot 4 |
| Speed | ~45x realtime headless on one core |

**Three changes were needed outside the driver.**

1. **`i8x9x.cpp` gated the HSI.0 interrupt on `IOC1` bit 1.** This firmware sets
   `IOC1 = 0x21` — bit 1 clear, so EXTINT stays on the pin for IC15 — while enabling
   `INT_MASK` bit 4 for HSI.0 from IC8. MAME made the two mutually exclusive, so the LCD
   text ring at `0x2700` filled and the firmware spun forever at `0xD2F6`. `INT_MASK` is the
   real enable and `check_irq()` already honours it, so the gate was redundant as well as
   wrong. **This is very likely the same wall the CM-32P driver papers over with a ROM patch
   in `machine_start()`**, and it is the most plausible upstream contribution here.
2. **`34.816 MHz` added to `emu/xtal.cpp`.** The running device reported
   `Clock 17408000, Rate 34000` at this stage. `[C]` The rate was later corrected to
   **32,000 Hz** via `set_rate_divider(1088)` — see §0.3 and correction #22.
3. **The CPU's wave-ROM read port must wrap at 18 bits before the bank is OR'd in.** The
   firmware asks for logical address 0 by parking at `0 - 2`, which sign-extends through its
   `SHRAL` to `phase = 0xFFFF8000`; without the wrap, `(phase >> 14) + 2` lands a bank away
   and every card reports `"  Illegal CARD"`. The CM-32P driver masks identically — I had
   dismissed that as a quirk in §1 of this plan, and was wrong.

**On the LCD.** The controller part is unidentified and no CGROM is dumped, so the display
renders blank glyphs. `tools/make_lcd_cgrom.py` synthesises an ASCII font purely for
legibility; it is loaded as a deliberately `BAD_DUMP` region and is **not** a dump of
anything. Everything behind the glyphs is genuine.

**Still open from Phase 1:** MIDI in/out are not connected to host ports, and `PORT1` bit 4
remains unexplained.

---

## 1. Baseline established during planning `[C]`

Before committing to a schedule, the descramble/decode toolchain was run against the real
dumps. It works. This is the ground the plan stands on.

| Check | Result |
|---|---|
| MAME `UNSCRAMBLE_ADDR` + `UNSCRAMBLE_DATA` on `waverom0` | applies cleanly |
| Tone list at descrambled `0x1000`, `0x50` stride | `A.PIANO 1`…`DRUMS`, all in manual order |
| Tone types at `+0x0A` | `03` V-MIX, `00` single, `02` detune, `80` rhythm for `DRUMS` |
| Sample table at descrambled `0x100`, `0x0A` stride | **226 entries**, all structurally sane |
| ID header | plain ASCII in the **raw** dump at offset 0, scattered after descrambling |

The header being readable *before* descrambling and scattered *after* is correct and
expected: the firmware's 48-byte table at `0x9357` reads the scrambled positions to
reassemble linear text. MAME notes the same (*"Only the first 0x80 bytes of the ROMs are
readable text in a raw dump"*). Do not treat a garbled descrambled header as a bug.

### 1.1 Open question §6.6 / #2 is now closed `[C]`

`ROM-ANALYSIS.md` carried the key-split / sample-index reading of a partial block as `[I]`,
*"unproven against playback"*. The sample table proves it without playback. Tone 1
`A.PIANO 1` lists note IDs `1E 27 2E 34 3D 47 54 60` (MIDI 30, 39, 46, 52, 61, 71, 84, 96)
and sample IDs `00 01 … 08`. Reading the reference-note field (`+0x08`) of those nine
sample-table entries in order gives:

```
48  55  64  66  71  79  89  94      <- ascending, in lockstep with the splits
```

Nine samples, eight split points, reference notes climbing monotonically across the
keyboard. That is a multisample keymap and nothing else. **Promote to `[C]`.**

Two further fields fall out: `+0x07` sits at `0x3F`–`0x44` across entries, i.e. centred on
`0x40` — a fine-tune. `+0x09` (`0x41`–`0x60`) is still `[I]`.

### 1.2 The reference WAVs — what they are, and what they are worth `[C]`

All three files in `waves/` are mono 16-bit, tagged 32000 Hz.

| File | Frames | Corresponding ROM |
|---|---|---|
| `roland_t110_u110_u220_waves.wav` | 2,026,236 | `waverom0..3`, 2,097,152 bytes |
| `roland_sn-u110-08_waves.wav` | 495,179 | `waverom4`, 524,288 bytes |
| `roland_sn-u110-09_waves.wav` | 507,889 | `waverom5`, 524,288 bytes |

**They are derived, not raw.** Consecutive differences take 26,519 distinct values where a
byte-granular differential decode allows at most 255. They have been resampled or
interpolated, confirming corrections entry #13 on that point.

**They are still the best oracle available**, and more usable than the earlier documents
credited, because their alignment to the ROM is recoverable by machine rather than by ear.
Decoding sample 0 (start `0x075F9F`, bank 0, length 24,518) with MAME's differential
decoder and FFT cross-correlating a 4,096-frame window against the full 2M-frame WAV gives:

```
best normalized correlation 0.318 at WAV frame 358606
neighbours 358604..358607 all ~0.317   (a smooth peak, not a fluke)
```

Noise floor for a 4,096-sample window is ~0.02. **0.32 is the signature of right data,
wrong decoder** — the sample bytes are being located correctly and the reconstruction is
wrong. That number is the objective function for §4.

Corollary worth stating plainly: this makes decoder repair a **search problem with a
numerical score**, not a listening exercise. That is a much better position than
`EMULATOR-PLAN.md` §4 assumed.

### 1.3 Samples overlap `[C]`

The 226 sample-table entries sum to 3,056,042 bytes of sample data against 2,097,152 bytes
of ROM — a 1.46× excess. Entries therefore share regions (same waveform, different loop
points and reference notes). Any tooling that assumes samples partition the ROM will be
wrong.

---

## 2. Route

**MAME driver first, LV2 extraction second.** Unchanged from `EMULATOR-PLAN.md` §7 and still
right, for a stronger reason than that document gives: the U-110 is a *better* driver target
than the CM-32P MAME already has, because it lacks the CM-32P's DSP at `0x1080-0x10FF` and
its `some_ram` guesswork. Fewer unknowns, not more.

The target file is `mame/src/mame/roland/roland_u110.cpp`, modelled on
`roland_cm32p.cpp`.

### 2.1 What MAME already gives us, verified

| Asset | Location | Note |
|---|---|---|
| `N8097BH` CPU device | `cpu/mcs96/i8x9x.cpp:577` | **our exact part**, 16-bit program space |
| `MB87419_MB87420` sound device | `sound/roland_lp.cpp` | our exact gate array pair |
| `BU3905` output control | `mame/roland/bu3905.cpp` | our IC26/IC6 |
| `u110_card` software list | `hash/u110_card.xml` | **already exists**, with our scramble in its header |
| `MSM6222B_01` LCD | `video/msm6222b.h` | candidate for our 16x2 |
| `u220` skeleton | `mame/roland/roland_u20.cpp` | same wave engine; our work feeds it |

`EMULATOR-PLAN.md` under-credits this. The card software list and the CPU variant both
already exist; neither needs creating.

### 2.2 The register-window adapter — complete `[C]`

The U-110/CM-32P delta reduces to one line. MAME's device is byte-addressed because the
CM-32P's P8098 has an 8-bit bus; the U-110's 8097BH has a 16-bit bus and therefore sees the
chip's register-select lines shifted one place:

```
mame_reg = (cpu_addr - 0x1400) >> 1
```

A 16-bit CPU access covers `mame_reg` and `mame_reg + 1` (low byte, then high byte).
Every address in `ROM-ANALYSIS.md` §3 lands correctly under this rule:

| CPU addr | → MAME reg(s) | Function |
|---|---|---|
| `0x1402` | `01` | ROM/card data read port |
| `0x1404` | `02`,`03` | ROM bank + loop mode |
| `0x1408` | `04`,`05` | pitch, 2.14 fixed point |
| `0x140C` | `06`,`07` | volume |
| `0x1410` | `08`,`09` | start address, fraction |
| `0x1414` | `0A`,`0B` | start address, high word |
| `0x1418` | `0C`,`0D` | end address |
| `0x141C` | `0E`,`0F` | loop address |
| `0x1422` / `0x142A` | `11` / `15` | voice enable masks |
| `0x143E` | `1F` | voice select |

`0x1400-0x143F` is 64 bytes = 32 chip registers, exactly the device's register file.

### 2.3 The four-slot cartridge question is a ROM layout, not code `[C]`

MAME's device computes `addr = (chn.addr >> 14) | ((chn.bank & 0x3C00) << 8)`. The U-110
writes its bank selector as `(slot & 3) << 4 | (card ? 8 : 0)` into the **high** byte, i.e.
bits 12-13 = slot, bit 11 = card. Those land on address bits 20-21 and 19 respectively, so
the required 4 MB `pcm` region is:

```
0x000000  waverom0   (internal bank 0)      0x080000  card slot 0
0x100000  waverom1   (internal bank 1)      0x180000  card slot 1
0x200000  waverom2   (internal bank 2)      0x280000  card slot 2
0x300000  waverom3   (internal bank 3)      0x380000  card slot 3
```

Four internal banks plus four cards fill the region exactly. This is the identical decode
the CM-32P uses (IC18 at 0, card at `0x080000`, IC19 at `0x100000`, IC20 at `0x200000`) —
**the sound device needs no change at all** for four slots. `EMULATOR-PLAN.md` lists this as
a U-110-specific work item; it is not one.

---

## 3. Phase 1 — boot the firmware

Goal: `U-110  Ver2.03` on the LCD, menus navigable, MIDI accepted, and a timestamped
register trace coming out. No audio.

### 3.1 Milestone 1a — CPU and memory map

`N8097BH` at `12_MHz_XTAL`. Address map per `SYSTEM-DESIGN.md` §2.1:

```
0x0000-0x0FFF   rom  region "maincpu" 0x0000    (blank in both images; map it anyway)
0x1100          w    lcd_ctrl        1102 w  lcd_data
0x1200          w    led_w           (16-bit latch, firmware writes INVERTED)
0x1300          r    switch_r
0x1400-0x143F   rw   snd_io          (adapter per §2.2)
0x1F00-0x1F08   w    out_ctrl        (BU3905)
0x2000-0x20FF   rom  region "maincpu" 0x2000    (vectors, CCB, reset stub @0x2080)
0x2100-0x3FFF   ram                              (IC10 work RAM)
0x4000-0xDFFF   rom  region "maincpu" 0x4000
0xE000-0xFFFF   bankswitched on P2.7             (see 3.2)
```

**On the 16-bit program space.** I flagged this as a likely time sink; on inspection it is
milder than feared. `N8097BH` gets a 16-bit `AS_PROGRAM`, but MAME's memory system reads a
mapped region linearly regardless of space width, so a word read at `0x4000` returns
`rom[0x4000] | rom[0x4001] << 8` — precisely what the real machine produces after two
byte-wide bus cycles. Byte-wide EPROM and SRAM map with plain `.rom()` / `.ram()` and
need no mirroring trick.

What *is* lost is timing: the real machine spends two bus cycles fetching a word from
byte-wide memory and MAME's `m_cache16` path will spend one, so the emulated CPU executes
somewhat faster than the real one. `[I]` Every periodic behaviour in this firmware hangs off
HSO software timers clocked from `TIMER1`, not off instruction count, so this should not
affect correctness — but it is a real fidelity gap and the first thing to suspect if
timing-sensitive firmware code misbehaves. Record it; do not chase it in Phase 1.

### 3.2 Milestone 1b — the P2.7 bank switch

`P2.7` high maps EPROM `0xE000-0xFFFF` (factory default patches); low maps IC11 SRAM (user
patches). `ORB PORT2,#0x80` occurs exactly once in the image, at `0x8479`. Battery-backed
SRAM should persist via `nvram_device`.

This is genuinely U-110-specific — the CM-32P has no equivalent — but it is six
instructions' worth of behaviour and fully documented in `SYSTEM-DESIGN.md` §3.

### 3.3 Milestone 1c — the LCD, and IC8's `LCD INT` ← **the gating item**

This is the piece that decides whether Phase 1 takes days or weeks, and
`EMULATOR-PLAN.md` does not name it.

MAME's `cm32p` driver is `MACHINE_NOT_WORKING` and carries two live ROM patches in
`machine_start()`:

```c
rom[0xbb2d] = 0x03; // hack to make test mode not freeze when displaying the LCD text
rom[0x7d80] = 0x00; // hack to exit some loop waiting for interrupt #8
```

with the comment *"The IC8 gate array has an 'LCD INT' line that needs to be emulated. Then,
the hack can be removed."* The CM-32P's IC8 is **M60012-0141FP** — the identical part number
to the U-110's IC8. We inherit the problem. On the CM-32P it only breaks test mode; on the
U-110 the LCD *is* the user interface, so a text-queue drain that never fires means no menus
at all.

**The interrupt assignment resolves cleanly**, by lining MAME's 1-based interrupt numbering
up against our vector table:

| MAME says | Vector | Our handler | Source |
|---|---|---|---|
| *"Interrupt #5 (calls 0x4014)"*, LCD-related | `0x2008` HSI.0 | `0x4032` | IC8 `LCD INT` |
| *"Interrupt #8 (calls 0x4020)"*, needed while playing notes | `0x200E` EXTINT | `0x41BB` | IC15 |

`ROM-ANALYSIS.md` independently labelled `0x4032` the **LCD queue drain**, and `cm32p`
independently wires the PCM chip to `EXTINT_LINE`. Both agree. **HSI.0 = LCD INT from IC8;
EXTINT = interrupt from IC15.** `[C]`

Note that `roland_u20.cpp`'s `u220` config instead routes the PCM chip to `HSI0_LINE`. That
is either a U-220 difference or a guess in a skeleton driver; **do not copy it.**

Work items:
1. Instantiate the LCD. Try `MSM6222B_01` first — same era, same Roland design office, and
   it is what `cm32p` and the D-110 use. Fall back to `video/hd44780.h` if the character set
   or busy behaviour mismatches. `[I]`
2. Model IC8's busy/ready handshake and assert `HSI0_LINE` on the ready transition.
3. **Success test: the firmware prints `U-110  Ver2.03` with no ROM patches applied.** If a
   `machine_start()` hack is needed to get text on screen, Milestone 1c is not done.

### 3.4 Milestone 1d — panel, ports, MIDI

| Item | Detail |
|---|---|
| Switch matrix | read at `0x1300`; needs the U-110's own layout from the service notes, not the CM-32P's test switches |
| Panel LEDs | `0x1200`, 16-bit latch, written **inverted** |
| EDIT / PAGE LEDs | IC5 HC74 2-bit latch |
| `PORT1` bits 0-3 | cartridge presence, **active low**; drive from which card images are mounted |
| `PORT1` bit 4 | open question — log it and see what the firmware does |
| `PORT2` bit 6 | MIDI activity LED |
| A/D channel 0 | battery voltage, polled; return a healthy constant |
| MIDI in/out | `serial_w` / `serial_tx_cb`, 31250 baud. Replace `cm32p`'s hardcoded 3-byte test blip with a real MIDI-in device |

Four cartridge slots via `generic_cartslot_device`, `SOFTWARE_LIST` set to the existing
`u110_card` list.

### 3.5 Milestone 1e — the register trace

The highest-value Phase 1 deliverable, and the primary input to Phase 2. Log every
`0x1400` window access as `(cpu cycle, voice, mame_reg, value)` to a file. Add a matching
log for `0x1F00-0x1F08`.

**Prefetch sign warning.** MAME's `cm32p` read path applies `addr = ((addr >> 6) + 2)` — a
**+2** compensation for the chip's internal prefetch — while the U-110 firmware applies its
own **−2** at `0x7BB2` (`SUB RW56,RW50,#0x2`). Both model the same hardware behaviour from
opposite sides and should cancel. Get the sign wrong and every card mounts as
`"  Illegal CARD"` while internal banks look fine — a failure that will read as a card-format
problem for hours. **Test both signs deliberately on day one.**

Phase 1 exit criteria:
- boots to `U-110  Ver2.03` with **zero ROM patches**
- menus navigable from the emulated panel
- all four card slots mount real `SN-U110-*` images and list their tones
- MIDI in accepted, note-ons visible in the trace
- register trace file produced

At that point the ROM-modding use case is already served for everything except sound.

---

## 4. Phase 2 — the sound engine

Not research, but not free either. One genuinely open item, now with a numerical target.

### 4.1 Wire it up (mechanical)

`MB87419_MB87420` at `34.816_MHz_XTAL`. `[C]` The device's own `clock() / 2 / 512` gives
34,000 Hz, which is **wrong for the U-110**; the driver overrides it with
`set_rate_divider(1088)` for 32,000 Hz (§0.3). `int_callback` → `EXTINT_LINE` (§3.3). `pcm` region per §2.3.
Register adapter per §2.2. Nothing here is invention.

### 4.2 Resolve the 34 kHz / 32 kHz tuning question ~~`[I]`~~ — **CLOSED: 32,000 Hz** `[C]`

The device derives 34,000 Hz from the U-110's 34.816 MHz crystal, but MAME documents the
sample table's reference-note field as *"when played back at 32000 Hz"* — and that field
lives in the **shared** wave ROM that the U-110, U-220, T-110 and CM-32P all read. Something
must absorb a 6.25% error, or the instrument plays a quarter-tone sharp.

Candidates: the U-110's pitch table at `0xA516` compensates; or the 34.816 MHz reading needs
re-checking against the schematic; or the reference-note field is scaled elsewhere.

**This is cheap to settle and expensive to get wrong**, because it is a constant error that
sounds "fine" in isolation and only reveals itself against a tuner. Play a known tone at its
reference note and measure. Settle it before touching the decoder, or every decoder
experiment inherits an unknown pitch offset.

### 4.3 Fix the differential decoder ← the real work

> `[C]` **SUPERSEDED — solved.** The format is an 8-bit float (sign + 3-bit exponent +
> 4-bit mantissa on the magnitude), and it was settled not by the scoring harness below but
> by the U-110's own Sound Check tone, whose correct output is a known pure sine — so the
> transfer curve could be read off directly. See §3 of `ROM-ANALYSIS.md` and correction #30.
> The plan below is kept as a record of the approach that did **not** settle it.

Method, following directly from §1.2:

1. Build a scoring harness. For each of N sample-table entries: decode from the descrambled
   ROM with a candidate decoder, FFT cross-correlate against the full reference WAV, take
   the peak. Score = mean peak across N. **MAME's current decoder scores ~0.32.**
2. Establish the ceiling. Resampling alone caps the achievable correlation; estimate it by
   resampling a decoded segment and correlating against itself.
3. Search decoder variants against the score. Candidates, cheapest first:
   - delta scaling factor (the `±0x7FF` clamp against a `±1984` delta range is suspicious
     on its face — the accumulator is clamped to roughly one delta's span)
   - accumulator reset behaviour at loop points (MAME's DC-offset complaint lives here)
   - exponent bias / the `shift - 1` term in the 1-3-4 expansion
   - whether the ping-pong reflection formulae are right (MAME says *"probably incorrect"*)
4. Once the score jumps, confirm by ear and against the manual's tone chart.

Two things make this tractable that the earlier plan did not have: the alignment is found by
machine rather than by ear, and there is a scalar that says whether a change helped.

### 4.4 Voice freeing `[S]`

`roland_cm32p.cpp:40`: *"TODO: figure out how 'freeing a voice' works — right now the
firmware gets stuck when playing the 32nd note."* A six-part multitimbral U-110 will hit
this sooner than a CM-32P does. Related to the EXTINT handler at `0x41BB`. Budget for it
explicitly rather than discovering it at the 32nd note.

### 4.5 Output stage

> `[C]` **SUPERSEDED — solved**, see §4.6. Routing is per **part**, from patch byte
> `+0x0B >> 5` (Output Assign), with `+0x0E + 1` giving the Owner's Manual Output Mode that
> partitions the 31 voices into groups. The prediction below — that stepping `+0x0E` would
> answer it — was right in spirit, but the field that mattered was `+0x0B`.
>
> The line "the six analog LPFs are low value; skip until it sounds right digitally" was
> **wrong**: without the reconstruction filter the emulator runs **24-30 dB hot above 2 kHz**
> against hardware. It is the single largest spectral error remaining.

`BU3905` for `0x1F00-0x1F08`, eight routing registers from the table at `0xA8B6`. Six
individual outputs plus stereo mix.

---

## 4.6 Phase 2 status

### Settled — and settled against real hardware

A U-110 owner ran `tools/capture_u110.py`, which drives the machine over MIDI and records
its audio. That capture (and a stereo reference file the owner assembled) decided three
questions that months of metric-fitting had not.

| Item | Result |
|---|---|
| §4.2 tuning question | **Closed.** Reference notes are engine-rate relative; no cross-rate correction exists. |
| **Engine rate** | **32,000 Hz** = `34,816,000 / 1088`. MAME's `/1024` made the emulator +104 cents sharp. The original derivation was right and correction #13 was wrong. |
| **Sample format** | **8-bit float: sign + 3-bit exponent + 4-bit mantissa**, applied to the magnitude of the two's complement byte; full scale ±1984. MAME's `decode_sample()` was already correct — only its `+=` (delta) was wrong. Sample 212 decodes to a sine at 0.99975 with h3 at −66.7 dB, against −15.3 dB read linearly. See `ROM-ANALYSIS.md` §3 and correction #30. |
| Absolute tuning | **A = 440 Hz** (±2.5 cents across five octaves, patches without chorus). Not A442. |
| Voice numbering | Playing voices are **1-31**. **Voice 0 is the CPU's wave-ROM read port** — the CPU sets an address in regs `08`-`0B` and reads the byte back from reg `01`; it is never allocated to a note. |
| Voice enable | 16-bit writes at `0x1422`/`0x142A` cover voices 0-15 and 16-31; high halves routed to device regs `0x13`/`0x17`. 12 notes now start 12 voices. |
| Output level | `[I]` **Stale.** `sample * 16` was tuned for the linear reading. The float decode has full scale ±1984 already, and applies no scaling; absolute level is uncalibrated and the renders normalise on output. |
| MIDI in | Bit-level UART in the driver; `-min file.mid` works. |
| Bank decode | Bit 10 is wave address bit 18; the phase accumulator supplies bits 0-17. |
| Panel | Six switches on IC8 `READ0`-`READ5`; IC5 latches D0/D1 for the PART and EDIT lamps. |

Pitch now tracks hardware within **±2.5 cents** across five octaves and four patches.

### Voice volume — **solved** `[C]`

Registers `06`/`07` are two independent fields, not one linear 16-bit magnitude:

- **reg 06** — 7-bit level, clamped by the firmware to `1..0x7F`
- **reg 07** — **logarithmic level, 16 units per octave** (0.3763 dB/unit), full scale `0xFF`

The scale is the firmware's own: its table at `0xAEC6` maps 1, 2, 4, 8, 16, 32, 64, 128 to
143, 159, 175, 191, 207, 223, 239, 255 — a doubling of amplitude per 16 units. A note takes a
**voice pair** whose partials are layered by velocity and sum.

`roland_lp.cpp` decodes it through `volume_gain()`. Velocity span went from **5.3 dB** to
**20.1 dB** against hardware's **21.3 dB**, within 1.3 dB at every measured velocity.

`[I]` Open: what the low byte does, and a residual ~1.3 dB.

### Output demultiplexing — **solved end-to-end** `[C]`

The stereo image follows from two tables, with no need to model the slot multiplex at all:

| step | source | status |
|---|---|---|
| part → output (1-6) | patch part byte **`+0x0B >> 5`** | `[C]` verified against OM p.5 Patch Setting Chart |
| patch → routing preset | patch byte **`+0x0E`**; `+1` = the chart's Output Mode | `[C]` verified on three patches |
| output → MIX pan | service test 11 on real hardware | `[C]` +/-65.9, +/-6.8, 0.0 dB, correlation 1.000, zero delay |

`tools/render_stereo.py` implements it: it reads the patch's per-part key ranges and output
assigns, renders each part on its own (feeding the emulator only the notes in that part's
zone -- exact, because the zones are disjoint), and pans the results together.
P-04 Wide Piano renders as a monotonic left-to-right sweep with rising pitch,
`-68, -6.8, 0.0, 0.0, +6.8, +69 dB`.

**Still open, but not blocking:**

- `[I]` **Which jack sits at which pan position.** The test-11 capture has no step marks, so
  the output → pan assignment in `render_stereo.py` is the one that makes Wide Piano a
  monotonic sweep. Re-capture test 11 with `tools/capture_u110_test.py --test 11` marks to
  confirm.
- `[I]` **How the assignment reaches IC16.** No per-voice register carries it and the routing
  registers are written once per patch, yet two patches give contradictory constraints for any
  fixed voice → slot map (see `SYSTEM-DESIGN.md` §5.4). Only matters for a cycle-accurate
  device model.
- **In-emulator stereo.** `render_stereo.py` works part-by-part offline. Doing it inside the
  driver needs the device to carry six output buses, which needs the mechanism above.

### Effects — **hardware, in IC15**; not modelled `[C]`

`[C]` The CPU performs **no modulation of any kind**. On P-52 Fantasy (maximum tremolo depth
`0F`) with a 10 s sustained note, **no register of any playing voice changes value**: the
frequency word holds `0x3BFE` and is merely rewritten, volume is written 4 times. There is no
software LFO and no software envelope, so chorus, tremolo and the amplitude envelope are all
generated inside the gate arrays.

They belong to **IC15**, not IC16. IC17 (CXK5814, **2K x 8** — about **64 ms** at 32 kHz, a
chorus delay line) hangs off IC15's address bus, is written and read only by IC15, and is never
visible to the CPU. IC16 only carries wave data to the DAC.

Parameters are patch header bytes **`+0x0F`..`+0x12`** = **CHORUS RATE, CHORUS DEPTH, TREMO.
RATE, TREMO. DEPTH** (the firmware's own strings at `0x09813`-`0x0987D`), written once at patch
load. Eleven distinct combinations across the 64 factory patches; 14 patches share
`07 07 07 07`.

**To model:** an LFO plus a ~64 ms delay line in the device, driven by those four bytes.
`[I]` Where they land in IC15's register window is unresolved — `19` and `1B` are constant
across patches (`00`, `21`), `1D` varies (Fantasy `20`, Wide Piano `60`, Ac.Piano `00`) but does
not map obviously onto the four values. Until then every chorus/tremolo patch renders dry.
See `SYSTEM-DESIGN.md` §4.5.

### Method note

Two of the three decisive facts here came from hardware, and both **contradicted**
conclusions reached and defended from the data alone. The decode hypothesis went memoryless
-> differential -> bit-permuted -> companded -> linear, and no internal metric settled it;
one owner recording did. Where ground truth is obtainable at moderate cost, get it early.
`tools/capture_u110.py` exists for exactly this and is cheap to re-run.

---

## 4.7 Running it — the tools `[C]`

```
tools/u110run.sh [-p PATCH] [-t SECONDS] [-m MIDI] [-w WAV] [extra mame args]
```

Headless, one scratch NVRAM directory per run, patch selected from the panel.

| tool | what it does |
|---|---|
| `u110run.sh` | Headless runner. **`SDL_VIDEODRIVER=dummy` is the part that suppresses the window** — MAME's `-video none` still asks SDL for one and pops a black rectangle onto the desktop. |
| `select_patch.lua` | Taps `[INC]` from P-01. Needs a scratch NVRAM dir: the patch number is battery-backed and `[INC]`/`[DEC]` wrap mod 64, so a known start is the only way to address a patch absolutely. |
| `render_stereo.py` | Renders a patch in stereo from its per-part Output Assign. |
| `u110_output_filter.py` | The Fig. 4 reconstruction filter (6740 Hz Q1.74, 11708 Hz Q1.21, 7234 Hz RC). Post-processing only — **not yet in the driver**, so raw renders run 24-30 dB hot above 2 kHz. |
| `plot_sample212.py` | Plots the Sound Check waveform to PDF. |
| `render_sample.py` / `render_note.py` | Decode and render a wave-ROM sample directly, no emulator. |
| `trace_voices.py` | Voice-allocation tracing. |
| `capture_u110.py` | Drives real hardware over MIDI and records it. **Its program numbers are TONES, not patches** — see §5.3 of `SYSTEM-DESIGN.md`; earlier revisions mislabelled them and the `listen/hardware/1` capture's log names are wrong. |
| `capture_u110_test.py` | Service-test capture with per-step marks. |

`[I]` **MIDI timing.** `-min` events reach the machine about **10 s** after their file
timestamp — the U-110 ignores MIDI until it has booted. Put notes at least that far in, and
allow for it when aligning renders against hardware captures.

## 4.7b Getting the filter and stereo into the driver `[C]`

The renders so far are **not** what the emulator produces. MAME's device allocates two streams
and writes **identical** data to both, and the filter is applied afterwards in Python;
`tools/render_stereo.py` gets its stereo by running the emulator **six times**, once per part.

**Filter — DONE** `[C]`. Two `FILTER_BIQUAD` (MAME's `opamp_sk_lowpass_setup`, fed the
schematic's component values so it derives fc and Q itself) plus a `FILTER_RC`, chained
`pcm -> bq1 -> bq2 -> rc -> speaker` in `roland_u110.cpp`. Verified against
`tools/u110_output_filter.py`: identical through the audible range (8 kHz: -38 dB both).
Above ~12 kHz the driver rolls off less steeply because its filters run at the device's
**32 kHz** stream rate, where Nyquist is 16 kHz, while the Python reference filtered an
already-48 kHz signal — a resampling artefact at -55 dB, not a filter error.

`[C]` **It does not fix the spectral gap, and my earlier claim that it would was wrong.**
The plan said the emulator ran "24-30 dB hot above 2 kHz" and named the filter as the fix.
Measured against hardware on note 60:

| band | 2520 | 3175 | 4000 | 5040 | 6350 | 8000 | 12699 | 16000 |
|---|---|---|---|---|---|---|---|---|
| emu raw − hw | +24 | +22 | +24 | +28 | +30 | +28 | +30 | +30 |
| emu +LPF − hw | +24 | +22 | +26 | +31 | +33 | +24 | **+11** | **+13** |

The filter does its job above ~8 kHz and does **nothing** at 2.5-6.3 kHz — where it is flat by
design, as service test 8 independently confirmed on hardware. Mean excess above 4 kHz falls
only from +31.5 to +23.2 dB. The dominant error was never the filter.

`[I]` **What it actually is — two measurements.** On a sustained note 60 the hardware decays
**44 dB over 2.4 s** while the emulator falls to -13 dB by 0.8 s and then **flattens**; and the
emulator holds **15-20 dB more relative energy in 2-8 kHz at every instant**, not just late in
the note. So the emulator's piano is both too bright and too sustained. Candidates: the
chip-generated amplitude envelope that MAME does not model at all (the firmware writes volume
once, §4.6), and MAME's loop handling, which its own source calls "probably incorrect".

**Stereo — DONE** `[C]`. The device gained `set_output_count()` (the CM-32P keeps its
historical 2-channel behaviour) and a per-voice **output mask**, bit k enabling Multi Output
k+1 — the same semantics as the BU3905's own slot registers. The driver turns the current
Output Mode into that mask:

1. `out_ctrl_w` reads the mode index from **RAM `0x280E`** — the byte the firmware itself uses
   at `0xB721` to pick its eight routing bytes. Reading the index is version-independent;
   matching the eight written bytes against a table in program ROM would not be, since that
   table sits at `0xA8B6` in v2.03 but `0xA7D6` in v2.00.
2. The mode's group sizes (the 50-row table from OM p.27, generated from the validated
   `tools/output_modes.py` rather than re-typed) partition voices **1-31** into contiguous
   groups; group N gets mask `1 << (N-1)`. In modes 21-50 the first group takes outputs 1+2
   together (mask `0x03`) and later groups start at output 3.
3. The six device outputs are summed to the MIX pair at the measured pan gains, then filtered.

**Verified against the Owner's Manual chart**, rendering straight from the emulator:

| patch | mode | result |
|---|---|---|
| P-04 Wide Piano | 20 (`7,8,4,4,4,4`) | all six assigns land on their output; the four graded pans measure **exactly** their targets (0.0 dB error), hard pans fully hard |
| P-01 Ac.Piano | 22 (`M31`) | every voice masked `0x03` — centred, L/R correlation 1.000 |
| P-05 Double A.P | 8 (`15,16`) | parts split hard L / hard R, correlation **0.27** — a genuine wide double |

`[I]` **Filtering placement.** The hardware has six filter chains, one per Multi Output. They
are identical and the filter is linear, so the driver sums to L/R first and filters the two mix
buses — equivalent, and two chains instead of eighteen. It stops being equivalent only if the
six chains are ever given different characteristics.

`[I]` Modes 21-50 put outputs 1 and 2 on one voice group with the effect switchable. `M`
(centred, dry) is handled; the `L/R` wet pair is treated the same, since a voice's output
cannot be told from its number when two outputs share one pool — and no factory patch uses an
L/R mode, so nothing exercises it yet.

## 4.7c Note-off release `[C]`

**Release — DONE** `[C]`. The device gained a per-voice envelope: `set_env_release_db_per_s()`
starts an exponential fade when the enable bit clears, and holds the voice alive until it drops
below -84 dB instead of cutting it on the same sample. The CM-32P is untouched — the rate
defaults to 0, which leaves the old behaviour bit for bit.

Measured, not guessed. `tools/envelope_measure.py` divides the hardware capture by a **dry
render of the same wave-ROM data**, so the sample's own decay, the multisample choice, the two
partials' mix and the output filter all cancel and only the chip's contribution is left. The
dry render was validated against MAME's own output first: they agree to **0.2 dB** across every
note. Results are in `listen/hardware/2/ENVELOPE.md` with the raw values in `envelope_data.csv`.

| | hardware | emulator now |
|---|---|---|
| release rate | 94-166 dB/s, mean 127 | 130 dB/s |
| curve | exponential, dB-linear R2 **0.92-1.00** | exponential |
| duration | 124-252 ms for a 14-30 dB drop | matches |

`[I]` **Held-note decay is deliberately left at zero.** The same measurement shows hardware
fading a further **3.7-7.8 dB/s** during a held note that the sample data does not account for,
but no register the CPU writes differs between notes that decay at different rates, and the
U-110's sysex exposes only **ENV ATTACK RATE** and **ENV RELEASE RATE** per part
(`0x001n0A`/`0x001n0B`, -7..+7) — there is no decay parameter at all. A fixed rate in the driver
would hide the error rather than fix it. `set_env_decay_db_per_s()` exists for when the source
is found; the tone table is the place to look next.

`[I]` The rate is not constant: it rises with pitch (-5.2 dB/s at note 36, -7.8 at note 84) and
with velocity (-3.7 at v40, -7.6 at v127 on the same note). Envelope rates scaled by note and
velocity are ordinary synth practice, so this is expected rather than anomalous.

`[C]` **Two traps fixed while doing this.** MAME persists CONFIG ports to `cfg/u110.cfg`, so one
run with a service test enabled silently left *every* later run booting into the test menu with
no MIDI notes at all. `tools/u110run.sh` now uses a scratch `-cfg_directory`, both it and the
driver announce the boot mode on every run, and the script warns if the persistent config still
has a test enabled. Separately: note-on lands **0.18-0.28 s after the times in the capture
logs**, which record when the script sent the message, not when the U-110 acted on it.

## 4.8 Where playback stands `[C]`

| | state |
|---|---|
| Boot, panel, LCD, MIDI in, NVRAM, cards | working |
| **Sample format** | **solved** — 8-bit float (§3 of `ROM-ANALYSIS.md`) |
| **Engine rate / pitch** | 32 kHz, A440, ±2.5 cents over five octaves |
| **Voice volume / velocity** | **solved** — 20.1 dB span against hardware's 21.3 |
| **Patch selection** | `tools/select_patch.lua` |
| **Output assign → stereo** | **solved** — offline via `tools/render_stereo.py` |
| **Reconstruction filter** | **in the driver** — two Sallen-Key sections + output RC per mix bus |
| **Stereo in the driver** | **done** — six output buses, panned by Output Assign |
| **Note-off release** | **in the driver** — exponential, 127 dB/s, measured (§4.7c) |
| **Held-note decay** | **not modelled** — real (3.7-7.8 dB/s) but not yet sourced to a register |
| **Effects (chorus/tremolo)** | **not modelled** — every effect patch renders dry |
| Absolute level | uncalibrated; renders normalise on output |

## 5. Phase 3 — LV2

Straightforward once the engine is correct, with one correction to `EMULATOR-PLAN.md` §7.

**Reusing `roland_lp` "as a library" is not really available.** It is welded to
`device_sound_interface`, `device_rom_interface` and MAME's stream scheduler. But the actual
DSP inside it is about 100 lines, and Phase 2 will have rewritten the interesting part
anyway. **Reimplement it standalone using the MAME source as reference** — that is easier
than extracting it, and it is where the corrected decoder lives.

The CPU core is the genuine reuse candidate, and that one is a real extraction job:
`mcs96ops.lst` plus `mcs96make.py` codegen, wrapped in `device_execute_interface`.

Otherwise `EMULATOR-PLAN.md` §5 stands: allocation-free `run()`, CPU stepped on the audio
thread for determinism, ROM and card paths as `state:interface` properties rather than
control ports, resample 34 kHz → host rate with a decent kernel, ship no ROMs.

---

## 6. Sequencing

| # | Item | Gate | Scale |
|---|---|---|---|
| 1 | ~~CPU, memory map, bank switch~~ | **done** | |
| 2 | ~~**LCD + IC8 `LCD INT`**~~ | **done** — the `IOC1`/HSI.0 core fix | |
| 3 | ~~Panel, cards~~ (MIDI host ports outstanding) | **done** | |
| 4 | Register trace | wired into `snd_w`; `-debugscript` + `trace` also available | **done** |
| 5 | Wire the sound device | voices audible, however wrong | days |
| 6 | Tuning question (§4.2) | measured against a known reference note | hours |
| 7 | Decoder repair (§4.3) | correlation score materially above 0.32 | **weeks, open-ended** |
| 8 | Voice freeing | 32-note run without a hang | days |
| 9 | Output routing | six jacks behave per patch byte `+0x0E` | days |
| 10 | LV2 extraction | plugin loads and plays | weeks |

Items 2 and 7 carry essentially all the schedule risk. Everything else is specified.

**Do not start item 7 before item 6.** A pitch error of unknown size will corrupt every
correlation measurement the decoder search depends on.

**And do item 8's voice-enable fix early.** The firmware sets voice enable with 16-bit
writes at `0x1422`/`0x142A`, which the driver splits to device registers `0x11`/`0x12` and
`0x15`/`0x16`. `roland_lp.cpp` only decodes `0x11`/`0x13`/`0x15`/`0x17`, so **voices 8-15
and 24-31 are silently dropped** — two thirds of the polyphony. Its own
`((offset >> 1) & 3)` formula already computes the right group; only the case labels are
missing.

### 6.1 A worthwhile side effect

Items 1-5 also unblock MAME's `u220` skeleton, which uses the same wave engine and whose
program ROMs are already in `roms/`. Contributing `roland_u110.cpp` upstream is the natural
home for these findings, and the U-220 comes nearly free afterwards.

---

## 7. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| IC8 `LCD INT` resists modelling | medium | Service notes p.6 publish IC8's internal block diagram; the handshake is documented there. Fall back to `cm32p`'s ROM-patch hack to unblock items 3-9, but **do not** call Phase 1 done |
| Decoder never exceeds ~0.5 correlation | medium | The reference WAVs are themselves derived; the ceiling may be lower than 1.0. Estimate the ceiling (§4.3 step 2) *before* concluding the decoder is wrong |
| 34 kHz crystal reading is wrong | low | Settled by §4.2 in hours |
| MCS-96 timing gap (§3.1) breaks firmware timing | low | All periodic behaviour is `TIMER1`-clocked, not instruction-counted |
| Chip regs `0x10`-`0x12`, `0x19`-`0x1D` matter | low | Unknown to MAME too. Trace them; if the firmware writes them at init only, they are probably configuration |

---

## 8. Licensing

MAME is BSD-3 and LV2 is permissive; both are compatible with an open plugin.
**Ship no ROM images** — user-supplied dumps only, as Munt does for the MT-32. The reference
WAVs in `waves/` are third-party derived data of unknown provenance and should not be
redistributed either; they are a development oracle, not an asset.
