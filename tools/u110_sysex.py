#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
u110_sysex.py -- Roland exclusive messages for the U-110.

The framing is confirmed against the firmware's own parser, not just the manual (whose
scan OCRs badly around the address map).  At 0x5BA7 the handler buffers the incoming
message from 0x2300 and checks, in order:

    5BAF: jbs 41, 5, 5bb5      ; R41 is the SETUP receive-switch mask -- bit 5 is
                              ; EXCLUSIVE.  Clear -> the message is discarded silently.
    5BB5: ldb 70, 2300
    5BBA: cmpb 70, #41         ; manufacturer: Roland
    5BC2: ldb 70, 2301
    5BC7: cmpb 70, 42          ; device ID, from NVRAM 0x3C01 (loaded at 0x5624)
    5BCF: ldb 70, 2302
    5BD4: cmpb 70, #23         ; model ID: U-110
    5BDC: cmpb e8, #08         ; and the message must be longer than 8 bytes
    5BE4..5BFB                 ; checksum over everything from 0x2304 to the F7,
                              ; masked to 7 bits, must come to zero

so a message is  F0 41 <dev> 23 <cmd> <addr hi mid lo> <data...> <sum> F7  and the
checksum covers address and data.  A wrong sum is not silently ignored: the U-110 puts
"Chk Sum Err [" on the display (the string is at 0x5C61) along with the value it wanted.

DEVICE ID.  Read it out of the machine rather than guessing -- it is one byte of
battery-backed RAM:

    python3 -c "print('%02X' % open('mame/nvram/u110/workram','rb').read()[0x3C01-0x2100])"

EXCLUSIVE MUST BE ON.  Bit 5 of 0x3C00, SETUP:MIDI:EXCLUSIVE on the panel.  There is no
way around this over MIDI -- the switch that gates exclusive messages cannot itself be set
by an exclusive message.  A factory-initialised mask is 0x7F, which has it on; a wiped one
is 0x00, which does not, and then every message here does nothing at all.  That is the
failure this module's verify_link() exists to catch.
"""

ROLAND   = 0x41
MODEL_ID = 0x23
DT1      = 0x12          # data set 1  -- write
RQ1      = 0x11          # request 1   -- read

# Address map, section 4.2.2 of the Owner's Manual.  Patch parameters (temporary) are the
# EDIT BUFFER: writing here changes what is playing now and is discarded by the next patch
# change.  It does NOT touch the 64 stored patches unless the operator presses WRITE, which
# is why every address used here is a temporary one.
PATCH_COMMON = (0x00, 0x01, 0x00)     # + offset below
CHORUS_RATE   = 0x19
CHORUS_DEPTH  = 0x1A
TREMOLO_RATE  = 0x1B
TREMOLO_DEPTH = 0x1C

# Part parameters (temporary), 00 1n xx with n the part number 0-5.
PART_OUTPUT_ASSIGN = 0x00
PART_RX_CHANNEL    = 0x01
PART_TONE_MEDIA    = 0x02
PART_TONE_NUMBER   = 0x03
PART_BEND_RANGE    = 0x04
PART_KEY_RANGE_LO  = 0x05
PART_KEY_RANGE_HI  = 0x06
PART_LEVEL         = 0x07
PART_VELOCITY_SENS = 0x08


def checksum(body):
    """Roland: address and data sum to zero in seven bits."""
    return (128 - (sum(body) & 0x7F)) & 0x7F


def dt1(addr, data, device_id=0x0F):
    """One DT1 write.  addr is a 3-tuple, data a sequence of 7-bit values."""
    a = list(addr)
    d = [int(x) & 0x7F for x in data]
    if any(x > 0x7F for x in a):
        raise ValueError('address bytes must be 7-bit: %r' % (addr,))
    body = a + d
    return bytes([0xF0, ROLAND, device_id & 0x7F, MODEL_ID, DT1] + body
                 + [checksum(body), 0xF7])


# The probe address for "is this machine listening?".  It MUST be a parameter's own top
# address: the firmware ignores a request that points into the middle of a block, without
# a reply and without a display change.  0x000100 -- the start of the patch-common block --
# is NOT such an address, and an RQ1 there gets nothing back, which looks exactly like the
# machine being deaf.  Verified against the emulator's MIDI OUT log:
#
#   RQ1 00 01 00  ->  (nothing)
#   RQ1 00 01 1A  ->  F0 41 0F 23 12 00 01 1A 07 5E F7      chorus depth = 7
#   RQ1 00 10 03  ->  F0 41 0F 23 12 00 10 03 01 6C F7      part 0 tone  = 1
#
# Chorus depth is used as the probe because it is a real single-byte parameter AND it is
# one of the two values this module exists to write, so a successful probe tests the exact
# path that matters.
PROBE_ADDR = (0x00, 0x01, CHORUS_DEPTH)
PROBE_SIZE = 1


def rq1(addr, size, device_id=0x0F):
    """One RQ1 read request; the U-110 answers with a DT1."""
    body = list(addr) + [(size >> 14) & 0x7F, (size >> 7) & 0x7F, size & 0x7F]
    return bytes([0xF0, ROLAND, device_id & 0x7F, MODEL_ID, RQ1] + body
                 + [checksum(body), 0xF7])


# Bulk dump, Owner's Manual section 4.3.  Confirmed empirically now that the emulator has
# MIDI OUT: each RQ1 below was sent and the replies counted, framed and checksummed.
#
#   RQ1 01 00 00 / 00 00 20  ->    1 packet  at 010000            (SETUP)
#   RQ1 02 00 00 / 01 00 00  ->  128 packets at 020000..027F00    (patches 1-64)
#
# The 128-packet dump is 17706 bytes and comes out clean -- 0 malformed, 0 bad checksums.
# Note the size field is 7-bit packed like the address, so 01 00 00 is 1 << 14, not 0x10000.
BULK_SETUP        = ((0x01, 0x00, 0x00), (0x00, 0x00, 0x20))
BULK_TEMP_PATCH   = ((0x00, 0x02, 0x00), (0x00, 0x01, 0x00))
BULK_PATCH_1_64   = ((0x02, 0x00, 0x00), (0x01, 0x00, 0x00))


def rq1_raw(addr, size_bytes, device_id=0x0F):
    """RQ1 with the size given as three 7-bit bytes, as the manual's tables print it."""
    body = list(addr) + list(size_bytes)
    return bytes([0xF0, ROLAND, device_id & 0x7F, MODEL_ID, RQ1] + body
                 + [checksum(body), 0xF7])


