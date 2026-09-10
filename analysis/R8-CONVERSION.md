# Roland R-8 → U-110 wave card conversion

**Goal.** Build U-110 card images out of Roland R-8 sample data, playable in Voltaire 110
and burnable onto a real SN-U110 card.

This document is the *verified* account. It supersedes [`R8-SYSTEM-DESIGN.md`](R8-SYSTEM-DESIGN.md),
which was written by Gemini from a single card and is right about the architecture and
wrong in several specifics; §7 lists what it got right and what it got wrong, because the
places it went wrong are instructive.

Everything below was checked against bytes rather than inferred. Source dumps are in
`~/Downloads/roland_sn-u-stuff_for-elliott/` (outside the repo). Code:
[`tools/roland_card.py`](../tools/roland_card.py) (format primitives),
[`tools/r8_extract.py`](../tools/r8_extract.py) (decode a card to WAVs). Audio:
[`listen/r8-decode/`](../listen/r8-decode/).

---

## 1. The one thing that makes this possible

**R-8 and U-110 PCM data are the same format on the same wiring.** Not similar — identical.

The dumps ship a `.bin` (as read off the chip) beside a `.opn`/`.ope` (descrambled) for
most images, which makes this checkable in one line. Using the `ADDR_ORDER` and
`DATA_ORDER` this project derived from **U-110 firmware alone**
([`ROM-ANALYSIS.md` §6.4](ROM-ANALYSIS.md)):

| image | `descramble(bin) == opn` |
|---|---|
| all six SN-R8 cards | **yes**, byte for byte |
| SN-SPLA card (D-70) | **yes** |
| `d70_waverom-d/e/f` | **yes** |
| SN-U110-10 (control) | **yes** |

`scramble()` is its exact inverse, verified by round-trip — which is what lets us *write*
a card and not only read one.

And the sample encoding is the same 8-bit float delta:

| image | integral stays in ±4096 | notes |
|---|---|---|
| U-110 `waverom0` (control) | 97.3 % | known-good delta data |
| SN-R8-10 card | 97.7 % | |
| R-8 **MK II** wave ROMs (all 3 MB) | 98.2–100 % | |
| D-70 wave ROMs and SN-SPLA | 97.7–98.2 % | |
| R-8 (original) `IC30`/`IC31` | 34.8 % / 43.6 % | **anomaly, §6** |

Two independent properties confirm *delta* rather than absolute levels, on the R-8 exactly
as on the U-110: over each sample the decoded values **sum to zero** (21 of 26 A blocks on
the Dance card sum to exactly 0, the rest to within ±76 of it), and the running total stays
inside the decoder's own ±2048 range. Absolute data would do neither.

**Consequence: sample bytes are copied verbatim.** No transcoding, no requantisation, no
loss. That is the whole reason this project is cheap.

Audible confirmation ([`listen/r8-decode/`](../listen/r8-decode/)), and it is the good kind
— the decode predicts things about a TR-909 that are true:

| tone | block A peak | block B peak | reading |
|---|---|---|---|
| `909_S` | 5346 Hz | 132 Hz | snare = noise + shell tone |
| `909_T` | 1176 Hz | 68 Hz | tom = attack + low body |
| `909_CHH` | 5688 Hz | 6099 Hz | hi-hat, noise in both |
| `303BASS` | 185 Hz | 185 Hz | one TB-303 note, ≈F#3 |

---

## 2. The SN-R8 card format

Header at **physical** offset 0, linear and unscrambled (same reason as the U-110's:
the firmware reads it through compensation tables, `ROM-ANALYSIS.md` §6.4):

```
0000  52 6F 6C 61 6E 64 52 2D 38 20 20 20 4E B1 53 AC   "RolandR-8   N" B1 53 AC
0010  53 4E 2D 52 38 2D 31 30 53 77 61 6E 67 69 6E 21   "SN-R8-10" "Swangin!"
0020  FF ...                                            padding
```

The 16-byte magic is the U-110's template with the model string swapped, which is exactly
why a real U-110 rejects the card: the check at `0x6EF2` compares all 16 bytes against
`"RolandU-110 N\xb1S\xac"` and shows `"  Illegal CARD"`.

