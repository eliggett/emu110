// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
/***************************************************************************************

    The device interfaces the U-110's six device sources inherit from.

    Included at the end of emu.h; not to be included directly.  See the banner there for
    what this shim is and, more importantly, what it is not.

***************************************************************************************/

#ifndef VOLTAIRE_COMPAT_EMU_INTERFACES_H
#define VOLTAIRE_COMPAT_EMU_INTERFACES_H

#pragma once

/***************************************************************************************
    Sound

    MAME's stream system resamples and mixes an arbitrary graph.  This does neither: every
    stream in the U-110 runs at the chip's 32 kHz and the topology is fixed, wired by the
    core.  Dropping the resampler is not a shortcut -- it is what makes the null test
    meaningful, because a resampler in the path would put the two renders permanently a
    fraction of an LSB apart.

    PARTIAL UPDATES ARE LOAD-BEARING.  roland_lp calls m_stream->update() before it acts
    on a register write, so that the write takes effect on the correct SAMPLE rather than
    at the next block boundary.  Ignoring that would still sound fine and would never be
    bit-identical.  So a stream renders up to the current scheduler time on demand, and
    presents the caller a WINDOW covering only the samples not yet generated -- which is
    exactly what MAME hands to sound_stream_update().
***************************************************************************************/

constexpr u32 SAMPLE_RATE_OUTPUT_ADAPTIVE = 0xffffffff;
constexpr u32 SAMPLE_RATE_INPUT_ADAPTIVE  = 0xfffffffe;

class device_sound_interface;

class sound_stream
{
public:
	using sample_t = float;

	// --- the API the device sources use -------------------------------------------
	int samples() const { return int(m_window_len); }
	u32 sample_rate() const { return m_rate; }

	sample_t get(int input, int index) const
	{ return m_in[input][m_window_start + index]; }

	void put(int output, int index, sample_t value)
	{ m_out[output][m_window_start + index] = value; }

	void add(int output, int index, sample_t value)
	{ m_out[output][m_window_start + index] += value; }

	void fill(int output, sample_t value)
	{ std::fill_n(m_out[output].begin() + m_window_start, m_window_len, value); }

	int input_count() const { return int(m_in.size()); }
	int output_count() const { return int(m_out.size()); }

	/// Catch up to the current scheduler time.  Cheap and idempotent when already there.
	void update();

	// --- wiring, done by the core --------------------------------------------------
	void connect(int input, sound_stream *src, int src_output, float gain = 1.0f);
	const std::vector<sample_t> &output(int n) const { return m_out[n]; }

	void begin_block(u32 frames, attotime block_start);
	void render_to(u32 target);              ///< generate up to `target` samples this block
	void finish_block() { render_to(m_frames); }

private:
	friend class device_sound_interface;
	sound_stream(device_sound_interface *owner, int inputs, int outputs, u32 rate);

	struct link { sound_stream *src; int out; float gain; };

	device_sound_interface *m_owner;
	u32 m_rate;
	std::vector<std::vector<sample_t>> m_in, m_out;
	std::vector<std::vector<link>> m_links;
	u32 m_frames = 0;            ///< block length
	u32 m_done = 0;              ///< samples generated so far this block
	u32 m_window_start = 0, m_window_len = 0;
	attotime m_block_start;
};

class device_sound_interface : public device_interface
{
public:
	explicit device_sound_interface(const machine_config &mconfig, device_t &device)
		: device_interface(device) { }

	virtual void sound_stream_update(sound_stream &stream) = 0;

	sound_stream *stream() const { return m_streams.empty() ? nullptr : m_streams[0].get(); }

protected:
	sound_stream *stream_alloc(int inputs, int outputs, u32 rate)
	{
		if (rate == SAMPLE_RATE_OUTPUT_ADAPTIVE || rate == SAMPLE_RATE_INPUT_ADAPTIVE)
			rate = s_default_rate;
		m_streams.emplace_back(new sound_stream(this, inputs, outputs, rate));
		return m_streams.back().get();
	}

public:
	/// The one rate everything runs at.  Set by the core before any device starts.
	static u32 s_default_rate;

private:
	std::vector<std::unique_ptr<sound_stream>> m_streams;
};