def bulk_request(which, device_id=0x0F):
    """RQ1 for one of the BULK_* blocks above."""
    return rq1_raw(which[0], which[1], device_id)


def patch_common(offset, value, device_id=0x0F):
    return dt1((PATCH_COMMON[0], PATCH_COMMON[1], offset), [value], device_id)


def part_param(part, offset, value, device_id=0x0F):
    if not 0 <= part <= 5:
        raise ValueError('part must be 0-5')
    return dt1((0x00, 0x10 | part, offset), [value], device_id)


def effects_off(device_id=0x0F):
    """Chorus and tremolo silenced for the whole patch, hence for every part.

    Depth 0 rather than routing parts away from the effect through Output Assign: one
    parameter each, no per-part bookkeeping, and nothing to get wrong."""
    return [patch_common(CHORUS_DEPTH,  0, device_id),
            patch_common(TREMOLO_DEPTH, 0, device_id)]


if __name__ == '__main__':
    show = lambda m: ' '.join('%02X' % b for b in m)
    print('chorus depth = 0 :', show(patch_common(CHORUS_DEPTH, 0)))
    print('tremolo depth= 0 :', show(patch_common(TREMOLO_DEPTH, 0)))
    print('part 0 tone  =62 :', show(part_param(0, PART_TONE_NUMBER, 62)))
    print('part 0 level =127:', show(part_param(0, PART_LEVEL, 127)))
    print('RQ1 patch common :', show(rq1((0x00, 0x01, 0x00), 0x20)))
    print('RQ1 bulk 1-64    :', show(bulk_request(BULK_PATCH_1_64)))
    print('RQ1 bulk SETUP   :', show(bulk_request(BULK_SETUP)))
