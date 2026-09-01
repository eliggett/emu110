// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
//
// Out-of-line parts of the emu.h shim.  See plugin/compat/emu.h for what this is.

#include "emu.h"

#include <cstdarg>

u32 device_sound_interface::s_default_rate = 32000;


/***************************************************************************************
    device_t
***************************************************************************************/

void device_t::start()
{
	if (m_started)
		return;
	for (device_interface *i : m_interfaces)
		i->interface_pre_start();
	device_start();
	for (device_interface *i : m_interfaces)
		i->interface_post_start();
	m_started = true;
}

void device_t::reset()
{
	for (device_interface *i : m_interfaces)
		i->interface_pre_reset();
	device_reset();
	for (device_interface *i : m_interfaces)
		i->interface_post_reset();
}

void device_t::notify_clock_changed()
{
	device_clock_changed();
	for (device_interface *i : m_interfaces)
		i->interface_clock_changed(false);
}

void device_t::logerror(const char *fmt, ...) const
{
	// Nothing is formatted unless someone is listening.  The render path must not log, and
	// the cheapest way to guarantee that is for the default to cost one branch.
	if (!m_machine)
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	m_machine->log(buf);
}


/***************************************************************************************
    Scheduler and timers
***************************************************************************************/

emu_timer *device_scheduler::alloc(std::function<void(s32)> cb)
{
	m_timers.emplace_back(new emu_timer(this, std::move(cb)));
	return m_timers.back().get();
}

attotime device_scheduler::next_event() const
{
	attotime best = attotime::never();
	for (const auto &t : m_timers)
		if (t->m_active && t->m_expire < best)
			best = t->m_expire;
	return best;
}

void device_scheduler::advance_to(attotime target)
{
	// Fire every timer due at or before `target`, in time order, advancing the clock to
	// each as we go -- a callback that reads machine().time() must see its own expiry, and
	// one that re-arms a timer inside the window must have that honoured.
	for (;;)
	{
		emu_timer *next = nullptr;
		for (const auto &t : m_timers)
			if (t->m_active && t->m_expire <= target
					&& (!next || t->m_expire < next->m_expire))
				next = t.get();
		if (!next)
			break;
		m_now = next->m_expire;
		next->m_active = false;
		auto cb = next->m_cb;
		s32 param = next->m_param;
		if (cb)
			cb(param);
	}
	if (m_now < target)
		m_now = target;
}


/***************************************************************************************
    Sound streams
***************************************************************************************/

sound_stream::sound_stream(device_sound_interface *owner, int inputs, int outputs, u32 rate)
	: m_owner(owner), m_rate(rate)
{
	m_in.resize(inputs);
	m_out.resize(outputs);
	m_links.resize(inputs);
}

void sound_stream::connect(int input, sound_stream *src, int src_output, float gain)
{
	m_links[input].push_back({ src, src_output, gain });
}

void sound_stream::begin_block(u32 frames, attotime block_start)
{
	m_frames = frames;
	m_done = 0;
	m_window_start = m_window_len = 0;
	m_block_start = block_start;
	for (auto &b : m_in)  { b.assign(frames, 0.0f); }
	for (auto &b : m_out) { b.assign(frames, 0.0f); }
}

void sound_stream::update()
{
	// How far into the block the scheduler has got.  Rounding down means a write landing
	// mid-sample takes effect on the NEXT sample, which is what MAME does.
	const attotime now = m_owner->device().machine().time();
	const double elapsed = (now - m_block_start).as_double();
	s64 want = s64(elapsed * double(m_rate));
	if (want < 0) want = 0;
	if (want > s64(m_frames)) want = s64(m_frames);
	render_to(u32(want));
}

void sound_stream::render_to(u32 target)
{
	if (target <= m_done)
		return;

	// Upstream first: our inputs must cover the same span before we can read them.
	for (auto &links : m_links)
		for (auto &l : links)
			l.src->render_to(target);

	for (size_t i = 0; i < m_in.size(); i ++)
	{
		std::fill(m_in[i].begin() + m_done, m_in[i].begin() + target, 0.0f);
		for (auto &l : m_links[i])
		{
			const auto &src = l.src->m_out[l.out];
			for (u32 s = m_done; s < target; s ++)
				m_in[i][s] += src[s] * l.gain;
		}
	}

	m_window_start = m_done;
	m_window_len = target - m_done;
	m_done = target;
	m_owner->sound_stream_update(*this);
	m_window_len = 0;
}


/***************************************************************************************
    address_space
***************************************************************************************/

const address_space::region *address_space::find(offs_t a) const
{
	// Last match wins, so a later install overlays an earlier one.
	for (auto it = m_regions.rbegin(); it != m_regions.rend(); ++it)
		if (a >= it->start && a <= it->end)
			return &*it;
	return nullptr;
}

u8 address_space::read_byte(offs_t a) const
{
	const region *r = find(a);
	if (!r)
		return 0xff;
	if (r->rom) return r->rom[a - r->start];
	if (r->ram) return r->ram[a - r->start];
	if (r->r)   return r->r(a - r->start);
	return 0xff;
}

void address_space::write_byte(offs_t a, u8 v)
{
	const region *r = find(a);
	if (!r)
		return;
	if (r->ram) { r->ram[a - r->start] = v; return; }
	if (r->w)   { r->w(a - r->start, v); }
}

u16 address_space::read_word(offs_t a) const
{ return u16(read_byte(a)) | (u16(read_byte(a + 1)) << 8); }

