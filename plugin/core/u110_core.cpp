// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
/***************************************************************************************

    U110Core -- the machine, assembled from MAME's devices with no MAME around them

    PLUGIN-PLAN.md section 3 splits roland_u110.cpp by kind.  The constant tables and the
    pure functions are SHARED, in roland_u110_data.h.  What is here is the other half: the
    machine wiring -- memory map, port handlers, the audio graph -- which correctly differs
    between a MAME driver and a plugin and so is written twice, on purpose.

    Everything below that touches the emulation must behave identically to the driver, and
    the null test is what proves it.  Where a line looks arbitrary it is almost certainly
    copying the driver deliberately; check roland_u110.cpp before changing it.

***************************************************************************************/

#include "u110_core.h"

#include "emu.h"

#include "sound/roland_lp.h"
#include "cpu/mcs96/i8x9x.h"
#include "video/msm6222b.h"
#include "sound/flt_biquad.h"
#include "sound/flt_rc.h"

#include "roland_u110_data.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace voltaire {

using namespace roland_u110;

namespace {

// The diagnostic switches, read ONCE.  getenv walks the environment block and is not for
// the audio thread; these used to be called per register access, which the real-time
// audit counted as 23383 calls inside one run of the callback.
#define TRACE_FLAG(NAME, ENV) \
    inline bool NAME() { static const bool on = std::getenv(ENV) != nullptr; return on; }
TRACE_FLAG(trace_u110_lcdtrace,  "U110_LCDTRACE")
TRACE_FLAG(trace_u110_envtrace,  "U110_ENVTRACE")
TRACE_FLAG(trace_u110_tgtrace,   "U110_TGTRACE")
TRACE_FLAG(trace_u110_miditrace, "U110_MIDITRACE")
TRACE_FLAG(trace_u110_cycles,    "U110_CYCLES")
#undef TRACE_FLAG

constexpr u32 CPU_CLOCK  = 12000000;        // 12 MHz XTAL
constexpr u32 PCM_CLOCK  = 34816000;        // IC15's own crystal, X2
constexpr u32 PCM_DIV    = 1088;            // 34.816 MHz / 1088 = 32000 Hz exactly

constexpr size_t PROGRAM_ROM_SIZE = 0x10000;
constexpr size_t PCM_SPACE_SIZE   = 0x400000;
constexpr size_t WORKRAM_SIZE     = 0x1f00;     // 0x2100-0x3fff
constexpr size_t PATCHRAM_SIZE    = 0x2000;     // 0xe000-0xffff

} // anonymous namespace


/***************************************************************************************
    Impl
***************************************************************************************/

struct U110Core::Impl
{
	Impl()
		: mconfig(machine)
		, cpu(mconfig, "maincpu", nullptr, CPU_CLOCK)
		, pcm(mconfig, "pcm", nullptr, PCM_CLOCK)
		, lcd(mconfig, "lcd", nullptr, 0)
		, sk1{ { mconfig, "sk1l", nullptr, 0 }, { mconfig, "sk1r", nullptr, 0 } }
		, sk2{ { mconfig, "sk2l", nullptr, 0 }, { mconfig, "sk2r", nullptr, 0 } }
		, rc { { mconfig, "rcl",  nullptr, 0 }, { mconfig, "rcr",  nullptr, 0 } }
		, eq { { mconfig, "eql",  nullptr, 0 }, { mconfig, "eqr",  nullptr, 0 } }
		, program(0x10000, 16)
	{ }

	// --- the machine ---------------------------------------------------------------
	running_machine machine;
	machine_config mconfig;

	n8097bh_device            cpu;
	mb87419_mb87420_device    pcm;
	msm6222b_device           lcd;
	filter_biquad_device      sk1[2], sk2[2], eq[2];
	filter_rc_device          rc[2];

	address_space program;

	// The CPU's AS_DATA is its own internal register file, declared by i8x9x with MAME's
	// address_map DSL.  We run that declaration and wrap the result in a space.
	std::unique_ptr<address_map> regs_map;
	std::unique_ptr<address_space> regs;

	// --- memory --------------------------------------------------------------------
	std::vector<u8> program_rom = std::vector<u8>(PROGRAM_ROM_SIZE, 0xff);
	std::vector<u8> pcmrom      = std::vector<u8>(PCM_SPACE_SIZE, 0xff);
	std::vector<u8> workram     = std::vector<u8>(WORKRAM_SIZE, 0);
	std::vector<u8> patchram    = std::vector<u8>(PATCHRAM_SIZE, 0);
	bool have_program = false;

	// --- driver state, mirroring roland_u110.cpp ------------------------------------
	u8  snd_regs[0x20] = { 0 };
	u8  card_present = 0x0f;        // bit per slot, 1 = empty (PORT1 is active low)
	u8  port2 = 0;
	u16 leds = 0;
	// ACTIVE LOW, and all EIGHT bits read high when nothing is pressed.  Bits 6 and 7 are
	// unused inputs, not zeros: the firmware's edge detector at 0x4118 keeps ~raw as
	// "previously pressed", so a clear bit is a HELD KEY.  Returning 0x3F here meant two
	// phantom buttons held down for the life of the machine, which sent the firmware down
	// branches MAME never takes.
	u8  switches = 0xff;
	u64 tg_writes = 0, rom_reads = 0, extints = 0;
	bool patch_view_rom = false;