/***************************************************************************************
    Memory

    address_space here is a flat callback-per-region map, not MAME's handler tree.  The
    CPU core only ever calls read/write byte and word on it.
***************************************************************************************/

constexpr int AS_PROGRAM = 0;
constexpr int AS_DATA    = 1;
constexpr int AS_IO      = 2;

enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };

class address_map;

class address_space_config
{
public:
	address_space_config() = default;
	address_space_config(const char *name, endianness_t endian, u8 datawidth, u8 addrwidth,
			s8 addrshift = 0)
		: m_name(name), m_endianness(endian), m_databus_width(datawidth)
		, m_addrbus_width(addrwidth), m_addr_shift(addrshift) { }

	// The internal-map form.  The map constructor is KEPT, not discarded: the i8x9x
	// declares its whole register file this way, and the core has to run it to build that
	// space.  address_map_constructor converts implicitly, being callable with an
	// address_map &.
	template <typename T>
	address_space_config(const char *name, endianness_t endian, u8 datawidth, u8 addrwidth,
			s8 addrshift, T &&map)
		: m_name(name), m_endianness(endian), m_databus_width(datawidth)
		, m_addrbus_width(addrwidth), m_addr_shift(addrshift)
		, m_internal_map(std::forward<T>(map)) { }

	const char *m_name = "";
	endianness_t m_endianness = ENDIANNESS_LITTLE;
	u8 m_databus_width = 8;
	u8 m_addrbus_width = 16;
	s8 m_addr_shift = 0;
	std::function<void(address_map &)> m_internal_map;
};

using space_config_vector = std::vector<std::pair<int, const address_space_config *>>;

// Address-translation intentions, from MAME's dimemory.h.
enum { TR_READ, TR_WRITE, TR_FETCH };

/// A flat memory space.  The core installs handlers; the CPU reads and writes through it.
class address_space
{
public:
	using read8_cb  = std::function<u8(offs_t)>;
	using write8_cb = std::function<void(offs_t, u8)>;

	address_space(u32 size, u8 data_width) : m_size(size), m_data_width(data_width) { }

	u8 data_width() const { return m_data_width; }

	// MAME's caches are lookup accelerators; here they just hold a pointer to us.
	template <typename C> void cache(C &c) { c.set(this); }
	template <typename S> void specific(S &s) { s.set(this); }

	u8   read_byte(offs_t a) const;
	u16  read_word(offs_t a) const;                  // little-endian pair of bytes
	void write_byte(offs_t a, u8 v);
	void write_word(offs_t a, u16 v);

	// The core's wiring.  Ranges are inclusive and are matched in installation order,
	// last match winning -- which is what lets a view be overlaid on RAM.
	void install_rom(offs_t start, offs_t end, const u8 *base);
	void install_ram(offs_t start, offs_t end, u8 *base);
	void install_handler(offs_t start, offs_t end, read8_cb r, write8_cb w);

	// A 16-bit device on the 16-bit bus.  The offset handed to the callback is a WORD
	// offset and the mask says which lane is live, exactly as MAME's ACCESSING_BITS_*
	// expect -- a byte access to an odd address arrives as mask 0xff00 with the data in
	// the high lane.  Getting this wrong would still boot and would not be bit-identical.
	using read16_cb  = std::function<u16(offs_t, u16)>;
	using write16_cb = std::function<void(offs_t, u16, u16)>;
	void install_handler16(offs_t start, offs_t end, read16_cb r, write16_cb w);

	/// Forward a range into an address_map built by a device's own internal map.
	void install_map(offs_t start, offs_t end, address_map *map);

