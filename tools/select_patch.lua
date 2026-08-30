-- Copyright (c) 2026 Elliott H. Liggett
-- SPDX-License-Identifier: GPL-3.0-or-later
-- Select a U-110 patch from the front panel, for automated runs.
--
--   U110_PATCH=4 mame u110 -autoboot_script tools/select_patch.lua
--
-- MIDI program change does NOT change the patch on a U-110: it selects a PART's TONE
-- (the 99 tones listed in reference/U-110.ins), which is why sending one leaves the
-- display on the same patch name with a "TEMP:" prefix.  Patches are panel-selected,
-- so this taps [INC] the required number of times from the power-on patch, P-01.
--
-- IMPORTANT: run with a scratch NVRAM directory, e.g.
--
--   mame u110 -nvram_directory /tmp/nv -autoboot_script tools/select_patch.lua
--
-- The current patch number lives in battery-backed RAM and MAME saves it on exit, so
-- without this each run starts wherever the last one finished and the count is applied
-- from there.  [INC]/[DEC] wrap modulo 64, so there is no way to home the selection by
-- pressing keys -- a known starting patch is the only way to address one absolutely.
--
-- Two things this has to get right:
--
--  * The notifier subscription MUST be kept alive.  If the handle returned by
--    add_machine_frame_notifier is garbage-collected the callback is silently
--    unsubscribed, with no error at all.  It is stored in a global below.
--
--  * The key state must be re-asserted EVERY frame.  Setting it released once and
--    then returning early leaves the firmware seeing a held key, which auto-repeats
--    and runs the patch number away -- the further the run, the bigger the overshoot.

local target  = tonumber(os.getenv("U110_PATCH") or "1")
local presses = math.max(0, math.min(63, target - 1))

local START, PRESS, GAP = 420, 3, 15      -- ~7 s in; 50 ms held, 250 ms apart
local inc = manager.machine.ioport.ports[":SW"].fields["Inc / Enter"]
local n = 0

print(string.format("select_patch: target P-%02d (%d presses)", target, presses))

_G.u110_patch_sub = emu.add_machine_frame_notifier(function()
	n = n + 1
	local v = 0
	if n >= START then
		local t     = n - START
		local cycle = PRESS + GAP
		if math.floor(t / cycle) < presses and (t % cycle) < PRESS then
			v = 1
		end
	end
	inc:set_value(v)          -- asserted every frame, pressed or not
end)