	emu_timer *lcd_timer = nullptr;

	// MAME's sound_manager flushes every stream on a 50 Hz periodic timer
	// (STREAMS_UPDATE_FREQUENCY), on top of the updates a device forces for itself.  That
	// cadence is not cosmetic: roland_lp sets its envelope-arrival PENDING bit from inside
	// sound_stream_update(), and the 64 kHz env_tick then offers the interrupt.  So HOW
	// FAR the stream has been rendered decides which tick raises the interrupt, and a core
	// that flushes on a different schedule puts the interrupt on a different tick.
	emu_timer *stream_timer = nullptr;
	void stream_tick(s32);

	// MIDI.  The CPU core takes whole bytes through serial_w(), so unlike the MAME driver
	// there is no bit clock here -- but the SPACING still has to be the wire's, or the
	// firmware sees a burst it could never receive.  A byte is delivered when its stop bit
	// would have arrived, and consecutive bytes are held at least one byte time apart.
	static constexpr double MIDI_BYTE_SECONDS = 10.0 / double(MIDI_BAUD);
	// Queued in ABSOLUTE emulated time.  Block-relative offsets were rebased every block,
	// which was both fragile and one rounding step more than necessary.
	std::vector<u8> midi_in_queue;
	std::vector<attotime> midi_in_at;
	size_t midi_in_pos = 0;
	attotime midi_next_deliverable;

	attotime midi_due() const;
	void midi_deliver_due();
	std::vector<u8> midi_out_bytes;
	std::vector<u32> midi_out_at;

	// --- rendering -------------------------------------------------------------------
	u32 block_frames = 0;
	u64 frames_done = 0;        ///< absolute sample position; the audio timeline's anchor
	attotime block_start;
	bool started = false;

	void build();
	void wire_audio();
	void install_map();
	void render_block(float *const outs[kNumOutputs], u32 frames);
	void run_block(u32 frames);

	// --- LCD mirror ------------------------------------------------------------------
	// msm6222b keeps DDRAM private and renders glyphs, but the UI needs CHARACTER CODES
	// and the live CGRAM (PLUGIN-PLAN.md section 7: the boot logo is a CGRAM animation, so
	// the glyphs cannot be baked).  The controller's interface is two registers, so the
	// cheapest correct answer is to follow the same address counter the chip does.
	u8 lcd_ddram[80] = { 0 };
	u8 lcd_cgram[64] = { 0 };
	u8 lcd_adc = 0x80;
	bool lcd_cgram_dirty = false;
	void lcd_track_ctrl(u8 data);
	void lcd_track_data(u8 data);
	std::string lcd_line(int n) const;

	// handlers -- one per line in the driver's mem_map
	void lcd_ctrl_w(u8 data);
	void lcd_data_w(u8 data);
	void lcd_int_cb(s32);
	u8   switch_r();
	void led_w(offs_t offset, u16 data, u16 mem_mask);
	u16  snd_r(offs_t offset, u16 mem_mask);
	void snd_w(offs_t offset, u16 data, u16 mem_mask);
	u8   pcm_data_r();
	void out_ctrl_w();
	void update_voice_routing();
	void port2_w(u8 data);
	void midi_tx_w(u8 data);
};


/***************************************************************************************
    Construction
***************************************************************************************/

