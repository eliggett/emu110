# The R-8 MK II's internal wave ROMs, and why they are not converted

**Status: blocked, not abandoned.** Three megabytes of Roland R-8 MK II sample data that
decode perfectly and cannot yet be used, because nothing found so far says where one sample
ends and the next begins. This records what is known, everything that was tried, and what a
future attempt should do differently — so the dead ends are not walked twice.

Everything here is about the **internal** ROMs. The six SN-R8 **cards** are fully converted;
see [`R8-CONVERSION.md`](R8-CONVERSION.md).

---

## 1. What we have, and it is good

| file | size | descrambles? | 8-bit float delta? |
|---|---|---|---|
| `R8MK2_IC30_wave1.bin` | 1 MB | yes | **98.2 % / 100 %** (lower / upper half) |
| `R8MK2_IC31_wave2.bin` | 1 MB | yes | yes |
| `R8MK2_IC82_wave3.bin` | 1 MB | yes | **100 %** |
| `R8MK2A25/26/27.BIN` | 1 MB each | — | alternative dumps of the same three |

They use **the same address and data permutations as the U-110**, and the same 8-bit float
delta sample encoding (`R8-CONVERSION.md` §1). Treated as 512 KB banks they decode with the
integrated signal staying inside the decoder's own range for 98-100 % of the ROM — as clean
a result as the U-110's own wave ROMs give.

So the audio is in hand. The **directory** is not.

## 2. Why there is no directory in the wave ROM

An SN-R8 card is self-describing: a header at physical 0, a card ID and tone count at
logical `0x0080`, a tone offset table at `0x0090`, and 48-byte tone records carrying the
start and end of every sample (`R8-CONVERSION.md` §2). **The internal ROMs have none of
this.** Their first bytes are sample data:

```
R8_IC30_wave1.bin    0000: 00 00 38 0f 19 46 f3 cb 87 b3 3b 2b 75 8a 5c 85
R8MK2_IC30_wave1.bin 0000: 1c f5 44 65 00 09 00 04 64 35 2a 98 19 89 65 7d
R8MK2_IC82_wave3.bin 0000: 00 00 67 49 5e 08 67 64 d5 e5 83 17 6b 3e 7b 1b
```

That is expected — a machine's internal waves are described by its own firmware, exactly as
the U-110 describes its 99 internal tones from the program ROM rather than from
`waverom0..3`. So the directory must be in `R8MK2_prog.bin` (128 KB, and identical to
`R8 MKII EPROM v1-0-3.bin` for every structure searched below).

## 3. What was searched in the program ROM, and what it found

**The card's own record shape — 8-byte name followed by the `0x8F` marker.** Two hits in
128 KB, neither a table. The internal format is not the card format.

**Name tables.** One real instrument-name table, at **`0x19A6C`**: 26 names of **7** bytes
(the cards use 8), `DBLH_K4`, `RIMSHT2`, `DOOM_T2`, `DOOM_T3`, `CLSD_H6`, `CLSD_H7`,
`OPEN_H6`, `OPEN_H7`, `CRSHC11`, `CHOK_C1`, `SPLA_C1`, `SPLA_C2`, `DRYCLAP`, `OPEN_D1`,
`CABASA2`, `COWBEL3`, `HIGH_CG`, `AGOGO2`, `PLATE1`, `RING1`, `PIPE1`, `WBLOCK2`, `MUT_TRI`,
`THRILLR`, `GUNSHT1`, `SHADOW`. Every other long run of printable text in the ROM is UI
strings (`CARD01`…`CARD26`, `PD1`…`PD16`, note names, `COPY01`…).

26 is a card's worth, and these read like the MK II's *added* sounds rather than its full
internal list — the R-8 has around 68. So either the rest of the names are stored
differently, or this table is something else entirely.