The `+0x10` field is **two** 8-byte fields: the part number, then a name that is not the
catalogue title — `"Swangin!"`, `"WALKON"`, `"AVATAR"`, `"COCONUT"`, `"SUMMIT*A"`,
`"AsianGod"`. Demo-pattern names, most likely.

Then in **logical** space:

| logical | field |
|---|---|
| `0x0080` | card ID — the catalogue number **minus one**: SN-R8-10 carries `0x09` |
| `0x0081` | tone count — `0x1A` (26) on all six cards |
| `0x0090` | tone offset table, `count` LE words, relative to `0x0100` |
| `0x0100 + off` | 48-byte tone records |

The offset table is uniform stride 48 on every card, so it is redundant — but it exists, so
read it rather than assuming. (U-110 cards carry the ID *as* the catalogue number, `0x0A`
for SN-U110-10. The off-by-one is a real difference between the two formats.)

### 2.1 The 48-byte tone record

```
+0x00   8   name, NUL-padded ASCII        e.g. "909_K  \0"
+0x08   4   header/flags                  +0x08 is 0x8F on every tone on every card
+0x0C  18   sample block A
+0x1E  18   sample block B
```

A sample block is nine LE words, close to a register image for the MB87419 voice that will
play it — which is why the field meanings can be read straight off our own chip emulation
([`roland_lp.cpp`](../mame/src/devices/sound/roland_lp.cpp)):

| word | chip reg | meaning |
|---|---|---|
| `w0` | 02/03 | bank + mode. **Bit 10 (`0x0400`) is wave address bit 18** |
| `w1` | 0A/0B | start address, in **4-byte units** |
| `w2` | 08/09 | start fraction — `0x4000` on every block on every card |
| `w3` | 0C/0D | **end** address, in 4-byte units |
| `w4` | 0E/0F | loop address — sits *past* the end on every block, so **nothing loops** |
| `w5` | 04/05 | playback step |
| `w6..w8` | — | level / envelope; all zero on blocks the card leaves unused |

Bit 10 of `w0` being address bit 18 is not a guess: on the Dance card tone 13's A block
ends at `0x3EDD0` and its B block, the first with `w0 = 0x0400`, starts at logical
`0x40000` and runs to `0x41984` — byte-continuous across the boundary. It matches the
U-110's own bank selector, where bit 10 of the value written to `0x1404` supplies wave
address bit 18 (`ROM-ANALYSIS.md` §6.2).

### 2.2 Every R-8 tone has up to two samples, and the machine sounds both

This is the structural fact that decides the whole conversion, so it was tested three ways.

**Coverage.** If only block A were real audio, A would tile the card's sample area. It does
not:

| card | A covers | A+B covers |
|---|---|---|
| SN-R8-04 | 70.3 % | 94.9 % |
| SN-R8-06 | 89.5 % | 97.1 % |
| SN-R8-07 | 54.6 % | 90.8 % |
| SN-R8-09 | 83.8 % | 94.7 % |
| SN-R8-10 | 57.1 % | 93.4 % |
| SN-R8-11 | 56.2 % | 83.0 % |

Only when both blocks count does the sample area add up. On the Dance card the two tile
each other exactly — tone 0's B block begins at `0x37E0`, which is where its A block ends.

**Spectrum.** The two blocks split by frequency band in the way a drum synthesiser does —
see the 909 table in §1. A snare's noise and its shell tone are in different blocks.

**Ear.** `listen/r8-decode/sn-r8-10-dance/04_909_S*.wav`: neither layer alone is a snare
drum; the sum is.

So an R-8 tone maps naturally onto a **U-110 tone's two partials**, which is a lucky fit
and the basis of the plan in §5.

### 2.3 Open: which B blocks are live, and are they layered or velocity-switched

Some B blocks are placeholders — they carry `w6..w8 == 0`, a stub length (688 or 152
bytes), and an address pointing into territory another tone's A block already owns. Nine of
the Dance card's 26 are like this, all of them percussion (`RVB_CLP`, `78_COW`, `78_TAMB`,
`55CLAVE` …) where a low layer would make no sense.

**Two candidate discriminators were tested and neither is reliable:**