void U110Core::Impl::build()
{
	if (started)
		return;

	device_sound_interface::s_default_rate = kCoreSampleRate;

	// --- the tone generator.  Every setting here is the driver's; see roland_u110.cpp
	// for why each one is what it is.
	pcm.set_pcm_mode(mb87419_mb87420_device::PCM_FLOAT8_DELTA);
	pcm.set_delta_dc_hz(0.0);
	pcm.set_rate_divider(PCM_DIV);
	pcm.set_output_count(int(kNumOutputs));
	pcm.set_env_engine(true);
	pcm.set_env_max_int_hz(kCoreSampleRate);
	pcm.set_env_release_db_per_s(127.0);
	pcm.set_env_decay_db_per_s(0.0);
	pcm.set_rom(pcmrom.data(), pcmrom.size());
	pcm.int_callback().set([this](int state) {
		if (state) extints ++;
		cpu.set_input_line(i8x9x_device::EXTINT_LINE, state);
	});

	// --- the CPU's ports
	cpu.in_p1_cb().set([this]() -> u8 { return 0xf0 | card_present; });
	cpu.out_p2_cb().set([this](u8 data) { port2_w(data); });
	cpu.ach0_cb().set([]() -> u16 { return 0x2a0; });      // battery sense
	cpu.serial_tx_cb().set([this](u8 data) { midi_tx_w(data); });

	// --- the filter chain, from the schematic's component values
	for (int ch = 0; ch < 2; ch ++)
	{
		sk1[ch].opamp_sk_lowpass_setup(SK1_R1, SK1_R2, SK_R3, SK_R4, SK1_C1, SK1_C2);
		sk2[ch].opamp_sk_lowpass_setup(SK2_R1, SK2_R2, SK_R3, SK_R4, SK2_C1, SK2_C2);
		rc[ch].set_lowpass(RC_R, RC_C);
		eq[ch].setup(filter_biquad_device::biquad_type::PEAK, EQ_FC, EQ_Q, EQ_GAIN);
	}

	install_map();

	// Build the CPU's two spaces before it starts -- device_start() reads both, and
	// resolves required_shared_ptr("register_file") out of the register map.
	{
		const auto configs = cpu.space_configs();
		cpu.set_space(AS_PROGRAM, &program);

		for (const auto &c : configs)
		{
			if (c.first != AS_DATA || !c.second->m_internal_map)
				continue;
			regs_map.reset(new address_map(cpu, nullptr));
			c.second->m_internal_map(*regs_map);
			regs_map->allocate();
			regs.reset(new address_space(0x100, c.second->m_databus_width));
			regs->install_map(0x00, 0xff, regs_map.get());
			cpu.set_space(AS_DATA, regs.get());
		}
		cpu.set_share_provider([this](const char *tag) -> std::pair<void *, size_t> {
			auto found = regs_map ? regs_map->find_share(tag)
								  : std::pair<u8 *, size_t>{ nullptr, 0 };
			return { found.first, found.second };
		});
	}

	// Touch every trace flag here, so its one-time getenv and its static-init guard happen
	// on THIS thread rather than the first time the audio callback reaches them.
	(void)trace_u110_lcdtrace(); (void)trace_u110_envtrace(); (void)trace_u110_tgtrace();
	(void)trace_u110_miditrace(); (void)trace_u110_cycles();

	// Reserve the MIDI queues once.  They are pushed to from the audio thread, and a
	// vector that grows there is an allocation on the render path.
	midi_in_queue.reserve(1024);
	midi_in_at.reserve(1024);
	midi_out_bytes.reserve(1024);
	midi_out_at.reserve(1024);

	lcd_timer = machine.scheduler().alloc([this](s32 p) { lcd_int_cb(p); });
	stream_timer = machine.scheduler().alloc([this](s32 p) { stream_tick(p); });

	pcm.start();
	cpu.start();
	lcd.start();
	for (int ch = 0; ch < 2; ch ++)
	{
		sk1[ch].start(); sk2[ch].start(); rc[ch].start(); eq[ch].start();
	}

	wire_audio();

	const attotime streams_update = attotime::from_hz(u32(50));
	stream_timer->adjust(streams_update, 0, streams_update);

	started = true;
}

// pcm's six Multi Outputs -> the pan matrix -> two identical filter chains.
//
// The hardware has six chains, one per jack.  They are identical and the filter is
// linear, so summing first and filtering the two mix buses is equivalent -- two chains
// instead of eighteen.  The driver does exactly this, which is what lets the null test
// compare like with like.
void U110Core::Impl::wire_audio()
{
	for (int ch = 0; ch < 2; ch ++)
	{
		for (int out = 0; out < int(kNumOutputs); out ++)
			sk1[ch].stream()->connect(0, pcm.stream(), out,
					float(ch ? PAN_R[out] : PAN_L[out]));
		sk2[ch].stream()->connect(0, sk1[ch].stream(), 0, 1.0f);
		rc[ch].stream()->connect(0, sk2[ch].stream(), 0, 1.0f);
		eq[ch].stream()->connect(0, rc[ch].stream(), 0, 1.0f);
	}
}

void U110Core::Impl::install_map()
{
	// The driver's mem_map, line for line.  0x0000-0x0fff and 0x2000-0x20ff are separate
	// entries because the gap between them is not mapped on the real machine either.
	program.install_rom(0x0000, 0x0fff, program_rom.data() + 0x0000);

	program.install_handler(0x1100, 0x1101, nullptr,
			[this](offs_t o, u8 d) { if (o == 0) lcd_ctrl_w(d); });
	program.install_handler(0x1102, 0x1103, nullptr,
			[this](offs_t o, u8 d) { if (o == 0) lcd_data_w(d); });
	program.install_handler16(0x1200, 0x1201, nullptr,
			[this](offs_t o, u16 d, u16 m) { led_w(o, d, m); });
	program.install_handler(0x1300, 0x1301,
			[this](offs_t o) -> u8 { return o == 0 ? switch_r() : 0xff; }, nullptr);
	program.install_handler16(0x1400, 0x143f,
			[this](offs_t o, u16 m) -> u16 { return snd_r(o, m); },
			[this](offs_t o, u16 d, u16 m) { snd_w(o, d, m); });
	program.install_handler16(0x1f00, 0x1f0f, nullptr,
			[this](offs_t, u16, u16) { out_ctrl_w(); });

	program.install_rom(0x2000, 0x20ff, program_rom.data() + 0x2000);
	program.install_ram(0x2100, 0x3fff, workram.data());
	program.install_rom(0x4000, 0xdfff, program_rom.data() + 0x4000);

	// 0xE000-0xFFFF is bank switched on P2.7: the battery-backed patch store, or the
	// EPROM's factory defaults.  The firmware selects the EPROM in exactly one place,
	// the "Mem Initialized" copy loop at 0x8475.
	program.install_handler(0xe000, 0xffff,
			[this](offs_t o) -> u8 {
				return patch_view_rom ? program_rom[0xe000 + o] : patchram[o];
			},
			[this](offs_t o, u8 d) { if (!patch_view_rom) patchram[o] = d; });
}


