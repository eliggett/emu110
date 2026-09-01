// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
/***************************************************************************************

    cpu_device and the address_map DSL

    Included at the end of emu.h; not to be included directly.

    The i8x9x declares its internal register file with MAME's address_map DSL --
    `map(0x0e, 0x0e).rw(FUNC(port0_r), FUNC(baud_rate_w))` and so on -- so the DSL has to
    exist here even though the core wires the OUTER memory map itself.  Only the forms
    those twenty lines use are implemented: ram, share, r, w, rw, lr16 and nopw.

    Handler width is taken from the member function's own signature, exactly as MAME
    infers it, so an 8-bit handler on a two-byte range is called once per byte and a
    16-bit handler is called once per word.

***************************************************************************************/

#ifndef VOLTAIRE_COMPAT_EMU_CPU_H
#define VOLTAIRE_COMPAT_EMU_CPU_H

#pragma once

/// MAME's finders take a tag; DEVICE_SELF means "the device that owns me".
class finder_base
{
public:
	static constexpr const char *DUMMY_TAG = "\0";
};
constexpr const char *DEVICE_SELF = "";

[[noreturn]] void fatalerror(const char *fmt, ...) ATTR_PRINTF(1, 2);


/***************************************************************************************
    address_map
***************************************************************************************/

class address_map;

class map_entry
{
public:
	map_entry(address_map *m, size_t index) : m_map(m), m_index(index) { }

	map_entry &ram();
	map_entry &rom();
	map_entry &share(const char *tag);
	map_entry &nopw();
	map_entry &nopr();
	map_entry &noprw() { return nopr().nopw(); }

	// 8-bit handlers, with and without an offset argument.
	//
	// The owner is captured BY VALUE.  map_entry is a temporary -- `map(a,b).rw(...)`
	// builds one on the stack -- so a lambda capturing `this` would be reading freed
	// memory the first time the CPU touched the register.  It crashes on the first
	// instruction, which at least fails loudly.
	template <typename T> map_entry &r(u8 (T::*fn)(), const char *name)
	{ auto *o = static_cast<T *>(owner());
	  return set_r8([o, fn](offs_t) { return (o->*fn)(); }); }
	template <typename T> map_entry &r(u8 (T::*fn)(offs_t), const char *name)
	{ auto *o = static_cast<T *>(owner());
	  return set_r8([o, fn](offs_t a) { return (o->*fn)(a); }); }
	template <typename T> map_entry &w(void (T::*fn)(u8), const char *name)
	{ auto *o = static_cast<T *>(owner());
	  return set_w8([o, fn](offs_t, u8 d) { (o->*fn)(d); }); }
	template <typename T> map_entry &w(void (T::*fn)(offs_t, u8), const char *name)
	{ auto *o = static_cast<T *>(owner());
	  return set_w8([o, fn](offs_t a, u8 d) { (o->*fn)(a, d); }); }

	// 16-bit handlers
	template <typename T> map_entry &r(u16 (T::*fn)(), const char *name)
	{ auto *o = static_cast<T *>(owner());
	  return set_r16([o, fn](offs_t) { return (o->*fn)(); }); }
	template <typename T> map_entry &w(void (T::*fn)(u16), const char *name)
	{ auto *o = static_cast<T *>(owner());
	  return set_w16([o, fn](offs_t, u16 d) { (o->*fn)(d); }); }

	template <typename R, typename W>
	map_entry &rw(R rfn, const char *rname, W wfn, const char *wname)
	{ r(rfn, rname); return w(wfn, wname); }

	/// A read lambda, used for the 8x9x's hardwired zero register.
	template <typename F> map_entry &lr16(F fn, const char *name)
	{ return set_r16([fn](offs_t) { return fn(); }); }
	template <typename F> map_entry &lr8(F fn, const char *name)
	{ return set_r8([fn](offs_t) { return fn(); }); }

private:
	device_t *owner() const;
	map_entry &set_r8(std::function<u8(offs_t)> fn);
	map_entry &set_w8(std::function<void(offs_t, u8)> fn);
	map_entry &set_r16(std::function<u16(offs_t)> fn);
	map_entry &set_w16(std::function<void(offs_t, u16)> fn);

	address_map *m_map;
	size_t m_index;
};

class address_map
{
public:
	address_map(device_t &owner, address_space *space) : m_owner(&owner), m_space(space) { }

	// Every call makes a NEW entry, because the 8x9x deliberately overlaps them -- a
	// 16-bit read across 0x02..0x03 and an 8-bit write at 0x02 are two entries on the
	// same address, and lookup picks the latest one that defines the operation wanted.
	map_entry operator()(offs_t start, offs_t end)
	{ m_entries.push_back(entry{ start, end }); return map_entry(this, m_entries.size() - 1); }