	void unmap(offs_t start, offs_t end);

private:
	struct region { offs_t start, end; const u8 *rom; u8 *ram;
			read8_cb r; write8_cb w; read16_cb r16; write16_cb w16; address_map *map; };
	const region *find(offs_t a) const;

	u32 m_size;
	u8 m_data_width = 8;
	std::vector<region> m_regions;
};

class device_memory_interface : public device_interface
{
public:
	explicit device_memory_interface(const machine_config &mconfig, device_t &device)
		: device_interface(device) { }

	using space_config_vector = ::space_config_vector;
	virtual space_config_vector memory_space_config() const = 0;

	// mcs96 narrows memory_space_config() to protected, so the core reaches it through
	// this: access is checked against the base, and the call still dispatches virtually.
	space_config_vector space_configs() const { return memory_space_config(); }

	address_space &space(int index = 0) const { return *m_spaces.at(index); }
	bool has_space(int index = 0) const { return index < int(m_spaces.size()) && m_spaces[index]; }

	/// Called by the core once the space is built.
	void set_space(int index, address_space *s)
	{ if (int(m_spaces.size()) <= index) m_spaces.resize(index + 1); m_spaces[index] = s; }

	bool has_configured_map(int index = 0) const { return false; }

private:
	mutable std::vector<address_space *> m_spaces;
};


/***************************************************************************************
    device_rom_interface -- the sound chip's window onto the wave ROMs

    MAME's version sits on a real address space with banking.  The U-110 hands the chip a
    single flat 4 MB descrambled image, so this is a pointer and a mask.
***************************************************************************************/

template <int AddrWidth, int DataWidth = 0, int AddrShift = 0,
		endianness_t Endian = ENDIANNESS_LITTLE>
class device_rom_interface : public device_memory_interface
{
public:
	device_rom_interface(const machine_config &mconfig, device_t &device)
		: device_memory_interface(mconfig, device)
		, m_rom_config("rom", Endian, 8, AddrWidth, AddrShift) { }

	virtual space_config_vector memory_space_config() const override
	{ return space_config_vector{ std::make_pair(0, &m_rom_config) }; }

	/// The core points this at the descrambled PCM image.
	void set_rom(const u8 *base, size_t size) { m_base = base; m_size = size; }

	u8 read_byte(offs_t addr)
	{ return (m_base && addr < m_size) ? m_base[addr] : 0xff; }

	void set_rom_bank(int bank)
	{
		if (bank == m_bank) return;
		rom_bank_pre_change();
		m_bank = bank;
		rom_bank_post_change();
	}

protected:
	virtual void rom_bank_pre_change() { }
	virtual void rom_bank_post_change() { }

private:
	address_space_config m_rom_config;
	const u8 *m_base = nullptr;
	size_t m_size = 0;
	int m_bank = 0;
};


/***************************************************************************************
    Execution
***************************************************************************************/

constexpr int CLEAR_LINE  = 0;
constexpr int ASSERT_LINE = 1;
constexpr int HOLD_LINE   = 2;
constexpr int INPUT_LINE_NMI  = -1;
constexpr int INPUT_LINE_IRQ0 = 0;

class device_execute_interface : public device_interface
{
public:
	explicit device_execute_interface(const machine_config &mconfig, device_t &device)
		: device_interface(device) { }

	// --- what the CPU core calls ---------------------------------------------------
	virtual void execute_run() = 0;
	virtual void execute_set_input(int line, int state) { }
	virtual u32 execute_min_cycles() const { return 1; }
	virtual u32 execute_max_cycles() const { return 1; }
	virtual u64 execute_clocks_to_cycles(u64 clocks) const { return clocks; }
	virtual u64 execute_cycles_to_clocks(u64 cycles) const { return cycles; }

	void set_icountptr(int &icount) { m_icountptr = &icount; }
	int *icountptr() const { return m_icountptr; }

	u64 total_cycles() const { return m_totalcycles + (m_running ? (m_cycles_this_slice - (m_icountptr ? *m_icountptr : 0)) : 0); }