/***************************************************************************************
    Handlers
***************************************************************************************/

// The address counter, decoded exactly as msm6222b_device does it: bit 7 selects DDRAM,
// and the counter auto-increments after every data write so a string is one command
// followed by a burst of bytes.
void U110Core::Impl::lcd_track_ctrl(u8 data)
{
	int cmd;
	for (cmd = 7; cmd >= 0 && !(data & (1 << cmd)); cmd --) { }
	switch (cmd)
	{
	case 0:                                   // clear display
		std::fill(std::begin(lcd_ddram), std::end(lcd_ddram), 0x20);
		lcd_adc = 0x80;
		break;
	case 1:                                   // return home
		lcd_adc = 0x80;
		break;
	case 6:                                   // set CGRAM address
		lcd_adc = data & 0x3f;
		break;
	case 7:                                   // set DDRAM address
		lcd_adc = data;
		break;
	default:
		break;
	}
}

void U110Core::Impl::lcd_track_data(u8 data)
{
	if (lcd_adc & 0x80)
	{
		u8 adr = lcd_adc & 0x7f;
		// Two-line layout: line 2 starts at 0x40 and folds down to 40-79.
		if (adr < 40)                        lcd_ddram[adr] = data;
		else if (adr >= 0x40 && adr < 0x40 + 40) lcd_ddram[adr - 0x40 + 40] = data;
		lcd_adc = 0x80 | ((adr + 1) & 0x7f);
	}
	else
	{
		if (lcd_adc < 64)
		{
			lcd_cgram[lcd_adc] = data;
			lcd_cgram_dirty = true;
		}
		lcd_adc = (lcd_adc + 1) & 0x3f;
	}
}

std::string U110Core::Impl::lcd_line(int n) const
{
	std::string s;
	for (int i = 0; i < 16; i ++)
	{
		const u8 c = lcd_ddram[n * 40 + i];
		s += (c >= 0x20 && c < 0x7f) ? char(c) : '.';
	}
	return s;
}

void U110Core::Impl::lcd_ctrl_w(u8 data)
{
	lcd_track_ctrl(data);
	if (trace_u110_lcdtrace())
		std::fprintf(stderr, "LCD %f cmd %02X\n", machine.time().as_double(), data);
	lcd.control_w(data);
	lcd_timer->adjust(attotime::from_usec(40));
}

void U110Core::Impl::lcd_data_w(u8 data)
{
	lcd_track_data(data);
	if (trace_u110_lcdtrace())
		std::fprintf(stderr, "LCD %f chr %02X '%c'\n", machine.time().as_double(), data,
				(data >= 0x20 && data < 0x7f) ? char(data) : '.');
	lcd.data_w(data);
	lcd_timer->adjust(attotime::from_usec(40));
}

// The controller-ready pulse on HSI.0.  The firmware's handler at 0x4032 drains a
// 32-entry text ring; starve it and the firmware wedges at 0xD2F6.
void U110Core::Impl::lcd_int_cb(s32)
{
	cpu.set_input_line(i8x9x_device::HSI0_LINE, ASSERT_LINE);
	cpu.set_input_line(i8x9x_device::HSI0_LINE, CLEAR_LINE);
}

u8 U110Core::Impl::switch_r()
{
	return switches;                 // active low; bit clear = pressed
}

void U110Core::Impl::led_w(offs_t, u16 data, u16 mem_mask)
{
	leds = (leds & ~mem_mask) | (data & mem_mask);
}

void U110Core::Impl::port2_w(u8 data)
{
	if (BIT(data ^ port2, 7))
		patch_view_rom = BIT(data, 7);
	port2 = data;
}

// The CPU has no bus of its own to the wave ROMs.  It borrows voice 0 of the tone
// generator as a read engine: park that voice's phase accumulator at the wanted address,
// busy-wait, then read the byte back.  See ROM-ANALYSIS.md 6.1.
u8 U110Core::Impl::pcm_data_r()
{
	u32 const phase = snd_regs[0x08] | (snd_regs[0x09] << 8)
					| (snd_regs[0x0a] << 16) | (snd_regs[0x0b] << 24);
	u16 const bank  = snd_regs[0x02] | (snd_regs[0x03] << 8);

	// The address must wrap within the accumulator before the bank is applied -- without
	// the 18-bit wrap every card reports "  Illegal CARD".
	offs_t addr = (((phase >> 14) + PCM_READ_PREFETCH) & 0x3ffff) | ((bank & 0x3c00) << 8);
	return pcmrom[addr & 0x3fffff];
}

