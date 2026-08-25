# U-110 factory patch names

Read straight out of the firmware: the patch table starts at **0xE004** in
`roms/roland_u110_pgm_(15179960).bin`, 0x80 bytes per patch, name in the first 10 bytes.
Verified against the Owner's Manual for P-01, P-04, P-23, P-25, P-48 and P-52.

A **Program Change on the control channel (default 16)** selects a patch, and the
program number is the patch number minus one. A program change on a *part's* channel
selects that part's tone instead -- a different thing entirely.

| PC | patch | name | PC | patch | name |
|---|---|---|---|---|---|
| 0 | P-01 | Ac.Piano | 1 | P-02 | Brt Piano |
| 2 | P-03 | ff Piano | 3 | P-04 | Wide Piano |
| 4 | P-05 | Double A.P | 5 | P-06 | Dtun Piano |
| 6 | P-07 | E.Piano | 7 | P-08 | Double E.P |
| 8 | P-09 | Detune E.P | 9 | P-10 | Hard E.P |
| 10 | P-11 | Vibraphone | 11 | P-12 | Hard Vib |
| 12 | P-13 | DetuneBell | 13 | P-14 | Marimba |
| 14 | P-15 | A.Guitar | 15 | P-16 | Double A.G |
| 16 | P-17 | 12str A.G | 17 | P-18 | Mute Sw EG |
| 18 | P-19 | Double EG | 19 | P-20 | Slap Bass |
| 20 | P-21 | DetuneBass | 21 | P-22 | V-Sw Slap |
| 22 | P-23 | Fing Bass | 23 | P-24 | Pick Bass |
| 24 | P-25 | Fless Bass | 25 | P-26 | Ac.Bass |
| 26 | P-27 | Synth Bass | 27 | P-28 | Choir |
| 28 | P-29 | Oct Choir | 29 | P-30 | Double Chr |
| 30 | P-31 | Strings | 31 | P-32 | Double Str |
| 32 | P-33 | E.Organ | 33 | P-34 | DoubleOrg1 |
| 34 | P-35 | DoubleOrg2 | 35 | P-36 | Soft Tp |
| 36 | P-37 | Tp/Tromb | 37 | P-38 | Oct Tp/Trb |
| 38 | P-39 | Sax | 39 | P-40 | Bright Sax |
| 40 | P-41 | Detune Sax | 41 | P-42 | Oct Sax |
| 42 | P-43 | Brass | 43 | P-44 | Oct Brass |
| 44 | P-45 | Double Brs | 45 | P-46 | Flute |
| 46 | P-47 | Dtn Flute | 47 | P-48 | Shakuhachi |
| 48 | P-49 | Drums | 49 | P-50 | Double Drm |
| 50 | P-51 | Short Drm | 51 | P-52 | Fantasy |
| 52 | P-53 | Brs + Str | 53 | P-54 | 5th Br+Str |
| 54 | P-55 | Choir+Str | 55 | P-56 | Thick Bell |
| 56 | P-57 | Guit>Piano | 57 | P-58 | Trump>Sax |
| 58 | P-59 | Sax / Tp | 59 | P-60 | Multi-Set1 |
| 60 | P-61 | Multi-Set2 | 61 | P-62 | Multi-Set3 |
| 62 | P-63 | Multi-Set4 | 63 | P-64 | Multi-Set5 |
