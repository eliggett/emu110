# Wild Ideas & Architectural Exploits — Roland U-110

This document records architectural insights into the Roland U-110 hardware and wave subsystem, detailing how cartridge handling works and outlining concepts for custom firmware—specifically using the sound engine's internal read mechanism as a **"sneak path"** to stream arbitrary wave ROM and cartridge sample data out over MIDI SysEx.

---

## 1. Background & Wave Subsystem Topology

In the Roland U-110 (and its successor, the U-220), the main CPU (**Intel 8097BH**, 16-bit MCS-96 architecture) does not have a direct address or data bus to the wave ROMs or external sound cartridges.

Instead, the wave subsystem is segregated behind two custom gate arrays:
* **IC15 (MB87419)**: Address generator and tone control engine. Drives the 19-bit wave address bus `WA0-WA18` (512 KiB per bank) and bank select logic (`IC13 HC02` / `IC14 HC32`).
* **IC16 (MB87420)**: Wave data receiver and DAC feeder. Receives 8-bit PCM data from the wave ROM / cartridge bus and streams it to the **PCM54HP** 16-bit DAC.
* **IC15 $\leftrightarrow$ IC16 9-bit Private Bus**: Joins the two gate arrays.

```
                          8097BH CPU (IC3)
                                 |
                        AD0-AD15 System Bus
                                 |
                  +--------------+--------------+
                  |                             |
             IC8 Gate Array             IC15 MB87419 (Tone Gen)
            (Address Decode)            Owns register window 0x1400-0x143F
                  |                             |
                  |                       WA0-WA18 (19-bit bus)
                  |                             |
                  |              +--------------+---------------+
                  |              |                              |
                  |       Internal Wave ROMs             PCM Card Slots x4
                  |         (IC18-IC21, 2 MB)             (Slots 1 to 4)
                  |              |                              |
                  +------- Wave Data D0-D7 ---------------------+
                                 |
                                 v
                           IC16 MB87420 ---> IC22 PCM54HP DAC ---> Audio Outs
                                 ^
                                 | (9-bit internal bus)
                           IC15 MB87419
```

---

## 2. Cartridge Operation & Memory Mapping

### 2.1 The SN-U110 Cartridge Family & U-220 Built-In ROMs
* The U-110 provides **4 front-panel PCM card slots** (Slots 1–4) supporting the **SN-U110** Sound Library series (512 KiB mask ROM cards).
* On the U-220, Roland took the exact ROM dumps from two popular cards—**SN-U110-08 (Synthesizer)** and **SN-U110-09 (Guitar & Bass)**—and soldered them onto the U-220 motherboard as **Wave ROM 4** and **Wave ROM 5**.
* These ROM dumps (`waverom4` and `waverom5`) retain the standard `SN-U110` cartridge header and binary card ID. When loaded into any of the U-110's four card slots in emulation or on real hardware, they function identically to the physical expansion cards.

### 2.2 Card Detection & Mounting
1. **Hardware Presence Detection**:
   Card insertion connects ground to active-low pins wired to CPU **`PORT1`** bits 0–3 (Bit 0 = Slot 1, Bit 1 = Slot 2, Bit 2 = Slot 3, Bit 3 = Slot 4).
2. **Mount & Validation**:
   At boot or card scan, the firmware checks `PORT1`. For each occupied slot, it reads the first 48 bytes via the Voice 0 sneak path and validates the 16-byte header:
   ```
   52 6F 6C 61 6E 64 55 2D 31 31 30 20 4E B1 53 AC
   "RolandU-110 N" + 0xB1 0x53 0xAC
   ```
3. **ID Caching**:
   On valid match, the firmware caches the card's catalogue ID byte (offset `+0x20`, e.g. `0x08` for SN-U110-08) into RAM at `0x2743 + slot` (`0x2743`..`0x2746`). Empty slots read `0x00`; mount failures set `0xFF`.

### 2.3 PCM Address Space
The MB87419 sound engine decodes a 4 MB PCM address space, interleaving the 4 internal wave ROMs with the 4 external card slots:

| Sound Engine Address | Destination | Content |
|---|---|---|
| `0x000000 - 0x07FFFF` | Internal Bank 0 | Factory Preset Tones (1–99) & Samples |
| `0x080000 - 0x0FFFFF` | **Card Slot 1** | SN-U110 Card in Slot 1 (512 KiB) |
| `0x100000 - 0x17FFFF` | Internal Bank 1 | Internal Sample Data |
| `0x180000 - 0x1FFFFF` | **Card Slot 2** | SN-U110 Card in Slot 2 (512 KiB) |
| `0x200000 - 0x27FFFF` | Internal Bank 2 | Internal Sample Data |
| `0x280000 - 0x2FFFFFF` | **Card Slot 3** | SN-U110 Card in Slot 3 (512 KiB) |
| `0x300000 - 0x37FFFF` | Internal Bank 3 | Internal Sample Data & Demo Sequences |
| `0x380000 - 0x3FFFFF` | **Card Slot 4** | SN-U110 Card in Slot 4 (512 KiB) |

### 2.4 Logical Card Data Layout (Descrambled)
In logical space (after compensating for hardware address/data bus permutations):
```
0x0000 - 0x002F : 48-byte ID Header ("RolandU-110 N\xB1S\xAC", "SN-U110-08", ID 0x08)
0x0100 + 10 * i : Sample Definition Table (10 bytes per sample: start, loop mode, length, loop len, tune, ref note)
0x1000 + 0x50 * t : Tone Parameter Records (80 bytes per tone):
                  +0x00: 10-byte ASCII Tone Name (e.g. "FANTASIA  ")
                  +0x0A: 6-byte header parameters
                  +0x10: Partial 1 multisample split points (8 bytes), sample indices (9 bytes), level/envelope
                  +0x30: Partial 2 multisample split points, sample indices, level/envelope
0x020000+       : Raw Sample Waveforms (8-bit float / companded delta PCM)
```

---

## 3. The "Sneak Path": Reading Wave Data via Voice 0

### 3.1 Mechanism
Because the CPU lacks direct bus access to wave memory, the factory firmware **borrows Voice 0 of IC15 as a DMA read engine**. By setting Voice 0's phase accumulator to a specific address, IC15 drives `WA0-WA18` and triggers IC16 to fetch the byte into an internal read latch readable at register **`0x1402`**.

The primitive sequence (firmware routine `0x7BB2`):
1. **Select Voice 0**: Write `0x00` to `0x143E` (`0x1F` Voice Select).
2. **Select Bank / Slot**: Write `0x1404` (`0x02` Bank Select).
   - Bits 12–13: Bank / Slot index (0–3).
   - Bit 11: Mode flag (`0` = Internal ROM, `1` = Card Slot).
   - Bit 10: Address bit $A_{18}$ (picks lower 256 KiB vs upper 256 KiB of the 512 KiB chip).
3. **Set Phase Accumulator**: Write 32-bit fixed point phase (`0x1410` low word, `0x1414` high word).
   - $\text{Phase} = ((\text{address} \ \& \ \text{0x3FFFF}) - 2) \ll 14$
   - The $-2$ offset compensates for IC15's internal 2-byte read-ahead prefetch.
4. **Busy-Wait**: A short loop ($\sim 16$ CPU cycles) allows the gate arrays to complete the bus transaction.
5. **Read Port**: Read raw byte from `0x1402` (`0x01` PCM Data Port).
6. **Data Permutation**: Pass the byte through the 256-byte lookup table at **`0x9257`** to reverse hardware data-line routing (`ROM -> IC16 -> 9-bit bus -> IC15 -> CPU`).

### 3.2 Addressing Arbitrary Sample Data
The stock firmware uses this mechanism strictly to read the header (`0x0000`), sample table (`0x0100`), and tone records (`0x1000`).

However, **there is zero hardware restriction stopping Voice 0 from reading deeper into wave memory**. The entire 19-bit address space ($0\text{x}00000 \text{ to } 0\text{x}7FFFF$, 512 KiB) of all four card slots and all four internal ROMs can be read simply by supplying the full 19-bit target address:

$$\text{Bank Reg Value} = (\text{Slot} \ll 12) \mid (1 \ll 11) \mid \left(\left(\frac{\text{Target Address}}{0\text{x}40000}\right) \ll 10\right)$$

$$\text{Phase Accumulator} = ((\text{Target Address} \ \& \ 0\text{x}3FFFF) - 2) \ll 14$$

---