	device_t *owner() const { return m_owner; }

	struct entry
	{
		offs_t start, end;
		bool is_ram = false;
		std::string share;
		std::function<u8(offs_t)>       r8;
		std::function<void(offs_t, u8)> w8;
		std::function<u16(offs_t)>      r16;
		std::function<void(offs_t, u16)> w16;
		bool nop_w = false, nop_r = false;
		u8 *ram = nullptr;
	};

	entry &at(size_t index) { return m_entries[index]; }
	const std::vector<entry> &entries() const { return m_entries; }

	// Dispatch, honouring handler width: an 8-bit handler on a two-byte range is called
	// once per byte, a 16-bit handler once per word, exactly as MAME infers it.
	u8   read_byte(offs_t a) const;
	void write_byte(offs_t a, u8 v);
	u16  read_word(offs_t a) const;
	void write_word(offs_t a, u16 v);

	/// RAM blocks named with .share(); the core resolves required_shared_ptr from these.
	std::pair<u8 *, size_t> find_share(const char *tag);
	void allocate();

private:
	device_t *m_owner;
	address_space *m_space;
	std::vector<entry> m_entries;
	std::vector<std::vector<u8>> m_ram;
};

/// MAME wraps `FUNC(member), this` in one of these and hands it to the space.
class address_map_constructor
{
public:
	address_map_constructor() = default;
	template <typename T>
	address_map_constructor(void (T::*fn)(address_map &), const char *name, T *obj)
		: m_fn([fn, obj](address_map &m) { (obj->*fn)(m); }) { }

	explicit operator bool() const { return bool(m_fn); }
	void operator()(address_map &m) const { if (m_fn) m_fn(m); }

private:
	std::function<void(address_map &)> m_fn;
};


/***************************************************************************************
    Shared pointers

    `.ram().share("register_file")` names a RAM block; required_shared_ptr finds it.
***************************************************************************************/

template <typename T> class required_shared_ptr : public share_finder_base
{
public:
	required_shared_ptr(device_t &owner, const char *tag)
		: share_finder_base(tag), m_tag(tag) { owner.register_share(this); }

	void resolve(void *base, size_t bytes) override
	{ m_base = static_cast<T *>(base); m_count = bytes / sizeof(T); }

	void set(T *base, size_t count) { m_base = base; m_count = count; }
	T *target() const { return m_base; }
	T &operator[](int index) const { return m_base[index]; }
	size_t bytes() const { return m_count * sizeof(T); }
	const char *tag() const { return m_tag; }
	explicit operator bool() const { return m_base != nullptr; }
private:
	const char *m_tag;
	T *m_base = nullptr;
	size_t m_count = 0;
};

template <typename T> using optional_shared_ptr = required_shared_ptr<T>;


/***************************************************************************************
    memory_access<>::cache and ::specific

    MAME's are lookup-table accelerators over the handler tree.  The shim's address_space
    is already a flat lookup, so these just forward.
***************************************************************************************/

template <int AddrWidth, int DataShift, int AddrShift, endianness_t Endian>
struct memory_access
{
	class cache
	{
	public:
		void set(address_space *s) { m_space = s; }
		u8  read_byte(offs_t a) const { return m_space->read_byte(a); }
		u16 read_word(offs_t a) const { return m_space->read_word(a); }
	private:
		address_space *m_space = nullptr;
	};

	class specific
	{
	public:
		void set(address_space *s) { m_space = s; }
		u8   read_byte(offs_t a) const { return m_space->read_byte(a); }
		u16  read_word(offs_t a) const { return m_space->read_word(a); }
		void write_byte(offs_t a, u8 v) { m_space->write_byte(a, v); }
		void write_word(offs_t a, u16 v) { m_space->write_word(a, v); }
	private:
		address_space *m_space = nullptr;
	};
};


/***************************************************************************************
    cpu_device
***************************************************************************************/

class cpu_device : public device_t, public device_execute_interface,
		public device_memory_interface, public device_state_interface,
		public device_disasm_interface
{
protected:
	cpu_device(const machine_config &mconfig, device_type type, const char *tag,
			device_t *owner, u32 clock)
		: device_t(mconfig, type, tag, owner, clock)
		, device_execute_interface(mconfig, *this)
		, device_memory_interface(mconfig, *this)
		, device_state_interface(mconfig, *this)
		, device_disasm_interface(mconfig, *this)
	{ }

public:
	virtual bool memory_translate(int spacenum, int intention, offs_t &address,
			address_space *&target_space)
	{ target_space = &space(spacenum); return true; }
};

#endif // VOLTAIRE_COMPAT_EMU_CPU_H
