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

Related to the above, we need an atomic read/write system to a plugin settings file which contains a database of user patches. The databasde of user patches can be enormous since we have a computer available to us. The patch button shall allow selection. You can repurpose OEM preset 64 as a "loader preset" with which to load the user-selected preset. A preset is a standard U-110 preset, entirely defined by sysex commands. Other attributes, such as filter and volume setting, are not sysex... if possible we can keep these as additional bits in the user preset. 

