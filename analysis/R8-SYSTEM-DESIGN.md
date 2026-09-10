# Roland R-8 Wave ROM / PCM Card Architecture

Subject file:
- `SN-R8-10_Dance.bin` (524,288 bytes / 512 KB, Roland R-8 / R-8M Sound ROM Card SN-R8-10 "Dance", label: `"Swangin!"`)

This document compares the wave ROM and tone record architecture of the **Roland R-8 / R-8M Human Rhythm Composer** (specifically PCM card `SN-R8-10`) against the **Roland U-110** analyzed in [`ROM-ANALYSIS.md`](file:///home/eliggett/Documents/projects/emu110/analysis/ROM-ANALYSIS.md) and [`SYSTEM-DESIGN.md`](file:///home/eliggett/Documents/projects/emu110/analysis/SYSTEM-DESIGN.md).

---

## 1. Executive Summary

| Layer | Roland U-110 (`SN-U110`) | Roland R-8 (`SN-R8`) | Relationship |
|---|---|---|---|
| **Sound Generator Silicon** | Fujitsu MB87419 + MB87420 | Fujitsu MB87419 + MB87420 | **Identical** custom chip family |
| **PCM Bus Line Scrambling** | 19-bit address, 8-bit data | 19-bit address, 8-bit data | **Bit-for-bit identical** |
| **Sample Audio Format** | 8-bit companded float (sign + 3 exp + 4 mantissa) | 8-bit companded float (sign + 3 exp + 4 mantissa) | **Identical** format |
| **Card Header Signature** | `"RolandU-110 N"` + `B1 53 AC` | `"RolandR-8   N"` + `B1 53 AC` | **Same template**, different model string |
| **Tone Directory Base** | Logical `0x1000` | Logical `0x0100` (offset table at `0x0090`) | **Different** location & mechanism |
| **Tone Record Size** | **80 bytes** (`0x50`) | **48 bytes** (`0x30`) | **Incompatible** record structure |
| **Tone Name Length** | 10 characters ASCII | 8 characters ASCII | Different field width |
| **Sample Referencing** | Indirect (points to Sample Table at `0x0100`) | Direct (MB87419 register values embedded in tone record) | **Fundamentally different** |
| **Multisample Splits** | Yes (8 key split points, 9 sample zones per partial) | No (drum hit architecture; no keyboard splits) | U-110 = keyboard synth, R-8 = drum machine |

While the **underlying hardware bus, silicon interface, and PCM sample encoding are identical**, the **tone records and table structures are not directly compatible**. A Roland U-110 cannot mount or read an SN-R8 card without translation because:
1. The 16-byte card mount signature check in U-110 firmware fails (`"R-8   "` vs `"U-110 "`), causing `"  Illegal CARD"`.
2. The U-110 tone loader expects 80-byte records at `0x1000` referencing a 10-byte sample table at `0x0100`, whereas the R-8 stores 48-byte tone records at `0x0100` that embed tone generator registers directly and omit the sample table.

---

## 2. Shared Hardware Foundation

Both the Roland U-110 (and its siblings U-20, U-220, CM-32P) and the Roland R-8 / R-8M rhythm composer are built around the **Fujitsu MB87419 / MB87420** PCM sound generator chipset.

### 2.1 Hardware Bus Scrambling (Confirmed Identical)

The PCM card slot in the R-8 connects to the MB87419/MB87420 using the exact same physical trace routing as the U-110 internal wave ROMs and `SN-U110` expansion cards:

- **19-bit Address Scrambling**:
  ```
  bitswap<19>(offset, 18, 17, 15, 14, 16, 12, 11, 7, 9, 13, 10, 8, 3, 2, 1, 6, 4, 5, 0)
  ```
- **8-bit Data Bus Scrambling**:
  ```
  bitswap<8>(data, 1, 2, 7, 3, 5, 0, 4, 6)
  ```

When decoded with these permutations, all internal headers, tone tables, ASCII strings, and sample pointers in `SN-R8-10_Dance.bin` align into clean binary and text structures.

### 2.2 PCM Sample Encoding

Sample waveforms on `SN-R8-10` use the identical **8-bit floating-point** companded format documented in [`ROM-ANALYSIS.md` §3](file:///home/eliggett/Documents/projects/emu110/analysis/ROM-ANALYSIS.md#L562-L589):
- Bit 7: Sign (applied to the two's complement magnitude)
- Bits 4-6: 3-bit exponent (shift count)
- Bits 0-3: 4-bit mantissa
- Expansion range: $\pm 1984$

---

## 3. Card Header and Identification

### 3.1 Raw Card Header (Offset `0x0000`)

In raw (un-descrambled) byte space at file offset `0x0000`:

```
0000: 52 6F 6C 61 6E 64 52 2D 38 20 20 20 4E B1 53 AC  "RolandR-8   N" + B1 53 AC
0010: 53 4E 2D 52 38 2D 31 30 53 77 61 6E 67 69 6E 21  "SN-R8-10Swangin!"
0020: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF  (Padding)
```

Compare with the U-110 card header ([`ROM-ANALYSIS.md` §6.5](file:///home/eliggett/Documents/projects/emu110/analysis/ROM-ANALYSIS.md#L1060-L1082)):
```
0000: 52 6F 6C 61 6E 64 55 2D 31 31 30 20 4E B1 53 AC  "RolandU-110 N" + B1 53 AC
0010: 53 4E 2D 55 31 31 30 2D 30 38 20 20 20 20 20 20  "SN-U110-08      "
0020: 08 ...                                           (Card ID at +0x20)
```

- Both use the 16-byte magic preamble: `Roland<Model: 6 bytes> N\xb1\x53\xac`.
- Both have a 16-byte ASCII sound set label at `+0x10`.
- The U-110 firmware strictly verifies the first 16 bytes against `0x93A2` (`"RolandU-110 N\xb1S\xac"`). An unmodified U-110 will reject the R-8 card at mount time with `"  Illegal CARD"`.

### 3.2 Logical Card Header (Descrambled Space)

In descrambled address space:
- `0x0080`: **Card ID**: `0x09` (9, representing SN-R8-10 since index 0 is SN-R8-01).
- `0x0081`: **Tone Count**: `0x1A` (26 tones).
- `0x0082-0x008F`: Padding (`0xFF`).
- `0x0090-0x00C3`: **Tone Offset Table**: 26 entries (2 bytes each, little-endian), storing byte offsets relative to `0x0100` where each tone record begins.

---

## 4. Tone Record Comparison

### 4.1 Structural Differences

| Field | Roland U-110 Tone Record | Roland R-8 Tone Record |
|---|---|---|
| **Location** | `0x1000 + 0x50*n` (fixed stride) | `0x0100 + offset[n]` (from offset table at `0x0090`) |
| **Total Size** | **80 bytes** (`0x50`) | **48 bytes** (`0x30`) |
| **Tone Name** | 10 bytes ASCII (`+0x00`..`+0x09`) | 8 bytes ASCII (`+0x00`..`+0x07`) |
| **Control Bytes** | 6 header bytes (`+0x0A`..`+0x0F`): tone type (single/dual/detune/v-mix/v-sw), flags | 4 bytes (`+0x08`..`+0x0B`): flags, pitch/playback frequency |
| **Multisampling** | **Yes**: 8 keyboard split points + 9 sample indices per partial | **None**: direct drum sample triggers |
| **Sample Links** | Points to 10-byte records in external **Sample Table at `0x0100`** | Embeds MB87419 register parameters directly |
| **Voice Layers** | Partial 1 (32 bytes) + Partial 2 (32 bytes) | Sample A (18 bytes) + Sample B (18 bytes) |

### 4.2 R-8 Tone Record Layout (48 Bytes)

Each 48-byte record in the R-8 tone list at `0x0100` is laid out as follows:

| Offset | Size | Description | MB87419 Register Equiv. |
|---|---|---|---|
| `+0x00` | 8 B | ASCII Tone Name (padded with spaces/NUL) | — |
| `+0x08` | 1 B | Marker / format flag (typically `0x8F`) | — |
| `+0x09` | 1 B | Parameter flag | — |
| `+0x0A` | 1 B | Playback Frequency / Tuning (`0xE0` = 44.1 kHz, `0x6C` = bass note, etc.) | Pitch / Step |
| `+0x0B` | 1 B | Parameter | — |
| **Sample A** | | | |
| `+0x0C` | 2 B | Sample A Bank & Loop Mode flags | Reg `0x02` / `0x03` |
| `+0x0E` | 2 B | Sample A Start Address High Word (bits 2..17) | Reg `0x0A` / `0x0B` |
| `+0x10` | 2 B | Sample A Start Address Fraction (2.14 fixed point) | Reg `0x08` / `0x09` |
| `+0x12` | 2 B | Sample A Loop Address High Word | Reg `0x0E` / `0x0F` |
| `+0x14` | 2 B | Sample A End Address High Word | Reg `0x0C` / `0x0D` |
| `+0x16` | 8 B | Sample A Envelope / dynamic parameters | Envelope rates / levels |
| **Sample B** | | | |
| `+0x1E` | 2 B | Sample B Bank & Loop Mode flags | Reg `0x02` / `0x03` |
| `+0x20` | 2 B | Sample B Start Address High Word | Reg `0x0A` / `0x0B` |
| `+0x22` | 2 B | Sample B Start Address Fraction | Reg `0x08` / `0x09` |
| `+0x24` | 2 B | Sample B Loop Address High Word | Reg `0x0E` / `0x0F` |
| `+0x26` | 2 B | Sample B End Address High Word | Reg `0x0C` / `0x0D` |
| `+0x28` | 8 B | Sample B Envelope / dynamic parameters | Envelope rates / levels |

Unlike the U-110 where the firmware CPU resolves tone records through a sample table and performs envelope generation via `EXTINT` software state machines, the R-8 tone records directly pre-package the exact register images consumed by the MB87419 sound chip.

---

## 5. Tone List: `SN-R8-10_Dance.bin`

Decoded from `SN-R8-10_Dance.bin`, the card contains **26 tones** (offset `0x0081` = `0x1A`):

| Tone # | Offset | Name | Freq | Sample A Start (Byte) | Sample A End (Byte) | Description |
|---|---|---|---|---|---|---|
| 1 | `0x0100` | `909_K   ` | `0xE0` | `0x03000` | `0x037E4` | Roland TR-909 Bass Drum |
| 2 | `0x0130` | `78_K    ` | `0xE0` | `0x03B80` | `0x03DC4` | Roland CR-78 Bass Drum |
| 3 | `0x0160` | `BOING_K ` | `0xE0` | `0x04B80` | `0x06BC4` | Electronic Boing Kick |
| 4 | `0x0190` | `VIDEO_K ` | `0xE0` | `0x09800` | `0x10100` | Video Game Kick |
| 5 | `0x01C0` | `909_S   ` | `0xE0` | `0x11E00` | `0x14D40` | Roland TR-909 Snare Drum |
| 6 | `0x01F0` | `78_S    ` | `0xE0` | `0x17D00` | `0x18CD0` | Roland CR-78 Snare Drum |
| 7 | `0x0220` | `BOING_S ` | `0xE0` | `0x19800` | `0x1C440` | Electronic Boing Snare |
| 8 | `0x0250` | `VIDEO_S ` | `0xE0` | `0x1E600` | `0x20340` | Video Game Snare |
| 9 | `0x0280` | `DANCE_S ` | `0xE0` | `0x22300` | `0x24740` | Dance Snare |
| 10 | `0x02B0` | `HOUSE_S ` | `0xE0` | `0x27700` | `0x2BB80` | House Snare |
| 11 | `0x02E0` | `TRASH_S ` | `0xE0` | `0x31800` | `0x35640` | Trash Snare |
| 12 | `0x0310` | `909SIDE ` | `0xE2` | `0x39300` | `0x39940` | Roland TR-909 Rimshot / Sidestick |
| 13 | `0x0340` | `909_T   ` | `0xE0` | `0x39A00` | `0x3A500` | Roland TR-909 Low/Mid Tom |
| 14 | `0x0370` | `909_CHH ` | `0xE0` | `0x3D400` | `0x3ED40` | Roland TR-909 Closed Hi-Hat |
| 15 | `0x03A0` | `909_OHH ` | `0xE0` | `0x41800` | `0x47500` | Roland TR-909 Open Hi-Hat |
| 16 | `0x03D0` | `78_CHH  ` | `0xE0` | `0x4D800` | `0x4EE00` | Roland CR-78 Closed Hi-Hat |
| 17 | `0x0400` | `78_OHH  ` | `0xE0` | `0x53100` | `0x57E00` | Roland CR-78 Open Hi-Hat |
| 18 | `0x0430` | `RVB_CLP ` | `0xE0` | `0x57E00` | `0x64EC0` | Reverb Handclap |
| 19 | `0x0460` | `78_COW  ` | `0xE0` | `0x64EC0` | `0x65A00` | Roland CR-78 Cowbell |
| 20 | `0x0490` | `78MBEAT ` | `0xE2` | `0x65A00` | `0x66980` | Roland CR-78 Metallic Beat |
| 21 | `0x04C0` | `78GUIRO ` | `0xE0` | `0x66980` | `0x6AC00` | Roland CR-78 Guiro |
| 22 | `0x04F0` | `78_TAMB ` | `0xE0` | `0x6AC00` | `0x6D980` | Roland CR-78 Tambourine |
| 23 | `0x0520` | `78_MARC ` | `0xE0` | `0x6D980` | `0x70EC0` | Roland CR-78 Maracas |
| 24 | `0x0550` | `78_BNG  ` | `0xE2` | `0x74D00` | `0x75280` | Roland CR-78 Bongo |
| 25 | `0x0580` | `55CLAVE ` | `0xEB` | `0x75300` | `0x75740` | Roland TR-55 Clave |
| 26 | `0x05B0` | `303BASS ` | `0x6C` | `0x7913C` | `0x7CFF0` | Roland TB-303 Synthesizer Bass |

---

## 6. Porting and Compatibility Analysis

### 6.1 Can a U-110 play an SN-R8 card directly?
**No.** Even if the card header check is bypassed or patched:
- The U-110 firmware looks for 10-byte sample records at `0x0100` (which on an R-8 card contains the 48-byte tone table).
- The U-110 firmware looks for 80-byte tone records at `0x1000` (which on an R-8 card contains demo song data / sample audio).

### 6.2 Can the sounds be converted to a U-110 card format?
**Yes.** Because the sample data encoding and hardware address/data bus maps are identical:
1. The PCM sample waveform blocks (at `0x03000`–`0x7D000`) can be retained directly.
2. A converter tool could synthesize a standard U-110 **Sample Table** at `0x0100` using the `start_hi`, `start_frac`, `loop`, `end`, and `bank` fields from the R-8 tone records.
3. The converter could generate standard U-110 **80-byte Tone Records** at `0x1000`, setting:
   - 10-character name (e.g. `"TR-909 K  "` instead of `"909_K   "`)
   - Tone Type: SINGLE or DUAL
   - Keyboard split zone: spanning full MIDI range (e.g. note 0..127 pointing to the sample index)
4. Update the card header signature to `"RolandU-110 N\xb1S\xac"`.

Such a converted card would allow the U-110 to play the Roland TR-909, CR-78, and TB-303 sounds from the SN-R8-10 card natively using its existing tone generator engine.