- `w6..w8 != 0` vs bit 7 of the record's `+0x0B` byte: **132/156** agree.
- A geometric "does B overlap any A span, and is its length a stub size" test vs the same
  bit: **136/156** agree.

They also disagree with each other. So the live/dead rule is **not settled**, and the
converter should decide per block from the geometry (which is checkable) rather than from a
flag byte (which is not yet understood).

A further guess was tested **and refuted**: bit 6 of `+0x0B` does *not* separate "layered"
from "velocity-switched" pairs. Median |correlation| between A and B is 0.023 with the bit
set and 0.032 with it clear — no separation at all. The A/B correlations *are* bimodal
(`909_CHH` 0.80, `303BASS` 0.91, most others under 0.1), so both relationships probably
exist; what selects them is unknown. The R-8 MK II program ROM (`R8MK2_prog.bin`) contains
the card-tone loader and would settle it.

---

## 3. The U-110 target format, corrected

Writing a card needs more of the U-110 format than reading names did, and pinning it down
turned up **an error in [`ROM-ANALYSIS.md` §6.6](ROM-ANALYSIS.md)**: a tone record's
partial holds **11 key split points and 12 sample indices**, not the "8 split points, 9
sample zones" recorded there. Corrected in place; the reasoning is in that file's
corrections log.

```
80-byte TONE RECORD at logical 0x1000 + 0x50*n
  +0x00  10   name, space-padded
  +0x0A   6   header: tone type and flags
  +0x10  32   partial 1
  +0x30  32   partial 2

32-byte PARTIAL
  +0x00  11   key split points, ascending; 0xFF = unused
  +0x0B  12   sample index per zone
  +0x17   9   level / envelope parameters
```

11 + 12 + 9 = 32 exactly, and 12 zones per partial × 2 partials = 24 sample slots per part
— which is the `0xF0` (240 = 24 × 10) bytes per part the firmware reserves at work RAM
`0x2A60`, and the `x12` loop at `0x8179`. The independent third-party `*_list.log` decodes
shipped with the dumps lay the record out the same way.

**Zone selection**, from the firmware's own routine: walk the splits and take the first one
the note does not exceed; past every split, take the twelfth sample.

### 3.1 The 10-byte sample record at `0x0100`

Field meanings are as `tools/export_sample.py` already had them, plus two that the
`*_list.log` header notes name:

| byte | field |
|---|---|
| 0-1 | start address, low 16 bits (bytes) |
| 2 | bits 0-2 start bits 16-18; **bit 3 = "this is a card"**; bits 4-5 bank/slot; bits 6-7 loop mode |
| 3-4 | length − 1, in bytes |
| 5-6 | loop length, counted back from the end |
| 7 | fine tune, `0x40` = centre |
| 8 | **reference note** — the MIDI note at which the sample plays at its stored rate |
| 9 | per-sample level trim, `0x40` = nominal |

Loop mode is `0 = forward, 1 = off, 2 = ping-pong` (the log states it; §3a of
`ROM-ANALYSIS.md` describes the ping-pong welding). Bits 3-5 together are what the log calls
the "segment", and its note *"waverom odd segments are cards"* explains bit 3: a card
sample carries **segment 1**.

### 3.2 How a real U-110 drum tone is built — the idiom to copy

`SN-U110-10 Rock Drums` has exactly two tones, and `ROCK DRUMS` is the template:

```
name   "ROCK DRUMS"   header 80 00 40 00 00 00
P1 splits   19 28 29 2A 2B 2C 2D 2E 2F 30 31        (MIDI 25,40,41..49)
P1 samples  00 01 02 03 04 05 06 07 08 09 0A 0B
P1 params   7F 7F 7F 6D 00 55 7F 50 A0
P2 splits   31 3D 41 4E 5A 66 67 68 69 6A 6B        (MIDI 49,61,65,78,90,102..107)
P2 samples  0B 0C 0D 0E 0F 10 11 12 13 14 15 16
P2 params   7F 7F 7F 7F 00 4C 7F 4C 20
```

Three things to copy:

1. **The two partials are used as a keyboard split**, not a layer — P1 covers up to note
   49, P2 from 49 up. That is how the tone reaches 22 playable zones.