// A word offset here is a REGISTER NUMBER: register n lives at 0x1400 + 2n, so the low
// lane of word offset n is register n and the high lane is register n+1.  Copied from the
// driver's snd_r/snd_w, including the exceptions -- get any of this wrong and the machine
// still boots, which is precisely why it has to be copied rather than reasoned out.
u16 U110Core::Impl::snd_r(offs_t offset, u16 mem_mask)
{
	u16 result = 0;

	if (mem_mask & 0x00ff)
	{
		if (offset == 0x01)
			{ rom_reads ++; result |= pcm_data_r(); }   // the CPU's wave-ROM read port
		else if (offset == 0x00)
		{
			const u8 v = pcm.read(0x00);
			if (trace_u110_envtrace())
				std::fprintf(stderr, "ENVRD %f -> v%02X\n", machine.time().as_double(), v);
			return v;                               // NOT offset+1: 01 is the read port
		}
		else if (offset == 0x02)
			return u16(pcm.read(0x02)) | (u16(pcm.read(0x03)) << 8);
		else
			result |= snd_regs[offset];
	}
	if ((mem_mask & 0xff00) && offset < 0x1f)
		result |= u16(snd_regs[offset + 1]) << 8;

	return result;
}

void U110Core::Impl::snd_w(offs_t offset, u16 data, u16 mem_mask)
{
	if (mem_mask & 0x00ff)
	{
		snd_regs[offset] = data & 0xff;
		tg_writes ++;
		if (trace_u110_tgtrace())
			std::fprintf(stderr, "TG %f v%02X reg %02X = %02X cyc %llu\n",
					machine.time().as_double(), snd_regs[0x1f] & 0x1f,
					unsigned(offset), data & 0xff,
					(unsigned long long)cpu.total_cycles());
		pcm.write(offset, data & 0xff);
	}

	// Registers 10, 12, 16 and 1A take no high lane at all.
	if (offset == 0x10 || offset == 0x12 || offset == 0x16 || offset == 0x1a)
		return;

	if ((mem_mask & 0xff00) && offset < 0x1f)
	{
		offs_t const hi = (offset == 0x11 || offset == 0x15) ? offset + 2 : offset + 1;
		snd_regs[offset + 1] = data >> 8;
		if (trace_u110_tgtrace())
			std::fprintf(stderr, "TG %f v%02X reg %02X = %02X cyc %llu\n",
					machine.time().as_double(), snd_regs[0x1f] & 0x1f,
					unsigned(offset + 1), data >> 8,
					(unsigned long long)cpu.total_cycles());
		pcm.write(hi, data >> 8);
	}
}

void U110Core::Impl::out_ctrl_w() { update_voice_routing(); }

void U110Core::Impl::update_voice_routing()
{
	u8 const idx = program.read_byte(RAM_OUTPUT_MODE_INDEX);
	u8 mask[NUM_VOICE_SLOTS];
	voice_masks_for_mode(idx, mask);
	for (int v = 0; v < NUM_VOICE_SLOTS; v ++)
		pcm.set_voice_mask(v, mask[v]);
}

// When the head byte of the queue may be handed to the CPU, or never if there is none.
attotime U110Core::Impl::midi_due() const
{
	if (midi_in_pos >= midi_in_queue.size())
		return attotime::never();
	attotime const arrival = midi_in_at[midi_in_pos]
			+ attotime::from_double(MIDI_BYTE_SECONDS);
	return (midi_next_deliverable > arrival) ? midi_next_deliverable : arrival;
}

void U110Core::Impl::midi_deliver_due()
{
	while (midi_in_pos < midi_in_queue.size() && machine.time() >= midi_due())
	{
		// Space the next byte from when this one was DUE, not from when we noticed it had
		// come due.  Noticing happens on a slice boundary, so measuring from there makes
		// delivery depend on the audio block size -- which broke the "identical at any
		// block size" property the null test rests on.
		const attotime due = midi_due();
		const u8 byte = midi_in_queue[midi_in_pos ++];
		if (trace_u110_miditrace())
			std::fprintf(stderr, "MIDI IN %f %02X   (due %f)\n",
					machine.time().as_double(), byte, due.as_double());
		cpu.serial_w(byte);
		midi_next_deliverable = due + attotime::from_double(MIDI_BYTE_SECONDS);
	}
}

void U110Core::Impl::midi_tx_w(u8 data)
{
	// The plugin hands the host whole bytes, so unlike the MAME driver there is no bit
	// clock here -- serialising and immediately deserialising would only lose timing.
	midi_out_bytes.push_back(data);
	const double into_block = (machine.time() - block_start).as_double();
	midi_out_at.push_back(u32(into_block > 0.0 ? into_block * kCoreSampleRate : 0.0));
}


/***************************************************************************************
    Rendering
***************************************************************************************/