	attotime cycles_to_attotime(u64 cycles) const;
	u64 attotime_to_cycles(attotime t) const;

	void suspend(u32 reason, bool eatcycles = false) { m_suspended = true; if (eatcycles && m_icountptr) *m_icountptr = 0; }
	void resume(u32 reason) { m_suspended = false; }
	bool suspended(u32 reason = ~0u) const { return m_suspended; }
	void spin_until_time(attotime t) { }
	void eat_cycles(int cycles) { if (m_icountptr) *m_icountptr -= cycles; }

	int standard_irq_callback(int irqline, offs_t pc) { return 0; }

	// There is no debugger here, so these compile to nothing and the CPU's inner loop
	// keeps its shape.  The ImGui debug window (PLUGIN-PLAN.md section 5) reads state
	// through the core's own accessors instead.
	bool debugger_enabled() const { return false; }
	void debugger_instruction_hook(offs_t pc) { }
	void debugger_exception_hook(int exception) { }
	void debugger_privilege_hook() { }
	void set_input_line(int line, int state) { execute_set_input(line, state); }

	// --- what the core calls -------------------------------------------------------
	/// Run for `cycles`, returning how many were actually consumed.
	int run_cycles(int cycles);

	/// End the slice early, as MAME's abort_timeslice does.  The cycles not run are
	/// STOLEN -- removed from the slice's budget -- so that the caller is charged only for
	/// what was executed.  Charging the whole slice instead makes the CPU appear to run
	/// far slower than its clock, which is exactly what it looks like from the outside.
	void steal_remaining_cycles();

private:
	int *m_icountptr = nullptr;
	u64 m_totalcycles = 0;
	int m_cycles_this_slice = 0;
	bool m_running = false;
	bool m_suspended = false;
};

constexpr u32 SUSPEND_REASON_HALT    = 0x0001;
constexpr u32 SUSPEND_REASON_RESET   = 0x0002;
constexpr u32 SUSPEND_REASON_SPIN    = 0x0004;
constexpr u32 SUSPEND_REASON_TRIGGER = 0x0008;
constexpr u32 SUSPEND_REASON_DISABLE = 0x0010;
constexpr u32 SUSPEND_REASON_TIMESLICE = 0x0020;
constexpr u32 SUSPEND_ANY_REASON     = ~0u;


/***************************************************************************************
    Debugger state -- registered and then ignored

    37 state_add() calls exist purely so MAME's debugger can show registers.  There is no
    debugger here.  The calls have to compile and the chain of .mask().formatstr() has to
    work, so this returns a sink.  The ImGui debug window (PLUGIN-PLAN.md section 5) will
    read registers through the core's own accessors, not through this.
***************************************************************************************/

class device_state_entry
{
public:
	int index() const { return m_index; }
	void set_index(int i) { m_index = i; }

	device_state_entry &mask(u64 m) { return *this; }
	device_state_entry &signed_mask(u64 m) { return *this; }
	device_state_entry &formatstr(const char *f) { return *this; }
	device_state_entry &noshow() { return *this; }
	device_state_entry &callimport() { return *this; }
	device_state_entry &callexport() { return *this; }

private:
	int m_index = 0;
};

constexpr int STATE_GENPC     = -1;
constexpr int STATE_GENPCBASE = -2;
constexpr int STATE_GENFLAGS  = -3;

class device_state_interface : public device_interface
{
public:
	explicit device_state_interface(const machine_config &mconfig, device_t &device)
		: device_interface(device) { }

	template <typename... A> device_state_entry &state_add(A &&...)
	{ return m_sink; }
	template <typename T, typename... A> device_state_entry &state_add(A &&...)
	{ return m_sink; }

	virtual void state_import(const device_state_entry &entry) { }
	virtual void state_export(const device_state_entry &entry) { }
	virtual void state_string_export(const device_state_entry &entry, std::string &str) const { }

private:
	mutable device_state_entry m_sink;
};