2. **Sample `0x0B` is silence** — 8 bytes long, reference note `0x7E` (126). It is the
   *last* entry of P1 and the *first* of P2, so each partial mutes itself over the other's
   range. This is the mechanism that makes the split work, and the converter needs its own
   silent sample for the same job.
3. **Reference note = the key.** Samples `0x02`..`0x09` carry refs 41,42,43,44,45,46,47,48
   against splits 41..48 — one sample per key at its natural pitch, exactly what a drum map
   wants. (Sample `0x0A` refs 48 on key 49, off by a semitone; and the two wide low zones
   use refs 18 and 35, so they *are* transposed.)

### 3.3 Card layout constraints, measured

Every one of the fifteen SN-U110 cards puts its first sample byte at **`0x4001`–`0x4004`**.
The full 99-record tone table (`0x1000`..`0x3EF0`) is reserved whether the card uses it or
not — Rock Drums declares 2 tones and still starts data at `0x4003`. So:

- sample table `0x0100`..`0x0FFF` → **384 records** maximum
- tone table `0x1000`..`0x3EFF` → 99 records
- sample data `0x4000`..`0x7FFFF` → **507,904 bytes**

Tone count is not stored anywhere. The firmware scans until a record whose name is all
spaces (`ROM-ANALYSIS.md` §6.6), so the converter must blank the records after the last
real one. Rock Drums' blank records read: name all `0x20`, header `00 00 40 00 02 00`.

**Writing the header is the one ordering trap.** The 48-byte ID header sits at *physical*
0, but everything else is authored in *logical* space and then scrambled. Physical
`0x00`–`0x2F` maps onto the logical lattice `{0x00-0x05, 0x10-0x15, … 0x70-0x75}` — all of
it below `0x76`, hence clear of the sample table at `0x100`. So: author the logical image,
`scramble()` it, then overwrite physical `0x00`–`0x2F` with the plain header. Leave logical
`0x00`–`0x7F` unused and nothing is lost.

---

## 4. What the R-8 gives up in the move

The U-110 is a keyboard sampler and the R-8 is a drum machine, so some things do not cross:

- **Pitch per pad.** The R-8 tunes each instrument; the U-110's drum tone has one reference
  note per sample and no per-key offset beyond choosing that note. Recoverable: the R-8
  block's `w5` step can be folded into the U-110 sample's reference note and fine-tune
  bytes at conversion time.
- **The R-8's own envelopes** (`w6..w8`). The U-110 runs its envelope in firmware from the
  tone record's partial parameters, so R-8 envelope data cannot transfer directly. Phase 1
  copies known-good parameters from `ROCK DRUMS`.
- **Nuance/velocity behaviour**, which depends on the unresolved §2.3 question.
- **Nothing loops** on these cards, so loop handling is a non-issue — every sample is
  one-shot, loop mode `1`.

---

## 5. The conversion plan

### Phase 1 — built, and it plays.  `tools/r8_to_u110.py`

**Not** the layered design this section first proposed. That was: partial 1 takes the A
blocks, partial 2 the B blocks, identical splits in both so note *k* selects the same zone
in each and the two sound together. It is a nice idea and **the machine will not play it**;
what it produced is in §6. What works is to copy `ROCK DRUMS` literally (§3.2):

- the two partials are a **key split**, not a layer — partial 1 takes 11 keys and partial 2
  the next 11, each muting itself over the other's range with a shared silent sample;
- sample index runs are **consecutive** — 0..11 in partial 1, 11..22 in partial 2;
- partial 2's first split equals partial 1's last;
- the card carries the **`"TABLE"` record at logical `0x3760`**, without which a drum tone
  is silent on almost every key (`ROM-ANALYSIS.md` §6.6). Its zones point at the silence
  sample so it satisfies the firmware and contributes nothing.

22 keys per tone, block A only, sample bytes copied verbatim — no re-encoding, so what
plays is what the R-8 played. A 26-instrument card becomes 2 tones. Reference notes are
taken from `ROCK DRUMS`' own table rather than computed, for the reason in §6.

Loop mode `1`, loop length 4, segment `1`, byte 7 `0x40`, byte 9 (the fine tune) `0x40`,
partial parameters and the tone type byte `0x80` all copied from `ROCK DRUMS`.

