// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drive the panel from a script, in emulated time, and look at memory.
//
//   u110_panel --roms DIR [--script FILE]      (script on stdin if not given)
//
// Commands, one per line:
//   boot                 reset and run until the play screen shows (or 12 s)
//   run MS               advance emulated time
//   press BTN [MS]       hold BTN for MS (default 60), release, then 180 ms gap
//   down BTN / up BTN    edges, for chords like DEC+INC
//   lcd                  print the two LCD lines
//   peek ADDR LEN        hex dump
//   poke ADDR VAL        one byte
//   card SLOT FILE       put a card image in a slot, as inserting one does
//   eject SLOT           take it out again
//   midi HEX...          bytes onto the wire
//   dt1 A1 A2 A3 V       one Roland DT1 parameter write, checksum computed here
//   copy DST SRC LEN     what the plugin would do: lift memory into a slot
//   mark NAME ADDR LEN   remember a region
//   cmp NAME ADDR LEN    diff a region against a mark
//   cmpm A B             diff two marks of equal length
//   require NAME ADDR LEN          fail unless the region still matches the mark
//   requiredifferent NAME ADDR LEN fail unless it changed
//   requiretext ROW TEXT           fail unless LCD row 0 or 1 contains TEXT
//                                  (sampled across a blink period, so menu selections
//                                   and edited values can be asserted on)
//   requirebyte ADDR VALUE         fail unless one byte of RAM reads VALUE
//   echo TEXT
//
// Exits non-zero if any require failed, so a script is a check and not just a transcript.
//
// BTN is part, exit, left, right, dec, inc.

#include "u110_core.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const std::string &path)
{
    std::vector<uint8_t> data;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(size_t(n < 0 ? 0 : n));
    if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size())
        data.clear();
    std::fclose(f);
    return data;
}

voltaire::U110Core core;
std::vector<float> bufL(512), bufR(512);

void advance_ms(double ms)
{
    uint32_t n = uint32_t(ms * voltaire::kCoreSampleRate / 1000.0);
    while (n > 0)
    {
        const uint32_t k = n < 512 ? n : 512;
        core.renderStereo(bufL.data(), bufR.data(), k);
        n -= k;
    }
}

std::string lcd_line(const voltaire::PanelState &st, int row)
{
    std::string s;
    for (int i = 0; i < 16; i ++)
    {
        const uint8_t c = st.lcd[row * 16 + i];
        s += (c >= 0x20 && c < 0x7f) ? char(c) : '.';
    }
    return s;
}

void print_lcd(const char *tag)
{
    voltaire::PanelState st;
    core.snapshot(st);
    std::printf("  %-10s [%s]  cur=%u%s led=%X\n             [%s]\n",
                tag, lcd_line(st, 0).c_str(), st.cursor_pos,
                (st.cursor_flags & 1) ? ((st.cursor_flags & 2) ? " blink" : " on") : " off", st.leds,
                lcd_line(st, 1).c_str());
}

int button_by_name(const std::string &n)
{
    if (n == "part")  return voltaire::kButtonPartJump;
    if (n == "exit")  return voltaire::kButtonEditExit;
    if (n == "left")  return voltaire::kButtonLeft;
    if (n == "right") return voltaire::kButtonRight;
    if (n == "dec")   return voltaire::kButtonDec;
    if (n == "inc")   return voltaire::kButtonIncEnter;
    return -1;
}

std::vector<uint8_t> grab(unsigned addr, unsigned len)
{
    std::vector<uint8_t> v(len);
    for (unsigned i = 0; i < len; i ++)
        v[i] = core.readMem(uint16_t(addr + i));
    return v;
}

void hexdump(unsigned addr, const std::vector<uint8_t> &v)
{
    for (size_t i = 0; i < v.size(); i += 16)
    {
        std::printf("  %04X ", unsigned(addr + i));
        for (size_t j = 0; j < 16 && i + j < v.size(); j ++)
            std::printf("%02X ", v[i + j]);
        std::printf(" |");
        for (size_t j = 0; j < 16 && i + j < v.size(); j ++)
        {
            const uint8_t c = v[i + j];
            std::printf("%c", (c >= 0x20 && c < 0x7f) ? char(c) : '.');
        }
        std::printf("|\n");
    }
}

