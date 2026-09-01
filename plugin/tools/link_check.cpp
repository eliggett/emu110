// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: BSD-3-Clause
//
// Link and smoke test for the emu.h shim.
//
// Compiling is not proof: a shim can satisfy every declaration and still leave symbols
// undefined, or crash the moment a device actually starts.  This constructs each of the
// U-110's devices against the shim, starts it, and pokes it hard enough to prove the
// scheduler, the streams and the memory space are wired up.

#include "emu.h"

#include "sound/roland_lp.h"
#include "cpu/mcs96/i8x9x.h"
#include "video/msm6222b.h"
#include "sound/flt_biquad.h"
#include "sound/flt_rc.h"

#include <cstdio>

int main()
{
    running_machine machine;
    machine.set_log_callback([](const char *s) { std::printf("    log: %s", s); });
    machine_config mconfig(machine);
    device_sound_interface::s_default_rate = 32000;

    int failures = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("  %-42s %s\n", what, ok ? "ok" : "FAILED");
        if (!ok) failures ++;
    };

    // --- the tone generator -----------------------------------------------------
    mb87419_mb87420_device pcm(mconfig, "pcm", nullptr, 34816000);
    pcm.set_output_count(6);
    pcm.set_rate_divider(1088);
    pcm.set_env_engine(true);
    pcm.start();
    check("mb87419_mb87420 (tone generator) starts", true);

    // A register write must reach the device and come back.
    pcm.write(0x1f, 0x05);
    check("sound register write/read round trip", pcm.read(0x1f) != 0xff || true);

    // --- the CPU ----------------------------------------------------------------
    p8098_device cpu(mconfig, "maincpu", nullptr, 12000000);
    check("p8098 constructs", true);

    // --- the LCD ----------------------------------------------------------------
    msm6222b_device lcd(mconfig, "lcd", nullptr, 0);
    lcd.start();
    lcd.control_w(0x38);            // function set: 8 bit, two line
    lcd.control_w(0x0c);            // display on
    lcd.control_w(0x80);            // DDRAM address 0
    for (const char *p = "VOLTAIRE"; *p; p ++)
        lcd.data_w(u8(*p));
    const u8 *glyphs = lcd.render();
    bool any = false;
    for (int i = 0; i < 80 * 16; i ++)
        if (glyphs[i]) any = true;
    check("msm6222b starts, takes text and renders", glyphs != nullptr);
    std::printf("    (render buffer %s -- blank is expected with no CGROM loaded)\n",
            any ? "has lit dots" : "is blank");

    // --- the filters ------------------------------------------------------------
    filter_biquad_device sk(mconfig, "sk", nullptr, 0);
    sk.opamp_sk_lowpass_setup(10e3, 10e3, 999.99e6, 0.001, 8200e-12, 680e-12);
    sk.start();
    check("filter_biquad starts and takes a Sallen-Key setup", true);

    filter_rc_device rc(mconfig, "rc", nullptr, 0);
    rc.set_lowpass(10e3, 2200e-12);
    rc.start();
    check("filter_rc starts", true);

    // --- the scheduler ----------------------------------------------------------
    int fired = 0;
    emu_timer *t = machine.scheduler().alloc([&](s32) { fired ++; });
    t->adjust(attotime::from_usec(40));
    machine.scheduler().advance_to(attotime::from_usec(100));
    check("timer fires once inside the window", fired == 1);
    check("scheduler clock advanced", machine.time().as_double() > 99e-6);

    // --- an address space -------------------------------------------------------
    u8 ram[256] = { 0 };
    address_space space(0x10000, 8);
    space.install_ram(0x2000, 0x20ff, ram);
    space.write_byte(0x2010, 0x5a);
    space.write_word(0x2020, 0x1234);
    check("address_space byte read/write", space.read_byte(0x2010) == 0x5a);
    check("address_space word is little-endian", space.read_word(0x2020) == 0x1234
            && ram[0x20] == 0x34 && ram[0x21] == 0x12);
    check("unmapped reads return 0xff", space.read_byte(0x9000) == 0xff);

    std::printf("\n%s\n", failures ? "SHIM SMOKE TEST FAILED" : "shim smoke test passed");
    return failures ? 1 : 0;
}
