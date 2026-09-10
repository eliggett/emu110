// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Will the MACHINE accept this card image, and does a tone on it make a sound?
//
// Format-agnostic on purpose: it reads nothing but the image and the firmware's opinion of
// it, so it checks a real SN-U110 dump and a card built by tools/r8_to_u110.py the same
// way.  The firmware is the authority -- it mounts the card by the same signature check
// and presence-bit poll it uses on hardware, caches the card's ID at 0x2743, and refuses
// anything it does not recognise.  No amount of agreement between our own writer and our
// own reader proves a card is valid; this does.
//
//   card_check CARD.bin [--slot N] [--tone N] [--note N] [--part N] [--data DIR]
//
// Exits nonzero if the machine did not mount the card.

#include "u110_core.h"

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Where the firmware caches one mounted card ID per slot.  ROM-ANALYSIS.md section 6.5:
// the value is the card's catalogue ID on success, 0xFF on a signature failure, and 0 for
// a slot it never looked at because the presence bit said empty.
constexpr uint16_t kCardIdCache = 0x2743;
constexpr uint16_t kPartBase    = 0x2814;
constexpr uint16_t kPartStride  = 16;
constexpr uint16_t kRxSwitchAddr = 0x3C00;
constexpr uint8_t  kRxExclusive  = 0x20;
constexpr uint16_t kDeviceIdAddr = 0x3C01;

constexpr uint32_t kToneBase   = 0x1000;
constexpr uint32_t kToneStride = 0x50;

std::vector<uint8_t> readFile(const std::string &path)
{
    std::vector<uint8_t> out;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0)
    {
        out.resize(size_t(n));
        if (std::fread(out.data(), 1, out.size(), f) != out.size())
            out.clear();
    }
    std::fclose(f);
    return out;
}

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

/// First file in `dir` whose lowercased name contains every one of `needles`.
std::string findRom(const std::string &dir, const std::vector<std::string> &needles)
{
    DIR *d = ::opendir(dir.c_str());
    if (d == nullptr)
        return std::string();
    std::string best;
    while (const dirent *e = ::readdir(d))
    {
        const std::string name = lower(e->d_name);
        bool all = true;
        for (const std::string &n : needles)
            all = all && name.find(n) != std::string::npos;
        if (all && (best.empty() || name < lower(best)))
            best = e->d_name;
    }
    ::closedir(d);
    return best.empty() ? std::string() : dir + "/" + best;
}

int g_fail = 0;