/***************************************************************************************
    Disassembly

    MAME's disassembler headers are standalone (disasmintf.h needs only osdcomm.h and
    <ostream>), so the real ones are used and mcs96d.cpp / i8x9xd.cpp compile unchanged.
    That gives the debug window a real disassembler for free.
***************************************************************************************/

#include "disasmintf.h"

class device_disasm_interface : public device_interface
{
public:
	explicit device_disasm_interface(const machine_config &mconfig, device_t &device)
		: device_interface(device) { }

	virtual std::unique_ptr<util::disasm_interface> create_disassembler() = 0;
};


/***************************************************************************************
    Callback objects

    MAME's devcb resolves a target at machine_config time.  Here the core binds a
    std::function directly; the call sites are identical.
***************************************************************************************/

template <typename Sig> class devcb_base
{
public:
	explicit devcb_base(device_t &owner) { }

	/// MAME writes `.bind()` and then attaches a target; the core assigns a function.
	devcb_base &bind() { return *this; }
	template <typename T> void set(T &&fn) { m_fn = std::forward<T>(fn); }
	devcb_base &operator=(std::function<Sig> fn) { m_fn = std::move(fn); return *this; }

	void resolve_safe(int) { }
	void resolve() { }

	template <typename... A> auto operator()(A &&... a) const
	{ if (m_fn) return m_fn(std::forward<A>(a)...); return decltype(m_fn(std::forward<A>(a)...))(); }

	bool isnull() const { return !m_fn; }

private:
	std::function<Sig> m_fn;
};

class devcb_write_line
{
public:
	explicit devcb_write_line(device_t &owner) { }
	template <typename T> devcb_write_line(device_t &owner, T &&dflt) { }
	// MAME's accessors are `auto foo_cb() { return m_foo_cb.bind(); }`.  `auto` deduces BY
	// VALUE, so bind() must NOT return *this -- the caller would configure a copy and the
	// real callback would stay unset, silently.  It returns a proxy holding a pointer.
	struct binder
	{
		devcb_write_line *target;
		template <typename T> void set(T &&fn) { target->m_fn = std::forward<T>(fn); }
		template <typename T> binder &operator=(T &&fn)
		{ target->m_fn = std::forward<T>(fn); return *this; }
		void set_inputline(...) { }
	};
	binder bind() { return binder{ this }; }
	template <typename T> void set(T &&fn) { m_fn = std::forward<T>(fn); }
	void operator()(int state) const { if (m_fn) m_fn(state); }
private:
	std::function<void(int)> m_fn;
};

class devcb_write8
{
public:
	explicit devcb_write8(device_t &owner) { }
	template <typename T> devcb_write8(device_t &owner, T &&dflt) { }
	// MAME's accessors are `auto foo_cb() { return m_foo_cb.bind(); }`.  `auto` deduces BY
	// VALUE, so bind() must NOT return *this -- the caller would configure a copy and the
	// real callback would stay unset, silently.  It returns a proxy holding a pointer.
	struct binder
	{
		devcb_write8 *target;
		template <typename T> void set(T &&fn) { target->m_fn = std::forward<T>(fn); }
		template <typename T> binder &operator=(T &&fn)
		{ target->m_fn = std::forward<T>(fn); return *this; }
		void set_inputline(...) { }
	};
	binder bind() { return binder{ this }; }
	template <typename T> void set(T &&fn) { m_fn = std::forward<T>(fn); }
	void operator()(u8 data) const { if (m_fn) m_fn(data); }
	void operator()(offs_t offset, u8 data) const { if (m_fn) m_fn(data); }
	// The three-argument form carries a write mask; i8x9x uses it for the HSO lines.
	void operator()(offs_t offset, u8 data, u8 mask) const { if (m_fn) m_fn(data); }
private:
	std::function<void(u8)> m_fn;
};