std::map<std::string, std::vector<uint8_t>> marks;
std::map<std::string, unsigned> mark_addr;
unsigned failures = 0;

void diff(const std::string &name, const std::vector<uint8_t> &a,
          const std::vector<uint8_t> &b, unsigned addr)
{
    if (a.size() != b.size())
    { std::printf("  %s: length mismatch\n", name.c_str()); return; }
    unsigned n = 0;
    for (size_t i = 0; i < a.size(); i ++)
        if (a[i] != b[i])
        {
            std::printf("  %s: +0x%02zX (%04X) %02X -> %02X\n",
                        name.c_str(), i, unsigned(addr + i), a[i], b[i]);
            n ++;
        }
    std::printf("  %s: %u of %zu bytes differ%s\n",
                name.c_str(), n, a.size(), n == 0 ? "  == IDENTICAL ==" : "");
}

bool on_play_screen()
{
    voltaire::PanelState st;
    core.snapshot(st);
    const uint8_t *t = st.lcd;
    if (std::memcmp(t, "TEMP:", 5) == 0) return true;
    return t[0] == 'P' && t[1] == '-' && t[4] == ':'
        && t[2] >= '0' && t[2] <= '9' && t[3] >= '0' && t[3] <= '9';
}

} // namespace

int main(int argc, char **argv)
{
    std::string roms = "../roms", script;
    for (int i = 1; i < argc; i ++)
    {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--roms")   roms = next();
        else if (a == "--script") script = next();
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    const char *pgm_names[] = { "U110v203.BIN", "roland_u110_pgm_(15179960).bin" };
    bool loaded = false;
    for (const char *n : pgm_names)
    {
        auto d = read_file(roms + "/" + n);
        if (d.size() == 0x10000)
        { loaded = core.loadProgramRom(d.data(), d.size()) == voltaire::LoadResult::Ok; break; }
    }
    if (!loaded) { std::fprintf(stderr, "no program ROM under %s\n", roms.c_str()); return 1; }

    for (unsigned b = 0; b < voltaire::kNumWaveBanks; b ++)
    {
        char name[256];
        std::snprintf(name, sizeof(name), "%s/roland_t110_u110_u220_waverom%u.bin",
                      roms.c_str(), b);
        auto d = read_file(name);
        if (d.size() != voltaire::kCardBytes)
        { std::fprintf(stderr, "wave ROM %u missing\n", b); return 1; }
        core.loadWaveRom(b, d.data(), d.size());
    }

    FILE *in = script.empty() ? stdin : std::fopen(script.c_str(), "r");
    if (!in) { std::fprintf(stderr, "cannot read %s\n", script.c_str()); return 1; }

    char line[512];
    while (std::fgets(line, sizeof(line), in))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p ++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char cmd[64] = {0}, a1[128] = {0}, a2[128] = {0}, a3[128] = {0};
        const int got = std::sscanf(p, "%63s %127s %127s %127s", cmd, a1, a2, a3);
        if (got < 1) continue;
        const std::string c = cmd;

        if (c == "boot")
        {
            core.reset();
            for (int i = 0; i < 240; i ++)      // up to 12 s
            {
                advance_ms(50);
                if (on_play_screen()) break;
            }
            print_lcd("boot");
        }
        else if (c == "run")   advance_ms(std::atof(a1));
        else if (c == "echo")  std::printf("%s", p + 5);
        else if (c == "lcd")   print_lcd(got > 1 ? a1 : "lcd");
        else if (c == "press" || c == "down" || c == "up")
        {
            const int b = button_by_name(a1);
            if (b < 0) { std::printf("  ?? button %s\n", a1); continue; }
            if (c == "down") core.setButton(voltaire::Button(b), true);
            else if (c == "up") core.setButton(voltaire::Button(b), false);
            else
            {
                const double hold = (got > 2) ? std::atof(a2) : 60.0;
                core.setButton(voltaire::Button(b), true);
                advance_ms(hold);
                core.setButton(voltaire::Button(b), false);
                advance_ms(180.0);
            }
        }
        else if (c == "peek")
        {
            const unsigned addr = unsigned(std::strtoul(a1, nullptr, 0));
            const unsigned len  = got > 2 ? unsigned(std::strtoul(a2, nullptr, 0)) : 16;
            hexdump(addr, grab(addr, len));
        }
        else if (c == "poke")
        {
            core.writeMem(uint16_t(std::strtoul(a1, nullptr, 0)),
                          uint8_t(std::strtoul(a2, nullptr, 0)));
        }
        // The card commands do exactly what a hand does: change what the slot contains
        // WHILE THE MACHINE IS RUNNING, and then let the firmware find out for itself.
        // Nothing here pokes RAM or tells the firmware anything -- the only thing that
        // crosses over is the presence bit on PORT1, which is the one wire the hardware
        // has for this.  A relative FILE is resolved against --roms.
        else if (c == "card")
        {
            const unsigned slot = unsigned(std::strtoul(a1, nullptr, 0));
            const std::string file = a2[0] == '/' ? std::string(a2) : roms + "/" + a2;
            const std::vector<uint8_t> d = read_file(file);
            if (d.empty())
            { std::printf("  FAIL  cannot read %s\n", file.c_str()); failures ++; }
            else if (core.loadCard(slot, d.data(), d.size()) != voltaire::LoadResult::Ok)
            { std::printf("  FAIL  %s is not a card image\n", file.c_str()); failures ++; }
            else
                std::printf("  ..    slot %u <- %s (%zu bytes)\n", slot,
                            file.c_str(), d.size());
        }
        else if (c == "cards")
        {
            voltaire::PanelState st;
            core.snapshot(st);
            std::printf("  ..    core says present: %X   firmware ids: %02X %02X %02X %02X"
                        "   PORT1 snapshot 2747: %02X\n",
                        st.card_present,
                        core.readMem(0x2743), core.readMem(0x2744),
                        core.readMem(0x2745), core.readMem(0x2746),
                        core.readMem(0x2747));
        }
        else if (c == "eject")
        {
            const unsigned slot = unsigned(std::strtoul(a1, nullptr, 0));
            core.loadCard(slot, nullptr, 0);
            std::printf("  ..    slot %u ejected\n", slot);
        }
        else if (c == "midi")
        {
            std::vector<uint8_t> bytes;
            const char *s = p + 4;
            char *end = nullptr;
            for (;;)
            {
                const long v = std::strtol(s, &end, 16);
                if (end == s) break;
                bytes.push_back(uint8_t(v));
                s = end;
            }
            if (!bytes.empty()) core.midiIn(bytes.data(), bytes.size(), 0);
            advance_ms(double(bytes.size()) * 0.32 + 20.0);
        }
        else if (c == "dt1")
        {
            // One Roland DT1 write, checksum computed here so scripts stay readable.
            unsigned a1 = 0, a2 = 0, a3 = 0, v = 0;
            std::sscanf(p + 3, "%x %x %x %x", &a1, &a2, &a3, &v);
            const uint8_t sum = uint8_t((128 - ((a1 + a2 + a3 + v) & 0x7f)) & 0x7f);
            const uint8_t msg[] = { 0xF0, 0x41, 0x0F, 0x23, 0x12, uint8_t(a1), uint8_t(a2),
                                    uint8_t(a3), uint8_t(v), sum, 0xF7 };
            core.midiIn(msg, sizeof(msg), 0);
            advance_ms(60.0);
        }
        else if (c == "copy")
        {
            // What the plugin would do: lift the edit buffer straight into a slot.
            const unsigned dst = unsigned(std::strtoul(a1, nullptr, 0));
            const unsigned src = unsigned(std::strtoul(a2, nullptr, 0));
            const unsigned len = unsigned(std::strtoul(a3, nullptr, 0));
            for (unsigned i = 0; i < len; i ++)
                core.writeMem(uint16_t(dst + i), core.readMem(uint16_t(src + i)));
        }
        else if (c == "mark")
        {
            const unsigned addr = unsigned(std::strtoul(a2, nullptr, 0));
            const unsigned len  = unsigned(std::strtoul(a3, nullptr, 0));
            marks[a1] = grab(addr, len);
            mark_addr[a1] = addr;
        }
        else if (c == "cmp")
        {
            const unsigned addr = unsigned(std::strtoul(a2, nullptr, 0));
            const unsigned len  = unsigned(std::strtoul(a3, nullptr, 0));
            if (!marks.count(a1)) { std::printf("  no mark %s\n", a1); continue; }
            diff(a1, marks[a1], grab(addr, len), mark_addr[a1]);
        }
        else if (c == "cmpm")
        {
            if (!marks.count(a1) || !marks.count(a2))
            { std::printf("  missing mark\n"); continue; }
            diff(std::string(a1) + " vs " + a2, marks[a1], marks[a2], mark_addr[a1]);
        }
        else if (c == "require" || c == "requiredifferent")
        {
            const unsigned addr = unsigned(std::strtoul(a2, nullptr, 0));
            const unsigned len  = unsigned(std::strtoul(a3, nullptr, 0));
            if (!marks.count(a1)) { std::printf("  FAIL: no mark %s\n", a1); failures ++; continue; }
            const auto now = grab(addr, len);
            unsigned n = 0;
            for (size_t i = 0; i < now.size(); i ++)
                if (marks[a1][i] != now[i]) n ++;
            const bool want_same = (c == "require");
            if ((n == 0) == want_same)
                std::printf("  ok    %s vs %04X: %u of %u bytes differ\n",
                            a1, addr, n, len);
            else
            {
                std::printf("  FAIL  %s vs %04X: %u of %u bytes differ\n",
                            a1, addr, n, len);
                diff(a1, marks[a1], now, mark_addr[a1]);
                failures ++;
            }
        }
        else if (c == "requirebyte")
        {
            const unsigned addr = unsigned(std::strtoul(a1, nullptr, 0));
            const unsigned want = unsigned(std::strtoul(a2, nullptr, 0));
            const uint8_t got = core.readMem(uint16_t(addr));
            if (got == want) std::printf("  ok    %04X == %02X\n", addr, want);
            else { std::printf("  FAIL  %04X is %02X, wanted %02X\n", addr, got, want);
                   failures ++; }
        }
        else if (c == "requiretext")
        {
            // SAMPLED over a blink period, not read once.  The firmware shows the
            // selected menu item and the value being edited by blinking them, so a
            // single snapshot catches them blank about half the time.
            const char *want = p + 12 + std::strlen(a1);
            while (*want == ' ') want ++;
            std::string w(want);
            while (!w.empty() && (w.back() == '\n' || w.back() == ' ')) w.pop_back();
            const int row_i = std::atoi(a1);
            std::string row;
            bool found = false;
            for (int k = 0; k < 14 && !found; k ++)
            {
                voltaire::PanelState st;
                core.snapshot(st);
                row = lcd_line(st, row_i);
                found = row.find(w) != std::string::npos;
                if (!found) advance_ms(60.0);
            }
            if (found) std::printf("  ok    LCD%s contains \"%s\"\n", a1, w.c_str());
            else
            { std::printf("  FAIL  LCD%s is [%s], wanted \"%s\"\n",
                          a1, row.c_str(), w.c_str()); failures ++; }
        }
        else std::printf("  ?? %s\n", cmd);
    }
    if (in != stdin) std::fclose(in);
    if (failures != 0)
        std::printf("\n%u CHECK%s FAILED\n", failures, failures == 1 ? "" : "S");
    return failures == 0 ? 0 : 1;
}
