// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
/***************************************************************************************

    emu.h -- a drop-in replacement for MAME's, just large enough for the U-110's devices

    MAME's device sources begin with `#include "emu.h"`.  The include path decides which
    one they get, so this file is how the SAME sources compile inside the Voltaire 110
    plugin as inside MAME -- no fork, no port, no drift.  See PLUGIN-PLAN.md section 3.

    WHAT THIS IS NOT.  It is not a reimplementation of MAME, and it must never grow into
    one.  It implements exactly what these six files use:

        devices/sound/roland_lp.cpp      the tone generator
        devices/cpu/mcs96/mcs96.cpp      the CPU core
        devices/cpu/mcs96/i8x9x.cpp        "     "
        devices/video/msm6222b.cpp       the LCD controller
        devices/sound/flt_biquad.cpp     the reconstruction filter and the output EQ
        devices/sound/flt_rc.cpp           "     "

    and nothing more.  When a MAME update breaks the build, the fix is to add the one
    thing it now needs -- not to widen the shim speculatively.

    THE API IS MAME'S, NOT A NICER ONE.  Every signature here is dictated by the callers.
    Where MAME's design is awkward the awkwardness is reproduced, because the alternative
    is editing the device sources, which is the one thing this exists to avoid.

    HOW IT DIFFERS FROM MAME, deliberately:

      - No process-global state.  Everything hangs off a running_machine instance, so a
        host can open several plugin instances.  MAME cannot.
      - No frame scheduler.  Time advances only when the audio callback asks for samples,
        which is what removes MAME's 20 ms latency quantum (that is the 50 Hz LCD refresh,
        not anything in the hardware).
      - No allocation, no file I/O and no locking on the render path.

    Some genuinely standalone MAME headers are used as-is rather than reimplemented --
    osdcomm.h for the integer types, rescap.h for the resistor/capacitor macros,
    endianness.h, disasmintf.h.  They pull in nothing but the standard library, so there
    is no reason to copy them.

***************************************************************************************/

#ifndef VOLTAIRE_COMPAT_EMU_H
#define VOLTAIRE_COMPAT_EMU_H

#pragma once

// MAME's logmacro.h refuses to be included unless this is defined; the device sources
// expect it, so keep the name.
#define __EMU_H__ 1

// MAME builds define this for little-endian hosts; osdcomm.h and endianness.h both read it.
#ifndef LSB_FIRST
#define LSB_FIRST 1
#endif

#include "osdcomm.h"

// osdcomm.h puts the integer types in namespace osd; MAME's own emu.h hoists them into the
// global namespace, and every device source assumes that.
using osd::u8;  using osd::u16; using osd::u32; using osd::u64;
using osd::s8;  using osd::s16; using osd::s32; using osd::s64;

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using offs_t = u32;

class device_t;
class running_machine;
class machine_config;
class address_space;
class sound_stream;
class validity_checker;


/***************************************************************************************
    Bit helpers -- MAME's, reimplemented so this header stands alone
***************************************************************************************/

template <typename T, typename U> constexpr T BIT(T x, U n) noexcept
{ return (x >> n) & T(1); }

template <typename T, typename U, typename V> constexpr T BIT(T x, U n, V w) noexcept
{ return (x >> n) & ((T(1) << w) - 1); }

// bitswap<N>(value, b...) -- each argument names the SOURCE bit for one DESTINATION bit,
// most significant destination first.  Matches MAME's semantics exactly; verified against
// it over the whole 19-bit domain used by the wave ROM descrambler.
template <typename T, typename U> constexpr T bitswap_impl(T val, U b)
{ return T((val >> b) & 1); }

template <typename T, typename U, typename... V> constexpr T bitswap_impl(T val, U b, V... c)
{ return T((T((val >> b) & 1) << sizeof...(c)) | bitswap_impl(val, c...)); }

