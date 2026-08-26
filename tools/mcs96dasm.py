#!/usr/bin/env python3
"""Disassemble MCS-96 (8x9x) code -- the U-110's N8097BH program ROM.

    python3 tools/mcs96dasm.py 0x69f0 0x120
    python3 tools/mcs96dasm.py --xref 0x69f0        # who calls this address
    python3 tools/mcs96dasm.py --all > /tmp/rom.asm

MAME can do this from its debugger, but `-debugscript` only runs when the debugger
actually stops the machine, which under `-debugger none` it usually does not -- the
command silently does nothing.  This reads the same tables MAME's disassembler is built
from (build/generated/emu/cpu/mcs96/i8x9xd.hxx, produced by mcs96make.py from
mcs96ops.lst) and follows mcs96d.cpp's formatting, so the output matches the debugger's.
"""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(HERE, 'roms', 'roland_u110_pgm_(15179960).bin')
TBL = os.path.join(HERE, 'mame', 'build', 'generated', 'emu', 'cpu', 'mcs96', 'i8x9xd.hxx')

# The 8x9x names MAME gives the low registers (mcs96d.cpp / i8x9xd.cpp).
R8 = {0x00: ('0', '0'), 0x02: ('ad_command', 'ad_result_lo'), 0x03: ('hsi_mode', 'ad_result_hi'),
      0x06: ('hso_command', 'hsi_status'), 0x07: ('sbuf', 'sbuf'),
      0x08: ('int_mask', 'int_mask'), 0x09: ('int_pending', 'int_pending'),
      0x0a: ('watchdog', 'timer1_lo'), 0x0e: ('baud_rate', 'port0'),
      0x0f: ('port1', 'port1'), 0x10: ('port2', 'port2'), 0x11: ('sp_con', 'sp_stat'),
      0x15: ('ioc0', 'ios0'), 0x16: ('ioc1', 'ios1'), 0x17: ('pwm_control', 'pwm_control'),
      0x1c: ('al', 'al'), 0x1d: ('ah', 'ah'), 0x1e: ('dl', 'dl'), 0x1f: ('dh', 'dh'),
      0x20: ('bl', 'bl'), 0x21: ('bh', 'bh'), 0x22: ('cl', 'cl'), 0x23: ('ch', 'ch')}
R16 = {0x00: '0', 0x18: 'sp', 0x1c: 'ax', 0x1e: 'dx', 0x20: 'bx', 0x22: 'cx'}


def load_entries():
    """(mnemonic, opcode_fe, mode) for each of the 256 opcodes."""
    txt = open(TBL).read()
    out = []
    for m in re.finditer(r'\{\s*"([^"]*)"\s*,\s*(NULL|"[^"]*")\s*,\s*(DASM_\w+)\s*,', txt):
        fe = None if m.group(2) == 'NULL' else m.group(2).strip('"')
        out.append((m.group(1), fe, m.group(3)))
    if len(out) != 0x100:
        sys.exit("parsed %d entries from %s, expected 256" % (len(out), TBL))
    return out


