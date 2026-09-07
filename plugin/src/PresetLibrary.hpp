// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    The user's own patch library: files on disk, and the reading and writing of them.

    The machine has 64 slots and that is what the FIRMWARE has.  What a person with a
    computer wants is an unbounded library shared between every instance of the plugin, and
    those are not the same thing -- so the library lives in files and is authoritative, and
    the 64 slots are a working set the machine plays from.  PLUGIN-PLAN.md section 10.5.

    THIS IS THE UI's CODE, and deliberately so.  The DSP never opens a file: run() has a
    hard deadline, `make rtaudit` counts every allocation it makes, and a library of
    thousands of names could not travel over the atom port anyway.  The UI scans, reads and
    writes; all that goes down to the machine is one chosen 116-byte record.

    WHY A TEXT FORMAT.  Section 10.5 asked for a chunked container so that fields could be
    added as the patch record is decoded further.  Lines of `key value` do that better: an
    unknown key is skipped by construction, there is no length field to get wrong, and a
    preset that will not load can be looked at in any editor -- which is the same reason the
    `settings` session key is text.  A patch is 116 bytes, so even 10,000 of them written as
    hex is a couple of megabytes.
*/
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <cerrno>
#include <map>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace voltaire {
namespace preset {

/// The patch record, and it is 116 bytes rather than the 128-byte slot stride: the last 12
/// bytes of a slot are not part of the patch and the firmware's own WRITE leaves them
/// alone.  analysis/SYSTEM-DESIGN.md section 5.3.4.
inline constexpr unsigned kRecordBytes = 0x74;

inline constexpr unsigned kNameOffset = 4;    ///< ten ASCII bytes, the name the LCD shows
inline constexpr unsigned kNameLen    = 10;
inline constexpr unsigned kPartBase   = 0x14; ///< six part records of sixteen bytes
inline constexpr unsigned kPartStride = 0x10;
inline constexpr unsigned kNumParts   = 6;
inline constexpr unsigned kPartMedia  = 0x00; ///< 0 = internal, otherwise a card ID

/// The slot a library preset is played from.  P-64 by default: a patch can only be loaded
/// by the firmware, out of patchram, so auditioning one MUST spend a slot -- the mechanism
/// is forced rather than chosen.  The other 63 stay the machine's own.
inline constexpr unsigned kAuditionSlot = 63;

inline constexpr const char *kMagic   = "Voltaire110 patch";
inline constexpr int         kVersion = 1;
inline constexpr const char *kSuffix  = ".u110pat";

/// The bank a preset with no bank line is in.  Not written to any file: it is the absence
/// of a bank, named so that the browser has something to put in a tab.
inline constexpr const char *kUnfiled = "Unfiled";

struct Preset
{
    std::string          name;                   ///< what a human calls it, UTF-8
    /// Which group of patches this one belongs to -- "Strings", "Abstract" -- or empty.
    ///
    /// A BANK IS A NAME AND NOTHING ELSE.  It is not a directory, not a container file and
    /// not an index: it is one line in the preset, so a bank comes into existence when the
    /// first patch claims it and disappears when the last one stops.  Nothing can go stale,
    /// nothing has to be migrated, and moving a preset between banks rewrites one file.
    std::string          bank;
    std::vector<uint8_t> record;                 ///< kRecordBytes of patch
    float                volumeDb    = 0.0f;
    bool                 hf          = true;
    bool                 hasSettings = false;    ///< whether the two above came from the file
    std::string          created;
};

// ---- small helpers -------------------------------------------------------------------

inline int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string toHex(const std::vector<uint8_t> &v)
{
    static const char *const d = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (const uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

/// The ten-byte name the machine itself shows, trimmed.  Unprintable bytes become dots
/// rather than anything that could break a line of the file.
inline std::string lcdName(const std::vector<uint8_t> &record)
{
    if (record.size() < kNameOffset + kNameLen)
        return std::string();
    std::string s;
    for (unsigned i = 0; i < kNameLen; i ++)
    {
        const uint8_t c = record[kNameOffset + i];
        s += (c >= 0x20 && c < 0x7f) ? char(c) : '.';
    }
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

/// Which cards this patch needs, computed from the six part media bytes rather than
/// trusted from anything written in the file.  A part names a card by CATALOGUE ID and not
/// by slot, so this is what the browser can warn about: "needs card 08, not mounted".
inline std::vector<unsigned> cardsNeeded(const std::vector<uint8_t> &record)
{
    std::vector<unsigned> out;
    if (record.size() < kRecordBytes)
        return out;
    for (unsigned p = 0; p < kNumParts; p ++)
    {
        const unsigned m = record[kPartBase + p * kPartStride + kPartMedia];
        if (m != 0 && std::find(out.begin(), out.end(), m) == out.end())
            out.push_back(m);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- where the library lives ---------------------------------------------------------

inline bool makeDir(const std::string &p)
{
   #ifdef _WIN32
    return ::mkdir(p.c_str()) == 0 || errno == EEXIST;
   #else
    return ::mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
   #endif
}

/// The one directory presets are written to, created on demand.
///
/// Unlike the ROM search this is a single place and not a list: reading from several
/// directories is a convenience, but writing to whichever of them happened to answer first
/// is a good way to lose track of your own patches.  U110_DATA_DIR overrides it, the same
/// variable and the same meaning as everywhere else.
inline std::string libraryDir()
{
    std::string base;
    const auto env = [](const char *v) -> std::string
    {
        const char *const s = std::getenv(v);
        return (s != nullptr && s[0] != '\0') ? std::string(s) : std::string();
    };

    if (!env("U110_DATA_DIR").empty())
        base = env("U110_DATA_DIR");
   #ifdef _WIN32
    else if (!env("LOCALAPPDATA").empty())
        base = env("LOCALAPPDATA") + "/Voltaire110";
   #else
    else if (!env("XDG_DATA_HOME").empty())
        base = env("XDG_DATA_HOME") + "/Voltaire110";
    else if (!env("HOME").empty())
        base = env("HOME") + "/.local/share/Voltaire110";
   #endif
    if (base.empty())
        return std::string();

    // Every component, since the data directory may not exist either on a machine whose
    // ROMs were pointed at with U110_DATA_DIR.
    std::string at;
    for (size_t i = 0; i <= base.size(); i ++)
    {
        if (i == base.size() || base[i] == '/')
        {
            if (at.size() > 1)
                makeDir(at);
        }
        if (i < base.size())
            at += base[i];
    }
    makeDir(base);
    const std::string dir = base + "/patches";
    if (!makeDir(dir))
        return std::string();
    return dir;
}

/// A file name for a display name: the printable ASCII of it, and nothing that would mean
/// something to a shell or a filesystem.  Never empty, never a path.
inline std::string fileNameFor(const std::string &display)
{
    std::string s;
    for (const char c : display)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '-' || c == '_' || c == '.' || c == ' ')
            s += c;
        else if (!s.empty() && s.back() != ' ')
            s += ' ';
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
        s.pop_back();
    while (!s.empty() && s.front() == ' ')
        s.erase(s.begin());
    if (s.empty())
        s = "patch";
    if (s.size() > 64)
        s.resize(64);
    return s;
}

// ---- reading and writing -------------------------------------------------------------

/// Write a preset, atomically.
///
/// Into a temporary file in the SAME directory, flushed, then renamed over the target: a
/// reader therefore sees the old file or the new one and never half of either, and no
/// locking is needed anywhere.  Two instances saving the same name at the same instant is
/// the only real race, and last-writer-wins is both correct and free.
inline bool save(const std::string &path, const Preset &p, std::string &err)
{
    if (p.record.size() != kRecordBytes)
    { err = "the patch is not 116 bytes"; return false; }

    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr)
    { err = "cannot write " + tmp; return false; }

    std::fprintf(f, "%s %d\n", kMagic, kVersion);
    std::fprintf(f, "name %s\n", p.name.c_str());
    if (!p.bank.empty())
        std::fprintf(f, "bank %s\n", p.bank.c_str());
    if (!p.created.empty())
        std::fprintf(f, "created %s\n", p.created.c_str());
    std::fprintf(f, "volume %.4f\n", double(p.volumeDb));
    std::fprintf(f, "hfcorrection %d\n", p.hf ? 1 : 0);

    // Informational: a reader recomputes this from the record rather than believing it.
    // Written anyway because it is the one thing somebody looking at the file in an editor
    // would most want to know.
    const std::vector<unsigned> cards = cardsNeeded(p.record);
    if (!cards.empty())
    {
        std::fprintf(f, "cards");
        for (const unsigned c : cards)
            std::fprintf(f, " %u", c);
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "record %s\n", toHex(p.record).c_str());

    const bool wrote = std::fflush(f) == 0;
    std::fclose(f);
    if (!wrote)
    { std::remove(tmp.c_str()); err = "could not finish writing " + tmp; return false; }

   #ifdef _WIN32
    // rename() will not replace on Windows.  Move the old one aside rather than deleting
    // it first, so a crash in the middle costs a stray file and never the patch.
    const std::string old = path + ".old";
    std::remove(old.c_str());
    std::rename(path.c_str(), old.c_str());          // fails harmlessly if there is none
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        std::rename(old.c_str(), path.c_str());
        std::remove(tmp.c_str());
        err = "could not replace " + path;
        return false;
    }
    std::remove(old.c_str());
   #else
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
    { std::remove(tmp.c_str()); err = "could not replace " + path; return false; }
   #endif
    return true;
}

/// Read a preset.  Everything is checked before anything is believed: this file may have
/// been written by a different version, hand-edited, or truncated by a full disk, and what
/// comes out of it is handed to the machine's own patch loader.
inline bool load(const std::string &path, Preset &p, std::string &err)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    { err = "cannot read " + path; return false; }

    char line[1024];
    bool magicOk = false;
    p = Preset();

    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[-- n] = '\0';
        if (n == 0 || line[0] == '#')
            continue;

        if (!magicOk)
        {
            int ver = 0;
            const size_t m = std::strlen(kMagic);
            if (std::strncmp(line, kMagic, m) != 0 || std::sscanf(line + m, "%d", &ver) != 1)
            { std::fclose(f); err = "not a Voltaire 110 patch file"; return false; }
            if (ver > kVersion)
            { std::fclose(f); err = "written by a newer version of the plugin"; return false; }
            magicOk = true;
            continue;
        }

        char *sp = std::strchr(line, ' ');
        if (sp == nullptr)
            continue;                       // a key with no value says nothing
        *sp = '\0';
        const char *const key = line;
        const char *const val = sp + 1;

        if (std::strcmp(key, "name") == 0)
            p.name = val;
        else if (std::strcmp(key, "bank") == 0)
            p.bank = val;
        else if (std::strcmp(key, "created") == 0)
            p.created = val;
        else if (std::strcmp(key, "volume") == 0)
        { p.volumeDb = float(std::atof(val)); p.hasSettings = true; }
        else if (std::strcmp(key, "hfcorrection") == 0)
        { p.hf = std::atoi(val) != 0; p.hasSettings = true; }
        else if (std::strcmp(key, "record") == 0)
        {
            const size_t len = std::strlen(val);
            if (len != kRecordBytes * 2)
            { std::fclose(f); err = "the patch is the wrong length"; return false; }
            p.record.resize(kRecordBytes);
            for (unsigned i = 0; i < kRecordBytes; i ++)
            {
                const int hi = hexNibble(val[i * 2]), lo = hexNibble(val[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                { std::fclose(f); err = "the patch is not hexadecimal"; return false; }
                p.record[i] = uint8_t((hi << 4) | lo);
            }
        }
        // Anything else is a key this version has not heard of, and is skipped.
    }
    std::fclose(f);

    if (!magicOk)
    { err = "the file is empty"; return false; }
    if (p.record.size() != kRecordBytes)
    { err = "the file has no patch in it"; return false; }
    if (p.name.empty())
        p.name = lcdName(p.record);
    return true;
}

// ---- the directory, as the browser sees it -------------------------------------------

struct Entry
{
    std::string path;
    std::string name;          ///< the display name, read out of the file
    std::string bank;          ///< empty for unfiled
    long long   mtime = 0;
    long long   size  = 0;
};

/// A bank name that is safe to write on a line of its own and to show in a tab.
inline std::string cleanBankName(const std::string &in)
{
    std::string s;
    for (const char c : in)
        if (c >= 0x20 && c != 0x7f)
            s += c;
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    while (!s.empty() && s.front() == ' ')
        s.erase(s.begin());
    if (s.size() > 24)
        s.resize(24);
    return s;
}

/// The library as a list, kept across rescans so that opening the browser does not mean
/// reading every file again.
///
/// A file is only re-read when its modification time or size has changed, which is what
/// makes this cheap with thousands of presets and also what makes another instance's save
/// show up here: the filesystem is the only thing the two share, by design.
class Index
{
public:
    void rescan(const std::string &dir)
    {
        m_entries.clear();
        if (dir.empty())
            return;

        DIR *d = ::opendir(dir.c_str());
        if (d == nullptr)
            return;

        const size_t suffixLen = std::strlen(kSuffix);
        while (const dirent *e = ::readdir(d))
        {
            const std::string fn = e->d_name;
            if (fn.size() <= suffixLen
                    || fn.compare(fn.size() - suffixLen, suffixLen, kSuffix) != 0)
                continue;

            const std::string path = dir + "/" + fn;
            struct stat st;
            if (::stat(path.c_str(), &st) != 0)
                continue;                    // vanished between readdir and here: skip it

            Entry en;
            en.path  = path;
            en.mtime = (long long)st.st_mtime;
            en.size  = (long long)st.st_size;

            const auto cached = m_cache.find(path);
            if (cached != m_cache.end() && cached->second.mtime == en.mtime
                    && cached->second.size == en.size)
            {
                en.name = cached->second.name;
                en.bank = cached->second.bank;
            }
            else
            {
                Preset p;
                std::string err;
                // A file that will not parse is still SHOWN, named after itself, rather
                // than silently missing.  Somebody who put it there should be able to see
                // that it is there and that something is wrong with it.
                if (load(path, p, err))
                { en.name = p.name; en.bank = p.bank; }
                else
                    en.name = fn.substr(0, fn.size() - suffixLen) + "  (?)";
                m_cache[path] = en;
            }
            m_entries.push_back(en);
        }
        ::closedir(d);

        std::sort(m_entries.begin(), m_entries.end(),
                  [](const Entry &a, const Entry &b)
                  {
                      // Case-insensitive, so a library is in the order a person reads.
                      std::string x = a.name, y = b.name;
                      for (char &c : x) if (c >= 'A' && c <= 'Z') c = char(c + 32);
                      for (char &c : y) if (c >= 'A' && c <= 'Z') c = char(c + 32);
                      return x < y;
                  });
    }

    const std::vector<Entry> &entries() const { return m_entries; }

    /// Every bank in use, sorted, without repeats.  Derived from the presets on each
    /// rescan rather than kept anywhere, which is what stops a bank list from outliving
    /// the patches that justified it.
    std::vector<std::string> banks() const
    {
        std::vector<std::string> out;
        for (const Entry &e : m_entries)
            if (!e.bank.empty()
                    && std::find(out.begin(), out.end(), e.bank) == out.end())
                out.push_back(e.bank);
        std::sort(out.begin(), out.end(),
                  [](const std::string &a, const std::string &b)
                  {
                      std::string x = a, y = b;
                      for (char &c : x) if (c >= 'A' && c <= 'Z') c = char(c + 32);
                      for (char &c : y) if (c >= 'A' && c <= 'Z') c = char(c + 32);
                      return x < y;
                  });
        return out;
    }

    /// Whether anything is unfiled, so the browser knows whether that tab is worth a place.
    bool anyUnfiled() const
    {
        for (const Entry &e : m_entries)
            if (e.bank.empty())
                return true;
        return false;
    }

private:
    std::vector<Entry> m_entries;
    std::map<std::string, Entry> m_cache;
};

} // namespace preset
} // namespace voltaire