// The 50 Hz flush.  Same set of streams MAME's sound_manager walks.
void U110Core::Impl::stream_tick(s32)
{
	pcm.stream()->update();
	for (int ch = 0; ch < 2; ch ++)
	{
		sk1[ch].stream()->update();
		sk2[ch].stream()->update();
		rc[ch].stream()->update();
		eq[ch].stream()->update();
	}
}

void U110Core::Impl::run_block(u32 frames)
{
	// Anchor the block to the ABSOLUTE sample grid, not to machine.time().
	//
	// A slice ends on an instruction boundary, so the CPU always overshoots the end of a
	// block slightly.  Taking the next block's start from machine.time() lets that
	// overshoot accumulate, and how much it accumulates depends on the block size -- which
	// destroys the "identical at any block size" property in section 4 and, with it, the
	// null test.  MAME's scheduler time is absolute and its streams are sample-indexed;
	// this is the same thing.  The CPU may run past block_end; the next block simply asks
	// it to run correspondingly less.
	block_start = attotime::from_ticks(frames_done, kCoreSampleRate);
	const attotime block_end =
			attotime::from_ticks(frames_done + frames, kCoreSampleRate);

	sound_stream *streams[] = { pcm.stream(),
			sk1[0].stream(), sk1[1].stream(), sk2[0].stream(), sk2[1].stream(),
			rc[0].stream(), rc[1].stream(), eq[0].stream(), eq[1].stream() };
	for (sound_stream *s : streams)
		s->begin_block(frames, block_start);

	// Run the CPU in slices bounded by the next timer event, exactly as MAME's scheduler
	// does.  Time advances INSIDE a slice too (see device_scheduler::time), so a register
	// write mid-slice still lands on the right sample.
	//
	// Time advances by the cycles the CPU ACTUALLY consumed, not by the cycles asked for.
	// An instruction straddling the end of a slice overruns it, and if the clock were
	// simply set to the requested target those cycles would be free -- total_cycles()
	// would run ahead of machine time.  The i8x9x's timers are derived from
	// total_cycles() while the scheduler runs on machine time, so any drift between them
	// makes the firmware's timed waits come out wrong: the symptom was the LCD queue
	// draining one character every 320 ms instead of in bursts.
	while (machine.time() < block_end)
	{
		midi_deliver_due();
		const attotime now = machine.time();

		attotime target = machine.scheduler().next_event();
		const attotime midi = midi_due();
		if (midi < target)
			target = midi;
		if (block_end < target)
			target = block_end;
		if (target <= now)
		{
			machine.scheduler().advance_to(target);
			continue;
		}

		u64 want = cpu.attotime_to_cycles(target - now);
		if (want == 0)
			want = 1;
		const int used = cpu.run_cycles(int(want));
		machine.scheduler().advance_to(now + cpu.cycles_to_attotime(used > 0 ? u64(used) : 0));
	}
	midi_deliver_due();
	frames_done += frames;

	// Drop what has been delivered.  Times are absolute, so nothing needs rebasing.
	if (midi_in_pos)
	{
		midi_in_queue.erase(midi_in_queue.begin(), midi_in_queue.begin() + midi_in_pos);
		midi_in_at.erase(midi_in_at.begin(), midi_in_at.begin() + midi_in_pos);
		midi_in_pos = 0;
	}

	if (trace_u110_cycles())
	{
		static u64 last_cycles = 0; static double last_t = 0; static u64 last_tg = 0;
		const double t = machine.time().as_double();
		if (t - last_t >= 1.0)
		{
			const u64 c = cpu.total_cycles();
			std::fprintf(stderr, "  t=%6.2f s  cycles/s = %.0f   TG writes = %llu"
					"   romreads = %llu   extint = %llu\n", t,
					double(c - last_cycles) / (t - last_t),
					(unsigned long long)(tg_writes - last_tg),
					(unsigned long long)rom_reads, (unsigned long long)extints);
			last_cycles = c; last_t = t; last_tg = tg_writes;
			rom_reads = 0; extints = 0;
		}
	}

	// Flush the graph to the end of the block and hand back the two mix buses.
	for (sound_stream *s : streams)
		s->finish_block();

}

void U110Core::Impl::render_block(float *const outs[kNumOutputs], u32 frames)
{
	if (!started || !have_program || frames == 0)
	{
		for (unsigned o = 0; o < kNumOutputs; o ++)
			std::fill_n(outs[o], frames, 0.0f);
		return;
	}
	run_block(frames);
	for (unsigned o = 0; o < kNumOutputs; o ++)
	{
		const auto &src = pcm.stream()->output(o);
		std::copy(src.begin(), src.begin() + frames, outs[o]);
	}
}


/***************************************************************************************
    Public interface
***************************************************************************************/

U110Core::U110Core() : m_impl(new Impl) { }
U110Core::~U110Core() = default;

LoadResult U110Core::loadProgramRom(const uint8_t *data, size_t len)
{
	if (!data || len != PROGRAM_ROM_SIZE)
		return LoadResult::WrongSize;
	std::memcpy(m_impl->program_rom.data(), data, len);
	m_impl->have_program = true;
	m_impl->build();
	return LoadResult::Ok;
}