**The records that follow the names.** Immediately after the name table, at `0x19B22`, is a
run of **six-byte** records: `8F 9B 32 08 09 0A`, `8F 9B 32 08 15 0E`, `8F 9C 32 08 32 00`,
`8F 9A 6E 08 32 00`, `FF 0A 00 08 0A 0C`, … They carry the cards' `0x8F` marker and there
are roughly 26 of them, so they pair with the names. **But six bytes cannot hold a sample
address**: a 3 MB space needs 20+ bits even in the cards' 4-byte units, and no two adjacent
bytes in these records ascend across the table. They are parameters, not a directory.

**Ascending-value scans.** A sample directory must hold start addresses, and starts ascend.
Every stride and width was scanned for long monotone runs:

| read as | longest ascending run | at |
|---|---|---|
| LE16, stride 2 | 986 values | `0x18C5A` |
| LE16, stride 4 | 516 | `0x18C00` |
| LE24, stride 3 | 63 | `0x10023` |
| LE24, stride 4 | 516 | `0x18BFF` |

The big one is **not** a directory. From `0x18C00` it reads 128, 128, 129, 130, 130, 131,
132, 133, … — a smooth curve rising by about one per step, i.e. a **lookup table**, almost
certainly the pitch or rate curve (the U-110 has its own at `0xA516`). The stride-4 and
stride-3 "runs" are the same table read at other alignments.

## 4. The directory-free shortcut, and why it fails

Since every sample's deltas sum to zero (`R8-CONVERSION.md` §1), the running accumulator
should return to the same value at every sample boundary — which would segment a ROM with no
directory at all. It is a genuinely attractive idea and it **does not work**.

Validated against SN-R8-10, whose 72 block boundaries are known from its own directory:

- the accumulator is never 0 anywhere in the image, because non-audio bytes early on give it
  a large offset it never loses;
- corrected to "returns to the value it had at a boundary", the most common such value
  accounts for only **19 of 72** boundaries, and the values at the others differ;
- that value occurs at **8487** positions in 512 KB — 118 candidates per real boundary.

The zero-sum property is real and was measured (21 of 26 A blocks sum to exactly zero, the
rest within ±76 of it), but "within ±76" is fatal for segmentation and the property is not
selective in any case. **Do not retry this.**

## 5. What a future attempt should do

In rough order of expected cost:

1. **Disassemble `R8MK2_prog.bin`.** The R-8's CPU is not the U-110's 8x9x, so
   `tools/mcs96dasm.py` does not apply and identifying the architecture is step zero. The
   payoff is certain rather than likely: the firmware must compute a sample address from an
   instrument number, and that arithmetic *is* the directory. This is what finally cracked
   the U-110's drum map (`R8-DRUM-TONES.md`) after ten black-box hypotheses failed, and the
   lesson there applies here.
2. **Run the MK II in an emulator and watch the address lines**, the way
   `U110_TGTRACE` was used to recover what the U-110 reads from a card. MAME has a
   `roland_r8.cpp` driver, but it lists every wave ROM as `NO_DUMP` and the dumps we hold
   are unofficial, so this needs the driver taught about them first.
3. **Correlate against the cards.** Some MK II internal sounds are near-certainly the same
   recordings as card sounds. Cross-correlating the 3 MB against known card samples would
   locate those, giving real boundaries to reverse the addressing from — a way in that needs
   no firmware at all.

## 6. The other anomaly, kept here for company

The **original** (non-MK II) R-8's `R8_IC30_wave1.bin` and `R8_IC31_wave2.bin` do not decode
at all: the integrated signal reaches a median of 7601 against a decoder spanning ±2048, an
order of magnitude out. It is not a wrong high-bit permutation (that would relocate intact
blocks and leave local integration fine) and not absolute-rather-than-delta data (lag-1
autocorrelation is 0.10-0.22 where absolute audio would be ~0.8). MAME lists both as
`NO_DUMP`; these are unofficial dumps of mask ROMs where the MK II uses EPROMs, so a
different pinout is the leading suspect and a bad dump the next. `R8-CONVERSION.md` §6.