**Measured, on the emulator running the real firmware** (`plugin/tools/card_check.cpp`, and
`listen/r8-decode/via-card/`): all six SN-R8 cards convert; each mounts, is accepted, and
its tones read back and play. SN-R8-10 speaks on 17 of 23 keys, SN-R8-09 on 11 of 23. Every
key that speaks plays its own sample at its own level. The remaining gaps are §6.

One card overflows: an R-8 card can hold more sample data than a U-110 card has room for,
because the U-110 reserves its first 16 KB for tables and each sample costs one extra
interpolation byte. SN-R8-09 needs 550 KB of the 496 KB available, so the converter drops
from the end and says which — two toms, leaving 24 of 26 instruments and 99.2% full.

### Phase 2 — pre-mix, to get block B back

Phase 1 carries block A only, so every two-layer R-8 sound loses its other half: a 909 kick
keeps its click and loses its boom, a snare keeps its noise and loses its shell tone (§2.2).
That is the single biggest audible gap, and 17 of SN-R8-10's 26 instruments are affected.

The fix is to sum A and B into one sample at conversion time, which fits phase 1's layout
unchanged — one sample per key — at the cost of a fixed layer balance. It also *saves*
space: one stream of `max(len A, len B)` replaces two.

This needs a **re-encoder**, the only real signal processing in the project: integrate both
blocks, sum, re-differentiate, and requantise each delta to an 8-bit float code. Because the
player integrates, quantisation error accumulates — so choose each code greedily against
the *running reconstruction* rather than the instantaneous delta (a DPCM encoder with error
feedback). Must be checked by measuring drift over the whole sample, not by eye.

(Space is not the constraint it was: phase 1 uses 56% of SN-R8-10's card, and pre-mixing
would drop that further.)

### Phase 3 — the rest

- **Per-key tuning** from `w5`, as above.
- **The R-8 MK II internal ROMs**: 3 MB that decode perfectly (§1) but whose sample
  directory is not in the wave ROM. It is in `R8MK2_prog.bin` — there are 6-byte records of
  shape `8F xx xx 08` around `0x19B1A` that share the cards' `0x8F` marker and are the
  obvious place to start.
- **§2.3**, from the MK II card-tone loader.
- **Card ID choice.** The part record's group selector is 5 bits (`ROM-ANALYSIS.md` §6.7),
  so IDs up to 31 exist while the catalogue only reaches ~15. A converted card should claim
  an unused ID rather than shadow a real one — but that the firmware accepts an ID above 15
  is an assumption, and is worth one test in the emulator before relying on it.

---

## 5.5 What phase 1 cost, and the two things still unexplained

Both are recorded because the way they were found is the useful part.

### The `"TABLE"` record — solved, mechanism unknown

A card built from scratch mounted, was accepted, had its tone record loaded into work RAM
correctly and its 24 sample slots built correctly by the firmware — and produced **no
voice-enable write to the chip at all** on almost every key. Every field was cleared one at
a time against a card known to play: start address (relocated to a dozen addresses, all
fine), length, loop mode, loop length, bytes 7 and 9, the reference note, the tone record,
the card ID, the fill value, the split layout. All fine. The fault was not in any of them.

What found it was giving up on reasoning and **bisecting the 512 KB image** against the
working card, byte range by byte range, until the difference was sixteen bytes at logical
`0x3770` — partial 1 of a tone record at index 126 whose name reads `"TABLE"`. Supplying it
made every key speak. `ROM-ANALYSIS.md` §6.6 has the structure and the four images that
carry it; what the firmware does with it is still open.

**The lesson is about method, not about the U-110.** Nine of the ten hypotheses tested
before the bisect were about fields I had authored, because those were the things I knew I
had guessed at. The defect was in a field I did not know existed, and no amount of
single-variable testing over the fields I *had* written could reach it. A whole-image
bisect against a known-good example costs about twenty runs and finds anything; it should
have been the second move, not the twentieth.

### The reference note — worked around, not understood