LoadResult U110Core::loadWaveRom(unsigned bank, const uint8_t *data, size_t len)
{
	if (bank >= kNumWaveBanks)
		return LoadResult::NoSuchSlot;
	if (!data || len != kCardBytes)
		return LoadResult::WrongSize;
	descramble_pcm(&m_impl->pcmrom[bank * CARD_STRIDE], data, CARD_SIZE);
	return LoadResult::Ok;
}

LoadResult U110Core::loadCard(unsigned slot, const uint8_t *data, size_t len)
{
	if (slot >= kNumCardSlots)
		return LoadResult::NoSuchSlot;
	u8 *const dst = &m_impl->pcmrom[slot * CARD_STRIDE + CARD_OFFSET];
	if (!data)
	{
		std::fill_n(dst, CARD_SIZE, u8(0xff));
		m_impl->card_present |= 1 << slot;
		return LoadResult::Ok;
	}
	if (len > CARD_SIZE)
		return LoadResult::WrongSize;

	// Undersized dumps are mirrored up.  Address descrambling means this can only be done
	// in 128K pages, which is why the mirror is rounded down to a power of two and floored.
	std::vector<u8> image(CARD_SIZE, 0xff);
	std::memcpy(image.data(), data, len);
	if (len < CARD_SIZE)
	{
		size_t mirror = 1;
		while (mirror * 2 <= len) mirror *= 2;
		if (mirror < 0x20000) mirror = 0x20000;
		for (size_t ofs = mirror; ofs < CARD_SIZE; ofs += mirror)
			std::memcpy(image.data() + ofs, image.data(), mirror);
	}
	descramble_pcm(dst, image.data(), CARD_SIZE);
	m_impl->card_present &= ~(1 << slot);
	return LoadResult::Ok;
}

void U110Core::reset()
{
	if (!m_impl->started)
		return;
	m_impl->port2 = 0;
	m_impl->patch_view_rom = false;
	std::fill(std::begin(m_impl->snd_regs), std::end(m_impl->snd_regs), 0);
	m_impl->pcm.reset();
	m_impl->cpu.reset();
	m_impl->lcd.reset();
}

void U110Core::render(float *const outs[kNumOutputs], uint32_t nframes)
{
	m_impl->render_block(outs, nframes);
}

void U110Core::renderStereo(float *left, float *right, uint32_t nframes)
{
	if (!m_impl->started || !m_impl->have_program || nframes == 0)
	{
		std::fill_n(left, nframes, 0.0f);
		std::fill_n(right, nframes, 0.0f);
		return;
	}
	m_impl->run_block(nframes);
	const auto &l = m_impl->eq[0].stream()->output(0);
	const auto &r = m_impl->eq[1].stream()->output(0);
	std::copy(l.begin(), l.begin() + nframes, left);
	std::copy(r.begin(), r.begin() + nframes, right);
}

void U110Core::midiIn(const uint8_t *bytes, size_t n, uint32_t sampleOffset)
{
	const attotime when = attotime::from_ticks(m_impl->frames_done + sampleOffset,
			kCoreSampleRate);
	for (size_t i = 0; i < n; i ++)
	{
		m_impl->midi_in_queue.push_back(bytes[i]);
		m_impl->midi_in_at.push_back(when);
	}
}

void U110Core::midiInAtTime(const uint8_t *bytes, size_t n, double seconds)
{
	const attotime when = attotime::from_double(seconds);
	for (size_t i = 0; i < n; i ++)
	{
		m_impl->midi_in_queue.push_back(bytes[i]);
		m_impl->midi_in_at.push_back(when);
	}
}

size_t U110Core::midiOut(uint8_t *buf, size_t cap, uint32_t *offsets)
{
	const size_t n = std::min(cap, m_impl->midi_out_bytes.size());
	std::copy_n(m_impl->midi_out_bytes.begin(), n, buf);
	if (offsets)
		std::copy_n(m_impl->midi_out_at.begin(), n, offsets);
	m_impl->midi_out_bytes.erase(m_impl->midi_out_bytes.begin(),
			m_impl->midi_out_bytes.begin() + n);
	m_impl->midi_out_at.erase(m_impl->midi_out_at.begin(),
			m_impl->midi_out_at.begin() + n);
	return n;
}

void U110Core::setHfCorrection(bool on)
{
	if (!m_impl->started)
		return;
	// modify() flushes the stream before retuning, so the switch is clean mid-note.  Off
	// is an exact bypass: a PEAK biquad with gain 1.0 has identical numerator and
	// denominator, so the samples pass through untouched.
	for (int ch = 0; ch < 2; ch ++)
		m_impl->eq[ch].modify(filter_biquad_device::biquad_type::PEAK,
				EQ_FC, EQ_Q, on ? EQ_GAIN : 1.0);
}

