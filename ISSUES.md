# emu110 CURRENT issues

## Audio artifacts: 

Sometimes there are little noises, especially with multi-tone parts. Likely some kind of timing issue with the emulated DSP. It's not something where I can give directions to reproduce it, and it is not all the time, more or less every ten minutes or so. 

## Window:

### window size redraw: 
The window sometimes resizes and the graphics are messed up, often showing "layers" of prior renderings. Clicking on buttons such as "tone" will generally redraw the UI and the issue goes away. 


### window shows more than it should: 

Sometimes after a resize or even on the initial showing, the window will show content below the main UI when the "dive" section is not even open. This content is generally just the background image, or some of it anyway. 

### Rough transition when more tabs shown:

When the user clicks to one of the "P1"... "P6" tabs from "Set" or "Common", the second row of tabs appears. This is an abrupt transition that "shakes" the entire window. I think it would be better to just account for this space with a "faint" rendering of the second row of buttons always visible. Faint can mean more transparent. 

## Setup UI:

Many of the buttons appear to not be wired up yet. "Map edit", "Control Change", etc. 

## Patch Title Edit: 

The patch title edit cursor does not blink. 

## Write button: 

**Done.** The DIVE common page's WRITE opens the PATCH list to choose a destination, then
asks before overwriting, naming the patch that would be lost. It stores the edit buffer at
`0x2800` into the slot and re-selects it, which is exactly what the machine's own
PATCH:WRT:WRITE does -- 116 bytes, not the 128-byte stride, and `make writecheck` runs both
paths and compares what they leave behind. `analysis/SYSTEM-DESIGN.md` section 5.3.4.

Write protect is deliberately NOT exposed. The machine has one -- `0x3C00` bit 0, on from
the factory -- but it gates the *firmware's* write path, and the plugin writes memory
directly, so honouring it would mean a lock whose key is on a SETUP page nobody would think
to look for. The "are you sure?" box is the guard instead, and it can say what is about to
be lost, which one line of sixteen characters cannot.

## User Presets: 

**Done, for one preset per file.** The PATCH menu has an INTERNAL 64 tab and a LIBRARY
tab. Presets live in `~/.local/share/Voltaire110/patches` (beside `roms`, and
`U110_DATA_DIR` overrides it as everywhere else), one `.u110pat` file each, written
temp-then-rename so a reader sees the old file or the new one and never half of either.
Instances share the library by the filesystem and nothing else: the browser rescans when
it opens, re-reading only files whose size or modification time moved.

A preset carries the 116-byte patch record, a display name, the volume and the HF
correction, and the cards it needs. Loading one puts it in **P-64**, the audition slot --
a patch can only be played by the firmware out of patchram, so auditioning has to spend a
slot -- and the browser says so when a patch wants a card that is not mounted.

**Banks are done, as categories.** A bank is a name a patch claims -- one line in the
preset file -- so "Strings" and "Abstract" cost nothing to make and nothing to clean up:
a bank exists while a patch is in it and stops existing when the last one leaves -- except
that a bank named in the browser is **kept for as long as the plugin window is open**, even
with nothing in it, so that it can be made first and filled afterwards. It is shown as
"(empty)" in the move list, and closing the window is what forgets it: nothing empty is
ever written to disk. The
LIBRARY tab has a row of bank tabs (All, each bank, Unfiled, and **+ New bank**) that
filter the list. **Save Patch** and **Override Patch** are buttons in the header. Save Patch always makes a
new file and never overwrites; Override Patch writes back over the preset that was
recalled, so editing one and keeping it does not mean saving a copy and deleting the
original. Override is only live while a library preset is what the machine is playing,
something has changed, and the patch's own name is still the one it was recalled with --
renaming it is how you say "this is a different patch now". Hovering it says which of the
three is missing. Each preset shows its bank beside its name: clicking the name plays it, clicking
the bank moves it. **Right-clicking gives Rename, Move to bank and Delete** -- delete
asks again first, since there is no undo and no wastebasket. Naming a bank and renaming a preset are both
finished with an **OK button** beside the field, because a plugin UI does not reliably get
the Return key: the host sees it first and a DAW that binds Return to something of its own
never passes it on. `VOLTAIRE_KEYS=1` prints every key that does arrive. Every overlay also
has an **X** in its corner, for anyone who would not think to click in the dark or press
Escape.

**Note for anyone adding a key binding:** Ardour captures `Return`, `Space` and plain
letters including `a`; Carla passes everything through. Ardour has a full-keyboard-focus
setting that changes this, but it is off by default. So nothing may be reachable only by a
key -- see PLUGIN-PLAN.md 10.5.1, and the comment above `onKeyboard` in
`plugin/src/Voltaire110UI.cpp`. Every one of these ends back
in the library rather than closing the menus, and Escape steps back to the library too,
because the next thing anybody does after naming a bank or renaming a patch is in the
library as well. Renaming changes the display
name and the file to match, and deliberately not the ten-byte name inside the patch, which
is the machine's own field and what the LCD shows.

The DIVE common page's **WRITE** opens the same menu in a *write* mode: the machine's 64
slots and the library are the two places a patch can go, so they are the two tabs either
way, and only the verbs change. In write mode a slot or a preset is a destination, asked
about before anything is replaced.

Still to do: `.u110bank` files for exchanging a set with somebody, `.syx` interop,
free-text search, and making the audition slot a setting rather than a constant.

Original note follows.

Related to the above, we need an atomic read/write system to a plugin settings file which contains a database of user patches. The databasde of user patches can be enormous since we have a computer available to us. The patch button shall allow selection. You can repurpose OEM preset 64 as a "loader preset" with which to load the user-selected preset. A preset is a standard U-110 preset, entirely defined by sysex commands. Other attributes, such as filter and volume setting, are not sysex... if possible we can keep these as additional bits in the user preset. 