`[I]` Setting a sample's reference note to the key it sits on — the obvious thing, and
right for a melodic multisample — makes the firmware refuse to start a voice on *some*
keys. Which keys depends on the **(sample, reference note) pair** and not on the value:
reference note 48 works on one sample and is silent on another, and a constant 60 across
the board fails on exactly the same keys as key-tracking does. The step register the
firmware computes for a converted card also comes out about 4.2x lower than a real card's
for the same nominal `note == ref`, which is unexplained and may be the same puzzle.

The converter therefore reuses `ROCK DRUMS`' own 23 reference notes verbatim. That is
unsatisfying and it is also cheap: a drum map is not played as a melody, so the absolute
tuning of a kit piece carries no information. It does mean per-pad tuning (§4) waits until
this is understood.

---

## 6. Anomaly: the original R-8's own wave ROMs do not decode

`R8_IC30_wave1.bin` and `R8_IC31_wave2.bin` (512 KB each, the non-MK II machine) are the
only images in the whole set that do not read as U-110-format data. Under the permutation
that works everywhere else, the integrated signal reaches a **median of 7601** and a 90th
percentile of 20454, against a decoder that spans ±2048 — an order of magnitude out, so
this is real and not a threshold artefact.

It is not a wrong high-bit permutation (that would relocate intact blocks and leave local
integration fine) and it is not absolute-rather-than-delta data (lag-1 autocorrelation is
0.10–0.22 in the failing regions, where absolute audio would be ~0.8). MAME lists both
chips as `NO_DUMP`, so these are unofficial dumps of mask ROMs (`MN234000`) where the MK II
uses EPROMs — a different pinout is the leading suspect, a bad dump the next.

**Not on the critical path.** The MK II's 3 MB and all six cards decode perfectly, and the
original R-8's tone directory would need its program ROM, which is not in the set.

---

## 7. Scorecard for `R8-SYSTEM-DESIGN.md`

Kept because the errors are the useful part — every one of them comes from reasoning about
a format instead of checking it.

**Right, and worth having had early:** the shared MB87419/MB87420 silicon; both permutations
being bit-for-bit identical to the U-110's; the sample encoding being the same 8-bit
companded float; the header template and its model-string swap; the card ID being the
catalogue number minus one; the offset table at `0x0090` and count at `0x0081`; 26 tones;
48-byte records with 8-char names; the `+0x0C`/`+0x1E` split into two 18-byte blocks and
their field offsets; that a U-110 cannot mount the card and why; and that conversion is
possible because the sample data is portable. That is a lot to get right.

**Wrong:**

1. **The sample addresses in its tone table are off.** It lists `909_K` at
   `0x03000`–`0x037E4` (nearly right) but `VIDEO_K` at `0x09800` where the field says
   `0x09B68`, and `909_CHH` at `0x3D400` against `0x3D44C`. The pattern is rounded or
   partially-masked arithmetic, and it matters: a converter built on those numbers would cut
   every sample in the wrong place.
2. **It missed that `w0` is a live bank field.** Called "padding"/"bank & loop flags"
   without noticing bit 10 is address bit 18 — which is why its upper-ROM addresses drift.
3. **It labelled `w3`/`w4` as loop-then-end.** They are end-then-loop, and the loop word
   sits past the end on every block on every card, so nothing loops.
4. **"No multisample splits ... no keyboard splits"** for the R-8 is true of the R-8 but
   was used to argue the formats are structurally incompatible. They are not: the R-8's two
   blocks land on the U-110's two partials almost exactly (§2.2).
5. **It reported the U-110 record as "8 key split points, 9 sample zones"** — inherited from
   the error in `ROM-ANALYSIS.md` §6.6, now fixed in both. 11 and 12.
6. **It described the R-8 tone as one sample plus an alternate**, missing that A and B are
   summed and split by frequency band, which is the single most useful fact about the format.
7. **"The tone directory base is at logical `0x0100` vs the U-110's `0x1000`"** is presented
   as an incompatibility. Both are true, but the U-110 also has a table at `0x0100` (its
   sample records), so the difference is what the tables *hold*, not where they sit.

**Unverified in it and still unverified here:** the `+0x08`–`+0x0B` header bytes beyond
`0x8F` being constant, and the `w6..w8` envelope semantics.
