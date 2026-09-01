// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
//
// roland_lp.h includes "dirom.h" for device_rom_interface.  The shim defines that in
// emu.h, so this only has to exist and point there -- but it MUST exist, or the include
// resolves to MAME's, which drags in the whole memory system.
#ifndef VOLTAIRE_COMPAT_DIROM_H
#define VOLTAIRE_COMPAT_DIROM_H
#pragma once
#include "emu.h"
#endif