class Dasm:
    def __init__(self, rom):
        self.d = rom
        self.e = load_entries()

    def r8(self, a):
        return self.d[a & 0xffff]

    def r16(self, a):
        return self.d[a & 0xffff] | (self.d[(a + 1) & 0xffff] << 8)

    def n8(self, r, dest=False):
        if r in R8:
            return R8[r][0 if dest else 1]
        return "%02x" % r

    def n16(self, r, dest=False):
        return R16.get(r, "%02x" % r)

    def n_ind(self, r):
        return "[%s]+" % self.n16(r & 0xfe) if r & 1 else "[%s]" % self.n16(r)

    def n_idx(self, r, delta):
        delta = delta - 256 if delta > 127 else delta
        if r == 0:
            return "%04x" % (delta & 0xffff) if delta < 0 else "%02x" % delta
        return ("-%02x[%s]" % (-delta, self.n16(r))) if delta < 0 else ("%02x[%s]" % (delta, self.n16(r)))

    def one(self, pc):
        """-> (length, text).  Mirrors mcs96_disassembler::disassemble()."""
        off = 0
        base = pc
        if self.r8(pc) == 0xfe and self.e[self.r8(pc + 1)][1]:
            pc += 1
            off = 1
        op = self.r8(pc)
        name, fe, mode = self.e[op]
        if off:
            name = fe
        r8, r16, n8, n16 = self.r8, self.r16, self.n8, self.n16
        M = mode[5:]
        if M == 'none':
            t, n = "", 1
        elif M == 'nop_2':
            t, n = " %02x" % r8(pc + 1), 2
        elif M == 'rel8':
            t, n = " %04x" % ((pc + 2 + (r8(pc + 1) ^ 0x80) - 0x80) & 0xffff), 2
        elif M == 'rel11':
            dl = ((op << 8) | r8(pc + 1)) & 0x7ff
            if dl & 0x400:
                dl -= 0x800
            t, n = " %04x" % ((pc + 2 + dl) & 0xffff), 2
        elif M == 'rel16':
            t, n = " %04x" % ((pc + 3 + r16(pc + 1)) & 0xffff), 3
        elif M == 'rrel8':
            t, n = " %s, %04x" % (n8(r8(pc + 1), True), (pc + 3 + ((r8(pc + 2) ^ 0x80) - 0x80)) & 0xffff), 3
        elif M == 'brrel8':
            t, n = " %s, %d, %04x" % (n8(r8(pc + 1)), op & 7,
                                      (pc + 3 + ((r8(pc + 2) ^ 0x80) - 0x80)) & 0xffff), 3
        elif M == 'wrrel8':
            t, n = " %s, %04x" % (n16(r8(pc + 1), True), (pc + 3 + ((r8(pc + 2) ^ 0x80) - 0x80)) & 0xffff), 3
        elif M == 'direct_1b':
            t, n = " %s" % n8(r8(pc + 1), True), 2
        elif M == 'direct_1w':
            t, n = " %s" % n16(r8(pc + 1), op in (0x01, 0xcc)), 2
        elif M == 'direct_2b':
            t, n = " %s, %s" % (n8(r8(pc + 2), op == 0xb0), n8(r8(pc + 1), op == 0xc4)), 3
        elif M == 'direct_2e':
            t, n = " %s, %s" % (n16(r8(pc + 2), (op & 0xef) == 0xac), n8(r8(pc + 1), op == 0x0f)), 3
        elif M == 'direct_2w':
            t, n = " %s, %s" % (n16(r8(pc + 2), op == 0xa0), n16(r8(pc + 1), op == 0xc0)), 3
        elif M == 'direct_3b':
            t, n = " %s, %s, %s" % (n8(r8(pc + 3), True), n8(r8(pc + 2)), n8(r8(pc + 1))), 4
        elif M == 'direct_3e':
            t, n = " %s, %s, %s" % (n16(r8(pc + 3), True), n8(r8(pc + 2)), n8(r8(pc + 1))), 4
        elif M == 'direct_3w':
            t, n = " %s, %s, %s" % (n16(r8(pc + 3), True), n16(r8(pc + 2)), n16(r8(pc + 1))), 4
        elif M == 'immed_1b':
            t, n = " #%02x" % r8(pc + 1), 2
        elif M == 'immed_2b':
            t, n = " %s, #%02x" % (n8(r8(pc + 2), op == 0xb1), r8(pc + 1)), 3
        elif M == 'immed_2e':
            t, n = " %s, #%02x" % (n16(r8(pc + 2), (op & 0xef) == 0xad), r8(pc + 1)), 3
        elif M == 'immed_or_reg_2b':
            t = (" %s, %s" % (n8(r8(pc + 2)), n8(r8(pc + 1)))) if r8(pc + 1) >= 0x10 \
                else (" %s, #%02x" % (n8(r8(pc + 2)), r8(pc + 1)))
            n = 3
        elif M == 'immed_3b':
            t, n = " %s, %s, #%02x" % (n8(r8(pc + 3), True), n8(r8(pc + 2)), r8(pc + 1)), 4
        elif M == 'immed_3e':
            t, n = " %s, %s, #%02x" % (n16(r8(pc + 3), True), n8(r8(pc + 2)), r8(pc + 1)), 4
        elif M == 'immed_1w':
            t, n = " #%04x" % r16(pc + 1), 3
        elif M == 'immed_2w':
            t, n = " %s, #%04x" % (n16(r8(pc + 3), op == 0xa1), r16(pc + 1)), 4
        elif M == 'immed_or_reg_2w':
            t = (" %s, %s" % (n16(r8(pc + 2)), n8(r8(pc + 1)))) if r8(pc + 1) >= 0x10 \
                else (" %s, #%02x" % (n16(r8(pc + 2)), r8(pc + 1)))
            n = 3
        elif M == 'immed_3w':
            t, n = " %s, %s, #%04x" % (n16(r8(pc + 4), True), n16(r8(pc + 3)), r16(pc + 1)), 5
        elif M == 'indirect_1n':
            t, n = " [%s]" % n16(r8(pc + 1)), 2
        elif M == 'indirect_1w':
            t, n = " %s" % self.n_ind(r8(pc + 1)), 2
        elif M == 'indirect_2b':
            t, n = " %s, %s" % (n8(r8(pc + 2), op == 0xb2), self.n_ind(r8(pc + 1))), 3
        elif M == 'indirect_2w':
            t, n = " %s, %s" % (n16(r8(pc + 2), op == 0xa2), self.n_ind(r8(pc + 1))), 3
        elif M == 'indirect_3b':
            t, n = " %s, %s, %s" % (n8(r8(pc + 3), True), n8(r8(pc + 2)), self.n_ind(r8(pc + 1))), 4
        elif M == 'indirect_3e':
            t, n = " %s, %s, %s" % (n16(r8(pc + 3), True), n8(r8(pc + 2)), self.n_ind(r8(pc + 1))), 4
        elif M == 'indirect_3w':
            t, n = " %s, %s, %s" % (n16(r8(pc + 3), True), n16(r8(pc + 2)), self.n_ind(r8(pc + 1))), 4
        elif M.startswith('indexed_'):
            long_form = bool(r8(pc + 1) & 1)
            nops = int(M[8])                       # 1, 2 or 3
            wide = M[9] in 'we'                    # w/e -> 16-bit destination name
            is_dest = op in (0xa3, 0xb3) or (op & 0xef) == 0xaf
            if long_form:
                idx = ("%04x" % r16(pc + 2)) if r8(pc + 1) == 0x01 \
                    else ("%04x[%s]" % (r16(pc + 2), n16(r8(pc + 1) - 1)))
                if nops == 1:
                    t, n = " %s" % idx, 4
                elif nops == 2:
                    reg = n16(r8(pc + 4), is_dest) if M[9] == 'w' else n8(r8(pc + 4), is_dest)
                    t, n = " %s, %s" % (reg, idx), 5
                else:
                    dst = n8(r8(pc + 5), True) if M[9] == 'b' else n16(r8(pc + 5), True)
                    src = n8(r8(pc + 4)) if M[9] in 'be' else n16(r8(pc + 4))
                    t, n = " %s, %s, %s" % (dst, src, idx), 6
            else:
                idx = self.n_idx(r8(pc + 1), r8(pc + 2))
                if nops == 1:
                    t, n = " %s" % idx, 3
                elif nops == 2:
                    reg = n16(r8(pc + 3), is_dest) if M[9] == 'w' else n8(r8(pc + 3), is_dest)
                    t, n = " %s, %s" % (reg, idx), 4
                else:
                    dst = n8(r8(pc + 4), True) if M[9] == 'b' else n16(r8(pc + 4), True)
                    src = n8(r8(pc + 3)) if M[9] in 'be' else n16(r8(pc + 3))
                    t, n = " %s, %s, %s" % (dst, src, idx), 5
        else:
            t, n = "  ; unhandled mode %s" % mode, 1
        return n + off, name + t

    def listing(self, start, length):
        pc, end = start, start + length
        while pc < end:
            n, txt = self.one(pc)
            raw = " ".join("%02X" % self.d[(pc + i) & 0xffff] for i in range(n))
            yield "%04X: %-17s %s" % (pc, raw, txt)
            pc += n


def xref(dis, target):
    """Every CALL/LCALL/branch in the ROM whose destination is `target`."""
    hits = []
    for pc in range(0x2000, 0x10000):
        n, txt = dis.one(pc)
        m = re.search(r'\b(l?call|l?jmp|sjmp|scall|br)\b\s+([0-9a-f]{4})$', txt)
        if m and int(m.group(2), 16) == target:
            hits.append((pc, txt))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('start', nargs='?', default=None)
    ap.add_argument('length', nargs='?', default='0x40')
    ap.add_argument('--xref', default=None, help='list callers/branchers to this address')
    ap.add_argument('--all', action='store_true', help='disassemble 0x2000..0xdfff')
    ap.add_argument('--rom', default=ROM)
    a = ap.parse_args()

    dis = Dasm(open(a.rom, 'rb').read())
    if a.xref is not None:
        t = int(a.xref, 0)
        for pc, txt in xref(dis, t):
            print("%04X: %s" % (pc, txt))
        return
    if a.all:
        for l in dis.listing(0x2000, 0xc000):
            print(l)
        return
    if a.start is None:
        ap.error("give a start address, --xref or --all")
    for l in dis.listing(int(a.start, 0), int(a.length, 0)):
        print(l)


if __name__ == '__main__':
    main()
