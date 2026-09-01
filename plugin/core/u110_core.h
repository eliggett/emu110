// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
/***************************************************************************************

    U110Core -- the Roland U-110 emulation, with no MAME framework around it

    This is the interface the Voltaire 110 plugin talks to.  It is defined BEFORE the
    implementation exists, deliberately: the plugin builds against it, the null test
    measures it, and the implementation can be rewritten underneath without any of that
    moving.  See PLUGIN-PLAN.md section 4.

    WHY NOT JUST LINK MAME.  MAME's framework keeps process-global state (so one instance
    per host), is not real-time safe in an audio callback, and drives everything from a
    frame scheduler synced to the 50 Hz LCD refresh -- which is where the 20 ms latency
    quantum currently comes from.  None of that is inherent to the emulation, only to the
    framework, so the framework is what gets replaced.  The DEVICES are kept exactly as
    they are and compiled against plugin/compat/emu.h.

    LICENCE.  BSD-3-Clause, like the MAME device sources it drives.  The plugin layer above
    (plugin/src) is GPL-3.0-or-later and code flows only downward-to-upward: nothing from
    plugin/src may be moved into here, or MAME could never take these findings back.

    THREADING.  Every method is called from the audio thread unless marked otherwise.
    render() allocates nothing, takes no lock, opens no file and logs nothing.  The load*
    methods DO allocate and must be called from a worker thread's handover point, between
    render() calls -- never from inside one.  See PLUGIN-PLAN.md section 8.1.

    SAMPLE RATE.  The chip runs at exactly 32000 Hz and this class only ever produces that.
    Resampling to the host rate belongs above, in the plugin, where it can report its own
    latency to the host.  Keeping the core at native rate is also what makes the null test
    possible: no resampler in the path means bit-identical is achievable, not just close.

***************************************************************************************/