template <unsigned B, typename T, typename... U> constexpr T bitswap(T val, U... b)
{
	static_assert(sizeof...(b) == B, "wrong number of bits");
	return bitswap_impl(val, b...);
}


/***************************************************************************************
    attotime

    MAME's is a 96-bit fixed-point value; this is a signed 64-bit count of attoseconds,
    which overflows after about 9.2 seconds.  That is far too short, so the unit here is
    PICOseconds instead -- 106 days of range, and 1 ps is four orders of magnitude finer
    than a 32 kHz sample or a 31250 baud bit.  The API is MAME's; the resolution is not,
    and nothing in these six files can tell the difference.
***************************************************************************************/

class attotime
{
public:
	static constexpr s64 PER_SECOND = 1000000000000LL;   // picoseconds

	constexpr attotime() : m_ps(0) { }
	explicit constexpr attotime(s64 ps) : m_ps(ps) { }

	static constexpr attotime zero() { return attotime(0); }
	static constexpr attotime never() { return attotime(0x7fffffffffffffffLL); }

	static attotime from_hz(double hz)
	{ return hz > 0.0 ? attotime(s64(double(PER_SECOND) / hz + 0.5)) : never(); }
	static attotime from_hz(u32 hz)
	{ return hz ? attotime(s64(PER_SECOND / hz)) : never(); }
	static constexpr attotime from_seconds(s64 s)   { return attotime(s * PER_SECOND); }
	static constexpr attotime from_msec(s64 ms)     { return attotime(ms * (PER_SECOND / 1000)); }
	static constexpr attotime from_usec(s64 us)     { return attotime(us * (PER_SECOND / 1000000)); }
	static constexpr attotime from_nsec(s64 ns)     { return attotime(ns * (PER_SECOND / 1000000000)); }
	static constexpr attotime from_double(double s) { return attotime(s64(s * double(PER_SECOND))); }
	static attotime from_ticks(u64 ticks, u32 frequency)
	{ return frequency ? attotime(s64((double(ticks) * double(PER_SECOND)) / double(frequency))) : never(); }

	double as_double() const { return double(m_ps) / double(PER_SECOND); }
	u64 as_ticks(u32 frequency) const
	{ return u64((double(m_ps) / double(PER_SECOND)) * double(frequency)); }
	s64 as_ps() const { return m_ps; }
	bool is_never() const { return m_ps == never().m_ps; }

	attotime operator+(const attotime &o) const { return attotime(m_ps + o.m_ps); }
	attotime operator-(const attotime &o) const { return attotime(m_ps - o.m_ps); }
	attotime &operator+=(const attotime &o) { m_ps += o.m_ps; return *this; }
	attotime &operator-=(const attotime &o) { m_ps -= o.m_ps; return *this; }
	attotime operator*(u32 n) const { return attotime(m_ps * n); }
	attotime operator/(u32 n) const { return attotime(m_ps / n); }

	bool operator< (const attotime &o) const { return m_ps <  o.m_ps; }
	bool operator<=(const attotime &o) const { return m_ps <= o.m_ps; }
	bool operator> (const attotime &o) const { return m_ps >  o.m_ps; }
	bool operator>=(const attotime &o) const { return m_ps >= o.m_ps; }
	bool operator==(const attotime &o) const { return m_ps == o.m_ps; }
	bool operator!=(const attotime &o) const { return m_ps != o.m_ps; }

private:
	s64 m_ps;
};

constexpr attotime attotime_never = attotime(0x7fffffffffffffffLL);

constexpr s64 ATTOSECONDS_PER_SECOND      = attotime::PER_SECOND;
constexpr s64 ATTOSECONDS_PER_MILLISECOND = attotime::PER_SECOND / 1000;
constexpr s64 ATTOSECONDS_PER_MICROSECOND = attotime::PER_SECOND / 1000000;
using attoseconds_t = s64;

inline attotime attotime_from_attoseconds(attoseconds_t a) { return attotime(a); }