void address_space::write_word(offs_t a, u16 v)
{ write_byte(a, u8(v)); write_byte(a + 1, u8(v >> 8)); }

void address_space::install_rom(offs_t start, offs_t end, const u8 *base)
{ m_regions.push_back({ start, end, base, nullptr, nullptr, nullptr }); }

void address_space::install_ram(offs_t start, offs_t end, u8 *base)
{ m_regions.push_back({ start, end, nullptr, base, nullptr, nullptr }); }

void address_space::install_handler(offs_t start, offs_t end, read8_cb r, write8_cb w)
{ m_regions.push_back({ start, end, nullptr, nullptr, std::move(r), std::move(w) }); }

void address_space::unmap(offs_t start, offs_t end)
{ m_regions.push_back({ start, end, nullptr, nullptr, nullptr, nullptr }); }


/***************************************************************************************
    device_execute_interface
***************************************************************************************/

attotime device_execute_interface::cycles_to_attotime(u64 cycles) const
{
	const u32 clk = device().clock();
	return clk ? attotime::from_ticks(execute_cycles_to_clocks(cycles), clk) : attotime::zero();
}

u64 device_execute_interface::attotime_to_cycles(attotime t) const
{
	const u32 clk = device().clock();
	return clk ? execute_clocks_to_cycles(t.as_ticks(clk)) : 0;
}

int device_execute_interface::run_cycles(int cycles)
{
	if (m_suspended || !m_icountptr)
		return cycles;
	*m_icountptr = cycles;
	m_cycles_this_slice = cycles;
	m_running = true;
	execute_run();
	m_running = false;
	const int used = cycles - *m_icountptr;
	m_totalcycles += used;
	return used;
}


/***************************************************************************************
    fatalerror
***************************************************************************************/

void fatalerror(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "fatal: %s\n", buf);
	std::abort();
}


/***************************************************************************************
    address_map
***************************************************************************************/

device_t *map_entry::owner() const { return m_map->owner(); }

map_entry &map_entry::ram()   { m_map->at(m_index).is_ram = true; return *this; }
map_entry &map_entry::rom()   { m_map->at(m_index).is_ram = true; return *this; }
map_entry &map_entry::nopw()  { m_map->at(m_index).nop_w = true; return *this; }
map_entry &map_entry::nopr()  { m_map->at(m_index).nop_r = true; return *this; }

map_entry &map_entry::share(const char *tag)
{ m_map->at(m_index).share = tag; return *this; }

map_entry &map_entry::set_r8(std::function<u8(offs_t)> fn)
{ m_map->at(m_index).r8 = std::move(fn); return *this; }
map_entry &map_entry::set_w8(std::function<void(offs_t, u8)> fn)
{ m_map->at(m_index).w8 = std::move(fn); return *this; }
map_entry &map_entry::set_r16(std::function<u16(offs_t)> fn)
{ m_map->at(m_index).r16 = std::move(fn); return *this; }
map_entry &map_entry::set_w16(std::function<void(offs_t, u16)> fn)
{ m_map->at(m_index).w16 = std::move(fn); return *this; }

void address_map::allocate()
{
	for (auto &e : m_entries)
	{
		if (!e.is_ram || e.ram)
			continue;
		m_ram.emplace_back(e.end - e.start + 1, u8(0));
		e.ram = m_ram.back().data();
	}
}

std::pair<u8 *, size_t> address_map::find_share(const char *tag)
{
	for (auto &e : m_entries)
		if (e.share == tag && e.ram)
			return { e.ram, size_t(e.end - e.start + 1) };
	return { nullptr, 0 };
}

u8 address_map::read_byte(offs_t a) const
{
	// Latest matching entry that can answer wins; that is what makes the overlapping
	// 8-bit and 16-bit declarations at the same address behave as MAME's do.
	for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
	{
		if (a < it->start || a > it->end) continue;
		if (it->ram)  return it->ram[a - it->start];
		if (it->r8)   return it->r8(a - it->start);
		if (it->r16)  { const u16 v = it->r16((a - it->start) >> 1);
		                return u8((a & 1) ? (v >> 8) : v); }
		if (it->nop_r) return 0;
	}
	return 0;
}

void address_map::write_byte(offs_t a, u8 v)
{
	for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
	{
		if (a < it->start || a > it->end) continue;
		if (it->ram)  { it->ram[a - it->start] = v; return; }
		if (it->w8)   { it->w8(a - it->start, v); return; }
		if (it->w16)  { it->w16((a - it->start) >> 1, v); return; }
		if (it->nop_w) return;
	}
}

u16 address_map::read_word(offs_t a) const
{
	for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
	{
		if (a < it->start || a > it->end) continue;
		if (it->ram)   return u16(it->ram[a - it->start]) | (u16(it->ram[a - it->start + 1]) << 8);
		if (it->r16)   return it->r16((a - it->start) >> 1);
		if (it->r8)    break;      // fall through to the byte path
		if (it->nop_r) return 0;
	}
	return u16(read_byte(a)) | (u16(read_byte(a + 1)) << 8);
}

void address_map::write_word(offs_t a, u16 v)
{
	for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
	{
		if (a < it->start || a > it->end) continue;
		if (it->ram)   { it->ram[a - it->start] = u8(v); it->ram[a - it->start + 1] = u8(v >> 8); return; }
		if (it->w16)   { it->w16((a - it->start) >> 1, v); return; }
		if (it->w8)    break;
		if (it->nop_w) return;
	}
	write_byte(a, u8(v));
	write_byte(a + 1, u8(v >> 8));
}