void U110Core::setButton(Button sw, bool down)
{
	if (sw < 0 || sw >= kButtonCount)
		return;
	if (down) m_impl->switches &= u8(~(1 << sw));
	else      m_impl->switches |=  u8(1 << sw);
}

uint8_t U110Core::readMem(uint16_t addr) const
{ return m_impl->program.read_byte(addr); }

void U110Core::writeMem(uint16_t addr, uint8_t value)
{ m_impl->program.write_byte(addr, value); }

void U110Core::runUntilIdle(uint32_t timeoutMs)
{
	// Render into a scratch buffer and throw it away: the point is emulated time, and
	// nothing forces realtime here.
	std::vector<float> scratch(1024 * kNumOutputs);
	float *outs[kNumOutputs];
	for (unsigned o = 0; o < kNumOutputs; o ++)
		outs[o] = scratch.data() + o * 1024;
	const uint32_t blocks = (timeoutMs * kCoreSampleRate / 1000) / 1024;
	for (uint32_t i = 0; i < blocks; i ++)
		m_impl->render_block(outs, 1024);
}

void U110Core::snapshot(PanelState &out) const
{
	std::memset(&out, 0, sizeof(out));
	out.version = PanelState::kVersion;
	std::memcpy(out.lcd, m_impl->lcd_ddram, 16);
	std::memcpy(out.lcd + 16, m_impl->lcd_ddram + 40, 16);
	std::memcpy(out.cgram, m_impl->lcd_cgram, sizeof(out.cgram));
	out.cgram_dirty = m_impl->lcd_cgram_dirty;
	// All three lamps are driven ACTIVE LOW, so all three are inverted here.
	//
	// The two panel lamps are written inverted by the firmware itself.  The MIDI lamp is
	// CPU port 2 bit 6 through a 2SA1115, and 2SA is the JIS prefix for a PNP transistor:
	// it conducts when its base is pulled LOW, so a zero on P2.6 lights the LED.  Observed
	// on the running plugin before the part number was checked, and the part agrees.
	out.leds = u8((~m_impl->leds & 0x03) | (BIT(m_impl->port2, 6) ? 0x00 : 0x04));
	out.card_present = u8(~m_impl->card_present & 0x0f);
}

// What persists is the NVRAM, and only the NVRAM.
//
// The real U-110 is battery backed: IC10 holds the work and setup RAM, IC11 the 64 user
// patches, and everything else comes back from ROM when the power does.  So the honest
// model for a DAW session is the same one the hardware uses -- keep the two RAMs, and let
// the firmware boot from them.  That is why this does not try to be a MAME-style machine
// snapshot: a snapshot would also have to carry the CPU's registers, every device's
// internal state and the exact scheduler phase, and it would break whenever any of that
// changed.  The NVRAM layout is fixed by the hardware and cannot.
//
// The cost is that restoring reboots the machine, which is exactly what happens when a
// U-110 is switched off and on.

namespace {
constexpr u32 kStateMagic = 0x55313130;      // "U110"
constexpr u32 kStateVersion = 1;
struct StateHeader
{
	u32 magic, version;
	u32 workram_size, patchram_size;
};
}

size_t U110Core::saveState(uint8_t *buf, size_t cap) const
{
	const size_t need = sizeof(StateHeader) + WORKRAM_SIZE + PATCHRAM_SIZE;
	if (buf == nullptr || cap < need)
		return need;                         // caller may pass cap 0 to size the buffer

	StateHeader h { kStateMagic, kStateVersion, u32(WORKRAM_SIZE), u32(PATCHRAM_SIZE) };
	std::memcpy(buf, &h, sizeof(h));
	std::memcpy(buf + sizeof(h), m_impl->workram.data(), WORKRAM_SIZE);
	std::memcpy(buf + sizeof(h) + WORKRAM_SIZE, m_impl->patchram.data(), PATCHRAM_SIZE);
	return need;
}

bool U110Core::loadState(const uint8_t *buf, size_t n)
{
	if (buf == nullptr || n < sizeof(StateHeader))
		return false;

	StateHeader h;
	std::memcpy(&h, buf, sizeof(h));
	if (h.magic != kStateMagic || h.version != kStateVersion)
		return false;
	if (h.workram_size != WORKRAM_SIZE || h.patchram_size != PATCHRAM_SIZE)
		return false;
	if (n < sizeof(h) + h.workram_size + h.patchram_size)
		return false;

	std::memcpy(m_impl->workram.data(), buf + sizeof(h), WORKRAM_SIZE);
	std::memcpy(m_impl->patchram.data(), buf + sizeof(h) + WORKRAM_SIZE, PATCHRAM_SIZE);

	// Reboot into the restored memory.  The firmware caches the active patch into work RAM
	// at 0x2800, and the CPU's own registers are not restored, so resuming in place would
	// leave the machine half in one session and half in another.  Booting is both correct
	// and what the hardware does; nothing forces realtime, so 5.4 s of emulated boot costs
	// a fraction of a second.
	if (m_impl->started)
	{
		reset();
		runUntilIdle();
	}
	return true;
}

} // namespace voltaire
