#!/usr/bin/env python3
"""Reconstruct voice setups from a roland_u110 register trace (error.log).

Maintains per-voice register state and snapshots it whenever a voice is switched
on via the enable masks, then resolves the sample against waverom0's sample table.
"""
import re, sys, math

def load_sample_table(path):
    rom=open(path,'rb').read(); tbl={}
    for i in range(256):
        e=rom[0x100+10*i:0x100+10*i+10]
        if e[:3]==b'\xff\xff\xff': break
        st=e[0]|e[1]<<8|(e[2]&7)<<16
        tbl[(st,(e[2]>>4)&3,(e[2]>>3)&1)]=(i,e)
    return tbl

def main(log, wr0):
    tbl=load_sample_table(wr0)
    regs=[dict() for _ in range(32)]; sel=0; enabled=[0]*32; out=[]
    for line in open(log,errors='replace'):
        m=re.search(r'TG ([\d.]+) v(\w\w) reg (\w\w) = (\w\w)',line)
        if not m: continue
        t=float(m.group(1)); r=int(m.group(3),16); v=int(m.group(4),16)
        if r==0x1f: sel=v&0x1f; continue
        if r in (0x11,0x12,0x15,0x16):
            base={0x11:0,0x12:8,0x15:16,0x16:24}[r]
            for b in range(8):
                ch=base+b; on=(v>>b)&1
                if on and not enabled[ch]: out.append((t,ch,dict(regs[ch])))
                enabled[ch]=on
            continue
        regs[sel][r]=v
    for t,ch,R in out:
        need=[0x02,0x03,0x04,0x05,0x08,0x09,0x0a,0x0b]
        if any(k not in R for k in need): continue
        bank=R[0x02]|R[0x03]<<8
        phase=R[0x08]|R[0x09]<<8|R[0x0a]<<16|R[0x0b]<<24
        step=R[0x04]|R[0x05]<<8
        within=((bank>>10)&1)<<18 | ((phase>>14)&0x3ffff)
        key=(within,(bank>>12)&3,(bank>>11)&1)
        s=tbl.get(key)
        if not s: print(f't={t:8.4f} v{ch:02d} step=0x{step:04X} start=0x{within:05X} bank=0x{bank:04X}  <unmatched>"'); continue
        idx,e=s; ref,fine=e[8],e[7]
        semis=12*math.log2(step/0x4000) if step else float('nan')
        note_impl=ref+semis
        print(f't={t:8.4f} v{ch:02d} smpl={idx:3d} ref={ref:3d} fine=0x{fine:02X} '
              f'step=0x{step:04X} ({semis:+7.3f} st)  => implied note {note_impl:7.3f}')

if __name__=='__main__':
    main(sys.argv[1], sys.argv[2])