void check(bool ok, const char *what)
{
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    g_fail += ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    const char *cardPath = nullptr;
    unsigned slot = 0, tone = 1, part = 0;
    int note = 36;
    std::string data = std::getenv("U110_DATA_DIR") != nullptr
                     ? std::getenv("U110_DATA_DIR") : "../..";

    for (int i = 1; i < argc; i ++)
    {
        const char *a = argv[i];
        const auto next = [&]() { return i + 1 < argc ? argv[++ i] : "0"; };
        if      (std::strcmp(a, "--slot") == 0) slot = unsigned(std::atoi(next()));
        else if (std::strcmp(a, "--tone") == 0) tone = unsigned(std::atoi(next()));
        else if (std::strcmp(a, "--note") == 0) note = std::atoi(next());
        else if (std::strcmp(a, "--part") == 0) part = unsigned(std::atoi(next()));
        else if (std::strcmp(a, "--data") == 0) data = next();
        else if (a[0] != '-')                   cardPath = a;
        else { std::fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (cardPath == nullptr)
    {
        std::fprintf(stderr, "usage: card_check CARD.bin [--slot N] [--tone N] "
                             "[--note N] [--part N] [--data DIR]\n");
        return 2;
    }

    const std::string roms = data + "/roms";
    voltaire::U110Core core;

    const std::string pgm = findRom(roms, { "u110", "pgm" });
    if (pgm.empty())
    {
        std::fprintf(stderr, "no U-110 program ROM in %s\n", roms.c_str());
        return 2;
    }
    const std::vector<uint8_t> pgmData = readFile(pgm);
    if (core.loadProgramRom(pgmData.data(), pgmData.size()) != voltaire::LoadResult::Ok)
    {
        std::fprintf(stderr, "%s is not a usable program ROM\n", pgm.c_str());
        return 2;
    }
    std::printf("program  %s\n", pgm.c_str());
    for (unsigned b = 0; b < 4; b ++)
    {
        const std::string w = findRom(roms, { "waverom" + std::to_string(b) });
        const std::vector<uint8_t> d = readFile(w);
        if (d.empty() || core.loadWaveRom(b, d.data(), d.size()) != voltaire::LoadResult::Ok)
        {
            std::fprintf(stderr, "wave ROM bank %u missing or unusable\n", b);
            return 2;
        }
    }

    const std::vector<uint8_t> card = readFile(cardPath);
    std::printf("card     %s (%zu bytes)\n", cardPath, card.size());
    if (card.empty())
    {
        std::fprintf(stderr, "cannot read the card image\n");
        return 2;
    }
    // What the image says about itself, before the machine gets an opinion.  The header is
    // linear in a dump -- ROM-ANALYSIS.md section 6.4 -- so this needs no descrambling.
    if (card.size() > 0x20)
    {
        std::string label;
        for (size_t i = 0x10; i < 0x20 && card[i] >= 0x20 && card[i] < 0x7f; i ++)
            label.push_back(char(card[i]));
        std::printf("         header says id %u (0x%02X), label \"%s\"\n",
                    card[0x20], card[0x20], label.c_str());
    }
    if (core.loadCard(slot, card.data(), card.size()) != voltaire::LoadResult::Ok)
    {
        std::fprintf(stderr, "loadCard rejected the image\n");
        return 2;
    }

    core.reset();
    core.runUntilIdle();

    // runUntilIdle() stops the moment the firmware reaches its idle loop, which is BEFORE
    // the card poller has run a pass.  Mounting is edge-triggered against a snapshot of
    // PORT1 taken every service pass (ROM-ANALYSIS.md section 6.9) and boot is just the
    // first of those, so the machine needs real emulated time to notice a slot at all.
    // Without this the cache reads 0x00 for every slot and a perfectly good card looks
    // rejected -- which is exactly what it did on a known-good SN-U110-08.
    {
        std::vector<float> a(512), b(512);
        for (uint32_t i = 0; i < 60; i ++)          // ~640 ms at 48 kHz
            core.renderStereo(a.data(), b.data(), 512);
    }

    std::printf("\nthe firmware's own verdict, from its cache at 0x%04X:\n", kCardIdCache);
    unsigned mounted = 0;
    for (unsigned s = 0; s < 4; s ++)
    {
        const uint8_t v = core.readMem(uint16_t(kCardIdCache + s));
        const char *what = v == 0x00 ? "slot empty"
                         : v == 0xFF ? "MOUNT REFUSED -- signature check failed"
                                     : "mounted";
        std::printf("  slot %u: 0x%02X  %s%s\n", s, v, what, s == slot ? "   <- ours" : "");
        if (s == slot) mounted = v;
    }
    check(mounted != 0x00, "the machine saw a card in the slot");
    check(mounted != 0xFF, "the machine accepted the signature");
    if (card.size() > 0x20)
        check(mounted == card[0x20], "the ID it cached is the one the header declares");
    if (mounted == 0x00 || mounted == 0xFF)
    {
        std::printf("\n%d check(s) failed\n", g_fail);
        return 1;
    }

    // Tone names come out of the card as the machine addresses it, descrambled.  The
    // firmware finds the end of the list by scanning for an all-spaces name.
    std::printf("\ntones on the card, read through the machine:\n");
    unsigned ntones = 0;
    for (unsigned t = 0; t < 99; t ++)
    {
        uint8_t nm[10] = { 0 };
        if (core.readCardRom(slot, kToneBase + kToneStride * t, nm, sizeof(nm)) != sizeof(nm))
            break;
        bool blank = true, printable = true;
        for (const uint8_t c : nm)
        {
            blank = blank && c == ' ';
            printable = printable && c >= 0x20 && c < 0x7f;
        }
        if (blank || !printable)
            break;
        std::printf("  %2u  \"%.10s\"\n", t + 1, reinterpret_cast<const char *>(nm));
        ntones ++;
    }
    check(ntones > 0, "the card offers at least one tone");

    // Point a part at a card tone the way the UI does: a Roland DT1 to the part's media
    // and tone parameters, which makes the firmware rebuild its work RAM properly.
    // Writing the patch record directly would not -- ROM-ANALYSIS.md section 6.6.
    const uint8_t dev = core.readMem(kDeviceIdAddr) & 0x7f;
    core.writeMem(kRxSwitchAddr, uint8_t(core.readMem(kRxSwitchAddr) | kRxExclusive));
    const auto dt1 = [&](uint8_t a1, uint8_t a2, uint8_t a3, uint8_t v)
    {
        const int sum = a1 + a2 + a3 + v;
        const uint8_t msg[] = { 0xf0, 0x41, dev, 0x23, 0x12, a1, a2, a3, v,
                                uint8_t((128 - (sum & 0x7f)) & 0x7f), 0xf7 };
        core.midiIn(msg, sizeof(msg), 0);
    };

    std::vector<float> l(512), r(512);
    const auto runMs = [&](double ms)
    {
        uint32_t n = uint32_t(ms * voltaire::kCoreSampleRate / 1000.0);
        while (n != 0)
        {
            const uint32_t k = n > 512 ? 512 : n;
            core.renderStereo(l.data(), r.data(), k);
            n -= k;
        }
    };

    dt1(0x00, uint8_t(0x10 + part), 0x02, uint8_t(mounted));   // media := this card
    runMs(60);
    dt1(0x00, uint8_t(0x10 + part), 0x03, uint8_t(tone));      // tone within it
    runMs(200);

    const uint16_t rec = uint16_t(kPartBase + kPartStride * part);
    const uint8_t media = core.readMem(rec) & 0x1f;
    const uint8_t toneNo = core.readMem(uint16_t(rec + 1));
    const uint8_t chan = core.readMem(uint16_t(rec + 2)) & 0x0f;
    const uint8_t flags = core.readMem(uint16_t(rec + 0x0b));
    std::printf("\npart %u now reads media %u tone %u, MIDI channel %u%s\n",
                part + 1, media, toneNo, chan + 1,
                (flags & 0xe0) == 0xc0 ? "   (part is DISABLED in this patch)" : "");
    check(media == mounted, "the part took the card as its media");
    check(toneNo == tone, "the part took the tone number");

    // Play it.  Peak, not rms: a drum hit is mostly silence and an rms over a whole
    // second would hide a real transient.
    float peakBefore = 0.0f;
    runMs(200);
    for (uint32_t i = 0; i < 512; i ++)
        peakBefore = std::max(peakBefore, std::max(std::abs(l[i]), std::abs(r[i])));

    const uint8_t on[]  = { uint8_t(0x90 | chan), uint8_t(note & 0x7f), 100 };
    const uint8_t off[] = { uint8_t(0x80 | chan), uint8_t(note & 0x7f), 0 };
    core.midiIn(on, sizeof(on), 0);
    float peak = 0.0f;
    for (uint32_t block = 0; block < 94; block ++)      // ~1 s at 512 frames, 48 kHz
    {
        core.renderStereo(l.data(), r.data(), 512);
        for (uint32_t i = 0; i < 512; i ++)
            peak = std::max(peak, std::max(std::abs(l[i]), std::abs(r[i])));
    }
    core.midiIn(off, sizeof(off), 0);
    runMs(50);

    std::printf("\nnote %d on channel %u, tone %u: peak %.4f (silence before it was %.4f)\n",
                note, chan + 1, tone, peak, peakBefore);
    check(peak > 0.002f, "the tone made a sound");

    std::printf("\n%s\n", g_fail == 0 ? "all checks passed" : "checks failed");
    return g_fail == 0 ? 0 : 1;
}