/***************************************************************************************
    Timers

    MAME's timers are objects owned by the scheduler and re-armed with adjust().  Here a
    timer is a slot in the machine's queue; expiry is checked whenever time advances.
***************************************************************************************/

class emu_timer;

class device_execute_interface;

class device_scheduler
{
public:
	// Time INCLUDES the cycles the currently-executing CPU has consumed inside its slice.
	// MAME does the same, and it is not cosmetic: roland_lp calls stream->update() before
	// acting on a register write so the write lands on the right sample, and that only
	// works if time moves while the CPU runs.  Without this every write in a slice would
	// land at the slice's start and the render would not be bit-identical.
	attotime time() const;

	void set_executing(device_execute_interface *d, int slice_cycles);

	/// End the running CPU slice now.
	///
	/// MAME does this whenever a timer is armed to expire inside the slice already
	/// underway, and it is not an optimisation -- without it a device timer cannot fire
	/// any sooner than the end of the current slice.  The U-110's LCD arms a 40 us
	/// ready-interrupt on every write; with a 16 ms audio block that made the firmware's
	/// text queue drain one character per BLOCK instead of one per 40 us, and the boot
	/// took twice as long as the real machine.
	void abort_timeslice();

	attotime slice_end() const { return m_slice_end; }
	bool executing() const { return m_executing != nullptr; }

	emu_timer *alloc(std::function<void(s32)> cb);
	void advance_to(attotime target);          // fire everything due, then set the clock
	attotime next_event() const;

private:
	friend class emu_timer;
	attotime m_now;
	device_execute_interface *m_executing = nullptr;
	int m_slice_cycles = 0;
	attotime m_slice_end;
	std::vector<std::unique_ptr<emu_timer>> m_timers;
};

class emu_timer
{
public:
	void adjust(attotime duration, s32 param = 0)
	{
		m_expire = duration.is_never() ? attotime::never() : m_sched->time() + duration;
		m_param = param;
		m_active = !duration.is_never();
		m_period = attotime::zero();          // the two-argument form is one-shot
		// If this lands inside the slice already running, cut the slice short so it can
		// actually fire on time.  See device_scheduler::abort_timeslice().
		if (m_active && m_sched->executing() && m_expire < m_sched->slice_end())
			m_sched->abort_timeslice();
	}
	// The three-argument form is PERIODIC: it re-arms itself after every expiry.  The
	// sound chip's envelope interrupt is offered on one of these, so a shim that fires it
	// once leaves the machine with no envelope interrupts at all -- the notes still sound,
	// they just never decay properly, which is a very quiet way to be wrong.
	void adjust(attotime duration, s32 param, attotime period)
	{ adjust(duration, param); m_period = period; }
	void reset() { m_active = false; }
	bool enabled() const { return m_active; }
	attotime expire() const { return m_expire; }
	attotime remaining() const { return m_active ? m_expire - m_sched->time() : attotime::never(); }

private:
	friend class device_scheduler;
	emu_timer(device_scheduler *s, std::function<void(s32)> cb) : m_sched(s), m_cb(std::move(cb)) { }

	device_scheduler *m_sched;
	std::function<void(s32)> m_cb;
	attotime m_expire = attotime::never();
	attotime m_period;
	s32 m_param = 0;
	bool m_active = false;
};


/***************************************************************************************
    running_machine

    One per core instance.  MAME's is a process-wide singleton in all but name; this is
    not, which is what lets a host load several copies of the plugin.
***************************************************************************************/

class running_machine
{
public:
	attotime time() const { return m_scheduler.time(); }
	device_scheduler &scheduler() { return m_scheduler; }

	// The device sources call this to suppress side effects during a debugger read.  There
	// is no debugger here, so it is always false -- but the calls must compile.
	bool side_effects_disabled() const { return false; }

	std::string describe_context() const { return std::string("core"); }

