// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The preset file format: that a patch survives the round trip, and that nothing which is
// not a patch is ever believed.  The second half is the important one -- these files are
// hand-editable by design, and what comes out of one is handed to the machine's own patch
// loader.
//
//   preset_check [DIR]        DIR defaults to a temporary directory

#include "PresetLibrary.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks = 0, g_pass = 0;

void check(bool ok, const char *what)
{
    g_checks ++;
    g_pass += ok ? 1 : 0;
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
}

/// A patch record that is not all one value, so a copy that loses part of it shows up.
std::vector<uint8_t> madeUpRecord()
{
    std::vector<uint8_t> r(voltaire::preset::kRecordBytes);
    for (size_t i = 0; i < r.size(); i ++)
        r[i] = uint8_t(i * 7 + 3);
    const char *name = "Test Patch";
    for (unsigned i = 0; i < voltaire::preset::kNameLen; i ++)
        r[voltaire::preset::kNameOffset + i] = uint8_t(name[i]);
    // Parts 1 and 4 on cards 8 and 9, the rest internal.
    for (unsigned p = 0; p < voltaire::preset::kNumParts; p ++)
        r[voltaire::preset::kPartBase + p * voltaire::preset::kPartStride] = 0;
    r[voltaire::preset::kPartBase + 0 * voltaire::preset::kPartStride] = 8;
    r[voltaire::preset::kPartBase + 3 * voltaire::preset::kPartStride] = 9;
    r[voltaire::preset::kPartBase + 5 * voltaire::preset::kPartStride] = 8;
    return r;
}

void writeRaw(const std::string &path, const char *text)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    std::fputs(text, f);
    std::fclose(f);
}

} // namespace

