-- Copyright (c) 2026 Elliott H. Liggett
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Dump the U-110's patch EDIT BUFFER while a run is going.
--
--   tools/u110run.sh -t 20 -m seq.mid -autoboot_script tools/dump_patch.lua
--
-- The active patch lives at work RAM 0x2800 in the 128-byte layout of the stored patches
-- (analysis/ROM-ANALYSIS.md §4): name at +0x04, OUTPUT MODE at +0x0E, then CHORUS RATE,
-- CHORUS DEPTH, TREMO. RATE, TREMO. DEPTH.  Printing it is how a SysEx address is checked
-- against the parameter it is supposed to reach -- write a distinctive value, see where it
-- lands -- without needing MIDI OUT.
--
-- U110_DUMP_AT is a comma-separated list of seconds; the default dumps once a second.

local mem = manager.machine.devices[":maincpu"].spaces["program"]
local at = {}
for s in (os.getenv("U110_DUMP_AT") or ""):gmatch("[^,]+") do at[math.floor(tonumber(s) * 60)] = true end
local every = next(at) == nil
local n = 0

_G.u110_dump_sub = emu.add_machine_frame_notifier(function()
	n = n + 1
	if not (every and n % 60 == 0 or at[n]) then return end
	local b = {}
	for i = 0, 0x13 do b[#b + 1] = string.format("%02X", mem:read_u8(0x2800 + i)) end
	local name = ""
	for i = 4, 13 do
		local c = mem:read_u8(0x2800 + i)
		name = name .. ((c >= 32 and c < 127) and string.char(c) or ".")
	end
	print(string.format("RXMASK %6.2f  0x3C00=%02X devid=%02X", n / 60.0,
		mem:read_u8(0x3C00), mem:read_u8(0x3C01)))
	print(string.format("PATCHBUF %6.2f  %s  name='%s' outmode=%d(mode %d) cr=%d cd=%d tr=%d td=%d",
		n / 60.0, table.concat(b, " "), name,
		mem:read_u8(0x280E), mem:read_u8(0x280E) + 1,
		mem:read_u8(0x280F), mem:read_u8(0x2810),
		mem:read_u8(0x2811), mem:read_u8(0x2812)))
end)