#ifndef VOLTAIRE_U110_CORE_H
#define VOLTAIRE_U110_CORE_H

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace voltaire {

/// The chip's native rate.  Not configurable -- see the note above.
inline constexpr uint32_t kCoreSampleRate = 32000;

/// Six Multi Output jacks.
inline constexpr unsigned kNumOutputs = 6;

/// Six panel switches.  The order matches the byte the firmware reads at 0x1300.
enum Button : int
{
	kButtonPartJump = 0,
	kButtonEditExit,
	kButtonLeft,
	kButtonRight,
	kButtonDec,
	kButtonIncEnter,
	kButtonCount
};

/// Wave ROM banks (internal) and card slots.
inline constexpr unsigned kNumWaveBanks = 4;
inline constexpr unsigned kNumCardSlots = 4;
inline constexpr size_t   kCardBytes    = 0x80000;   // 512K, the maximum a card may be

/***************************************************************************************
    What the UI needs to draw the panel.

    Versioned, fixed layout, little-endian: it crosses a process boundary under LV2, which
    mandates the UI as a separate binary.  Roughly 120 bytes, or 190 when the custom glyphs
    changed -- at 30 Hz that is 4-6 KB/s, which marshals fine over anything.

    Publishing is FIRE AND FORGET.  If the UI is closed, or the host never opens one, the
    emulation must run bit-identically.  This must never be in the emulation's timing path.
***************************************************************************************/
struct PanelState
{
	static constexpr uint16_t kVersion = 1;

	uint16_t version;

	// The LCD is a CHARACTER display.  These are character codes, not pixels: codes
	// 0x20-0x7F come from the controller's CGROM, and 0x00-0x07 are the custom glyphs
	// defined in cgram below.  Two lines of 16 visible columns.
	uint8_t  lcd[32];
	uint8_t  cursor_pos;        ///< 0..31
	uint8_t  cursor_flags;      ///< bit 0 = cursor on, bit 1 = blinking, bit 2 = blink phase

	// The eight custom glyphs, 8 rows each, low 5 bits = the 5 dots across.
	//
	// These MUST be pushed when cgram_dirty is set rather than sampled at the 30 Hz text
	// cadence.  The boot logo is a CGRAM animation -- the firmware redefines the glyphs
	// every ~52.5 ms while the text never changes -- and polling at 30 Hz would alias it
	// into a stutter.  That animation is also the acceptance test for the whole LCD path.
	uint8_t  cgram_dirty;
	uint8_t  cgram[64];

	uint8_t  leds;              ///< from led_w at 0x1200 (written inverted by the firmware)
	uint8_t  card_present;      ///< 4 bits, one per slot, 1 = present (NOT PORT1's polarity)
	uint8_t  patch;             ///< current patch number
	uint8_t  part_tone[6];      ///< per-part tone index
	uint32_t voice_active;      ///< one bit per voice; voice 0 is the ROM read port, never sounds
	float    peak[2];           ///< L/R peak since the last snapshot, for the clip indicator
};

/***************************************************************************************
    Result of loading an image.  Loads fail for mundane reasons -- wrong size, missing
    file, a card whose header does not check out -- and the UI should say which.
***************************************************************************************/
enum class LoadResult : int
{
	Ok = 0,
	WrongSize,
	BadImage,
	NoSuchSlot,
};

class U110Core
{
public:
	U110Core();
	~U110Core();

	U110Core(const U110Core &) = delete;
	U110Core &operator=(const U110Core &) = delete;

	// --- images.  Worker thread, between render() calls.  These allocate. ------------

	/// The firmware EPROM (v2.00 or v2.03).  Required before the machine can run.
	LoadResult loadProgramRom(const uint8_t *data, size_t len);

	/// One of the four internal wave ROM banks, as dumped -- descrambling happens here.
	LoadResult loadWaveRom(unsigned bank, const uint8_t *data, size_t len);

	/// Mount a PCM card, or pass data == nullptr to eject.  Undersized dumps are mirrored
	/// up in 128K pages, as the address descrambling requires.
	LoadResult loadCard(unsigned slot, const uint8_t *data, size_t len);

	/// Hard reset.  Safe to call once images are loaded; the firmware then boots normally.
	void reset();

	/// Run the CPU as fast as possible until the firmware reaches its idle loop, producing
	/// no audio.  Nothing forces realtime, so a second of emulated boot costs a few
	/// milliseconds of wall time.  This is what makes "write patchram, reset, resume" a
	/// usable way to load a bank without the firmware's cache at 0x2800 going stale.
	void runUntilIdle(uint32_t timeoutMs = 5000);

	// --- the audio thread ------------------------------------------------------------

	/// Queue MIDI bytes.  sampleOffset is when the first byte STARTS arriving on the wire,
	/// within the next render() call.  Bytes are then clocked in at 31250 baud, so a byte
	/// takes about 10 samples to complete -- the same as the hardware, and the reason MIDI
	/// timing here is sample-accurate rather than block-quantised.
	void midiIn(const uint8_t *bytes, size_t n, uint32_t sampleOffset);

	/// Deliver bytes at an exact emulated time, in seconds since reset.
	///
	/// For offline rendering and for the null test.  A host's MIDI is inherently
	/// sample-quantised, so midiIn() above is the right interface for one; but a byte
	/// whose arrival is rounded to the nearest sample can start a voice one sample early,
	/// and one sample is a large error when compared against another emulator rather than
	/// against an ear.
	void midiInAtTime(const uint8_t *bytes, size_t n, double seconds);

	/// Collect bytes the firmware sent during the last render().  Returns how many were
	/// written; offsets, if non-null, receives one sample offset per byte.
	///
	/// The U-110 sends NOTHING unprompted -- no active sensing, and it has no keyboard --
	/// so this is empty except after a SysEx request or a bulk dump.  Do not treat silence
	/// as a fault.
	///
	/// MIDI THRU is not here and never will be: on the hardware it is a wire off the
	/// opto-isolator with no CPU involvement, so the plugin echoes its own input directly.
	size_t midiOut(uint8_t *buf, size_t cap, uint32_t *offsets);

	/// Render nframes of the six Multi Output jacks at kCoreSampleRate, DRY -- before the
	/// pan matrix, the reconstruction filters and the EQ, all of which live above.
	///
	/// outs must point to kNumOutputs buffers of at least nframes floats.  Allocates
	/// nothing.  Deterministic: same images, same reset, same MIDI at the same offsets
	/// gives bit-identical output at any block size.  That property is what the null test
	/// checks, and it is easy to lose by accident.
	void render(float *const outs[kNumOutputs], uint32_t nframes);

	/// The stereo mix: the six jacks through the pan matrix, the Sallen-Key cascade, the
	/// output RC and the HF correction, at kCoreSampleRate.
	///
	/// This is the MAME-equivalent path and it is what the null test compares.  Section 11
	/// wants the reconstruction chain moved after the resampler eventually; do that only
	/// once this is bit-identical, so the improvement has a trustworthy baseline.
	void renderStereo(float *left, float *right, uint32_t nframes);

	/// Panel switch state.  Send EDGES, and hold a press long enough in EMULATED time for
	/// the firmware's debouncer at 0x4118 to see it -- roughly 150 ms.  A press and release
	/// inside one buffer will be missed entirely.
	void setButton(Button sw, bool down);

	// --- observation.  Cheap, const, and never in the emulation's timing path. --------

	void snapshot(PanelState &out) const;

	/// Read firmware RAM directly.  This is how the UI reads patch and tone NAMES without
	/// spending emulated time driving the menus.
	uint8_t readMem(uint16_t addr) const;

	/// Write firmware RAM directly.  Beware: the firmware caches the active patch into
	/// work RAM at 0x2800, so poking patchram behind its back leaves that cache stale.
	/// Write the bank, then reset() and runUntilIdle().
	void writeMem(uint16_t addr, uint8_t value);

	// --- persistence -----------------------------------------------------------------

	/// Serialise the machine (both NVRAM regions plus device state) for DAW project state.
	/// Returns the number of bytes needed; pass cap == 0 to size the buffer first.
	/// ROMs are NOT included -- store those by name and SHA-256 and re-resolve on load.
	size_t saveState(uint8_t *buf, size_t cap) const;
	bool   loadState(const uint8_t *buf, size_t n);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace voltaire

#endif // VOLTAIRE_U110_CORE_H