## 4. Wild Idea: Standalone ROM & Cartridge Dumper over SysEx

### 4.1 Concept
Build custom firmware (or an injectable payload) that turns a physical Roland U-110 into an **autonomous ROM / expansion card dumping device**.

By walking the entire 512 KiB address range of any card inserted in Slots 1–4 (or the 2 MB internal wave ROMs) using the Voice 0 sneak path, the firmware can read every byte, descramble it, and stream the raw image out through the 5-pin DIN **MIDI OUT** port using standard MIDI SysEx or the **MIDI Sample Dump Standard (SDS)**.

### 4.2 Speed & Feasibility
* **Internal CPU Read Throughput**:
  The 8097BH running at 12 MHz can execute an optimized streaming read loop (such as routine `0x7B07`) at approximately **$60\text{--}100\text{ KiB/s}$**.
  - Sucking an entire 512 KiB card into CPU memory / processing pipeline takes **$\sim 5\text{--}8\text{ seconds}$**.
* **MIDI Transmission Throughput**:
  Standard MIDI runs at 31,250 baud ($\approx 3.125\text{ KiB/s}$ raw, or $\sim 2.4\text{ KiB/s}$ with 7-bit SysEx framing).
  - A full 512 KiB card dump transmits over MIDI in **$\sim 3.5\text{ minutes}$**.
  - An individual sample (e.g. 8 KiB) transmits in **$\sim 3.5\text{ seconds}$**.
* **No Additional Hardware Required**:
  No EPROM programmer, desoldering, or custom hardware adapter is needed to dump rare SN-U110 expansion cards. The U-110 synthesizer itself acts as the cartridge reader.

### 4.3 Implementation Architecture on MCS-96

```
+-------------------------------------------------------------------+
|                     Custom Firmware / Hook                        |
|                                                                   |
|  1. Trigger via Front Panel (Service Menu) or SysEx Command       |
|  2. Detect Cartridge Presence via PORT1                           |
|  3. Loop addr = 0x00000 .. 0x7FFFF:                               |
|       - Program IC15 Bank (0x1404) & Phase (0x1410/0x1414)        |
|       - Wait ~16 cycles                                           |
|       - Read raw byte from 0x1402                                 |
|       - Descramble via LUT at 0x9257                              |
|       - Pack into 7-bit SysEx payload (MIDI OUT ring buffer)      |
|  4. Transmit MIDI SysEx Packet:                                   |
|       F0 41 [dev] 23 12 [addr_hi] [addr_mid] [addr_lo] [data...]  |
|       [chksum] F7                                                 |
+-------------------------------------------------------------------+
```

#### ROM Placement & Free Space
The stock U-110 EPROM (27C512, 64 KB) contains **$\sim 6.3\text{ KB}$ of completely unused space**:
* `0x0100 - 0x0FFF` (3,840 bytes): Decoded by `/CS7` and entirely `0xFF` in factory ROMs.
* `0xD6C7 - 0xDFFF` (2,361 bytes): Unused `0xFF` padding between main code and bank-switched patch arena.

This is more than enough space to house the complete dumper routines, packet framer, and UI additions.

---

## 5. Additional Wild Ideas & Extensions

### 5.1 Onboard Waveform Analysis & Visualizer
* **LCD Oscilloscope**: Read sample loop regions into work RAM, compute min/max downsampling, and render mini ASCII waveform previews or envelope graphs on the 16x2 character LCD.
* **Loop Seam DC & Phase Inspector**: Calculate the mathematical sum of deltas across sample loop points on hardware to verify DC balance and loop-click elimination.
* **Automated Sound Library Auditing**: Scan all 4 card slots, check ROM checksums against a known database, and list missing/corrupted multisamples.

### 5.2 DIY Flash Cartridge Programmer / Verifier
* If custom flash cartridges (using rewritable flash memory mapped to the SN-U110 card connector) are developed, custom firmware can verify written data on-the-fly and perform block-level integrity checks directly on the synth.

### 5.3 Live Sample Extraction into Emulation Tooling
* Python tools (such as [`tools/export_sample.py`](file:///home/eliggett/Documents/projects/emu110/tools/export_sample.py)) can trigger automated SysEx dumps of connected cartridges directly into `.wav` renders on a host PC without needing offline cartridge readers.