	// Where logerror() goes.  Null by default: the render path must not log.
	void set_log_callback(std::function<void(const char *)> cb) { m_log = std::move(cb); }
	void log(const char *text) const { if (m_log) m_log(text); }

private:
	device_scheduler m_scheduler;
	std::function<void(const char *)> m_log;
};

// MAME threads a machine_config through every device constructor.  Nothing in these six
// files reads it, so it carries only the machine pointer the devices need.
class machine_config
{
public:
	explicit machine_config(running_machine &m) : m_machine(&m) { }
	running_machine &machine() const { return *m_machine; }
private:
	running_machine *m_machine;
};


/***************************************************************************************
    device_type and the DEVICE_TYPE macros

    MAME's device_type is a factory used by machine_config to build a device tree.  The
    core constructs its devices directly, so this only has to carry the identity that the
    protected constructors pass around.
***************************************************************************************/

struct device_type
{
	const char *shortname;
	const char *fullname;
	const char *source;
};

#define DECLARE_DEVICE_TYPE(Type, Class) \
		class Class; \
		extern const device_type Type;

#define DECLARE_DEVICE_TYPE_NS(Type, Namespace, Class) \
		extern const device_type Type;

#define DEFINE_DEVICE_TYPE(Type, Class, ShortName, FullName) \
		const device_type Type = { ShortName, FullName, __FILE__ };

#define DEFINE_DEVICE_TYPE_PRIVATE(Type, Base, Class, ShortName, FullName) \
		const device_type Type = { ShortName, FullName, __FILE__ };


/***************************************************************************************
    ROM regions

    Only msm6222b_01 declares one, and the U-110 does not use that variant -- its CGROM
    comes from the machine.  So the macros need to compile and produce a walkable table;
    nothing here loads from it.
***************************************************************************************/

struct tiny_rom_entry
{
	const char *name;
	const char *hashdata;
	u32 offset;
	u32 length;
	u32 flags;
};

#define ROM_NAME(name)                  rom_##name
#define ROM_START(name)                 static const tiny_rom_entry ROM_NAME(name)[] = {
#define ROM_END                         { nullptr, nullptr, 0, 0, 0 } };
#define ROM_REGION(length, tag, flags)  { tag, nullptr, 0, u32(length), u32(flags) },
#define ROM_LOAD(name, offset, length, hash) { name, nullptr, u32(offset), u32(length), 0 },
#define CRC(x)                          #x
#define SHA1(x)                         #x


/***************************************************************************************
    Save state

    MAME registers every member it wants persisted.  The core needs the same list for its
    own saveState(), so these are recorded rather than discarded -- name, address and
    size, in registration order, which is stable for a given build.
***************************************************************************************/

struct save_entry
{
	std::string name;
	void *data;
	size_t size;
};

#define NAME(x) &x, #x

// STRUCT_MEMBER registers a field of every element of an array of structs.  Nothing here
// walks the array, so it only has to name the field.
#define STRUCT_MEMBER(s, m) #s "." #m

// MAME needs an opt-in before an enum class can be saved.  Nothing here inspects the
// registration, but the declaration has to compile.
#define ALLOW_SAVE_TYPE(TYPE)

/***************************************************************************************
    device_t
***************************************************************************************/

class device_interface;

// MAME resolves its finders while the device tree starts.  The shim does the same, but
// the lookup is supplied by the core rather than walked out of a tree.
class share_finder_base
{
public:
	virtual ~share_finder_base() = default;
	virtual void resolve(void *base, size_t bytes) = 0;
	const char *share_tag() const { return m_share_tag; }
protected:
	explicit share_finder_base(const char *tag) : m_share_tag(tag) { }
	const char *m_share_tag;
};

class device_t
{
public:
	virtual ~device_t() = default;

	running_machine &machine() const { return *m_machine; }
	const char *tag() const { return m_tag.c_str(); }
	const char *shortname() const { return m_type.shortname; }
	const char *name() const { return m_type.fullname; }
	u32 clock() const { return m_clock; }
	device_t *owner() const { return m_owner; }