class devcb_read16
{
public:
	explicit devcb_read16(device_t &owner) { }
	template <typename T> devcb_read16(device_t &owner, T &&dflt) { }
	// MAME's accessors are `auto foo_cb() { return m_foo_cb.bind(); }`.  `auto` deduces BY
	// VALUE, so bind() must NOT return *this -- the caller would configure a copy and the
	// real callback would stay unset, silently.  It returns a proxy holding a pointer.
	struct binder
	{
		devcb_read16 *target;
		template <typename T> void set(T &&fn) { target->m_fn = std::forward<T>(fn); }
		template <typename T> binder &operator=(T &&fn)
		{ target->m_fn = std::forward<T>(fn); return *this; }
		void set_inputline(...) { }
	};
	binder bind() { return binder{ this }; }
	template <typename T> void set(T &&fn) { m_fn = std::forward<T>(fn); }
	u16 operator()() const { return m_fn ? m_fn() : 0; }
	u16 operator()(offs_t offset) const { return m_fn ? m_fn() : 0; }
	bool isunset() const { return !m_fn; }
	bool isnull() const { return !m_fn; }

	// i8x9x declares its eight A/D channel callbacks as devcb_read16::array<8>.  The
	// nested form is MAME's; the members are only instantiated once the enclosing class
	// is complete, which is why this compiles from inside it.
	template <unsigned N> class array
	{
	public:
		array(device_t &owner, u16 dflt = 0)
			: m_cb{ devcb_read16(owner), devcb_read16(owner), devcb_read16(owner),
					devcb_read16(owner), devcb_read16(owner), devcb_read16(owner),
					devcb_read16(owner), devcb_read16(owner) } { }
		devcb_read16 &operator[](unsigned n) { return m_cb[n]; }
		const devcb_read16 &operator[](unsigned n) const { return m_cb[n]; }
	private:
		devcb_read16 m_cb[N];
	};

private:
	std::function<u16()> m_fn;
};

class devcb_read8
{
public:
	explicit devcb_read8(device_t &owner) { }
	template <typename T> devcb_read8(device_t &owner, T &&dflt) { }
	// MAME's accessors are `auto foo_cb() { return m_foo_cb.bind(); }`.  `auto` deduces BY
	// VALUE, so bind() must NOT return *this -- the caller would configure a copy and the
	// real callback would stay unset, silently.  It returns a proxy holding a pointer.
	struct binder
	{
		devcb_read8 *target;
		template <typename T> void set(T &&fn) { target->m_fn = std::forward<T>(fn); }
		template <typename T> binder &operator=(T &&fn)
		{ target->m_fn = std::forward<T>(fn); return *this; }
		void set_inputline(...) { }
	};
	binder bind() { return binder{ this }; }
	template <typename T> void set(T &&fn) { m_fn = std::forward<T>(fn); }
	u8 operator()() const { return m_fn ? m_fn() : 0xff; }
	u8 operator()(offs_t offset) const { return m_fn ? m_fn() : 0xff; }
private:
	std::function<u8()> m_fn;
};


/***************************************************************************************
    Region pointers

    msm6222b takes its CGROM this way.  The core points it at whatever font table it
    baked; on the U-110 the real CGROM was never dumped.
***************************************************************************************/

template <typename T> class region_ptr_base
{
public:
	region_ptr_base(device_t &owner, const char *tag) { }
	template <typename... A> void set_tag(A &&...) { }
	void set(T *base, size_t len) { m_base = base; m_len = len; }
	bool found() const { return m_base != nullptr; }
	explicit operator bool() const { return found(); }
	T *target() const { return m_base; }
	size_t length() const { return m_len; }
	T &operator[](int index) const { return m_base[index]; }
	T *operator+(int offset) const { return m_base + offset; }
private:
	T *m_base = nullptr;
	size_t m_len = 0;
};

template <typename T> using optional_region_ptr = region_ptr_base<T>;
template <typename T> using required_region_ptr = region_ptr_base<T>;

#endif // VOLTAIRE_COMPAT_EMU_INTERFACES_H
