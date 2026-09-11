# How a U-110 drum tone really works, and what the R-8 says about pitch

Written while converting Roland R-8 cards into U-110 cards
([`R8-CONVERSION.md`](R8-CONVERSION.md)). Two questions drove it:

1. why a converted drum card mounted, loaded and still played on almost no keys; and
2. what the R-8 itself says about the pitch a drum should default to.

The first is answered completely, by disassembly. The second is answered for the U-110 and
only partly for the R-8; §4 says exactly how far it got.

---

## 1. A drum tone does not use the MIDI note

This is the whole thing, and it invalidates the model the earlier conversion work was built
on. A tone whose **type byte** (tone record `+0x0A`) has **bit 7 set** is a drum tone, and
the firmware does **not** compare the incoming note against the tone's key split points. It
looks the note up in a table first.

`0x7BFE`, reached on every note-on:

```asm
7BFE: ldbze 54, 0050        ; part index
7C03: mulub 54, #50
7C06: ldbze 54, 288a[54]    ; the loaded tone's TYPE byte, work RAM 0x2880 + 0x0A + 0x50*part
7C0B: jbs   54, 7, 7c10     ; bit 7 set  -> drum path
7C0E: sjmp  7ca3            ; clear      -> the ordinary path

7C10: ...                   ; DRUM PATH
7C18: shrb  54, #04         ;   type >> 4
7C1B: andb  54, #07         ;   bits 4-6  -> which of eight drum maps
7C1E: add   54, #377b       ;   -> 0x377B + that
7C22: st    54, 00f4        ;   read cursor
7C42: scall 7b07            ;   read one byte: a SAMPLE INDEX
7C49: mulub 54, #0a         ;   index * 10
7C4C: add   54, #0100       ;   -> its sample record at 0x0100 + 10*index
7C55: scall 7b07            ;   byte 0  \
7C5C: scall 7b07            ;   byte 1   >  the map's start address
7C63: scall 7b07            ;   byte 2  /
7C77: ldbze 50, 0051        ;   the MIDI note
7C7C: add   58, 0050        ;   start + note
7C8E: scall 7b07            ;   read ONE byte there
7C9D: ldb   51, 0054        ;   -> and that byte REPLACES the note
7CA2: ret

7CA3: mulub 54, 50, #10     ; ORDINARY PATH: note += part transpose - 0x40
7CA7: ldb   54, 281c[54]
7CAC: subb  54, #40
7CAF: addb  51, 0054
7CB4: ret
```

So for a drum tone:

```
mapped = card[ start_of( TABLE[(type >> 4) & 7] ) + note ]
```

and **everything downstream uses `mapped` in place of the note** — which zone of which
partial is selected, hence which sample, and the pitch. The MIDI note's only job is to
index the map.

`0x80` in the map means **the key is unassigned**, and the voice stays silent. That is why a
factory drum tone speaks on so few keys, and it is not a defect: SN-U110-10's `ROCK DRUMS`
assigns 35 of 128.

## 2. `TABLE`, and the eight drum maps

`TABLE` is the tone record at logical **`0x3760`** — index 126, far past the 99 a program
change can reach — whose ten-byte name field literally reads `"TABLE     "`. Its twelve
partial-1 sample indices are **eight usable entries**, one per tone type `0x80`, `0x90` …
`0xF0`. Each entry names a sample whose **128 bytes are the map**, not audio.

| image | TABLE entries | each 128 bytes? |
|---|---|---|
| `waverom0` (carries tone 99 `DRUMS`) | `d6 d7 d8 d9 da db dc dd` | yes, all eight |
| SN-U110-02 Latin / FX Percussion | `9e 9f a0 a1 a2 a3 a4 a5` | yes, all eight |
| SN-U110-10 Rock Drums | `2a 2b 2c 2a 2a 2a 2a 2a` | yes — three distinct maps |
| the twelve melodic SN-U110 cards | absent | — |

This is why `ROCK DRUMS` carries type `0x80` and `ELEC DRUMS` type `0x90`: they are two kits
on one card, each with its own key map.

`ROCK DRUMS`' map, read out of sample `0x2A`:

```
 note 32:  80 80 80 6c 6b 69 2b 6a 2c 40 2e 32 2f 54 2d 37
 note 48:  5b 14 3c 20 05 80 80 80 80 17 30 24 80 80 80 80
 note 64:  80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 60
 note 80:  80 65 80 80 29 2a 67 68 31 42 2e 48 12 4e 22 80
```

## 3. The default pitch, on the U-110 side

The mapped value **is a note number**, so a kit piece plays at its own stored rate when

> **mapped value == the sample's reference note**

and that is what the factory card does. Across `ROCK DRUMS`' 35 assigned keys, 17 sit
exactly there, and the rest are small deliberate offsets — which is how the U-110 pitches a
drum:

| sample | mapped values | keys | reading |
|---|---|---|---|
| 12 (38127 bytes) | 50, 55, 60 | 43, 47, 50 | one tom, **−5 / 0 / +5** semitones |
| 14 (28304 bytes) | 66, 72, 78 | 89, 91, 93 | one tom, **−4 / +2 / +8** |
| 0 (40698 bytes) | 5, 18, 20, 23 | 52, 92, 49, 57 | the kick at four pitches |
| 16 (23554 bytes) | 91, 96, 101 | 48, 79, 81 | **−5 / 0 / +5** again |

So the tuning convention is explicit: **offset the mapped value from the reference note.**
A tom set is one sample at −5, 0, +5. `tools/r8_to_u110.py` writes `mapped == ref` for every
key, i.e. every drum at its natural pitch, and that is the right default to vary from.

## 4. What the R-8 says about pitch `[I]`

Each R-8 sample block carries `w5`, the playback step (§2.1 of `R8-CONVERSION.md`). Its
distribution over all six cards:

| `w5` | count (block A) |
|---|---|
| `0x0100` | 42 |
| `0x0280` | 22 |
| `0x0180` | 22 |
| `0x0380` | 17 |
| `0x0080` | 7 |
| `0x0F80` | 6 |
| `0x1E00`, `0x2000` | 7 |

**`0x0100` is the base**, and it is the value on *every one* of SN-R8-11's 26 instruments —
a card that is entirely cymbals and metal, i.e. the material that must be stored at full
rate. Cards whose content is lower-bandwidth carry larger values: SN-R8-09 is `0x0280` on
almost everything.

`[I]` **This is a storage-rate field, not a tuning field, and its scale is not established.**
Read as `w5/0x100` the values imply ratios up to 38x, which are not musical intervals. Read
against the U-110's own unity step (`0x4000`, measured) they imply playback 64x too slow.
Neither is credible, so the mapping from `w5` to a rate is still unknown.

`[C]` **The R-8's own variation mechanism is not pitch at all**, at least on these cards.
Instruments that differ only in name share their sample bytes exactly and carry the *same*
`w5` — `SHARP_S`/`BRITE_S`/`SOLID_S`, `GATE_S`/`FUNK_S`, `ROCK_S1`/`SLAM_S`,
`ATAK_T1`/`ATAK_T2`, `CRSH_C6`/`CRSH_C7`/`SPLA_C3`, `RDBL_C2`/`BELL_C2`. What differs is
`w6..w8`, the level and envelope words, which are not decoded. So the R-8 makes "the same
drum, different feel" with envelopes, and the front-panel pitch control is applied live
rather than stored per instrument.

**Consequence for the converter, and a known limitation.** Within one R-8 tone, block A and
block B can carry different `w5` — on SN-R8-10 most instruments are A `0x0180`, B `0x0100`.
The pre-mix in `R8-CONVERSION.md` §5 sums them sample-for-sample, which is only right if
they share a rate. Two things argue they do: the paired lengths are often equal to within a
few bytes (`BOING_S` 9396/9396, `DANCE_S` 10744/10744, `909_CHH` 6532/6532), and equal
lengths at unequal rates would mean layers of unequal duration. But it is not proven, and if
`w5` does scale the rate then every mixed kick and snare on SN-R8-10 has its low layer a
fifth out. Settling it needs the R-8's own firmware, which is §6 of `R8-CONVERSION.md`: the
MK II program ROM is in the set, the original R-8's is not.

## 5. A second hardware rule, found the same day

`[C]` **A sample may not cross a `0x40000` boundary.** The chip's phase accumulator supplies
only address bits 0-17; bit 18 comes from the bank register, written once at voice start
(`ROM-ANALYSIS.md` §6.2), so a sample that straddles the boundary wraps instead of carrying.

Evidence: **zero of the 1172 samples across all fifteen factory SN-U110 cards cross one.**
Our converter placed a 53 KB reverb clap at `0x33632`-`0x40682` and it played as a runaway
ramp, ten times louder than its neighbours and spectrally unrecognisable. The allocator now
pads to the boundary.

## 6. What this fixed

Everything §5.5 of `R8-CONVERSION.md` recorded as unexplained:

- **"the reference note silences some keys, depending on the (sample, ref) pair."** It never
  depended on the pair. With no valid drum map the firmware was reading whatever bytes
  followed our 8-byte silence sample and using them as the note, so which zone and which
  pitch a key produced was arbitrary — and changing a reference note changed which garbage
  value happened to select which zone.
- **"what the firmware does with TABLE is not known."** It is §1.
- **"16 of 23 keys speak, 15 of them distinct."** Now 22 of 22 speak, in order, each at its
  own natural pitch, with silence outside the map.

The method lesson is the one §5.5 already drew, and it repeated: ten fields were cleared by
single-variable testing before a whole-image bisect found the sixteen bytes that mattered,
and then **one xref on the constant those bytes lived at** explained the entire mechanism in
a single screen of disassembly. Disassembling the code that reads the data should be the
first move, not the last — that is corrections #29, #31 and #33 in `ROM-ANALYSIS.md` making
the same point for the fourth time.