int main(int argc, char **argv)
{
    using namespace voltaire::preset;

    std::string dir = argc > 1 ? argv[1] : "/tmp/voltaire110_preset_check";
    makeDir(dir);
    std::printf("preset: working in %s\n", dir.c_str());

    // ---- the round trip --------------------------------------------------------------
    Preset out;
    out.name     = "A Name With Spaces";
    out.bank     = "Strings";
    out.record   = madeUpRecord();
    out.volumeDb = -3.5f;
    out.hf       = false;
    out.created  = "2026-09-07";

    const std::string path = dir + "/roundtrip" + kSuffix;
    std::string err;
    check(save(path, out, err), "a preset saves");

    Preset back;
    check(load(path, back, err), "and loads again");
    check(back.record == out.record, "the 116 bytes are identical");
    check(back.name == out.name, "the display name survives");
    check(back.volumeDb < -3.49f && back.volumeDb > -3.51f, "the volume survives");
    check(back.hf == false, "the HF correction setting survives");
    check(back.hasSettings, "and it says it carried settings");
    check(back.bank == "Strings", "the bank survives");

    // A preset with no bank is unfiled, and writes no bank line at all.
    {
        Preset u = out;
        u.bank.clear();
        u.name = "Unfiled One";
        const std::string up = dir + "/unfiled" + kSuffix;
        std::string e2;
        check(save(up, u, e2), "a preset with no bank saves");
        Preset ub;
        check(load(up, ub, e2) && ub.bank.empty(), "and comes back unfiled");
    }

    // ---- what the record itself says --------------------------------------------------
    check(lcdName(out.record) == "Test Patch", "the machine's own ten-byte name is read");
    const std::vector<unsigned> cards = cardsNeeded(out.record);
    check(cards.size() == 2 && cards[0] == 8 && cards[1] == 9,
          "the cards a patch needs are computed from its parts, deduplicated");

    // ---- nothing that is not a patch is believed --------------------------------------
    struct Bad { const char *name; const char *text; };
    static const Bad kBad[] = {
        { "empty",        "" },
        { "wrong magic",  "Some Other Thing 1\nrecord 00\n" },
        { "from the future", "Voltaire110 patch 99\nrecord 00\n" },
        { "no record",    "Voltaire110 patch 1\nname Lonely\n" },
        { "short record", "Voltaire110 patch 1\nrecord 0011223344\n" },
    };
    // Exactly the right LENGTH but not hexadecimal, so the length test cannot be what
    // catches it -- built rather than typed, because a literal of 232 z's is a literal
    // nobody can count.
    const std::string notHex = "Voltaire110 patch 1\nrecord "
                             + std::string(kRecordBytes * 2, 'z') + "\n";
    for (const Bad &b : kBad)
    {
        const std::string p = dir + "/bad" + kSuffix;
        writeRaw(p, b.text);
        Preset junk;
        std::string why;
        char what[128];
        std::snprintf(what, sizeof(what), "refused: %s (%s)", b.name,
                      load(p, junk, why) ? "IT LOADED" : why.c_str());
        check(!load(p, junk, why), what);
        std::remove(p.c_str());
    }
    {
        const std::string p = dir + "/bad" + kSuffix;
        writeRaw(p, notHex.c_str());
        Preset junk;
        std::string why;
        const bool loaded = load(p, junk, why);
        char what[128];
        std::snprintf(what, sizeof(what), "refused: right length, not hex (%s)",
                      loaded ? "IT LOADED" : why.c_str());
        check(!loaded && why.find("hexadecimal") != std::string::npos, what);
        std::remove(p.c_str());
    }

    // ---- an unknown key is skipped, not fatal: this is how the format grows ------------
    {
        const std::string p = dir + "/future" + kSuffix;
        std::string text = "Voltaire110 patch 1\nname From Later\n"
                           "somethingnew whatever it says\nrecord ";
        text += toHex(out.record);
        text += "\n";
        writeRaw(p, text.c_str());
        Preset f;
        std::string why;
        const bool ok = load(p, f, why);
        check(ok && f.name == "From Later" && f.record == out.record,
              "a key this version has never heard of is skipped");
        std::remove(p.c_str());
    }

    // ---- a name is always a file name, and never a path -------------------------------
    check(fileNameFor("../../etc/passwd").find('/') == std::string::npos,
          "a display name cannot become a path");
    check(fileNameFor("") == "patch", "an empty name still makes a file name");
    check(fileNameFor("Rhodes / EP #2").find('/') == std::string::npos,
          "and nor can one with a slash in the middle");

    // ---- filing an existing preset: read, change one line, write ----------------------
    //
    // This is exactly what the browser's right-click does, and the thing worth checking is
    // that changing the bank changes NOTHING ELSE -- the patch, the name and the settings
    // all survive a trip through the parser and back out again.
    {
        Preset before;
        std::string e2;
        (void)load(path, before, e2);

        Preset filed = before;
        filed.bank = "Abstract";
        check(save(path, filed, e2), "a preset is refiled by rewriting it");

        Preset after;
        check(load(path, after, e2), "and reads back");
        check(after.bank == "Abstract", "in its new bank");
        check(after.record == before.record, "with the patch untouched");
        check(after.name == before.name && after.created == before.created,
              "and the name and date untouched");
        check(after.volumeDb == before.volumeDb && after.hf == before.hf,
              "and the settings untouched");

        filed.bank = "Strings";
        (void)save(path, filed, e2);
    }

    // ---- bank names cannot break a line, or a tab -------------------------------------
    check(cleanBankName("  Strings  ") == "Strings", "a bank name is trimmed");
    check(cleanBankName("Two\nLines").find('\n') == std::string::npos,
          "and can never contain a newline, which would forge a second key");
    check(cleanBankName(std::string(200, 'x')).size() <= 24, "and cannot be enormous");

    // ---- writing a new version over an existing preset --------------------------------
    //
    // What "Override Patch" does, and what clicking a preset from the WRITE button does.
    // The point of it is that the preset keeps its IDENTITY -- name, bank, date -- and only
    // the patch and the plugin's settings move on.  Saving a second copy and deleting the
    // first is what this exists to avoid, so losing any of those three would defeat it.
    {
        std::string e2;
        Preset was;
        (void)load(path, was, e2);

        Preset now = was;
        now.record[20] = uint8_t(now.record[20] ^ 0xFF);   // a different patch
        now.volumeDb = -9.0f;
        now.hf = !was.hf;
        check(save(path, now, e2), "a new version writes over the same file");

        Preset back2;
        check(load(path, back2, e2), "and reads back");
        check(back2.name == was.name, "the name is kept");
        check(back2.bank == was.bank, "the bank is kept");
        check(back2.created == was.created, "and the date it was first saved");
        check(back2.record == now.record, "while the patch is the new one");
        check(back2.volumeDb < -8.99f && back2.hf == now.hf,
              "and so are the settings");

        (void)save(path, was, e2);
    }

    // ---- renaming and deleting, which the browser's right-click menu does --------------
    {
        std::string e2;

        // A new name must never land on a file that already exists...
        const std::string taken = dir + "/" + fileNameFor("Occupied") + kSuffix;
        Preset other = out;
        other.name = "Occupied";
        (void)save(taken, other, e2);
        const std::string fresh = uniquePath(dir, "Occupied");
        check(fresh != taken, "a second preset of the same name gets its own file");

        // ...except the file being renamed itself, or changing a name's capitals would
        // walk it up through "name 2", "name 3" every time.
        check(uniquePath(dir, "Occupied", taken) == taken,
              "and renaming a preset to what it is already called stays put");

        check(erase(taken, e2), "a preset can be deleted");
        struct stat st;
        check(::stat(taken.c_str(), &st) != 0, "and is really gone");
        check(!erase(taken, e2), "deleting one that is not there is refused, not ignored");
    }

    check(cleanPresetName("  A Long One  ") == "A Long One", "a preset name is trimmed");
    check(cleanPresetName("Two\nLines").find('\n') == std::string::npos,
          "and can never contain a newline either");

    // ---- banks named this session, which have no patches in them yet -------------------
    {
        const std::vector<std::string> onDisk = { "Strings", "abstract" };
        const std::vector<std::string> session = { "Pads", "Strings", "" };
        const std::vector<std::string> all = mergeBanks(onDisk, session);
        check(all.size() == 3, "an empty bank named this session is offered too");
        check(std::find(all.begin(), all.end(), "Pads") != all.end(),
              "and by name");
        check(std::count(all.begin(), all.end(), std::string("Strings")) == 1,
              "a bank in both lists is offered once, not twice");
        check(all[0] == "abstract" && all[1] == "Pads" && all[2] == "Strings",
              "and the order ignores capitals, so the list reads as written");
    }

    // ---- the index sees what is on disk ------------------------------------------------
    {
        Index idx;
        idx.rescan(dir);
        bool found = false;
        for (const Entry &e : idx.entries())
            if (e.name == out.name)
                found = true;
        check(found, "the browser's index finds it by name");

        // A second rescan must agree with the first, since it comes from the cache.
        idx.rescan(dir);
        bool again = false;
        for (const Entry &e : idx.entries())
            if (e.name == out.name && e.bank == "Strings")
                again = true;
        check(again, "and a rescan off the cache says the same, bank included");

        const std::vector<std::string> b = idx.banks();
        check(b.size() == 1 && b[0] == "Strings",
              "the bank list is derived from the presets, without repeats");
        check(idx.anyUnfiled(), "and it knows something is unfiled");

        // A bank exists only while a patch claims it: unfile the one preset that had one
        // and the bank stops being offered, with nothing to clean up anywhere.
        Preset moved;
        std::string e3;
        (void)load(path, moved, e3);
        moved.bank.clear();
        (void)save(path, moved, e3);
        idx.rescan(dir);
        check(idx.banks().empty(), "a bank disappears when its last patch leaves it");
    }
    std::remove((dir + "/unfiled" + kSuffix).c_str());

    std::remove(path.c_str());
    std::printf("preset: %d of %d checks passed%s\n", g_pass, g_checks,
                g_pass == g_checks ? "" : "   <-- FAILED");
    return g_pass == g_checks ? 0 : 1;
}