	// MAME calls this once the device tree is built.  The core calls it explicitly.
	void start();
	void reset();

	void logerror(const char *fmt, ...) const ATTR_PRINTF(2, 3);

	void set_clock(u32 clock) { m_clock = clock; notify_clock_changed(); }
	void set_unscaled_clock(u32 clock, bool sync = false) { set_clock(clock); }
	void notify_clock_changed();

	template <typename T> void save_item(T *data, const char *name, int index = 0)
	{ m_saves.push_back({ std::string(name) + (index ? "." + std::to_string(index) : ""), data, sizeof(T) }); }
	template <typename T, size_t N> void save_item(T (*data)[N], const char *name, int index = 0)
	{ m_saves.push_back({ name, data, sizeof(T) * N }); }
	template <typename T> void save_pointer(T *data, const char *name, u32 count, int index = 0)
	{ m_saves.push_back({ name, data, sizeof(T) * count }); }

	// STRUCT_MEMBER expands to a bare name.  Recorded so the list stays complete; the
	// core's own saveState() walks the machine, not this.
	void save_item(const char *name) { m_saves.push_back({ name, nullptr, 0 }); }

	const std::vector<save_entry> &save_entries() const { return m_saves; }

	// Devices whose interfaces need starting register themselves here.
	void register_interface(device_interface *i) { m_interfaces.push_back(i); }

	void register_share(share_finder_base *f) { m_share_finders.push_back(f); }

	/// The core installs this so `.share("register_file")` can be found at start time.
	using share_lookup = std::function<std::pair<void *, size_t>(const char *)>;
	void set_share_provider(share_lookup f) { m_share_provider = std::move(f); }

protected:
	device_t(const machine_config &mconfig, device_type type, const char *tag,
			device_t *owner, u32 clock)
		: m_machine(&mconfig.machine()), m_type(type), m_tag(tag ? tag : "")
		, m_owner(owner), m_clock(clock)
	{ }

	virtual void device_start() = 0;
	virtual void device_reset() { }
	virtual void device_clock_changed() { }
	virtual void device_post_load() { }
	virtual const tiny_rom_entry *device_rom_region() const { return nullptr; }

	// FUNC() expands to `&member, "member"`, so the name lands between the two arguments.
	template <typename T> emu_timer *timer_alloc(void (T::*cb)(s32), const char *name, T *obj)
	{ return machine().scheduler().alloc([obj, cb](s32 p) { (obj->*cb)(p); }); }

private:
	running_machine *m_machine;
	device_type m_type;
	std::string m_tag;
	device_t *m_owner;
	u32 m_clock;
	std::vector<save_entry> m_saves;
	std::vector<device_interface *> m_interfaces;
	std::vector<share_finder_base *> m_share_finders;
	share_lookup m_share_provider;
	bool m_started = false;
};

// MAME wraps the callback in FUNC() to give it a name for the debugger.  Here it only has
// to yield the member pointer.
#define FUNC(x) &x, #x

#define TIMER_CALLBACK_MEMBER(name) void name(s32 param)

class device_interface
{
public:
	virtual ~device_interface() = default;
	device_t &device() { return m_device; }
	const device_t &device() const { return m_device; }

protected:
	explicit device_interface(device_t &device) : m_device(device) { m_device.register_interface(this); }

	virtual void interface_pre_start() { }
	virtual void interface_post_start() { }
	virtual void interface_pre_reset() { }
	virtual void interface_post_reset() { }
	virtual void interface_post_load() { }
	virtual void interface_clock_changed(bool sync) { }
	virtual void interface_validity_check(validity_checker &valid) const { }

private:
	friend class device_t;
	device_t &m_device;
};

#include "strformat.h"

using util::string_format;

#include "emu_interfaces.h"
#include "emu_cpu.h"

#endif // VOLTAIRE_COMPAT_EMU_H
