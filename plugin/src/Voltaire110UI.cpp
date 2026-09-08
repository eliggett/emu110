// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    The Voltaire 110 panel.

    Geometry comes from the Inkscape artwork, not from this file: every rectangle here is
    read out of plugin/generated/panel_geometry.h, which panel_export.py writes from the
    SVG.  Move a control in Inkscape, re-export, and the hit box follows.  Nothing below
    contains a hand-typed coordinate.

    The artwork itself is drawn by nanosvg's parsed paths through NanoVG.  Two things are
    deliberately NOT in the SVG (PLUGIN-PLAN.md section 6): the knob pointer, which is one
    rotation rather than 128 exported frames, and the LCD dot matrix, which has to be built
    live from character codes because the firmware redefines its custom glyphs at runtime.
*/

#include "DistrhoUI.hpp"

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"

#include "panel_geometry.h"
#include "panel_svg.h"
#include "panel_background.h"
#include "dive_pages.h"
#include "u110_cgrom.h"
#include "about_text.h"
#include "DiveParams.h"
#include "PresetLibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

START_NAMESPACE_DISTRHO

// Kept in step with the plugin by hand; there are only a few and they are checked by the
// selftest, which fails loudly if the port layout moves.
enum Params
{
    kParamVolume = 0, kParamHfCorrection,
    kParamButtonFirst,
    kParamCount = kParamButtonFirst + 6
};

/// The panel as the DSP sends it: one fixed layout, hex encoded, over the atom port.
struct PanelBlob
{
    uint8_t lcd[32];
    uint8_t cgram[64];
    uint8_t leds, cursor_pos, cursor_flags;
    uint8_t patch;                  ///< 0-based, so the LCD's P-01 is 0
    uint8_t part_media[6];          ///< 0 = internal, otherwise a card ID
    uint8_t part_tone[6];           ///< tone within that media, counting from 0
    uint8_t part_flags[6];          ///< (b & 0xE0) == 0xC0: the part is switched off
    uint8_t part_chan[6];           ///< MIDI receive channel in the low nibble
};

/// One media's worth of tones: the internal wave ROM, or a mounted card.
struct ToneGroup
{
    unsigned media = 0;
    std::string label;
    std::vector<std::string> names;
};

class Voltaire110UI : public UI
{
public:
    Voltaire110UI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT)
    {
        loadArtwork();
        resolveDiveParams();
        // VOLTAIRE_MENU=patch|tone opens a menu at startup, for the same reason: a
        // palette that cannot be opened without a mouse cannot be looked at under Xvfb.
        if (const char *m = std::getenv("VOLTAIRE_MENU"))
            m_menu = sameName(m, "tone") ? Menu::Tone : Menu::Patch;

        // VOLTAIRE_DIAG opens the self-check at startup, for the same reason: it is the
        // one page whose whole job is to be readable when something is wrong, and a page
        // that can only be reached by clicking cannot be looked at under Xvfb.
        if (std::getenv("VOLTAIRE_DIAG") != nullptr)
            m_menu = Menu::Diag;

        // Same again for the About box, which is likewise only reachable by clicking.
        if (std::getenv("VOLTAIRE_ABOUT") != nullptr)
            m_menu = Menu::About;

        // And for the RESET menu.
        if (std::getenv("VOLTAIRE_RESET") != nullptr)
            m_menu = Menu::Reset;

        // The credits are one string with newlines in it, because that is what a text
        // file is; split once here rather than on every frame.
        for (const char *p = voltaire::kAboutText; *p != '\0'; )
        {
            const char *nl = std::strchr(p, '\n');
            m_aboutLines.emplace_back(p, nl != nullptr ? size_t(nl - p) : std::strlen(p));
            if (nl == nullptr)
                break;
            p = nl + 1;
        }

        // VOLTAIRE_DIVE=<tab> opens the drawer on that tab at startup.  The drawer is
        // otherwise only reachable by clicking, which a headless screenshot cannot do,
        // and a page whose layout nobody can look at is a page nobody checked.
        if (const char *d = std::getenv("VOLTAIRE_DIVE"))
        {
            m_diveOpen = true;
            for (int t = 0; t < voltaire::panel::DIVETABID_COUNT; t ++)
            {
                if (!sameName(d, voltaire::panel::kDiveTabName[t]))
                    continue;
                if (voltaire::panel::kDiveTabRow[t] == 1)
                {
                    // A sub-tab only exists under a part, so naming one implies one.
                    m_diveTab2 = t;
                    if (!tabIsPart(m_diveTab1))
                        m_diveTab1 = voltaire::panel::TAB_P1;
                }
                else
                    m_diveTab1 = t;
            }
            setSize(DISTRHO_UI_DEFAULT_WIDTH,
                    uint(DISTRHO_UI_DEFAULT_WIDTH * designHeight()
                         / voltaire::panel::kDesignWidth));
            m_diveNeedRead = true;
        }
        std::memset(m_lcd, ' ', sizeof(m_lcd));
        std::memset(m_cgram, 0, sizeof(m_cgram));

        // The panel itself carries no text -- the artwork's lettering is paths, and the
        // LCD is dots -- so nothing needed a font until the patch menu did.  DPF ships
        // DejaVu Sans inside the binary for exactly this.
       #ifdef DGL_NO_SHARED_RESOURCES
        createFontFromFile("sans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        m_font = "sans";
       #else
        loadSharedResources();
        m_font = NANOVG_DEJAVU_SANS_TTF;
       #endif
    }

    ~Voltaire110UI() override
    {
        if (m_svg != nullptr)
            nsvgDelete(m_svg);
        for (NSVGimage *p : m_pageSvg)
            if (p != nullptr)
                nsvgDelete(p);
    }

protected:
    // ---- state from the DSP ---------------------------------------------------------

    void parameterChanged(uint32_t index, float value) override
    {
        if (index == kParamVolume)
        { if (value != m_volume) { m_volume = value; m_dirty = true; } }
        else if (index == kParamHfCorrection)
        { const bool on = value > 0.5f; if (on != m_hf) { m_hf = on; m_dirty = true; } }
    }

    /// The panel arrives as ONE blob, not as a pile of scalars.
    ///
    /// Everything the display needs comes in a single message on the atom port: the 32
    /// character codes, the eight live custom glyphs, the lamps and the cursor.  No
    /// packing of bitfields into floats, no per-field ranges to get right, and no host
    /// deciding whether a change was big enough to be worth forwarding.
    void stateChanged(const char *key, const char *value) override
    {
        if (value == nullptr)
            return;

        // The patch list.  One name per line, already trimmed, in the machine's order.
        if (std::strcmp(key, "divevals") == 0)
        {
            if (std::getenv("VOLTAIRE_DIVE") != nullptr)
                d_stdout("divevals [%s]", value);
            applyDiveValues(value);
            return;
        }

        // The patch the machine is playing, as hex.  Kept so that saving to the library
        // is a copy of what is actually in the machine rather than of anything this UI
        // believes about it.
        if (std::strcmp(key, "patchdump") == 0)
        {
            m_patchDump = value;
            return;
        }

        // Why the machine is or is not running.  First line is "status: <word>" for the
        // LCD to act on; the rest is the text a person reads.
        if (std::strcmp(key, "diag") == 0)
        {
            m_diagLines.clear();
            m_diagStatus.clear();
            for (const char *p = value; *p != '\0'; )
            {
                const char *const nl = std::strchr(p, '\n');
                std::string line(p, nl != nullptr ? size_t(nl - p) : std::strlen(p));
                if (m_diagLines.empty() && m_diagStatus.empty()
                        && line.compare(0, 8, "status: ") == 0)
                    m_diagStatus = line.substr(8);
                else
                    m_diagLines.push_back(std::move(line));
                if (nl == nullptr)
                    break;
                p = nl + 1;
            }
            m_dirty = true;
            return;
        }

        if (std::strcmp(key, "patches") == 0)
        {
            m_patchNames.clear();
            for (const char *p = value; *p != '\0'; )
            {
                const char *const nl = std::strchr(p, '\n');
                m_patchNames.emplace_back(p, nl != nullptr ? size_t(nl - p) : std::strlen(p));
                if (nl == nullptr)
                    break;
                p = nl + 1;
            }
            m_dirty = true;
            return;
        }

        // The tones, grouped by media.  "G <media> <label>" opens a group and "T <name>"
        // is a tone in it; the marker is the first character rather than anything in the
        // text, so a name is free to say whatever it says.
        if (std::strcmp(key, "tones") == 0)
        {
            m_toneGroups.clear();
            for (const char *p = value; *p != '\0'; )
            {
                const char *const nl = std::strchr(p, '\n');
                const size_t len = nl != nullptr ? size_t(nl - p) : std::strlen(p);
                if (len > 2 && p[0] == 'G')
                {
                    ToneGroup g;
                    g.media = unsigned(std::atoi(p + 2));
                    const char *sp = std::strchr(p + 2, ' ');
                    if (sp != nullptr && size_t(sp + 1 - p) < len)
                        g.label.assign(sp + 1, size_t(p + len - sp - 1));
                    m_toneGroups.push_back(std::move(g));
                }
                else if (len > 2 && p[0] == 'T' && !m_toneGroups.empty())
                    m_toneGroups.back().names.emplace_back(p + 2, len - 2);
                if (nl == nullptr)
                    break;
                p = nl + 1;
            }
            if (m_toneGroup >= int(m_toneGroups.size()))
                m_toneGroup = 0;
            m_dirty = true;
            return;
        }

        if (std::strcmp(key, "panel") != 0)
            return;
        PanelBlob blob;
        if (!decodeHex(value, reinterpret_cast<uint8_t *>(&blob), sizeof(blob)))
            return;

        // The machine has spoken at least once, which is what retires the placeholder on
        // the glass.  Nothing else can stand in for this: an all-spaces LCD is a thing the
        // firmware really does draw, so the CONTENT cannot be used to tell the two apart.
        m_panelSeen = true;
        // VOLTAIRE_TRACE_STATE counts panels that actually DECODED, which is the only
        // proof that the DSP -> UI push is alive: the snapshot a host hands over when the
        // UI opens carries no panel at all, so a count of even one can only have been
        // pushed.  tools/clap_selftest.c is what reads this.
        static const bool trace = std::getenv("VOLTAIRE_TRACE_STATE") != nullptr;
        if (trace)
        {
            static int seen = 0;
            d_stdout("ui panel #%d", ++ seen);
        }
        std::memcpy(m_lcd, blob.lcd, sizeof(m_lcd));
        std::memcpy(m_cgram, blob.cgram, sizeof(m_cgram));
        m_leds = blob.leds;
        m_cursorPos = blob.cursor_pos;
        m_cursorFlags = blob.cursor_flags;
        // A patch change replaces every value the drawer is showing, so it is the one
        // event that has to re-read the page rather than just repaint it.
        if (m_diveOpen && blob.patch != m_patch)
            m_diveNeedRead = true;
        m_patch = blob.patch;
        // A library preset is played FROM the audition slot, so the moment the machine is
        // on any other patch it is no longer playing that file -- whether the panel moved
        // it, the host restored a session, or somebody picked from the machine's own bank.
        // Without this, Override would write whatever is playing over an unrelated preset.
        if (m_patch != voltaire::preset::kAuditionSlot)
            forgetLoadedPreset();
        std::memcpy(m_partMedia, blob.part_media, sizeof(m_partMedia));
        std::memcpy(m_partTone, blob.part_tone, sizeof(m_partTone));
        std::memcpy(m_partFlags, blob.part_flags, sizeof(m_partFlags));
        std::memcpy(m_partChan, blob.part_chan, sizeof(m_partChan));
        m_dirty = true;
    }

    static bool decodeHex(const char *src, uint8_t *dst, size_t n)
    {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        for (size_t i = 0; i < n; i ++)
        {
            const int hi = nib(src[i * 2]), lo = nib(src[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return false;
            dst[i] = uint8_t((hi << 4) | lo);
        }
        return true;
    }

    /// One repaint per idle, and only when something actually changed.
    ///
    /// This is what makes the panel cost nothing while the machine sits idle, and redraw
    /// promptly while it is being driven -- no fixed refresh rate to compromise over,
    /// because a static display genuinely needs no frames at all.
    /// Half a second on, half a second off, and only while a field is being typed into.
    static constexpr double kCaretPeriod = 0.5;

    /// A resize has to force a repaint.
    ///
    /// The panel is demand-driven -- uiIdle() turns a dirty flag into one repaint, and
    /// an idle machine sets it for nothing -- so a window that changes size while the
    /// machine is quiet had nothing to make it draw again.  What was left on screen was
    /// the old framebuffer at the new size: torn, or layered with whatever had been
    /// drawn before, until any button press happened to dirty the panel.
    void onResize(const ResizeEvent &ev) override
    {
        UI::onResize(ev);
        m_dirty = true;
        repaint();
    }

    void uiIdle() override
    {
        if (m_nameEdit || m_typing != Typing::None)
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const double now = double(ts.tv_sec) + ts.tv_nsec * 1e-9;
            if (now - m_caretAt >= kCaretPeriod)
            {
                m_caretAt = now;
                m_caretOn = !m_caretOn;
                m_dirty = true;
            }
        }
        else if (!m_caretOn)
            m_caretOn = true;

        // Same reason as the DIVE read below: a constructor has nowhere to send a
        // setState() yet, so the first ask happens on the first idle.
        if (!m_diagAsked)
        {
            m_diagAsked = true;
            askForDiag();
        }
        else if (m_diagStatus.empty() && m_diagWait < kDiagWaitIdles && ++ m_diagWait == kDiagWaitIdles)
            m_dirty = true;          // give up waiting and say so on the glass

        // Asking is deferred to here rather than done at the click, because setState()
        // is a message to the DSP and a constructor has nowhere to send one yet.
        if (m_diveNeedRead)
        {
            m_diveNeedRead = false;
            m_diveRetry = kDiveRetryIdles;
            requestDiveValues();
        }
        else if (m_diveOpen && m_diveRetry > 0 && -- m_diveRetry == 0 && pageIncomplete())
        {
            // The machine takes about five and a half seconds to boot before it answers
            // MIDI at all, and the drawer can be open before then -- a restored session
            // opens it immediately.  So an unanswered read is retried rather than left
            // showing dots forever.  It stops as soon as the page is complete.
            m_diveRetry = kDiveRetryIdles;
            requestDiveValues();
        }
        if (!m_dirty)
            return;
        m_dirty = false;
        repaint();
    }

    // ---- drawing --------------------------------------------------------------------

    void onNanoDisplay() override
    {
        // VOLTAIRE_FPS reports both how often the panel redraws and what one redraw costs.
        // Both numbers matter: a redraw is the whole SVG plus 1280 LCD dots, so the rate it
        // is asked for at is the difference between idling and saturating a core.
        struct timespec t0;
        if (m_countFrames)
            clock_gettime(CLOCK_MONOTONIC, &t0);

        const float s = panelScale();
        const float ox = (getWidth() - voltaire::panel::kDesignWidth * s) * 0.5f;
        const float oy = (getHeight() - designHeight() * s) * 0.5f;

        // Ground behind the panel, so a resized window does not show through.
        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(20, 20, 22);
        fill();

        save();
        translate(ox, oy);
        scale(s, s);

        // Everything below is clipped to the panel as it stands right now.  The artwork
        // is one document 676 units tall whether the drawer is open or not, and the
        // background image spans all of it, so a window taller than the shut panel used
        // to show the photograph carrying on below the instrument.  Clipping here rather
        // than teaching each piece its own limit means anything added later is bounded
        // too, and it is one line.
        scissor(0.0f, 0.0f, voltaire::panel::kDesignWidth, designHeight());

        drawBackground();
        drawArtwork();
        drawDivePage();
        drawLcd();
        drawLeds();
        drawKnob();
        drawButtonFeedback();

        restore();

        // Outside the panel transform on purpose: the menus are in window pixels.
        // Under them, so an overlay covers it rather than fighting with it.
        if (m_testMode)
            drawTestHint();

        if (m_menu == Menu::Patch && m_libraryTab)
            drawLibraryMenu();
        else if (m_menu == Menu::Patch)
            drawPatchMenu();
        else if (m_menu == Menu::WriteConfirm)
            drawWriteConfirm();
        else if (m_menu == Menu::BankPick)
            drawBankPick();
        else if (m_menu == Menu::PresetActions)
            drawPresetActions();
        else if (m_menu == Menu::DeleteConfirm)
            drawDeleteConfirm();
        else if (m_menu == Menu::OverwriteConfirm)
            drawOverwriteConfirm();
        else if (m_menu == Menu::Tone)
            drawToneMenu();
        else if (m_menu == Menu::Value)
            drawValueMenu();
        else if (m_menu == Menu::Diag)
            drawDiagMenu();
        else if (m_menu == Menu::About)
            drawAboutBox();
        else if (m_menu == Menu::Reset)
            drawResetMenu();
        else
            drawDiveTooltip();

        if (m_countFrames)
        {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            static double accum = 0.0, last = 0.0;
            static int frames = 0;
            const double now = double(t1.tv_sec) + t1.tv_nsec * 1e-9;
            accum += (now - (double(t0.tv_sec) + t0.tv_nsec * 1e-9)) * 1000.0;
            frames ++;
            if (last == 0.0) last = now;
            if (now - last >= 2.0)
            {
                std::fprintf(stderr,
                        "panel: %.1f redraws/s, %.2f ms each -> %.0f%% of a core\n",
                        frames / (now - last), accum / frames,
                        100.0 * accum / 1000.0 / (now - last));
                std::fflush(stderr);
                frames = 0; accum = 0.0; last = now;
            }
        }
    }

    // ---- input ----------------------------------------------------------------------

    bool onMouse(const MouseEvent &ev) override
    {
        // Before every palette's own handling: the X means the same thing everywhere, and
        // it sits over parts of some of them that would otherwise take the click.
        if (m_menu != Menu::None && ev.press
                && closeButtonHit(float(ev.pos.getX()), float(ev.pos.getY())))
        {
            closeCurrentMenu();
            m_hoverClose = false;
            repaint();
            return true;
        }

        // A menu is modal while it is up: it takes the click wherever it lands, so a
        // click meant to dismiss it cannot also press whatever is underneath.
        // The report is a wall of text with nothing to pick, so any click dismisses it --
        // the wheel is what reads it, and that is taken in onScroll before this.
        if (m_menu == Menu::Diag || m_menu == Menu::About)
        {
            if (ev.press)
            {
                m_menu = Menu::None;
                repaint();
            }
            return true;
        }

        if (m_menu == Menu::Reset)
        {
            if (!ev.press)
                return true;
            const int hit = resetHit(float(ev.pos.getX()), float(ev.pos.getY()));
            m_menu = Menu::None;
            m_menuHover = -1;
            // Cancel is an entry rather than only a click in the dark, because a menu
            // whose only way out is "click somewhere that does nothing" reads as a menu
            // you are stuck in.  Its arg is null, and so is a click that missed.
            if (hit >= 0 && kResetItems[hit].arg != nullptr)
            {
                clearLatches();
                // The UI knows because the UI asked.  Nothing else can tell: the test
                // menu is left only by rebooting, so the flag is right until the next
                // one either way, and reading it off the LCD would mean guessing which
                // screens belong to the service menu.
                m_testMode = std::strcmp(kResetItems[hit].arg, "test") == 0;
                setState("reboot", kResetItems[hit].arg);
            }
            repaint();
            return true;
        }

        if (m_menu == Menu::PresetActions)
        {
            if (!ev.press)
                return true;
            const BankPickLayout L = presetActionsLayout();
            if (m_typing == Typing::RenamePreset
                    && inRect(fieldOkRect(L, kActRename),
                              float(ev.pos.getX()), float(ev.pos.getY())))
            {
                commitTyping();
                repaint();
                return true;
            }
            const int hit = listHit(L, float(ev.pos.getX()), float(ev.pos.getY()));
            m_menuHover = -1;
            if (hit == kActRename)
            {
                if (m_typing != Typing::RenamePreset)
                {
                    // Starts from the name it has: renaming is usually a correction.
                    m_typing = Typing::RenamePreset;
                    std::snprintf(m_typeBuf, sizeof(m_typeBuf), "%s",
                                  m_presetTargetName.c_str());
                    m_typeCaret = int(std::strlen(m_typeBuf));
                    showCaret();
                }
            }
            else if (hit == kActMoveBank)
            { m_typing = Typing::None; m_menu = Menu::BankPick; }
            else if (hit == kActDelete)
            { m_typing = Typing::None; m_menu = Menu::DeleteConfirm; }
            else if (hit < 0)
                backToLibrary();
            repaint();
            return true;
        }

        if (m_menu == Menu::OverwriteConfirm)
        {
            if (!ev.press)
                return true;
            const int hit = listHit(deleteConfirmLayout(),
                                    float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit == 0)
                libraryWriteOver(m_presetTargetPath);
            backToLibrary();
            repaint();
            return true;
        }

        if (m_menu == Menu::DeleteConfirm)
        {
            if (!ev.press)
                return true;
            const int hit = listHit(deleteConfirmLayout(),
                                    float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit == 0)
                libraryDelete(m_presetTargetPath, m_presetTargetName);
            backToLibrary();
            repaint();
            return true;
        }

        if (m_menu == Menu::BankPick)
        {
            if (!ev.press)
                return true;
            const BankPickLayout bl = bankPickLayout();
            if (m_typing == Typing::NewBank
                    && inRect(fieldOkRect(bl, bl.rows - 1),
                              float(ev.pos.getX()), float(ev.pos.getY())))
            {
                commitTyping();
                repaint();
                return true;
            }
            const int hit = bankPickHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit < 0)
                backToLibrary();
            else
                bankPickChoose(hit);
            m_menuHover = -1;
            repaint();
            return true;
        }

        if (m_menu == Menu::WriteConfirm)
        {
            if (!ev.press)
                return true;
            const int hit = confirmHit(float(ev.pos.getX()), float(ev.pos.getY()));
            const int target = m_writeTarget;
            m_menu = Menu::None;
            m_menuHover = -1;
            m_writeTarget = -1;
            if (hit == 0 && target >= 0)
            {
                char n[16];
                std::snprintf(n, sizeof(n), "%d", target);
                setState("patchwrite", n);
            }
            repaint();
            return true;
        }

        if (m_menu == Menu::Patch)
        {
            if (!ev.press)
                return true;

            // The tabs first, and they do NOT close the menu: switching between the
            // machine's bank and the library is looking around, not choosing.
            const int tab = patchTabHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (tab >= 0)
            {
                if ((tab == 1) != m_libraryTab)
                {
                    m_libraryTab = tab == 1;
                    m_menuHover = -1;
                    m_libScroll = 0;
                    m_libraryNote.clear();
                    if (m_libraryTab)
                        libraryRescan();
                }
                repaint();
                return true;
            }

            if (m_libraryTab)
            {
                if (saveButtonHit(float(ev.pos.getX()), float(ev.pos.getY())))
                {
                    librarySave();
                    repaint();
                    return true;
                }
                if (overrideButtonHit(float(ev.pos.getX()), float(ev.pos.getY())))
                {
                    libraryOverride();
                    repaint();
                    return true;
                }

                // The bank row: looking around, so it does not close the menu.
                const int chip = bankChipHit(float(ev.pos.getX()), float(ev.pos.getY()));
                if (chip >= 0)
                {
                    const std::vector<Chip> chips = bankChips(libraryLayout());
                    if (size_t(chip) < chips.size())
                    {
                        const Chip &c = chips[size_t(chip)];
                        if (c.kind == 3)
                        {
                            // Naming a bank is all it takes to have one; the first patch
                            // saved into it is what puts it on disk.
                            m_presetTargetPath.clear();
                            m_presetTargetName.clear();
                            m_typeBuf[0] = '\0';
                            m_typeCaret = 0;
                            m_typing = Typing::NewBank;
                            m_menu = Menu::BankPick;
                            m_menuHover = -1;
                            showCaret();
                        }
                        else
                        {
                            m_bankMode = c.kind == 0 ? BankFilter::All
                                       : c.kind == 2 ? BankFilter::Unfiled
                                                     : BankFilter::Named;
                            m_bankName = c.name;
                            m_libScroll = 0;
                            m_libraryNote.clear();
                        }
                    }
                    repaint();
                    return true;
                }

                const LibHit lh = libraryHit(float(ev.pos.getX()), float(ev.pos.getY()));
                const std::vector<const voltaire::preset::Entry *> view = libraryView();
                const int idx = m_libScroll + lh.cell;

                if (lh.cell < 0 || size_t(idx) >= view.size())
                {
                    m_menu = Menu::None;                 // a click in the dark closes it
                    m_menuHover = -1;
                    repaint();
                    return true;
                }

                // The bank half of a row, and the right button anywhere on it, both open
                // the picker.  The name half plays it, which is what the list is for.
                if ((lh.bankZone && !m_writeMode) || ev.button == kMouseButtonRight)
                {
                    m_presetTargetPath = view[size_t(idx)]->path;
                    m_presetTargetName = view[size_t(idx)]->name;
                    m_presetTargetBank = view[size_t(idx)]->bank;
                    m_typing = Typing::None;
                    m_typeBuf[0] = '\0';
                    m_typeCaret = 0;
                    // The bank column means one thing, so it goes straight there; the
                    // right button is the general "what else can I do to this".
                    m_menu = lh.bankZone ? Menu::BankPick : Menu::PresetActions;
                    m_menuHover = -1;
                }
                else if (m_writeMode)
                {
                    m_presetTargetPath = view[size_t(idx)]->path;
                    m_presetTargetName = view[size_t(idx)]->name;
                    m_presetTargetBank = view[size_t(idx)]->bank;
                    m_menu = Menu::OverwriteConfirm;
                    m_menuHover = -1;
                }
                else
                {
                    libraryLoad(*view[size_t(idx)]);
                    m_menu = Menu::None;
                    m_menuHover = -1;
                }
                repaint();
                return true;
            }

            const int hit = patchHit(float(ev.pos.getX()), float(ev.pos.getY()));
            m_menuHover = -1;
            if (hit >= 0 && size_t(hit) < m_patchNames.size())
            {
                if (m_writeMode)
                {
                    // Picking does NOT write; it asks first, naming what would be lost.
                    m_writeTarget = hit;
                    m_menu = Menu::WriteConfirm;
                }
                else
                {
                    char n[8];
                    std::snprintf(n, sizeof(n), "%d", hit);
                    setState("patchsel", n);
                    forgetLoadedPreset();
                    m_menu = Menu::None;
                }
            }
            else
                m_menu = Menu::None;
            repaint();
            return true;
        }

        if (m_menu == Menu::Value)
        {
            if (!ev.press)
                return true;
            const int hit = valueHit(float(ev.pos.getX()), float(ev.pos.getY()));
            m_menu = Menu::None;
            m_menuHover = -1;
            if (hit >= 0)
                writeRaw(m_valueCtl, valueBase() + hit);
            repaint();
            return true;
        }

        if (m_menu == Menu::Tone)
        {
            if (!ev.press)
                return true;
            const ToneHit hit = toneHit(float(ev.pos.getX()), float(ev.pos.getY()));
            switch (hit.kind)
            {
            case ToneHit::Part:
                // Follow the part to wherever its tone lives, so the list opens showing
                // where that part actually is rather than where you were last looking.
                m_tonePart = hit.index;
                for (size_t g = 0; g < m_toneGroups.size(); g ++)
                    if (m_toneGroups[g].media == m_partMedia[m_tonePart])
                        m_toneGroup = int(g);
                repaint();
                return true;
            case ToneHit::Group:
                m_toneGroup = hit.index;
                repaint();
                return true;
            case ToneHit::Tone:
            {
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%d %u %d", m_tonePart,
                              m_toneGroups[size_t(m_toneGroup)].media, hit.index);
                setState("tonesel", msg);
                m_menu = Menu::None;
                repaint();
                return true;
            }
            default:
                m_menu = Menu::None;
                repaint();
                return true;
            }
        }

        float x, y;
        toDesign(ev.pos.getX(), ev.pos.getY(), x, y);

        if (ev.press)
        {
            // The knob first: it overlaps nothing, but a drag has to claim the mouse.
            const auto &k = voltaire::panel::kVolumeKnob;
            if (std::hypot(x - k.cx, y - k.cy) <= k.r * 1.3f)
            {
                m_dragKnob = true;
                m_dragY = ev.pos.getY();
                m_dragStart = m_volume;
                return true;
            }
            if (m_nameEdit)
        {
            // Clicking away keeps what was typed, which is what every other text field
            // in a plugin does; Escape is how you throw it away.
            commitPatchName();
        }

        // The LCD is the one thing on the panel that is always visible and never does
            // anything, which makes it the natural place to hang "tell me what is wrong"
            // off -- and the place somebody stares at when nothing is happening.
            // The logo is the About box's button.  A maker's mark is where anyone
            // looks for the credits, and it is the one thing on the panel the real
            // machine never had, so nothing is being taken away to put it there.
            const auto &lo = voltaire::panel::kLogo;
            if (lo.w > 0.0f && x >= lo.x && x <= lo.x + lo.w
                    && y >= lo.y && y <= lo.y + lo.h)
            {
                m_menu = Menu::About;
                m_aboutScroll = 0;
                repaint();
                return true;
            }

            const auto &lg = voltaire::panel::kLcdInner;
            if (x >= lg.x && x <= lg.x + lg.w && y >= lg.y && y <= lg.y + lg.h)
            {
                // Asked again rather than shown from memory: the usual way this ends is
                // somebody copying the ROMs in and clicking to see whether that did it.
                askForDiag();
                m_menu = Menu::Diag;
                m_diagScroll = 0;
                repaint();
                return true;
            }

        const int tab = diveTabHit(x, y);
            if (tab >= 0)
            {
                (voltaire::panel::kDiveTabRow[tab] == 1 ? m_diveTab2 : m_diveTab1) = tab;
                resizeToDrawer();
                m_diveNeedRead = true;
                repaint();
                return true;
            }

            const int ctl = diveControlHit(x, y);
            if (ctl >= 0)
            {
                pressDiveControl(ctl, y);
                return true;
            }
            for (int i = 0; i < voltaire::panel::BUTTONID_COUNT; i ++)
            {
                const auto &r = voltaire::panel::kButton[i];
                if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h)
                {
                    // FILT LATCHES.  It selects a state rather than driving a menu, so
                    // unlike the six machine buttons it toggles on press and stays put.
                    if (i == voltaire::panel::BUT_FILTER)
                    {
                        m_hf = !m_hf;
                        setParam(kParamHfCorrection, m_hf ? 1.0f : 0.0f);
                        repaint();
                        return true;
                    }
                    if (i == voltaire::panel::BUT_PATCH_MENU
                            || i == voltaire::panel::BUT_TONE)
                    {
                        // Not a toggle: a click while a menu is up never reaches here,
                        // because the menu takes it first and closes on anything that is
                        // not one of its entries -- this button included.
                        m_menu = i == voltaire::panel::BUT_TONE ? Menu::Tone : Menu::Patch;
                        m_writeMode = false;
                        m_menuHover = -1;
                        m_toneHover = ToneHit();
                        // Rescanned on open rather than watched: instances share the
                        // library by the filesystem and nothing else, so this is how one
                        // instance's save turns up in another's list.  The index only
                        // re-reads files whose size or modification time moved.
                        if (m_menu == Menu::Patch && m_libraryTab)
                        { m_libraryNote.clear(); libraryRescan(); }
                        repaint();
                        return true;
                    }
                    if (i == voltaire::panel::BUT_RESET)
                    {
                        m_menu = Menu::Reset;
                        m_menuHover = -1;
                        repaint();
                        return true;
                    }
                    if (i == voltaire::panel::BUT_CART)
                    {
                        // The same page the LCD opens, and deliberately the same code:
                        // what the cards are and whether the machine can see them is one
                        // question with one answer, and two pages that drifted apart
                        // would be worse than one page reached two ways.
                        askForDiag();
                        m_menu = Menu::Diag;
                        m_diagScroll = 0;
                        repaint();
                        return true;
                    }
                    if (i == voltaire::panel::BUT_DIVE)
                    {
                        m_diveOpen = !m_diveOpen;
                        resizeToDrawer();
                        m_diveNeedRead = true;
                        repaint();
                        return true;
                    }
                    const int p = mapButton(i);
                    if (p < 0)
                        return true;                 // a real control with nothing behind it yet

                    // SHIFT LATCHES, because the machine has combinations a mouse cannot
                    // make.  Its service menu is stepped with two keys at once --
                    // [DEC]+[INC] forward, [LEFT]+[RIGHT] back, and the firmware only
                    // reads a pair when both are down in the SAME 100 Hz scan
                    // (ROM-ANALYSIS.md 8.5) -- so with one pointer there is no way to
                    // reach it at all.  A real U-110 has two hands; this is the second.
                    //
                    // A plain click on a latched button lets it go whether or not shift
                    // is down, so a key cannot be left stuck by forgetting the modifier.
                    if ((ev.mod & kModifierShift) != 0 || m_latch[i])
                    {
                        m_latch[i] = !m_latch[i];
                        setParam(uint32_t(kParamButtonFirst + p),
                                 m_latch[i] ? 1.0f : 0.0f);
                        repaint();
                        return true;
                    }
                    m_held = i;
                    setParam(uint32_t(kParamButtonFirst + p), 1.0f);
                    repaint();
                    return true;
                }
            }
            return false;
        }

        // Release.  The panel buttons are MOMENTARY, unlike the host parameters they drive:
        // pressing and releasing here sends the edge pair the firmware's debouncer wants.
        m_dragKnob = false;
        if (m_dragCtl >= 0)
        {
            m_dragCtl = -1;
            repaint();
        }
        if (m_held >= 0)
        {
            const int p = mapButton(m_held);
            if (p >= 0)
                setParam(uint32_t(kParamButtonFirst + p), 0.0f);
            m_held = -1;
            repaint();
        }
        return false;
    }

    bool onMotion(const MotionEvent &ev) override
    {
        // The X lights up wherever it is, before each palette looks at the pointer itself.
        if (m_menu != Menu::None)
        {
            const bool over = closeButtonHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (over != m_hoverClose)
            { m_hoverClose = over; repaint(); }
        }
        else if (m_hoverClose)
            m_hoverClose = false;

        if (m_menu == Menu::Patch)
        {
            int hit;
            bool zone = false;
            if (m_menu == Menu::Patch && m_libraryTab)
            {
                if (saveButtonHit(float(ev.pos.getX()), float(ev.pos.getY())))
                    hit = kHoverSave;
                else if (overrideButtonHit(float(ev.pos.getX()), float(ev.pos.getY())))
                    hit = kHoverOverride;
                else
                {
                    const LibHit lh = libraryHit(float(ev.pos.getX()),
                                                 float(ev.pos.getY()));
                    hit = lh.cell;
                    zone = lh.bankZone && !m_writeMode;
                }
            }
            else
                hit = patchHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit != m_menuHover || zone != m_hoverBankZone)
            { m_menuHover = hit; m_hoverBankZone = zone; repaint(); }
            return true;
        }
        if (m_menu == Menu::BankPick || m_menu == Menu::PresetActions
                || m_menu == Menu::DeleteConfirm || m_menu == Menu::OverwriteConfirm)
        {
            const BankPickLayout L = m_menu == Menu::BankPick ? bankPickLayout()
                                   : m_menu == Menu::PresetActions ? presetActionsLayout()
                                                                   : deleteConfirmLayout();
            bool ok = false;
            if (m_typing == Typing::NewBank && m_menu == Menu::BankPick)
                ok = inRect(fieldOkRect(L, L.rows - 1),
                            float(ev.pos.getX()), float(ev.pos.getY()));
            else if (m_typing == Typing::RenamePreset && m_menu == Menu::PresetActions)
                ok = inRect(fieldOkRect(L, kActRename),
                            float(ev.pos.getX()), float(ev.pos.getY()));
            const int hit = listHit(L, float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit != m_menuHover || ok != m_hoverFieldOk)
            { m_menuHover = hit; m_hoverFieldOk = ok; repaint(); }
            return true;
        }
        if (m_menu == Menu::WriteConfirm)
        {
            const int hit = confirmHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit != m_menuHover)
            { m_menuHover = hit; repaint(); }
            return true;
        }
        if (m_menu == Menu::Value)
        {
            const int hit = valueHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit != m_menuHover)
            { m_menuHover = hit; repaint(); }
            return true;
        }
        if (m_menu == Menu::Reset)
        {
            const int hit = resetHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit != m_menuHover)
            { m_menuHover = hit; repaint(); }
            return true;
        }
        if (m_menu == Menu::Tone)
        {
            const ToneHit hit = toneHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit.kind != m_toneHover.kind || hit.index != m_toneHover.index)
            { m_toneHover = hit; repaint(); }
            return true;
        }
        m_tipX = float(ev.pos.getX());
        m_tipY = float(ev.pos.getY());
        if (m_dragCtl >= 0)
        {
            float x, y;
            toDesign(ev.pos.getX(), ev.pos.getY(), x, y);
            dragSliderTo(m_dragCtl, y);
            repaint();
            return true;
        }
        if (m_diveOpen)
        {
            float x, y;
            toDesign(ev.pos.getX(), ev.pos.getY(), x, y);
            const int over = diveControlHit(x, y);
            if (over != m_hoverCtl)
            { m_hoverCtl = over; repaint(); }
            if (over >= 0)
                repaint();          // the tooltip follows the pointer
        }
        if (!m_dragKnob)
            return false;
        const float dy = float(m_dragY - ev.pos.getY());
        float v = m_dragStart + dy * 0.08f;          // 250 px for the whole range
        v = v < -3.0f ? -3.0f : (v > 16.0f ? 16.0f : v);
        if (v != m_volume) { m_volume = v; setParam(kParamVolume, v); repaint(); }
        return true;
    }

    bool onScroll(const ScrollEvent &ev) override
    {
        if (m_menu == Menu::Patch && m_libraryTab)
        {
            // By whole columns: the grid reads downwards, so scrolling it a row at a time
            // would move every name into the column beside it.
            m_libScroll -= int(ev.delta.getY()) * kLibRows;
            const int last = int(libraryView().size()) - kLibCells;
            if (m_libScroll > last) m_libScroll = last;
            if (m_libScroll < 0)    m_libScroll = 0;
            repaint();
            return true;
        }
        if (m_menu == Menu::Diag)
        {
            m_diagScroll -= int(ev.delta.getY() * 3.0f);
            const int last = int(m_diagLines.size()) - diagRows();
            if (m_diagScroll > last) m_diagScroll = last;
            if (m_diagScroll < 0)    m_diagScroll = 0;
            repaint();
            return true;
        }
        if (m_menu == Menu::About)
        {
            // Clamped against the LAST wrapped row count, which is a drawing-time
            // measurement -- the window can be narrower than the text and only the
            // renderer knows how many rows that made.
            m_aboutScroll -= int(ev.delta.getY() * 3.0f);
            if (m_aboutScroll > m_aboutLast) m_aboutScroll = m_aboutLast;
            if (m_aboutScroll < 0)           m_aboutScroll = 0;
            repaint();
            return true;
        }
        if (m_menu != Menu::None)
            return true;

        // A fader takes the wheel, one step of the parameter per notch.  One step rather
        // than a proportional jump because the wheel is what you reach for when the drag
        // put you nearly right: dragging is the coarse control and already covers the
        // whole range in one gesture.
        if (m_diveOpen)
        {
            float dx, dy;
            toDesign(ev.pos.getX(), ev.pos.getY(), dx, dy);
            const int ctl = diveControlHit(dx, dy);
            if (ctl >= 0
                    && voltaire::panel::kDiveControl[ctl].kind == voltaire::panel::DK_SLIDER)
            {
                const int raw = rawOf(ctl);
                if (raw != kNoValue)
                {
                    const int step = ev.delta.getY() > 0.0f ? 1
                                   : (ev.delta.getY() < 0.0f ? -1 : 0);
                    if (step != 0)
                        writeRaw(ctl, raw + step);
                }
                repaint();
                return true;
            }
        }

        const auto &k = voltaire::panel::kVolumeKnob;
        float x, y;
        toDesign(ev.pos.getX(), ev.pos.getY(), x, y);
        if (std::hypot(x - k.cx, y - k.cy) > k.r * 1.3f)
            return false;
        float v = m_volume + ev.delta.getY() * 0.5f;
        v = v < -3.0f ? -3.0f : (v > 16.0f ? 16.0f : v);
        m_volume = v; setParam(kParamVolume, v); repaint();
        return true;
    }

    // ---- keys, and the ones that never get here ---------------------------------------
    //
    // READ THIS BEFORE ADDING A KEY BINDING.  A plugin UI is a window inside the host's, the
    // host sees every key first, and one it has bound to something of its own is simply
    // never passed on.  This is not a bug to be fixed here and there is nothing the plugin
    // can do about it: which keys arrive is the host's decision, and it differs per host and
    // per user.
    //
    // Measured, not assumed:
    //
    //   Ardour   swallows Return, Space and plain letters including "a" -- transport and
    //            its own global bindings.  There is a full-keyboard-focus setting that
    //            changes this, but it is off by default and is the user's to turn on.
    //   Carla    passes everything through.
    //
    // So: NO FUNCTION MAY BE REACHABLE ONLY BY A KEY.  Every one of these has a button or a
    // click behind it -- the text fields have OK, every palette has an X, and Escape has
    // both of those as well.  A key is a shortcut for people whose host allows it and never
    // the only way in.  Naming a bank once needed Return and nothing else, and in Ardour
    // that made the field impossible to finish at all.
    //
    // VOLTAIRE_KEYS=1 prints what actually arrives, which is the only way to tell "the
    // handler is wrong" from "the key was never delivered".

    bool onKeyboard(const KeyboardEvent &ev) override
    {
        {
            static const bool trace = std::getenv("VOLTAIRE_KEYS") != nullptr;
            if (trace && ev.press)
                d_stdout("key 0x%04X  mod 0x%X  typing %d  menu %d",
                         ev.key, ev.mod, int(m_typing), int(m_menu));
        }

        if (m_typing != Typing::None && ev.press)
        {
            if (ev.key == kKeyEscape)
            { m_typing = Typing::None; repaint(); return true; }
            if (ev.key == kKeyBackspace)
            {
                if (m_typeCaret > 0)
                    m_typeBuf[-- m_typeCaret] = '\0';
                showCaret();
                repaint();
                return true;
            }
            // The keypad's Enter as well as the main one: it is a different key and this
            // is a text field, not a transport control.
            if (ev.key == kKeyEnter || ev.key == kKeyPadEnter)
            {
                commitTyping();
                repaint();
                return true;
            }
            return true;                 // nothing else reaches the panel while typing
        }

        if (m_nameEdit && ev.press)
        {
            if (ev.key == kKeyEscape)
            { m_nameEdit = false; repaint(); return true; }
            if (ev.key == kKeyBackspace)
            {
                if (m_nameCaret > 0)
                    m_nameBuf[-- m_nameCaret] = '\0';
                showCaret();
                repaint();
                return true;
            }
            if (ev.key == kKeyEnter)
            {
                commitPatchName();
                return true;
            }
            return true;                 // the field has the keyboard while it is open
        }
        if (m_menu == Menu::None || !ev.press || ev.key != kKeyEscape)
            return false;
        // Escape goes back one step rather than all the way out, for the boxes that were
        // opened from the library: leaving the delete question should put the list back,
        // not send somebody hunting for the PATCH button again.  A second Escape, now in
        // the library, closes it.
        closeCurrentMenu();
        repaint();
        return true;
    }

    /// Typing into the patch name.
    ///
    /// The machine's own name editor offers one character set and no others, so anything
    /// outside it is dropped rather than silently turned into something else -- a name
    /// that came back different from what was typed would be worse than a key that did
    /// nothing.
    bool onCharacterInput(const CharacterInputEvent &ev) override
    {
        if (m_typing != Typing::None)
        {
            // A bank name is shown in a tab and written on a line of its own, so it takes
            // printable characters and nothing else -- a newline here would forge a second
            // key in the preset file.
            const unsigned c = ev.character;
            if (c >= 0x20 && c < 0x7f && m_typeCaret < int(sizeof(m_typeBuf)) - 1)
            {
                m_typeBuf[m_typeCaret ++] = char(c);
                m_typeBuf[m_typeCaret] = '\0';
            }
            showCaret();
            repaint();
            return true;
        }
        if (!m_nameEdit)
            return false;
        const uint c = ev.character;
        if (c < 0x20 || c > 0x7e)
            return true;
        if (m_nameCaret < voltaire::dive::kNameLen)
        {
            m_nameBuf[m_nameCaret ++] = char(c);
            m_nameBuf[m_nameCaret] = '\0';
        }
        showCaret();
        repaint();
        return true;
    }

    /// Restart the blink, so the caret is solid under the character just typed.
    void showCaret()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        m_caretAt = double(ts.tv_sec) + ts.tv_nsec * 1e-9;
        m_caretOn = true;
    }

    void commitPatchName()
    {
        char msg[voltaire::dive::kNameLen + 4];
        std::snprintf(msg, sizeof(msg), "n%s", m_nameBuf);
        setState("divewrite", msg);
        std::snprintf(m_patchName, sizeof(m_patchName), "%-*s",
                      voltaire::dive::kNameLen, m_nameBuf);
        m_nameEdit = false;
        m_dirty = true;
        repaint();
    }

    static void trimTrailingSpaces(char *s)
    {
        size_t n = std::strlen(s);
        while (n > 0 && s[n - 1] == ' ')
            s[-- n] = '\0';
    }

private:
    // ---- the PATCH menu -------------------------------------------------------------
    //
    // Sixty-four patches, and the machine's own way to reach one is [INC] pressed as many
    // times as it takes.  This is that list: four columns of sixteen, the current patch
    // marked, one click to load.  The DSP does the loading -- see the note above
    // tickPatchSelect() in the plugin -- and the LCD comes back with the new name, so
    // nothing here has to guess whether the click took.
    //
    // Drawn in WINDOW PIXELS rather than the artwork's design units, which is the one
    // place in this file that is true.  The panel is 779 x 213 units of artwork and a
    // sixteen-row list does not fit above the button that opens it at any useful size;
    // the menu is not part of the instrument, so it is sized to the window and centred
    // over it, and stays legible when the panel is small.

    static constexpr int kMenuCols = 4;
    static constexpr int kMenuRows = 16;

    // Every grid menu here -- PATCH, TONE, and the value list -- is one number wide.  The
    // text is kMenuFontOfRow of the row height, a column is so many ems of that text, and
    // the box is its columns plus half a row of margin at each side.  So the box's width
    // is a fixed multiple of its row height, and gridRowH() below is where that multiple
    // is turned back into a row the window can afford.
    static constexpr float kMenuFontOfRow = 0.68f;
    static constexpr float kNameColEms    = 9.0f;    // "02 SOME TONE NAME"
    static constexpr float kValueColEms   = 5.0f;    // "C#-1" and the numbered routings

    struct MenuLayout { float x, y, w, h, rowH, colW, headerH, fontSize; };

    /// The row height a grid of `colEms` ems across can have in this window.
    ///
    /// The window's HEIGHT used to be the only thing that set the row, which is fine for
    /// a panel whose window only ever grows sideways.  The DIVE drawer is not that: it
    /// makes the window three times taller without making it one pixel wider, the row
    /// grew with the height to its cap, and the eight-column TONE grid came out about
    /// 1200 px across a 1100 px window -- centred, so it hung off the left edge and the
    /// right edge at once.  The width gets the same vote as the height here, and every
    /// grid that shares the shape is fixed by the same line.
    ///
    /// The 9 px floor is legibility and it wins over the width: a window narrow enough to
    /// need a smaller row than that is one where the panel itself has stopped being
    /// usable, and text nobody can read is not an improvement on a box that is too wide.
    float gridRowH(float heightRows, float colEms, float maxRow) const
    {
        const float byHeight = (float(getHeight()) - 12.0f) / heightRows;
        const float byWidth  = (float(getWidth()) - 24.0f)   // a dozen px of air each side
                             / (colEms * kMenuFontOfRow + 1.0f);
        const float r = byHeight < byWidth ? byHeight : byWidth;
        return r < 9.0f ? 9.0f : (r > maxRow ? maxRow : r);
    }

    MenuLayout patchLayout() const
    {
        MenuLayout m;
        // Three rows of chrome, not two: a header and a line along the bottom that says
        // what the list is for and what just happened.  That line used to share the header
        // row with the tabs, and a long one was drawn straight over them.
        m.rowH = gridRowH(float(kMenuRows + 4), float(kMenuCols) * kNameColEms, 26.0f);
        m.fontSize = m.rowH * kMenuFontOfRow;
        m.colW = m.fontSize * kNameColEms;
        m.headerH = m.rowH * 1.6f;
        m.w = kMenuCols * m.colW + m.rowH;               // half a row of margin each side
        m.h = m.headerH + kMenuRows * m.rowH + kFooterRows * m.rowH;
        m.x = std::floor((float(getWidth()) - m.w) * 0.5f);
        m.y = std::floor((float(getHeight()) - m.h) * 0.5f);
        return m;
    }

    /// Which entry is under the pointer, or -1 for none -- including everywhere outside
    /// the menu, which is how a click lands on "close and choose nothing".
    int patchHit(float px, float py) const
    {
        const MenuLayout m = patchLayout();
        const float gx = px - (m.x + m.rowH * 0.5f);
        const float gy = py - (m.y + m.headerH);
        if (gx < 0.0f || gy < 0.0f)
            return -1;
        const int col = int(gx / m.colW), row = int(gy / m.rowH);
        if (col >= kMenuCols || row >= kMenuRows)
            return -1;
        return col * kMenuRows + row;
    }

    // ---- the value list -------------------------------------------------------------
    //
    // A menu button in the drawer opens the parameter's whole range as a list.  Key Range
    // is 128 note names and Output Mode is 50 numbered routings, so this is columns of
    // sixteen like the patch menu rather than a strip, and it is sized to what it holds.

    int valueCount() const
    {
        const voltaire::dive::Param *p = paramFor(m_valueCtl);
        if (p == nullptr)
            return 0;
        const bool tune = p->where == voltaire::dive::kRam
                       && p->show == voltaire::dive::kSigned;
        return tune ? (voltaire::dive::kMasterTuneMax - voltaire::dive::kMasterTuneMin + 1)
                    : (int(p->hi) - int(p->lo) + 1);
    }

    int valueBase() const
    {
        const voltaire::dive::Param *p = paramFor(m_valueCtl);
        if (p == nullptr)
            return 0;
        return (p->where == voltaire::dive::kRam && p->show == voltaire::dive::kSigned)
                ? voltaire::dive::kMasterTuneMin : int(p->lo);
    }

    MenuLayout valueLayout(int &rows, int &cols) const
    {
        const int n = valueCount();
        rows = n < kMenuRows ? (n > 0 ? n : 1) : kMenuRows;
        cols = (n + rows - 1) / (rows > 0 ? rows : 1);
        MenuLayout m;
        // Sized on the columns it actually has: a two-column list of On/Off has no
        // business being held to the width an eight-column one needs.
        m.rowH = gridRowH(float(kMenuRows + 2), float(cols) * kValueColEms, 26.0f);
        m.fontSize = m.rowH * kMenuFontOfRow;
        m.colW = m.fontSize * kValueColEms;
        m.headerH = m.rowH * 1.6f;
        m.w = float(cols) * m.colW + m.rowH;
        m.h = m.headerH + float(rows) * m.rowH + m.rowH * 0.5f;
        m.x = std::floor((float(getWidth()) - m.w) * 0.5f);
        m.y = std::floor((float(getHeight()) - m.h) * 0.5f);
        return m;
    }

    int valueHit(float px, float py) const
    {
        int rows = 0, cols = 0;
        const MenuLayout m = valueLayout(rows, cols);
        const float gx = px - (m.x + m.rowH * 0.5f);
        const float gy = py - (m.y + m.headerH);
        if (gx < 0.0f || gy < 0.0f)
            return -1;
        const int col = int(gx / m.colW), row = int(gy / m.rowH);
        if (col >= cols || row >= rows)
            return -1;
        const int idx = col * rows + row;
        return idx < valueCount() ? idx : -1;
    }

    void drawValueMenu()
    {
        const voltaire::dive::Param *p = paramFor(m_valueCtl);
        if (p == nullptr)
            return;
        int rows = 0, cols = 0;
        const MenuLayout m = valueLayout(rows, cols);
        const int base = valueBase(), n = valueCount(), cur = rawOf(m_valueCtl);

        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.55f));
        fill();

        beginPath();
        roundedRect(m.x, m.y, m.w, m.h, m.rowH * 0.35f);
        fillColor(Color(26, 28, 32));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(m.x, m.y, m.w, m.rowH);

        fontFace(m_font);
        fontSize(m.fontSize * 1.05f);
        fillColor(Color(0, 163, 224));
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        text(m.x + m.w * 0.5f, m.y + m.headerH * 0.55f,
             prettyLabel(voltaire::panel::kDiveControl[m_valueCtl].label), nullptr);

        fontSize(m.fontSize);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        for (int i = 0; i < n; i ++)
        {
            const int col = i / rows, row = i % rows;
            const float x = m.x + m.rowH * 0.5f + float(col) * m.colW;
            const float y = m.y + m.headerH + float(row) * m.rowH;
            const bool current = (base + i) == cur;
            if (i == m_menuHover || current)
            {
                beginPath();
                roundedRect(x, y, m.colW - 2.0f, m.rowH - 1.0f, 3.0f);
                fillColor(i == m_menuHover ? Color(0, 163, 224, 0.45f)
                                           : Color(255, 255, 255, 0.10f));
                fill();
            }
            char item[24];
            formatRaw(*p, base + i, item, sizeof(item));
            fillColor(current ? Color(190, 255, 190) : Color(214, 216, 220));
            text(x + m.fontSize * 0.4f, y + m.rowH * 0.5f, item, nullptr);
        }
    }

    /// One line of any of the three overlays -- the self check, the About box, RESET.
    ///
    /// Everything in all three is a multiple of this: box, margins, the lot.  So this is
    /// the one place their size is decided, and raising it scales each layout together
    /// rather than pushing text into its own margins.
    ///
    /// The window's own height sets the base, which is what keeps an overlay in
    /// proportion to the panel it covers.  The four steps on top of it are not in
    /// proportion to anything, and that is the point: at the base the body text came out
    /// around 9 px, which is smaller than anything anyone reads on purpose.  Legibility
    /// has a floor that does not scale with a window.
    float overlayRowH() const
    {
        const float h = float(getHeight()) / 34.0f;
        const float base = h < 9.0f ? 9.0f : (h > 18.0f ? 18.0f : h);
        // Four steps of body text, at 0.78 rows to the step, rounded to a whole pixel.
        return base + 5.0f;
    }

    /// A triangle marking text that carries on past the edge of the box.
    ///
    /// Drawn rather than typed: the panel's font is chosen for the lettering on the
    /// artwork and nothing guarantees it has a glyph at U+25BC, which would come out as a
    /// blank or a box in the one place whose whole job is to say "there is more".
    void drawScrollCaret(float cx, float cy, float size, bool down)
    {
        const float dy = down ? size * 0.5f : -size * 0.5f;
        beginPath();
        moveTo(cx - size * 0.6f, cy - dy);
        lineTo(cx + size * 0.6f, cy - dy);
        lineTo(cx, cy + dy);
        closePath();
        fillColor(Color(0, 163, 224, 0.85f));
        fill();
    }

    /// How many lines of the report fit.  The wheel needs this to know where the end is.
    int diagRows() const
    {
        const float rowH = overlayRowH();
        const float pad = std::floor(float(getHeight()) * 0.04f) + 4.0f;
        const int n = int((float(getHeight()) - pad * 2.0f - rowH * 3.0f) / rowH);
        return n < 1 ? 1 : n;
    }

    /// The report, as a page laid over the panel.
    ///
    /// A window rather than a message box on purpose.  A native dialog would mean one
    /// piece of code per platform for the one feature whose entire job is to work when
    /// something else did not, and it would block the host's UI thread while it was up.
    void drawDiagMenu()
    {
        const float rowH = overlayRowH();
        const float pad  = std::floor(float(getHeight()) * 0.04f) + 4.0f;
        const float x = pad, y = pad;
        const float w = float(getWidth()) - pad * 2.0f;
        const float h = float(getHeight()) - pad * 2.0f;

        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.72f));
        fill();

        beginPath();
        roundedRect(x, y, w, h, rowH * 0.4f);
        fillColor(Color(22, 24, 28));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(x, y, w, rowH);

        fontFace(m_font);
        fontSize(rowH * 0.95f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(m_diagStatus == "ok" ? Color(150, 255, 150) : Color(255, 180, 90));
        text(x + rowH * 0.6f, y + rowH * 1.1f, "VOLTAIRE 110 -- SELF CHECK", nullptr);

        const int rows = diagRows();
        const float bodyTop = y + rowH * 2.0f;

        if (m_diagLines.empty())
        {
            fontSize(rowH * 0.8f);
            fillColor(Color(214, 216, 220));
            text(x + rowH * 0.7f, bodyTop + rowH,
                 "The DSP side has not answered. If this does not clear in a second or",
                 nullptr);
            text(x + rowH * 0.7f, bodyTop + rowH * 2.0f,
                 "two, the host has loaded the UI but is not running the plugin.", nullptr);
        }
        else
        {
            save();
            // The right-hand gutter is reserved whether or not there is anything to
            // scroll, so a long path cannot run underneath a caret.
            scissor(x + rowH * 0.5f, bodyTop, w - rowH * 2.0f, float(rows) * rowH);
            fontSize(rowH * 0.78f);
            for (int i = 0; i < rows; i ++)
            {
                const int li = m_diagScroll + i;
                if (li < 0 || size_t(li) >= m_diagLines.size())
                    break;
                const std::string &line = m_diagLines[size_t(li)];
                // The report uses blank lines to separate its sections, and NanoVG's
                // text() asserts on an empty string rather than drawing nothing.
                if (line.empty())
                    continue;
                // Headings are the lines that start hard left; the listings under them are
                // indented.  Colouring on that alone means the report can grow new sections
                // without this having to be told about them.
                const bool heading = !line.empty() && line[0] != ' ';
                fillColor(heading ? Color(235, 235, 235) : Color(168, 174, 184));
                text(x + rowH * 0.7f, bodyTop + (float(i) + 0.5f) * rowH, line.c_str(),
                     nullptr);
            }
            restore();

            // Which way there is more of it.  The report is 70-odd lines and never fits,
            // and the line count in the footer says where you are without saying which
            // way to go.
            const int last = int(m_diagLines.size()) - rows;
            const float caretX = x + w - rowH * 1.0f;
            if (m_diagScroll > 0)
                drawScrollCaret(caretX, bodyTop + rowH * 0.55f, rowH * 0.5f, false);
            if (m_diagScroll < last)
                drawScrollCaret(caretX, bodyTop + (float(rows) - 0.55f) * rowH,
                                rowH * 0.5f, true);
        }

        char foot[160];
        std::snprintf(foot, sizeof(foot),
                      "wheel scrolls   |   click anywhere to close   |   line %u of %u",
                      unsigned(m_diagLines.empty() ? 0 : m_diagScroll + 1),
                      unsigned(m_diagLines.size()));
        fontSize(rowH * 0.72f);
        fillColor(Color(140, 146, 156));
        text(x + rowH * 0.6f, y + h - rowH * 0.8f, foot, nullptr);
    }

    /// One line of the About text, wrapped to `w` pixels, keeping its indent on every
    /// row it takes.
    ///
    /// NanoVG has textBox() for this and it is not usable here: it strips leading
    /// whitespace, which is the one piece of markup resources/about.txt has.  An
    /// indented line is a list item and has to stay looking like one.
    void wrapAboutLine(const std::string &src, float w, std::vector<std::string> &out)
    {
        const size_t ind = src.find_first_not_of(' ');
        if (ind == std::string::npos)
        {
            out.emplace_back();
            return;
        }
        const std::string pad(ind, ' ');
        std::string row = pad;
        bool empty = true;
        size_t i = ind;
        while (i < src.size())
        {
            size_t sp = src.find(' ', i);
            if (sp == std::string::npos)
                sp = src.size();
            const std::string cand = empty ? row + src.substr(i, sp - i)
                                           : row + " " + src.substr(i, sp - i);
            Rectangle<float> b;
            if (!empty && textBounds(0.0f, 0.0f, cand.c_str(), nullptr, b) > w)
            {
                out.push_back(row);
                row = pad + src.substr(i, sp - i);
            }
            else
                row = cand;
            empty = false;
            for (i = sp; i < src.size() && src[i] == ' '; i ++)
                ;
        }
        out.push_back(row);
    }

    /// The credits, as a page laid over the panel.
    ///
    /// The text is resources/about.txt, compiled in by tools/make_about.py -- there is no
    /// copy of it in this file to fall out of step, and a bundle cannot ship without it.
    /// Same window-not-dialog reasoning as the self-check above.
    void drawAboutBox()
    {
        const float rowH = overlayRowH();
        const float pad  = std::floor(float(getHeight()) * 0.04f) + 4.0f;
        // Capped rather than filling the window: a line of prose is unreadable when it
        // runs the full width of a panel that is three times as wide as it is tall.
        float w = float(getWidth()) - pad * 2.0f;
        const float wmax = rowH * 62.0f;
        if (w > wmax)
            w = wmax;
        const float x = std::floor((float(getWidth()) - w) * 0.5f);
        const float y = pad;
        const float h = float(getHeight()) - pad * 2.0f;

        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.72f));
        fill();

        beginPath();
        roundedRect(x, y, w, h, rowH * 0.4f);
        fillColor(Color(22, 24, 28));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(x, y, w, rowH);

        fontFace(m_font);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fontSize(rowH * 0.95f);
        fillColor(Color(0, 163, 224));                   // the logo's own cyan
        text(x + rowH * 0.6f, y + rowH * 1.1f, "ABOUT VOLTAIRE 110", nullptr);

        const float bodyX = x + rowH * 0.7f;
        // The right-hand gutter is reserved whether or not there is anything to scroll,
        // because the carets live in it and the wrap width decides the row count -- a
        // width that changed once the text overflowed would be deciding its own input.
        const float bodyW = w - rowH * 2.2f;
        const float bodyTop = y + rowH * 2.0f;
        const int rows = std::max(1, int((h - rowH * 3.2f) / rowH));

        // Wrapped at the width it is about to be drawn at, every frame, because that
        // width changes with the window and the wrap is what decides the row count.
        fontSize(rowH * 0.78f);
        m_aboutRows.clear();
        for (const std::string &ln : m_aboutLines)
            wrapAboutLine(ln, bodyW, m_aboutRows);

        m_aboutLast = int(m_aboutRows.size()) - rows;
        if (m_aboutLast < 0)             m_aboutLast = 0;
        if (m_aboutScroll > m_aboutLast) m_aboutScroll = m_aboutLast;
        if (m_aboutScroll < 0)           m_aboutScroll = 0;

        save();
        scissor(x + rowH * 0.5f, bodyTop, w - rowH, float(rows) * rowH);
        for (int i = 0; i < rows; i ++)
        {
            const int li = m_aboutScroll + i;
            if (li < 0 || size_t(li) >= m_aboutRows.size())
                break;
            const std::string &line = m_aboutRows[size_t(li)];
            if (line.empty())
                continue;               // NanoVG's text() asserts on an empty string
            // Indented lines are the file's lists; dimming them is the whole of the
            // formatting, and it costs the text file no markup to get it.
            fillColor(line[0] == ' ' ? Color(168, 174, 184) : Color(214, 216, 220));
            text(bodyX, bodyTop + (float(i) + 0.5f) * rowH, line.c_str(), nullptr);
        }
        restore();

        // Which way there is more of it.  A box that is one line short of showing
        // everything looks exactly like a box showing everything, and the credits end in
        // the list of MAME sources -- the part somebody scrolling is most likely after.
        const float caretX = x + w - rowH * 1.0f;
        const float caretY = bodyTop + float(rows) * rowH;
        if (m_aboutScroll > 0)
            drawScrollCaret(caretX, bodyTop + rowH * 0.55f, rowH * 0.5f, false);
        if (m_aboutScroll < m_aboutLast)
            drawScrollCaret(caretX, caretY - rowH * 0.55f, rowH * 0.5f, true);

        char foot[160];
        if (m_aboutLast > 0)
            std::snprintf(foot, sizeof(foot),
                          "wheel scrolls   |   click anywhere to close   |   "
                          "line %u of %u",
                          unsigned(m_aboutScroll + 1), unsigned(m_aboutRows.size()));
        else
            std::snprintf(foot, sizeof(foot), "click anywhere to close");
        fontSize(rowH * 0.72f);
        fillColor(Color(140, 146, 156));
        text(x + rowH * 0.6f, y + h - rowH * 0.8f, foot, nullptr);
    }

    // ---- the RESET button's menu ----------------------------------------------------
    //
    // Three ways to start the machine again and a way out, in the same palette the About
    // box uses.  Two of the three are the HARDWARE'S own boot options rather than
    // anything the plugin invented -- the firmware looks at the key matrix once while it
    // comes up and branches on what is held (ROM-ANALYSIS.md section 8.5) -- which is why
    // each entry says which keys it is standing in for.  Somebody who knows the machine
    // should recognise what this menu is doing, and somebody who does not should be able
    // to learn it from here.

    struct ResetItem
    {
        const char *title;
        const char *detail;
        const char *note;       ///< a second line, or null
        const char *arg;        ///< the "reboot" state value, or null for Cancel
        bool destructive;
    };

    static constexpr ResetItem kResetItems[] = {
        { "Reboot",
          "The machine restarts and keeps its memory: a U-110 switched off and on.",
          nullptr, "warm", false },
        { "Reboot into the service test menu",
          "DEC and INC held at power-on. RAM, LCD, keys, battery, MIDI, wave ROM,",
          "sound and output checks. Stepped through with two keys at once, which is "
          "what shift-click is for.",
          "test", false },
        { "Initialise the memory and reboot",
          "PART and EDIT held at power-on: the machine's own factory reset.",
          "THE 64 USER PATCHES GO BACK TO THE FACTORY SET -- reopening the saved "
          "project is what brings them back.",
          "init", true },
        { "Cancel",
          "Leave the machine running.",
          nullptr, nullptr, false },
    };
    static constexpr int kResetCount = int(sizeof(kResetItems) / sizeof(kResetItems[0]));

    /// Where the box and its entries are, in WINDOW pixels -- the overlays are drawn
    /// outside the panel transform, so a mouse position needs no conversion.
    struct ResetLayout { float x, y, w, h, rowH, top, cell; };

    /// Every measurement in the box is a multiple of one row, including its own height:
    /// 2.4 rows of heading, four cells of 3.4, and 1.2 for the footer.
    static constexpr float kResetCellRows = 3.4f;
    static constexpr float kResetBoxRows =
            2.4f + kResetCellRows * float(kResetCount) + 1.2f;

    ResetLayout resetLayout() const
    {
        ResetLayout L;
        const float pad = std::floor(float(getHeight()) * 0.04f) + 4.0f;

        // This box does not scroll, so the row it is set on is the row it can AFFORD:
        // unlike the About box it has a fixed number of entries and all of them matter,
        // and a menu whose last option is off the bottom of the window is a menu with a
        // missing option.  Only a window shorter than about 280 px ever notices.
        const float room = (float(getHeight()) - pad * 2.0f) / kResetBoxRows;
        L.rowH = std::min(overlayRowH(), room);
        L.cell = L.rowH * kResetCellRows;

        L.w = float(getWidth()) - pad * 2.0f;
        const float wmax = L.rowH * 50.0f;
        if (L.w > wmax)
            L.w = wmax;
        L.h = L.rowH * kResetBoxRows;
        L.x = std::floor((float(getWidth()) - L.w) * 0.5f);
        L.y = std::floor((float(getHeight()) - L.h) * 0.5f);
        if (L.y < pad)
            L.y = pad;
        L.top = L.y + L.rowH * 2.4f;
        return L;
    }

    int resetHit(float px, float py) const
    {
        const ResetLayout L = resetLayout();
        if (px < L.x || px > L.x + L.w || py < L.top)
            return -1;
        const int i = int((py - L.top) / L.cell);
        return i >= 0 && i < kResetCount ? i : -1;
    }

    void drawResetMenu()
    {
        const ResetLayout L = resetLayout();

        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.72f));
        fill();

        beginPath();
        roundedRect(L.x, L.y, L.w, L.h, L.rowH * 0.4f);
        fillColor(Color(22, 24, 28));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(L.x, L.y, L.w, L.rowH);

        fontFace(m_font);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fontSize(L.rowH * 0.95f);
        fillColor(Color(255, 180, 90));
        text(L.x + L.rowH * 0.7f, L.y + L.rowH * 1.2f, "RESET THE MACHINE", nullptr);

        for (int i = 0; i < kResetCount; i ++)
        {
            const ResetItem &it = kResetItems[i];
            const float top = L.top + float(i) * L.cell;

            if (i == m_menuHover)
            {
                beginPath();
                roundedRect(L.x + L.rowH * 0.4f, top, L.w - L.rowH * 0.8f,
                            L.cell - L.rowH * 0.2f, L.rowH * 0.25f);
                fillColor(Color(0, 163, 224, 0.30f));
                fill();
            }

            fontSize(L.rowH * 0.9f);
            // The one entry that throws something away is coloured as such, so it cannot
            // be picked out of a list of four by muscle memory alone.
            fillColor(it.destructive ? Color(255, 150, 120) : Color(225, 228, 232));
            text(L.x + L.rowH * 0.9f, top + L.rowH * 0.85f, it.title, nullptr);

            fontSize(L.rowH * 0.72f);
            fillColor(Color(168, 174, 184));
            text(L.x + L.rowH * 1.3f, top + L.rowH * 1.95f, it.detail, nullptr);
            if (it.note != nullptr)
                text(L.x + L.rowH * 1.3f, top + L.rowH * 2.8f, it.note, nullptr);
        }

        fontSize(L.rowH * 0.72f);
        fillColor(Color(140, 146, 156));
        text(L.x + L.rowH * 0.7f, L.y + L.h - L.rowH * 0.7f,
             "the machine takes about five seconds to come up   |   Escape closes this",
             nullptr);
    }

    /// What to do with a panel that needs two fingers.
    ///
    /// Shown for as long as the machine is in the service menu, because that is exactly
    /// how long it is impossible to work out: every one of those screens is stepped with
    /// a two-key press, none of them says how to make one with a mouse, and the way out
    /// is another reboot.  A line in a menu that has already been dismissed would not
    /// have helped the person who is stuck.
    void drawTestHint()
    {
        const float rowH = overlayRowH();
        static constexpr const char *kMsg =
                "TEST MODE   --   shift-click latches a button:   "
                "[DEC]+[INC] next test,   [LEFT]+[RIGHT] previous";

        fontFace(m_font);
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);

        // One long line that cannot be wrapped or shortened without losing the key
        // combinations it exists to give.  So it is measured, and if the window is too
        // narrow to hold it the type comes down until it fits rather than running off
        // both edges.
        float fs = rowH * 0.72f;
        fontSize(fs);
        Rectangle<float> b;
        textBounds(0.0f, 0.0f, kMsg, nullptr, b);
        const float room = float(getWidth()) - rowH * 0.8f;
        if (b.getWidth() + rowH * 2.0f > room && b.getWidth() > 0.0f)
        {
            fs *= (room - rowH * 2.0f) / b.getWidth();
            fs = fs < 6.0f ? 6.0f : fs;
            fontSize(fs);
            textBounds(0.0f, 0.0f, kMsg, nullptr, b);
        }

        const float w = b.getWidth() + rowH * 2.0f;
        const float h = rowH * 1.8f;
        const float x = std::floor((float(getWidth()) - w) * 0.5f);
        const float y = float(getHeight()) - h - rowH * 0.4f;

        beginPath();
        roundedRect(x, y, w, h, rowH * 0.35f);
        fillColor(Color(12, 14, 18, 0.92f));
        fill();
        strokeColor(Color(0, 163, 224, 0.8f));
        strokeWidth(1.0f);
        stroke();

        fillColor(Color(214, 226, 236));
        text(x + w * 0.5f, y + h * 0.5f, kMsg, nullptr);

        // NanoVG's alignment is global state and every other page here assumes this one.
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    }

    // ---- confirming a WRITE -----------------------------------------------------------
    //
    // The machine asks too, in its own way: PATCH:WRT:WRITE shows "TMP ~ PATCH-nn?" and
    // waits for ENTER.  This is that question, with the thing about to be lost named --
    // which the hardware cannot do, having one line of sixteen characters to say it in.
    //
    // MEM PROTECT is not consulted.  It gates the firmware's own write path and this one
    // goes around it, so the guard that exists is this box: a deliberate second click,
    // rather than a bit set on a SETUP page nobody would think to look at.

    static constexpr int   kConfirmCount    = 2;
    static constexpr float kConfirmCellRows = 2.7f;
    static constexpr float kConfirmHeadRows = 5.0f;
    static constexpr float kConfirmBoxRows  =
            kConfirmHeadRows + kConfirmCellRows * float(kConfirmCount) + 1.2f;

    struct ConfirmLayout { float x, y, w, h, rowH, top, cell; };

    ConfirmLayout confirmLayout() const
    {
        ConfirmLayout L;
        const float pad = std::floor(float(getHeight()) * 0.04f) + 4.0f;
        const float room = (float(getHeight()) - pad * 2.0f) / kConfirmBoxRows;
        L.rowH = std::min(overlayRowH(), room);
        L.cell = L.rowH * kConfirmCellRows;
        L.w = float(getWidth()) - pad * 2.0f;
        const float wmax = L.rowH * 46.0f;
        if (L.w > wmax)
            L.w = wmax;
        L.h = L.rowH * kConfirmBoxRows;
        L.x = std::floor((float(getWidth()) - L.w) * 0.5f);
        L.y = std::floor((float(getHeight()) - L.h) * 0.5f);
        if (L.y < pad)
            L.y = pad;
        L.top = L.y + L.rowH * kConfirmHeadRows;
        return L;
    }

    int confirmHit(float px, float py) const
    {
        const ConfirmLayout L = confirmLayout();
        if (px < L.x || px > L.x + L.w || py < L.top)
            return -1;
        const int i = int((py - L.top) / L.cell);
        return i >= 0 && i < kConfirmCount ? i : -1;
    }

    /// What is in the slot now, for the box to name.  Blank rather than "?" when the
    /// names have not arrived: a machine that has not booted has nothing to lose yet.
    const char *targetName() const
    {
        if (m_writeTarget < 0 || size_t(m_writeTarget) >= m_patchNames.size())
            return "";
        return m_patchNames[size_t(m_writeTarget)].c_str();
    }

    void drawWriteConfirm()
    {
        const ConfirmLayout L = confirmLayout();

        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.72f));
        fill();

        beginPath();
        roundedRect(L.x, L.y, L.w, L.h, L.rowH * 0.4f);
        fillColor(Color(22, 24, 28));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(L.x, L.y, L.w, L.rowH);

        fontFace(m_font);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fontSize(L.rowH * 0.95f);
        fillColor(Color(255, 180, 90));
        text(L.x + L.rowH * 0.7f, L.y + L.rowH * 1.2f, "STORE THE PATCH", nullptr);

        char line[128];
        fontSize(L.rowH * 0.78f);
        fillColor(Color(225, 228, 232));
        std::snprintf(line, sizeof(line), "The patch being edited goes into P-%02d, over %s.",
                      m_writeTarget + 1, targetName());
        text(L.x + L.rowH * 0.7f, L.y + L.rowH * 2.5f, line, nullptr);

        fontSize(L.rowH * 0.72f);
        fillColor(Color(168, 174, 184));
        text(L.x + L.rowH * 0.7f, L.y + L.rowH * 3.6f,
             "The machine's own WRITE, without its menus. What is there now is gone,",
             nullptr);
        text(L.x + L.rowH * 0.7f, L.y + L.rowH * 4.4f,
             "though reopening the saved project brings the whole bank back.", nullptr);

        static const char *const kTitles[kConfirmCount] = { "Write it", "Cancel" };
        static const char *const kNotes[kConfirmCount] = {
            "P-%02d is selected afterwards, so you hear what was stored.",
            "Leave every patch as it is.",
        };

        for (int i = 0; i < kConfirmCount; i ++)
        {
            const float top = L.top + float(i) * L.cell;
            if (i == m_menuHover)
            {
                beginPath();
                roundedRect(L.x + L.rowH * 0.4f, top, L.w - L.rowH * 0.8f,
                            L.cell - L.rowH * 0.2f, L.rowH * 0.25f);
                fillColor(Color(0, 163, 224, 0.30f));
                fill();
            }
            fontSize(L.rowH * 0.9f);
            fillColor(i == 0 ? Color(255, 150, 120) : Color(225, 228, 232));
            text(L.x + L.rowH * 0.9f, top + L.rowH * 0.8f, kTitles[i], nullptr);

            fontSize(L.rowH * 0.72f);
            fillColor(Color(168, 174, 184));
            std::snprintf(line, sizeof(line), kNotes[i], m_writeTarget + 1);
            text(L.x + L.rowH * 1.3f, top + L.rowH * 1.85f, line, nullptr);
        }

        fontSize(L.rowH * 0.72f);
        fillColor(Color(140, 146, 156));
        text(L.x + L.rowH * 0.7f, L.y + L.h - L.rowH * 0.7f,
             "Escape closes this and writes nothing", nullptr);
    }

    // ---- the user's own library -------------------------------------------------------
    //
    // The machine's 64 slots are what the FIRMWARE has; this is what the user has, and it
    // is unbounded because a computer is not a 1989 rompler.  PLUGIN-PLAN.md section 10.5.
    //
    // A preset is played from a slot because it MUST be: only the firmware can load a
    // patch, and it loads out of patchram.  So one slot is the audition slot -- P-64 -- and
    // every library load goes through it, leaving the other 63 as the machine's own bank,
    // editable from the panel exactly as on hardware.  That also makes a session
    // self-contained for free: patchram is saved whole into the `nvram` key, so the patch
    // comes back with the project even if the library file has since moved or gone.

    /// The library grid is a row shorter than the machine's, because its header carries
    /// the bank chips as well as the two tabs -- and TWO columns rather than four, because
    /// each row shows a preset's bank beside its name.  Showing it is what makes filing
    /// discoverable: the bank is on screen, so clicking it to change it needs no
    /// explaining, and 30 presets to a page is plenty with the wheel to hand.
    /// The line along the bottom of both lists.
    static constexpr float kFooterRows = 1.4f;

    static constexpr int kLibCols  = 2;
    static constexpr int kLibRows  = kMenuRows - 1;
    static constexpr int kLibCells = kLibCols * kLibRows;

    MenuLayout libraryLayout() const
    {
        MenuLayout m = patchLayout();
        m.headerH = m.rowH * 2.9f;          // the tabs, then the banks
        m.colW = (m.w - m.rowH) / float(kLibCols);
        m.h = m.headerH + float(kLibRows) * m.rowH + kFooterRows * m.rowH;
        m.y = std::floor((float(getHeight()) - m.h) * 0.5f);
        if (m.y < 4.0f)
            m.y = 4.0f;
        return m;
    }

    /// Where in the grid the pointer is: which preset, and whether it is over the bank
    /// shown at the right-hand end of the row rather than over the name.
    struct LibHit { int cell = -1; bool bankZone = false; };

    /// The fraction of a row given over to the bank, measured from the right.
    static constexpr float kBankZone = 0.38f;

    /// Where cell `i` is drawn.  THE ONE PLACE that decides, because the drawing and the
    /// hit test disagreeing is exactly the bug that made clicking the first preset save a
    /// duplicate instead: the list was drawn under a header of one height and clicked
    /// under a header of another.  Both go through here now, so they cannot drift apart.
    void libraryCellRect(const MenuLayout &m, int i, float &x, float &y) const
    {
        x = m.x + m.rowH * 0.5f + float(i / kLibRows) * m.colW;
        y = m.y + m.headerH + float(i % kLibRows) * m.rowH;
    }

    LibHit libraryHit(float px, float py) const
    {
        const MenuLayout m = libraryLayout();
        LibHit h;
        for (int i = 0; i < kLibCells; i ++)
        {
            float x, y;
            libraryCellRect(m, i, x, y);
            if (px < x || px >= x + m.colW || py < y || py >= y + m.rowH)
                continue;
            h.cell = i;
            h.bankZone = (px - x) > m.colW * (1.0f - kBankZone);
            break;
        }
        return h;
    }

    /// The Save button, which is a BUTTON in the header and not a cell in the list.  It
    /// was a cell, sharing the grid with the presets, and that was the bug: a click meant
    /// for the first preset landed on it and quietly saved a duplicate instead.
    struct HeaderRect { float x, y, w, h; };

    /// Save always makes a NEW preset and never overwrites -- two patches may reasonably
    /// share a name.  Override writes back over the one that was recalled, which is the
    /// other half of the same job and the half that used to mean saving a second copy and
    /// then deleting the first.
    /// A fixed label.  It used to name the bank, which read well but made the button as
    /// wide as the longest bank name somebody might invent -- and the header has two
    /// buttons and two tabs to fit.  Which bank it saves into is on screen anyway: the
    /// chip is lit, and the line along the bottom says so in words.
    static constexpr const char *kSaveLabel = "Save Patch";
    std::string saveButtonLabel() const { return std::string(kSaveLabel); }

    static constexpr const char *kOverrideLabel = "Override Patch";

    HeaderRect overrideButtonRect(const MenuLayout &m) const
    {
        HeaderRect r;
        r.h = m.rowH * 1.1f;
        r.y = m.y + m.rowH * 0.15f;
        r.w = m.fontSize * (0.58f * float(std::strlen(kOverrideLabel)) + 1.6f);
        // Clear of the X in the corner, which is drawn over this same row.
        r.x = m.x + m.w - m.rowH * (0.35f + kCloseSize + 0.4f) - r.w;
        return r;
    }

    HeaderRect saveButtonRect(const MenuLayout &m) const
    {
        const HeaderRect o = overrideButtonRect(m);
        HeaderRect r;
        r.h = o.h;
        r.y = o.y;
        r.w = m.fontSize * (0.58f * float(saveButtonLabel().size()) + 1.6f);
        r.x = o.x - m.fontSize * 0.6f - r.w;
        return r;
    }

    static bool inRect(const HeaderRect &r, float px, float py)
    {
        return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
    }

    bool saveButtonHit(float px, float py) const
    {
        return inRect(saveButtonRect(libraryLayout()), px, py);
    }

    bool overrideButtonHit(float px, float py) const
    {
        return inRect(overrideButtonRect(libraryLayout()), px, py);
    }

    /// One header button, drawn the same way whichever it is.
    void headerButton(const HeaderRect &r, const MenuLayout &m, const char *label,
                      bool hot, bool enabled)
    {
        beginPath();
        roundedRect(r.x, r.y, r.w, r.h, r.h * 0.28f);
        fillColor(!enabled ? Color(30, 32, 36)
                           : hot ? Color(52, 116, 60) : Color(40, 96, 46));
        fill();
        strokeColor(enabled ? Color(120, 200, 130) : Color(64, 68, 74));
        strokeWidth(1.0f);
        stroke();
        fontSize(m.fontSize * 0.85f);
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        fillColor(enabled ? Color(225, 255, 225) : Color(110, 114, 120));
        text(r.x + r.w * 0.5f, r.y + r.h * 0.5f, label, nullptr);
    }

    /// One tab along the bank row.  `kind` is 0 for All, 1 for a named bank, 2 for
    /// Unfiled; `label` is what it says.
    struct Chip { float x, w; std::string label; int kind; std::string name; };

    std::vector<Chip> bankChips(const MenuLayout &m) const
    {
        std::vector<Chip> out;
        const auto add = [&](const std::string &label, int kind, const std::string &name)
        {
            Chip c;
            c.label = label;
            c.kind  = kind;
            c.name  = name;
            // Estimated from the font size, as every other measurement in these menus is.
            c.w = m.fontSize * (0.58f * float(label.size()) + 1.2f);
            c.x = out.empty() ? 0.0f : out.back().x + out.back().w + 4.0f;
            out.push_back(c);
            return;
        };
        add("All", 0, std::string());
        for (const std::string &b : allBanks())
            add(b, 1, b);
        if (m_library.anyUnfiled())
            add(voltaire::preset::kUnfiled, 2, std::string());

        add("+ New bank", 3, std::string());
        Chip &mk = out.back();
        mk.x = (m.w - m.rowH) - mk.w;
        // Anything that would run into it is dropped rather than drawn underneath.  Only a
        // library with a lot of banks in a narrow window ever loses one, and the picker
        // reached from a preset's bank still lists them all.
        for (size_t i = 0; i + 1 < out.size(); i ++)
            if (out[i].x + out[i].w > mk.x - m.fontSize * 0.6f)
                out[i].w = 0.0f;
        return out;
    }

    bool chipSelected(const Chip &c) const
    {
        if (c.kind == 0) return m_bankMode == BankFilter::All;
        if (c.kind == 2) return m_bankMode == BankFilter::Unfiled;
        return m_bankMode == BankFilter::Named && m_bankName == c.name;
    }

    /// The presets the browser is showing, in order.  Rebuilt rather than cached: it is a
    /// vector of pointers over a list that is already in memory, and a cache here would be
    /// one more thing that can disagree with the files.
    std::vector<const voltaire::preset::Entry *> libraryView() const
    {
        std::vector<const voltaire::preset::Entry *> out;
        for (const voltaire::preset::Entry &e : m_library.entries())
        {
            const bool keep =
                    m_bankMode == BankFilter::All ? true
                  : m_bankMode == BankFilter::Unfiled ? e.bank.empty()
                  : e.bank == m_bankName;
            if (keep)
                out.push_back(&e);
        }
        return out;
    }

    /// Back to the browser.
    ///
    /// Every one of these boxes was opened FROM the library and the next thing anybody
    /// does is in the library too -- name a bank and you want to save into it, rename or
    /// delete one and you want to see that it took.  Closing the whole menu instead means
    /// finding the PATCH button again to carry on.  Only playing a preset closes it,
    /// because that is the one that was asking the machine to do something.
    void backToLibrary()
    {
        m_menu = Menu::Patch;
        m_libraryTab = true;
        m_menuHover = -1;
        m_hoverBankZone = false;
        m_typing = Typing::None;
    }

    /// Stop offering to write back to a file the machine is no longer playing.
    void forgetLoadedPreset()
    {
        m_loadedPath.clear();
        m_loadedRecord.clear();
        m_loadedLcdName.clear();
        m_loadedBank.clear();
    }

    /// The ten-byte name the machine shows, out of a hex record.
    std::string lcdNameOf(const std::string &hex) const
    {
        using namespace voltaire::preset;
        if (hex.size() != size_t(kRecordBytes) * 2)
            return std::string();
        std::vector<uint8_t> rec(kRecordBytes);
        for (unsigned i = 0; i < kRecordBytes; i ++)
        {
            const int hi = hexNibble(hex[i * 2]), lo = hexNibble(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return std::string();
            rec[i] = uint8_t((hi << 4) | lo);
        }
        return lcdName(rec);
    }

    /// Why Override cannot be pressed, or "" when it can.
    ///
    /// Three conditions, and the button says which one is missing rather than being greyed
    /// out for reasons nobody can see:
    ///
    ///   * a library preset has to be what is playing, or there is no file to write to;
    ///   * something has to have changed, or there is nothing to write;
    ///   * the patch's own name has to be the one it was recalled with -- renaming it is
    ///     how somebody says "this is a different patch now", and quietly writing it over
    ///     the one it came from would throw away the patch they started from.
    const char *overrideBlockedBecause() const
    {
        if (m_loadedPath.empty())
            return "recall a library patch first";
        if (m_patchDump.size() != m_loadedRecord.size())
            return "waiting for the machine";
        if (lcdNameOf(m_patchDump) != m_loadedLcdName)
            return "the patch name changed -- Save Patch makes a new one";
        if (m_patchDump == m_loadedRecord && m_volume == m_loadedVolume
                && m_hf == m_loadedHf)
            return "nothing has changed yet";
        return nullptr;
    }

    bool canOverride() const { return overrideBlockedBecause() == nullptr; }

    /// Every bank worth offering: the ones patches are in, plus the ones named here.
    std::vector<std::string> allBanks() const
    {
        return voltaire::preset::mergeBanks(m_library.banks(), m_sessionBanks);
    }

    void rememberBank(const std::string &name)
    {
        if (name.empty())
            return;
        if (std::find(m_sessionBanks.begin(), m_sessionBanks.end(), name)
                == m_sessionBanks.end())
            m_sessionBanks.push_back(name);
    }

    void libraryRescan()
    {
        if (m_libraryDir.empty())
            m_libraryDir = voltaire::preset::libraryDir();
        if (m_libraryDir.empty())
        {
            m_libraryNote = "nowhere to keep presets: no HOME or XDG_DATA_HOME";
            return;
        }
        m_library.rescan(m_libraryDir);
        if (m_libScroll > int(m_library.entries().size()))
            m_libScroll = 0;

        // A bank exists only while a patch claims it, so the one being browsed can vanish
        // under us -- another instance refiled its last patch, or somebody deleted the
        // file.  Showing an empty list with no tab lit is a browser nobody can leave
        // except by guessing, so fall back to All.
        if (m_bankMode == BankFilter::Named)
        {
            const std::vector<std::string> banks = allBanks();
            if (std::find(banks.begin(), banks.end(), m_bankName) == banks.end())
            { m_bankMode = BankFilter::All; m_bankName.clear(); }
        }
    }

    /// Save what the machine is playing.
    ///
    /// Named after the patch itself, because the machine already has a name for it and
    /// asking for a second one before anything can be saved is a text field in the way of
    /// the feature.  Renaming is what the patch name editor on the DIVE common page is
    /// for, and it is worth doing BEFORE saving rather than after.
    void librarySave()
    {
        using namespace voltaire::preset;

        if (m_patchDump.size() != size_t(kRecordBytes) * 2)
        { m_libraryNote = "the machine has not said what it is playing yet"; return; }

        libraryRescan();
        if (m_libraryDir.empty())
            return;

        Preset p;
        p.record.resize(kRecordBytes);
        for (unsigned i = 0; i < kRecordBytes; i ++)
        {
            const int hi = hexNibble(m_patchDump[i * 2]);
            const int lo = hexNibble(m_patchDump[i * 2 + 1]);
            if (hi < 0 || lo < 0)
            { m_libraryNote = "the machine sent something that is not a patch"; return; }
            p.record[i] = uint8_t((hi << 4) | lo);
        }

        p.name = lcdName(p.record);
        if (p.name.empty())
            p.name = "Untitled";
        // The plugin's own settings travel with the patch and come back with it.  They are
        // not SysEx and no other U-110 will ever read them, which is exactly why they are
        // separate lines the reader may ignore rather than anything inside the record.
        p.volumeDb = m_volume;
        p.hf = m_hf;
        // Saved into the bank being browsed, which is the one thing on screen that says
        // where a new patch belongs.  "All" and "Unfiled" both mean no bank.
        if (m_bankMode == BankFilter::Named)
            p.bank = m_bankName;
        char when[32];
        const std::time_t t = std::time(nullptr);
        std::tm tm {};
       #ifdef _WIN32
        if (const std::tm *lt = std::localtime(&t)) tm = *lt;
       #else
        ::localtime_r(&t, &tm);
       #endif
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        p.created = when;

        // Never overwrite a preset that is already there: two patches may perfectly
        // reasonably be called the same thing, and losing the first one to save the
        // second is not what anybody pressing Save is asking for.
        const std::string stem = m_libraryDir + "/" + fileNameFor(p.name);
        std::string path = stem + kSuffix;
        for (int n = 2; n < 1000; n ++)
        {
            struct stat st;
            if (::stat(path.c_str(), &st) != 0)
                break;
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), " %d", n);
            path = stem + suffix + kSuffix;
        }

        std::string err;
        // Qualified: this class has a save() of its own, NanoVG's.
        if (!voltaire::preset::save(path, p, err))
        { m_libraryNote = err; return; }

        libraryRescan();
        m_libraryNote = p.bank.empty() ? ("saved \"" + p.name + "\"")
                                       : ("saved \"" + p.name + "\" into " + p.bank);
    }

    /// Play one.  The record goes into the audition slot and the firmware loads it, which
    /// is the only route that produces sound: the derived state a patch needs is built by
    /// the tone loader and by nothing else.
    void libraryLoad(const voltaire::preset::Entry &e)
    {
        using namespace voltaire::preset;

        Preset p;
        std::string err;
        if (!voltaire::preset::load(e.path, p, err))
        { m_libraryNote = err; return; }

        std::string msg;
        msg.reserve(16 + kRecordBytes * 2);
        char slot[16];
        std::snprintf(slot, sizeof(slot), "%u ", kAuditionSlot);
        msg = slot;
        msg += toHex(p.record);
        setState("patchload", msg.c_str());

        // The user asked for this preset, so they get all of it.  A DAW that is
        // automating the volume will move it again on the next block, which is what
        // automation is for and not something to design around.
        if (p.hasSettings)
        {
            m_volume = p.volumeDb;
            setParam(kParamVolume, m_volume);
            m_hf = p.hf;
            setParam(kParamHfCorrection, m_hf ? 1.0f : 0.0f);
        }

        // Warn rather than hide.  A part names a card by CATALOGUE ID, so this is
        // computable from the record and worth saying: the patch will still load, and the
        // parts on a missing card will read "Illegal CARD" on the machine's own display.
        std::string missing;
        for (const unsigned c : cardsNeeded(p.record))
        {
            bool mounted = false;
            for (const ToneGroup &g : m_toneGroups)
                if (g.media == c)
                    mounted = true;
            if (!mounted)
            {
                char n[8];
                std::snprintf(n, sizeof(n), "%02u", c);
                missing += missing.empty() ? "" : ", ";
                missing += n;
            }
        }
        // Remember where it came from, so it can be written straight back.  The record is
        // taken from the FILE rather than from the machine: the machine has not loaded it
        // yet, and comparing against what we asked for is what makes "nothing has changed"
        // true the instant after a recall.
        m_loadedPath     = e.path;
        m_loadedRecord   = toHex(p.record);
        m_loadedLcdName  = lcdName(p.record);
        m_loadedBank     = p.bank;
        m_loadedVolume   = p.hasSettings ? p.volumeDb : m_volume;
        m_loadedHf       = p.hasSettings ? p.hf : m_hf;

        m_libraryNote = missing.empty()
                ? ("loaded \"" + p.name + "\" into P-64")
                : ("loaded \"" + p.name + "\" -- needs card " + missing + ", not mounted");
    }

    /// Move one preset into a bank, or out of every bank when `bank` is empty.
    ///
    /// Read, change one line, write.  The whole of what a bank is, is that line -- so this
    /// is the whole of what filing a patch costs, and there is no index anywhere that could
    /// disagree with it afterwards.
    void libraryFile(const std::string &path, const std::string &bank)
    {
        using namespace voltaire::preset;

        Preset p;
        std::string err;
        if (!voltaire::preset::load(path, p, err))
        { m_libraryNote = err; return; }

        p.bank = cleanBankName(bank);
        if (!voltaire::preset::save(path, p, err))
        { m_libraryNote = err; return; }

        libraryRescan();
        // If the bank being shown has just lost its last patch it no longer exists, so
        // showing it would be showing an empty list nobody can leave except by guessing.
        if (m_bankMode == BankFilter::Named)
        {
            const std::vector<std::string> banks = allBanks();
            if (std::find(banks.begin(), banks.end(), m_bankName) == banks.end())
                m_bankMode = BankFilter::All;
        }
        m_libScroll = 0;
        m_libraryNote = p.bank.empty()
                ? ("\"" + p.name + "\" is unfiled")
                : ("\"" + p.name + "\" filed under " + p.bank);
    }

    /// Write the patch being edited back over the preset it was recalled from.
    ///
    /// The file keeps its name, its bank and its date: this is a new version of the same
    /// preset, not a new preset. Only the patch and the plugin's settings change.
    void libraryOverride()
    {
        using namespace voltaire::preset;

        if (!canOverride())
        { m_libraryNote = overrideBlockedBecause(); return; }
        libraryWriteOver(m_loadedPath);
    }

    /// Put the patch being edited into an existing preset file.
    ///
    /// The file keeps its name, its bank and its date: this is a new version of the same
    /// preset, not a new preset.  Only the patch and the plugin's settings change.
    void libraryWriteOver(const std::string &path)
    {
        using namespace voltaire::preset;

        if (m_patchDump.size() != size_t(kRecordBytes) * 2)
        { m_libraryNote = "the machine has not said what it is playing yet"; return; }

        Preset p;
        std::string err;
        if (!voltaire::preset::load(path, p, err))
        { m_libraryNote = err; return; }

        p.record.resize(kRecordBytes);
        for (unsigned i = 0; i < kRecordBytes; i ++)
        {
            const int hi = hexNibble(m_patchDump[i * 2]);
            const int lo = hexNibble(m_patchDump[i * 2 + 1]);
            if (hi < 0 || lo < 0)
            { m_libraryNote = "the machine sent something that is not a patch"; return; }
            p.record[i] = uint8_t((hi << 4) | lo);
        }
        p.volumeDb = m_volume;
        p.hf = m_hf;

        if (!voltaire::preset::save(path, p, err))
        { m_libraryNote = err; return; }

        // If that was the preset being played it now matches what is on disk again, so
        // Override goes quiet until something changes rather than offering to repeat it.
        if (path == m_loadedPath)
        {
            m_loadedRecord  = m_patchDump;
            m_loadedLcdName = lcdName(p.record);
            m_loadedVolume  = m_volume;
            m_loadedHf      = m_hf;
        }
        libraryRescan();
        m_libraryNote = "\"" + p.name + "\" written over";
    }

    /// Rename one.
    ///
    /// The FILE is renamed to match, so that a library browsed in a file manager reads the
    /// same as one browsed here -- and never over another preset, which is what uniquePath
    /// is for.  What is NOT touched is the ten-byte name inside the patch: that is the
    /// machine's own field, it is what the LCD shows, and silently rewriting patch bytes
    /// because somebody renamed a file would make the preset no longer the patch that was
    /// captured.  The DIVE common page edits that one.
    void libraryRename(const std::string &path, const std::string &want)
    {
        using namespace voltaire::preset;

        const std::string name = cleanPresetName(want);   // one printable line, trimmed
        if (name.empty())
            return;

        Preset p;
        std::string err;
        if (!voltaire::preset::load(path, p, err))
        { m_libraryNote = err; return; }

        p.name = name;
        const std::string dest = uniquePath(m_libraryDir, name, path);
        if (!voltaire::preset::save(dest, p, err))
        { m_libraryNote = err; return; }
        if (dest != path && !erase(path, err))
        {
            // The new one is already written, so the library is not lost -- but say so
            // rather than leaving two copies about without a word.
            m_libraryNote = "renamed, but the old file is still there: " + err;
            libraryRescan();
            return;
        }
        if (m_loadedPath == path)
            m_loadedPath = dest;              // same patch, new file: still writable back to
        libraryRescan();
        m_libraryNote = "renamed to \"" + name + "\"";
    }

    void libraryDelete(const std::string &path, const std::string &name)
    {
        std::string err;
        if (!voltaire::preset::erase(path, err))
        { m_libraryNote = err; return; }
        if (m_loadedPath == path)
            forgetLoadedPreset();             // there is nothing to write back to now
        libraryRescan();
        if (m_libScroll > 0 && m_libScroll >= int(libraryView().size()))
            m_libScroll = 0;
        m_libraryNote = "deleted \"" + name + "\"";
    }

    /// The two halves of the PATCH menu, as tabs in its header.  0 is the machine's own
    /// 64, 1 is the library; -1 is anywhere else.
    /// Whichever of the two lists is showing, since the library's header is taller.
    MenuLayout currentPatchLayout() const
    {
        return m_libraryTab ? libraryLayout() : patchLayout();
    }

    static constexpr const char *kPatchTabs[2] = { "INTERNAL 64", "LIBRARY" };

    /// Where the two tabs are.  ONE definition, shared by the drawing and the hit test.
    ///
    /// Sized to their text rather than given a fixed nine ems each: two buttons went in
    /// beside them and eighteen ems of tab left the header 6.9 ems over the width of the
    /// box, so the buttons were drawn on top of the tabs.
    void patchTabRect(const MenuLayout &m, int i, HeaderRect &r) const
    {
        r.y = m.y + m.rowH * 0.2f;
        r.h = m.rowH;
        r.x = m.x + m.rowH * 0.5f;
        for (int k = 0; k < 2; k ++)
        {
            const float w = m.fontSize * (0.58f * float(std::strlen(kPatchTabs[k])) + 1.6f);
            if (k == i)
            { r.w = w; return; }
            r.x += w + m.fontSize * 0.4f;
        }
        r.w = 0.0f;
    }

    int patchTabHit(float px, float py) const
    {
        const MenuLayout m = currentPatchLayout();
        for (int i = 0; i < 2; i ++)
        {
            HeaderRect r;
            patchTabRect(m, i, r);
            if (inRect(r, px, py))
                return i;
        }
        return -1;
    }

    /// Which bank chip is under the pointer, as an index into bankChips(), or -1.
    int bankChipHit(float px, float py) const
    {
        if (!m_libraryTab)
            return -1;
        const MenuLayout m = libraryLayout();
        if (py < m.y + m.rowH * 1.7f || py >= m.y + m.rowH * 2.65f)
            return -1;
        const std::vector<Chip> chips = bankChips(m);
        const float x0 = m.x + m.rowH * 0.5f;
        for (size_t i = 0; i < chips.size(); i ++)
            if (chips[i].w > 0.0f
                    && px >= x0 + chips[i].x && px < x0 + chips[i].x + chips[i].w)
                return int(i);
        return -1;
    }

    void drawPatchTabs(const MenuLayout &m)
    {
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        for (int i = 0; i < 2; i ++)
        {
            HeaderRect r;
            patchTabRect(m, i, r);
            const bool on = (i == 1) == m_libraryTab;
            beginPath();
            roundedRect(r.x, r.y, r.w, r.h, 3.0f);
            fillColor(on ? Color(56, 60, 68) : Color(32, 34, 38));
            fill();
            fillColor(on ? Color(235, 235, 235) : Color(140, 146, 156));
            text(r.x + r.w * 0.5f, r.y + r.h * 0.5f, kPatchTabs[i], nullptr);
        }
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    }

    void drawLibraryMenu()
    {
        const MenuLayout m = libraryLayout();

        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.55f));
        fill();

        beginPath();
        roundedRect(m.x, m.y, m.w, m.h, m.rowH * 0.35f);
        fillColor(Color(26, 28, 32));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(m.x, m.y, m.w, m.rowH);

        fontFace(m_font);
        fontSize(m.fontSize * 1.05f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        drawPatchTabs(m);

        const std::vector<const voltaire::preset::Entry *> view = libraryView();

        // ---- the two buttons.
        const HeaderRect sb = saveButtonRect(m);
        const HeaderRect ob = overrideButtonRect(m);
        headerButton(sb, m, saveButtonLabel().c_str(), m_menuHover == kHoverSave, true);
        headerButton(ob, m, kOverrideLabel, m_menuHover == kHoverOverride, canOverride());

        // ---- what just happened, under the Save button and out of the tabs' way.
        char right[192];
        if (m_menuHover == kHoverOverride && !canOverride())
            std::snprintf(right, sizeof(right), "Override Patch: %s",
                          overrideBlockedBecause());
        else if (!m_libraryNote.empty())
            std::snprintf(right, sizeof(right), "%s", m_libraryNote.c_str());
        else if (m_library.entries().empty())
            std::snprintf(right, sizeof(right),
                          "nothing saved yet -- Save Patch puts one here");
        else if (m_writeMode)
            std::snprintf(right, sizeof(right),
                          "%d of %d presets   |   click one to write the patch over it",
                          int(view.size()), int(m_library.entries().size()));
        else if (m_bankMode == BankFilter::Named)
            std::snprintf(right, sizeof(right),
                          "%d of %d presets   |   Save Patch adds to %s",
                          int(view.size()), int(m_library.entries().size()),
                          m_bankName.c_str());
        else
            std::snprintf(right, sizeof(right),
                          "%d of %d presets   |   click to play   |   "
                          "right-click to rename, move or delete",
                          int(view.size()), int(m_library.entries().size()));
        fontSize(m.fontSize * 0.72f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(Color(150, 155, 165));
        text(m.x + m.rowH * 0.5f, m.y + m.h - m.rowH * 0.7f, right, nullptr);

        // ---- the bank row.
        const std::vector<Chip> chips = bankChips(m);
        const float chipY = m.y + m.rowH * 1.7f;
        const float chipX = m.x + m.rowH * 0.5f;
        fontSize(m.fontSize * 0.85f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        for (const Chip &c : chips)
        {
            if (c.w <= 0.0f)
                continue;                   // dropped: it would have run into "+ New bank"
            const bool on = chipSelected(c);
            beginPath();
            roundedRect(chipX + c.x, chipY, c.w, m.rowH * 0.95f, m.rowH * 0.3f);
            fillColor(on ? Color(0, 163, 224, 0.45f) : Color(34, 36, 42));
            fill();
            strokeColor(on ? Color(120, 190, 230) : Color(70, 74, 82));
            strokeWidth(1.0f);
            stroke();
            fillColor(on ? Color(235, 240, 245) : Color(165, 170, 180));
            text(chipX + c.x + m.fontSize * 0.6f, chipY + m.rowH * 0.48f,
                 c.label.c_str(), nullptr);
        }

        // ---- the presets: every cell is one, and each shows the bank it is filed in.
        for (int i = 0; i < kLibCells; i ++)
        {
            const int idx = m_libScroll + i;
            if (size_t(idx) >= view.size())
                break;

            float x, y;
            libraryCellRect(m, i, x, y);
            const float bankX = x + m.colW * (1.0f - kBankZone);

            if (i == m_menuHover)
            {
                beginPath();
                roundedRect(x, y + 1.0f, m.colW - 2.0f, m.rowH - 2.0f, 2.0f);
                fillColor(Color(56, 60, 68));
                fill();
                // The two halves do different things, so the pointer is told which one it
                // is over rather than being left to find out by clicking.
                beginPath();
                roundedRect(m_hoverBankZone ? bankX : x, y + 1.0f,
                            m_hoverBankZone ? (x + m.colW - 2.0f - bankX)
                                            : (bankX - x),
                            m.rowH - 2.0f, 2.0f);
                fillColor(Color(80, 86, 96));
                fill();
            }

            fontSize(m.fontSize);
            textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
            fillColor(Color(214, 216, 220));
            text(x + m.fontSize * 0.4f, y + m.rowH * 0.5f,
                 view[size_t(idx)]->name.c_str(), nullptr);

            fontSize(m.fontSize * 0.78f);
            textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
            const bool filed = !view[size_t(idx)]->bank.empty();
            fillColor(filed ? Color(150, 175, 195) : Color(96, 100, 108));
            text(x + m.colW - m.fontSize * 0.5f, y + m.rowH * 0.5f,
                 filed ? view[size_t(idx)]->bank.c_str() : "--", nullptr);
        }
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    }

    // ---- filing a preset into a bank ---------------------------------------------------
    //
    // The rows are: every bank in use, then Unfiled, then a line to type a new name on.
    // Nothing has to be created before it can be used -- a bank exists because a patch
    // claims it -- so "new bank" is one field and not a dialogue about making one.

    struct BankPickLayout { float x, y, w, h, rowH, top; int rows; };

    /// One list box: a heading and `rows` entries, centred, shrunk to fit a short window.
    /// Shared so that every box in the library reads the same and hit-tests the same.
    BankPickLayout listLayout(int rows) const
    {
        BankPickLayout L;
        L.rowH = overlayRowH();
        L.rows = rows;
        L.w = L.rowH * 22.0f;
        const float maxW = float(getWidth()) - 16.0f;
        if (L.w > maxW)
            L.w = maxW;
        L.h = L.rowH * (2.2f + float(L.rows) * 1.25f + 0.8f);
        const float room = float(getHeight()) - 16.0f;
        if (L.h > room && L.rows > 0)
        {
            L.rowH *= room / L.h;
            L.h = room;
        }
        L.x = std::floor((float(getWidth()) - L.w) * 0.5f);
        L.y = std::floor((float(getHeight()) - L.h) * 0.5f);
        if (L.y < 8.0f)
            L.y = 8.0f;
        L.top = L.y + L.rowH * 2.2f;
        return L;
    }

    /// Which row of a list box is under the pointer, or -1.  Takes the layout so that the
    /// drawing and the hit test cannot be looking at two different boxes.
    static int listHit(const BankPickLayout &L, float px, float py)
    {
        if (px < L.x || px > L.x + L.w || py < L.top)
            return -1;
        const int i = int((py - L.top) / (L.rowH * 1.25f));
        return i >= 0 && i < L.rows ? i : -1;
    }

    // ---- the way out of an overlay ----------------------------------------------------
    //
    // Clicking in the dark closes these, and so does Escape, but neither is visible and
    // somebody meeting the panel for the first time has no reason to guess either.  So
    // every palette gets an X in its corner.
    //
    // The rectangle is REMEMBERED AS IT IS DRAWN rather than computed twice.  Two of these
    // boxes work out their geometry inside the draw itself, and a hit test that recomputed
    // it would be a second expression for one rectangle -- which is the mistake that put
    // the library's grid and its clicks a row and a half apart.

    static constexpr float kCloseSize = 0.95f;      ///< of a row

    void drawCloseButton(float bx, float by, float bw, float rowH)
    {
        HeaderRect r;
        r.h = rowH * kCloseSize;
        r.w = r.h;
        r.x = bx + bw - r.w - rowH * 0.35f;
        r.y = by + rowH * 0.28f;
        m_closeRect = r;
        m_closeFor  = m_menu;

        const bool hot = m_hoverClose;
        beginPath();
        roundedRect(r.x, r.y, r.w, r.h, r.h * 0.3f);
        fillColor(hot ? Color(150, 60, 56) : Color(44, 46, 52));
        fill();
        strokeColor(hot ? Color(230, 150, 140) : Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        const float p = r.h * 0.32f;
        beginPath();
        moveTo(r.x + p, r.y + p);
        lineTo(r.x + r.w - p, r.y + r.h - p);
        moveTo(r.x + r.w - p, r.y + p);
        lineTo(r.x + p, r.y + r.h - p);
        strokeColor(hot ? Color(255, 225, 220) : Color(190, 195, 205));
        strokeWidth(std::max(1.0f, r.h * 0.11f));
        stroke();
    }

    bool closeButtonHit(float px, float py) const
    {
        return m_closeFor == m_menu && m_closeRect.w > 0.0f
                && inRect(m_closeRect, px, py);
    }

    /// What Escape does, so the X does the same thing rather than a second version of it.
    void closeCurrentMenu()
    {
        if (m_menu == Menu::BankPick || m_menu == Menu::PresetActions
                || m_menu == Menu::DeleteConfirm || m_menu == Menu::OverwriteConfirm)
            backToLibrary();
        else
        {
            m_menu = Menu::None;
            m_menuHover = -1;
            m_typing = Typing::None;
        }
    }

    /// The OK button at the right-hand end of a text field's row.
    ///
    /// A button, because a field that is finished by clicking its own text is not how text
    /// fields work anywhere else -- somebody clicking into the middle of what they typed to
    /// correct it would commit instead.  It exists at all because Enter cannot be relied
    /// on: the host sees the key first and a DAW that binds Return never passes it on.
    HeaderRect fieldOkRect(const BankPickLayout &L, int row) const
    {
        HeaderRect r;
        r.h = L.rowH * 0.95f;
        r.w = L.rowH * 2.4f;
        r.x = L.x + L.w - L.rowH * 0.6f - r.w;
        r.y = L.top + float(row) * L.rowH * 1.25f + (L.rowH * 1.15f - r.h) * 0.5f;
        return r;
    }

    void drawFieldOk(const BankPickLayout &L, int row, bool hot)
    {
        const HeaderRect r = fieldOkRect(L, row);
        beginPath();
        roundedRect(r.x, r.y, r.w, r.h, r.h * 0.28f);
        fillColor(hot ? Color(52, 116, 60) : Color(40, 96, 46));
        fill();
        strokeColor(Color(120, 200, 130));
        strokeWidth(1.0f);
        stroke();
        fontSize(L.rowH * 0.75f);
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        fillColor(Color(225, 255, 225));
        text(r.x + r.w * 0.5f, r.y + r.h * 0.5f, "OK", nullptr);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    }

    /// One row of a list box, drawn the same way everywhere.
    void listRow(const BankPickLayout &L, int i, bool hot, const char *label,
                 const Color &c)
    {
        const float y = L.top + float(i) * L.rowH * 1.25f;
        if (hot)
        {
            beginPath();
            roundedRect(L.x + L.rowH * 0.4f, y, L.w - L.rowH * 0.8f,
                        L.rowH * 1.15f, L.rowH * 0.25f);
            fillColor(Color(0, 163, 224, 0.30f));
            fill();
        }
        fontSize(L.rowH * 0.85f);
        fillColor(c);
        text(L.x + L.rowH * 0.9f, y + L.rowH * 0.6f, label, nullptr);
    }

    /// The frame every list box sits in, with its heading.
    void listBox(const BankPickLayout &L, const char *heading, const char *footer)
    {
        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.72f));
        fill();

        beginPath();
        roundedRect(L.x, L.y, L.w, L.h, L.rowH * 0.4f);
        fillColor(Color(22, 24, 28));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        fontFace(m_font);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fontSize(L.rowH * 0.9f);
        fillColor(Color(255, 180, 90));
        text(L.x + L.rowH * 0.7f, L.y + L.rowH * 1.1f, heading, nullptr);

        fontSize(L.rowH * 0.7f);
        fillColor(Color(140, 146, 156));
        text(L.x + L.rowH * 0.7f, L.y + L.h - L.rowH * 0.6f, footer, nullptr);

        drawCloseButton(L.x, L.y, L.w, L.rowH);
    }

    BankPickLayout bankPickLayout() const
    {
        // banks + Unfiled + New bank.  allBanks(), so a preset can be filed into one that
        // was named a moment ago and has nothing in it yet -- which is the whole reason
        // for making a bank before there is anything to put in it.
        return listLayout(int(allBanks().size()) + 2);
    }

    int bankPickHit(float px, float py) const
    {
        return listHit(bankPickLayout(), px, py);
    }

    // ---- what right-clicking a preset offers ------------------------------------------
    //
    // Three things can be done to a preset that are not "play it", and they are on one
    // menu because they are the same kind of thing: they change the file rather than the
    // machine.  Delete asks again, because there is no undo and no wastebasket.

    enum { kActRename = 0, kActMoveBank, kActDelete, kPresetActionCount };

    BankPickLayout presetActionsLayout() const { return listLayout(kPresetActionCount); }

    void drawPresetActions()
    {
        const BankPickLayout L = presetActionsLayout();
        char head[160];
        std::snprintf(head, sizeof(head), "\"%s\"", m_presetTargetName.c_str());
        listBox(L, head, m_typing == Typing::RenamePreset
                ? "type a name, then OK   |   Escape cancels"
                : "these change the file, not the machine   |   Escape closes this");

        char line[96];
        if (m_typing == Typing::RenamePreset)
        {
            std::snprintf(line, sizeof(line), "Name:  %s%s", m_typeBuf,
                          m_caretOn ? "_" : " ");
            listRow(L, kActRename, false, line, Color(190, 255, 190));
            drawFieldOk(L, kActRename, m_hoverFieldOk);
        }
        else
            listRow(L, kActRename, m_menuHover == kActRename, "Rename...",
                    Color(225, 228, 232));

        const std::string bank = m_presetTargetBank.empty()
                ? std::string("no bank") : m_presetTargetBank;
        std::snprintf(line, sizeof(line), "Move to bank...   (now: %s)", bank.c_str());
        listRow(L, kActMoveBank, m_menuHover == kActMoveBank, line, Color(225, 228, 232));

        listRow(L, kActDelete, m_menuHover == kActDelete, "Delete", Color(255, 150, 120));
    }

    // ---- and confirming the one that cannot be undone ----------------------------------

    BankPickLayout deleteConfirmLayout() const { return listLayout(2); }

    void drawOverwriteConfirm()
    {
        const BankPickLayout L = deleteConfirmLayout();
        char head[160];
        std::snprintf(head, sizeof(head), "WRITE OVER \"%s\"?", m_presetTargetName.c_str());
        listBox(L, head, "it keeps its name and its bank; the patch in it is replaced");
        listRow(L, 0, m_menuHover == 0, "Write over it", Color(255, 150, 120));
        listRow(L, 1, m_menuHover == 1, "Keep it as it is", Color(225, 228, 232));
    }

    void drawDeleteConfirm()
    {
        const BankPickLayout L = deleteConfirmLayout();
        char head[160];
        std::snprintf(head, sizeof(head), "DELETE \"%s\"?", m_presetTargetName.c_str());
        listBox(L, head, "the file goes for good -- there is no wastebasket");
        listRow(L, 0, m_menuHover == 0, "Delete it", Color(255, 150, 120));
        listRow(L, 1, m_menuHover == 1, "Keep it", Color(225, 228, 232));
    }

    /// What row `i` means: a bank name, "" for Unfiled, or the new-name field.
    /// The box does two jobs, told apart by whether a preset was named when it opened.
    /// With one, choosing a bank FILES it; without one -- the "+ New bank" chip -- choosing
    /// a bank just selects it to browse and to save into.
    bool bankPickIsFiling() const { return !m_presetTargetPath.empty(); }

    void bankSelect(const std::string &name)
    {
        m_bankMode = name.empty() ? BankFilter::Unfiled : BankFilter::Named;
        m_bankName = name;
        rememberBank(name);
        m_libScroll = 0;
    }

    void bankPickChoose(int i)
    {
        const std::vector<std::string> banks = allBanks();
        if (i < 0)
            return;
        if (size_t(i) < banks.size())
        {
            if (bankPickIsFiling())
                libraryFile(m_presetTargetPath, banks[size_t(i)]);
            else
                bankSelect(banks[size_t(i)]);
            backToLibrary();
        }
        else if (size_t(i) == banks.size())
        {
            if (bankPickIsFiling())
                libraryFile(m_presetTargetPath, std::string());
            else
                bankSelect(std::string());
            backToLibrary();
        }
        else if (m_typing != Typing::NewBank)
        {
            // The row arms the field; the OK button beside it is what finishes it.
            m_typing = Typing::NewBank;
            showCaret();
        }
    }

    /// Finish whatever is being typed.
    ///
    /// Reachable BY MOUSE as well as by Enter, and that is not a convenience.  A plugin UI
    /// does not reliably get the Return key: the host sees it first, and a DAW that binds
    /// Return to something of its own -- Ardour moves the playhead with it -- simply never
    /// passes it on.  Ordinary characters are not bound and do arrive, so a field that
    /// takes typing but can only be committed with Enter is a field that fills up and then
    /// does nothing.  Clicking the field itself finishes it.
    void commitTyping()
    {
        if (m_typing == Typing::RenamePreset)
        {
            libraryRename(m_presetTargetPath, m_typeBuf);
            backToLibrary();
        }
        else if (m_typing == Typing::NewBank)
            commitNewBank();
    }

    void commitNewBank()
    {
        const std::string name = voltaire::preset::cleanBankName(m_typeBuf);
        m_typing = Typing::None;
        if (name.empty())
        { backToLibrary(); return; }

        if (bankPickIsFiling())
            libraryFile(m_presetTargetPath, name);
        // Either way the new bank becomes the one being browsed, since that is almost
        // certainly what is wanted next -- and when nothing was filed into it, being
        // browsed is the only thing keeping it alive until a patch is saved there.
        bankSelect(name);
        m_libraryNote = bankPickIsFiling()
                ? m_libraryNote
                : ("bank \"" + name + "\" is empty -- save or move a patch into it");
        backToLibrary();
    }

    void drawBankPick()
    {
        const BankPickLayout L = bankPickLayout();
        const std::vector<std::string> banks = allBanks();

        char head[160];
        if (bankPickIsFiling())
            std::snprintf(head, sizeof(head), "FILE \"%s\" UNDER",
                          m_presetTargetName.c_str());
        else
            std::snprintf(head, sizeof(head), "CHOOSE A BANK");
        listBox(L, head, m_typing == Typing::NewBank
                ? "type a name, then OK   |   Escape cancels"
                : "a bank is just a name a patch claims   |   Escape closes this");

        for (int i = 0; i < L.rows; i ++)
        {
            if (size_t(i) < banks.size())
            {
                // An empty one is dimmer and says so, since it is the one thing here that
                // will not survive the window being closed.
                const std::vector<std::string> onDisk = m_library.banks();
                const bool real = std::find(onDisk.begin(), onDisk.end(),
                                            banks[size_t(i)]) != onDisk.end();
                char line[96];
                std::snprintf(line, sizeof(line), real ? "%s" : "%s   (empty)",
                              banks[size_t(i)].c_str());
                listRow(L, i, i == m_menuHover, line,
                        real ? Color(225, 228, 232) : Color(168, 174, 184));
            }
            else if (size_t(i) == banks.size())
                listRow(L, i, i == m_menuHover,
                        bankPickIsFiling() ? "Unfiled -- in no bank at all"
                                           : "Unfiled -- the ones in no bank",
                        Color(168, 174, 184));
            else
            {
                char line[96];
                if (m_typing == Typing::NewBank)
                    std::snprintf(line, sizeof(line), "New bank:  %s%s", m_typeBuf,
                                  m_caretOn ? "_" : " ");
                else
                    std::snprintf(line, sizeof(line), "New bank...");
                listRow(L, i, i == m_menuHover && m_typing != Typing::NewBank, line,
                        m_typing == Typing::NewBank ? Color(190, 255, 190)
                                                    : Color(168, 174, 184));
                if (m_typing == Typing::NewBank)
                    drawFieldOk(L, i, m_hoverFieldOk);
            }
        }
    }

    void drawPatchMenu()
    {
        const MenuLayout m = patchLayout();

        // The panel behind is dimmed, so the list reads as being in front of the
        // instrument rather than painted onto it.
        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.55f));
        fill();

        beginPath();
        roundedRect(m.x, m.y, m.w, m.h, m.rowH * 0.35f);
        fillColor(Color(26, 28, 32));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(m.x, m.y, m.w, m.rowH);

        fontFace(m_font);
        fontSize(m.fontSize * 1.05f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        drawPatchTabs(m);

        fontSize(m.fontSize * 0.72f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(Color(150, 155, 165));
        text(m.x + m.rowH * 0.5f, m.y + m.h - m.rowH * 0.7f,
             m_patchNames.empty() ? "waiting for the machine"
             : m_writeMode ? "click a slot to store the patch being edited there"
                           : "the machine's own bank -- click one to play it", nullptr);

        if (m_patchNames.empty())
            return;

        fontSize(m.fontSize);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        char label[48];
        for (int i = 0; i < kMenuCols * kMenuRows && size_t(i) < m_patchNames.size(); i ++)
        {
            const float x = m.x + m.rowH * 0.5f + float(i / kMenuRows) * m.colW;
            const float y = m.y + m.headerH + float(i % kMenuRows) * m.rowH;
            const bool current = i == int(m_patch);

            if (current || i == m_menuHover)
            {
                beginPath();
                roundedRect(x, y + 1.0f, m.colW - 2.0f, m.rowH - 2.0f, 2.0f);
                // The patch the machine is on gets the LCD's green; the pointer is grey.
                fillColor(current ? Color(40, 96, 46) : Color(56, 60, 68));
                fill();
            }
            std::snprintf(label, sizeof(label), "%02d %s", i + 1, m_patchNames[size_t(i)].c_str());
            fillColor(current ? Color(190, 255, 190) : Color(214, 216, 220));
            text(x + m.fontSize * 0.4f, y + m.rowH * 0.5f, label, nullptr);
        }
    }

    // ---- the TONE menu ---------------------------------------------------------------
    //
    // A patch has six PARTS and each part names one TONE, so this menu is two choices:
    // which part, then which tone.  The part tabs across the top show what each part is
    // playing now, which is most of what anyone opens this for.
    //
    // Tones live on media -- the internal wave ROM, or a card -- and a part names its card
    // by CATALOGUE ID rather than by slot (ROM-ANALYSIS.md section 6.7), which is why the
    // second row of tabs is the card list and why picking one sends its media number
    // rather than a slot.
    //
    // 99 internal tones do not fit a sixteen-row column the way 64 patches did, so the
    // grid is thirteen rows of eight, filled column-major, and it stays that size when a
    // card with sixteen tones is showing.  A layout that resized per card would move the
    // tabs out from under the pointer that just clicked one.

    static constexpr int kToneRows = 13;
    static constexpr int kToneCols = 8;

    struct ToneHit
    {
        enum Kind { None = 0, Part, Group, Tone };
        int kind = None;
        int index = -1;
    };

    struct ToneLayout
    {
        float x, y, w, h, rowH, colW, headerH, tabH, fontSize;
        float partsY, groupsY, gridY;
    };

    ToneLayout toneLayout() const
    {
        ToneLayout m;
        m.rowH = gridRowH(float(kToneRows + 5), float(kToneCols) * kNameColEms, 24.0f);
        m.fontSize = m.rowH * kMenuFontOfRow;
        m.colW = m.fontSize * kNameColEms;
        m.headerH = m.rowH * 1.6f;
        m.tabH = m.rowH * 1.3f;
        m.w = kToneCols * m.colW + m.rowH;
        m.h = m.headerH + m.tabH * 2.0f + kToneRows * m.rowH + m.rowH * 0.5f;
        m.x = std::floor((float(getWidth()) - m.w) * 0.5f);
        m.y = std::floor((float(getHeight()) - m.h) * 0.5f);
        m.partsY = m.y + m.headerH;
        m.groupsY = m.partsY + m.tabH;
        m.gridY = m.groupsY + m.tabH;
        return m;
    }

    /// Where each media tab starts, so drawing and hit testing cannot disagree.
    ///
    /// The width is estimated from the character count rather than measured: measuring
    /// needs the font set on the context, which the hit test has no business doing, and
    /// two different answers here would put the tabs somewhere other than where they were
    /// drawn.  0.66 em is about DejaVu's advance for the capitals and digits card names
    /// are made of.
    float groupTabX(const ToneLayout &m, size_t g) const
    {
        float x = m.x + m.rowH * 0.5f;
        for (size_t i = 0; i < g && i < m_toneGroups.size(); i ++)
            x += m.fontSize * 0.66f * float(m_toneGroups[i].label.size() + 3);
        return x;
    }

    ToneHit toneHit(float px, float py) const
    {
        const ToneLayout m = toneLayout();
        ToneHit hit;
        if (px < m.x || px > m.x + m.w)
            return hit;

        if (py >= m.partsY && py < m.groupsY)
        {
            const int i = int((px - m.x) / (m.w / 6.0f));
            if (i >= 0 && i < 6) { hit.kind = ToneHit::Part; hit.index = i; }
            return hit;
        }
        if (py >= m.groupsY && py < m.gridY)
        {
            for (size_t g = 0; g < m_toneGroups.size(); g ++)
                if (px >= groupTabX(m, g) && px < groupTabX(m, g + 1))
                { hit.kind = ToneHit::Group; hit.index = int(g); break; }
            return hit;
        }
        if (py >= m.gridY && m_toneGroup < int(m_toneGroups.size()))
        {
            const int col = int((px - (m.x + m.rowH * 0.5f)) / m.colW);
            const int row = int((py - m.gridY) / m.rowH);
            if (col < 0 || col >= kToneCols || row < 0 || row >= kToneRows)
                return hit;
            const int i = col * kToneRows + row;
            if (size_t(i) < m_toneGroups[size_t(m_toneGroup)].names.size())
            { hit.kind = ToneHit::Tone; hit.index = i; }
        }
        return hit;
    }

    /// What a part is playing, by name, or an empty string if its media is not mounted.
    std::string partToneName(int part) const
    {
        for (const ToneGroup &g : m_toneGroups)
            if (g.media == m_partMedia[part] && m_partTone[part] < g.names.size())
                return g.names[m_partTone[part]];
        return std::string();
    }

    /// A part whose flags read 0xCn is switched off in this patch and will not sound,
    /// whatever tone it names.  ROM-ANALYSIS.md section 4.
    bool partIsOff(int part) const { return (m_partFlags[part] & 0xE0) == 0xC0; }

    void drawToneMenu()
    {
        const ToneLayout m = toneLayout();

        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(0, 0, 0, 0.55f));
        fill();

        beginPath();
        roundedRect(m.x, m.y, m.w, m.h, m.rowH * 0.35f);
        fillColor(Color(26, 28, 32));
        fill();
        strokeColor(Color(96, 102, 112));
        strokeWidth(1.0f);
        stroke();

        drawCloseButton(m.x, m.y, m.w, m.rowH);

        fontFace(m_font);
        fontSize(m.fontSize * 1.05f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(Color(235, 235, 235));
        text(m.x + m.rowH * 0.5f, m.y + m.headerH * 0.5f, "TONE", nullptr);

        char buf[96];
        fontSize(m.fontSize * 0.8f);
        textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
        fillColor(Color(150, 155, 165));
        if (m_toneGroups.empty())
            std::snprintf(buf, sizeof(buf), "waiting for the machine");
        else
            std::snprintf(buf, sizeof(buf), "part %d  |  MIDI channel %u%s", m_tonePart + 1,
                          unsigned(m_partChan[m_tonePart] & 0x0f) + 1,
                          partIsOff(m_tonePart) ? "  |  part is off in this patch" : "");
        text(m.x + m.w - m.rowH * 0.5f, m.y + m.headerH * 0.5f, buf, nullptr);
        if (m_toneGroups.empty())
            return;

        // The six parts, with what each is playing.
        fontSize(m.fontSize * 0.92f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        for (int p = 0; p < 6; p ++)
        {
            const float tw = m.w / 6.0f;
            const float tx = m.x + tw * float(p);
            const bool sel = p == m_tonePart;
            if (sel || (m_toneHover.kind == ToneHit::Part && m_toneHover.index == p))
            {
                beginPath();
                roundedRect(tx + 1.0f, m.partsY + 1.0f, tw - 2.0f, m.tabH - 2.0f, 2.0f);
                fillColor(sel ? Color(58, 64, 76) : Color(40, 44, 52));
                fill();
            }
            const std::string name = partToneName(p);
            std::snprintf(buf, sizeof(buf), "%d %s", p + 1,
                          name.empty() ? "--" : name.c_str());
            // An off part is drawn dim: its tone is real, it just will not sound.
            fillColor(partIsOff(p) ? Color(120, 124, 132)
                                   : (sel ? Color(235, 235, 235) : Color(198, 202, 208)));
            text(tx + m.fontSize * 0.5f, m.partsY + m.tabH * 0.5f, buf, nullptr);
        }

        // The media: internal first, then a tab per mounted card.
        for (size_t g = 0; g < m_toneGroups.size(); g ++)
        {
            const float tx = groupTabX(m, g), tw = groupTabX(m, g + 1) - tx;
            const bool sel = int(g) == m_toneGroup;
            if (sel || (m_toneHover.kind == ToneHit::Group && m_toneHover.index == int(g)))
            {
                beginPath();
                roundedRect(tx, m.groupsY + 1.0f, tw - 4.0f, m.tabH - 2.0f, 2.0f);
                fillColor(sel ? Color(40, 96, 46) : Color(40, 44, 52));
                fill();
            }
            fillColor(sel ? Color(210, 255, 210) : Color(190, 194, 202));
            text(tx + m.fontSize * 0.6f, m.groupsY + m.tabH * 0.5f,
                 m_toneGroups[g].label.c_str(), nullptr);
        }

        // The tones themselves.
        const ToneGroup &grp = m_toneGroups[size_t(m_toneGroup)];
        const bool onThisMedia = grp.media == m_partMedia[m_tonePart];
        fontSize(m.fontSize);
        for (size_t i = 0; i < grp.names.size() && i < size_t(kToneRows * kToneCols); i ++)
        {
            const float x = m.x + m.rowH * 0.5f + float(int(i) / kToneRows) * m.colW;
            const float y = m.gridY + float(int(i) % kToneRows) * m.rowH;
            const bool current = onThisMedia && i == m_partTone[m_tonePart];
            const bool hover = m_toneHover.kind == ToneHit::Tone
                    && m_toneHover.index == int(i);
            if (current || hover)
            {
                beginPath();
                roundedRect(x, y + 1.0f, m.colW - 2.0f, m.rowH - 2.0f, 2.0f);
                fillColor(current ? Color(40, 96, 46) : Color(56, 60, 68));
                fill();
            }
            std::snprintf(buf, sizeof(buf), "%02d %s", int(i) + 1, grp.names[i].c_str());
            fillColor(current ? Color(190, 255, 190) : Color(214, 216, 220));
            text(x + m.fontSize * 0.4f, y + m.rowH * 0.5f, buf, nullptr);
        }
    }

    // ---- the DIVE drawer ------------------------------------------------------------
    //
    // Named for Sound Diver, and for what it does: the panel slides down to expose the
    // parameters the machine itself only reaches through its EDIT menus.  The drawer is
    // part of the same artwork as the panel, sitting below it in the same coordinate
    // space, so nothing here needs to know where anything is -- only which of the three
    // window heights is in force and which page is on show.

    /// Case-insensitive compare, spelled out rather than reached for: strcasecmp is
    /// POSIX and _stricmp is MSVC, and this is two lines.
    static bool sameName(const char *a, const char *b)
    {
        for (; *a && *b; a ++, b ++)
            if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
                return false;
        return *a == *b;
    }

    static bool tabIsPart(int t)
    {
        return t >= voltaire::panel::TAB_P1 && t <= voltaire::panel::TAB_P6;
    }

    /// Which of the six parts row 1 is pointing at, or -1 for SET and COMMON.
    int divePart() const
    {
        return tabIsPart(m_diveTab1) ? m_diveTab1 - voltaire::panel::TAB_P1 : -1;
    }

    /// The second row only means anything for a part; SET and COMMON are whole pages.
    bool row2Visible() const { return tabIsPart(m_diveTab1); }

    /// Tab -> page, matched on the name both tables carry.
    ///
    /// The alternative would be a switch mapping TAB_LEVEL to DIVE_LEVEL and so on,
    /// which is a second place to edit every time a tab is added in Inkscape.  Here
    /// the artwork's own naming is the mapping, and a tab whose name matches no page
    /// simply shows nothing rather than showing the wrong thing.
    int divePage() const
    {
        const char *want = voltaire::panel::kDiveTabName[
                row2Visible() ? m_diveTab2 : m_diveTab1];
        for (int p = 0; p < voltaire::panel::DIVEPAGE_COUNT; p ++)
            if (sameName(want, voltaire::panel::kDivePageName[p]))
                return p;
        return -1;
    }

    /// How tall the artwork is right now: shut, or open with one tab row or two.
    float designHeight() const
    {
        if (!m_diveOpen)
            return voltaire::panel::kPanelShutHeight;
        return row2Visible() ? voltaire::panel::kDiveOpenHeight
                             : voltaire::panel::kDiveOpenHeight1Row;
    }

    /// How far up a one-row page is drawn.  The exporter has already applied this to
    /// the page's own geometry; what is left is the drawer furniture drawn from the
    /// panel's artwork, which is authored at the two-row position.
    float diveScoot() const
    {
        return (m_diveOpen && !row2Visible()) ? voltaire::panel::kDiveRow2Height : 0.0f;
    }

    /// Follow the window to the height the drawer now needs, keeping the panel's scale.
    ///
    /// Two calls, because they do two different things.  setSize() resizes OUR window --
    /// under LV2 that is an X child window sitting inside whatever the host put it in,
    /// and a host that does not watch it for changes (Ardour does not; Carla does) leaves
    /// its container the size it was and clips everything past the old height, so the
    /// drawer opens where nobody can see it.  requestSizeChange() is the one that asks
    /// the host to follow.
    ///
    /// It is deliberately called HERE and nowhere else.  The obvious place is onResize,
    /// which would cover every resize at once -- and that is exactly the trap: it fires
    /// for the user's own drag too, hosts treat the request as a minimum size, and the
    /// window ends up able to grow but never shrink.  Only the drawer changes our size
    /// on purpose, so only the drawer says so.
    void resizeToDrawer()
    {
        const uint w = getWidth();
        const uint h = uint(std::lround(double(w) * designHeight()
                                        / voltaire::panel::kDesignWidth));
        if (h != getHeight())
        {
            setSize(w, h);
            requestSizeChange(w, h);
        }
    }

    /// Window pixels -> design units, the space every rectangle in panel_geometry.h is in.
    void toDesign(int px, int py, float &x, float &y) const
    {
        const float s = panelScale();
        x = (px - (getWidth() - voltaire::panel::kDesignWidth * s) * 0.5f) / s;
        y = (py - (getHeight() - designHeight() * s) * 0.5f) / s;
    }

    /// The tab under a design-space point, or -1.  Row 2 is only there when it is shown.
    int diveTabHit(float x, float y) const
    {
        if (!m_diveOpen)
            return -1;
        for (int t = 0; t < voltaire::panel::DIVETABID_COUNT; t ++)
        {
            if (voltaire::panel::kDiveTabRow[t] == 1 && !row2Visible())
                continue;
            const auto &r = voltaire::panel::kDiveTab[t];
            if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h)
                return t;
        }
        return -1;
    }

    bool diveTabSelected(int t) const
    {
        return t == (voltaire::panel::kDiveTabRow[t] == 1 ? m_diveTab2 : m_diveTab1);
    }

    // ---- the controls in the drawer --------------------------------------------------

    /// Tie every control in the artwork to the row of kParams that says what it means.
    ///
    /// Done once, and loudly: a control nobody has written a parameter for would
    /// otherwise just sit there doing nothing, which looks exactly like a bug in the
    /// machine rather than a gap in the table.
    void resolveDiveParams()
    {
        for (int i = 0; i < voltaire::panel::kDiveControlCount; i ++)
        {
            const auto &c = voltaire::panel::kDiveControl[i];
            const char *page = voltaire::panel::kDivePageName[c.page];
            const voltaire::dive::Param *p = voltaire::dive::find(page, c.label);
            m_paramOf[i] = (p != nullptr) ? int(p - voltaire::dive::kParams) : -1;
            if (p == nullptr)
                d_stderr("DIVE: %s on page %s is in the artwork but not in DiveParams.h",
                         c.label, page);
        }
    }

    const voltaire::dive::Param *paramFor(int ctl) const
    {
        const int i = m_paramOf[ctl];
        return i >= 0 ? &voltaire::dive::kParams[i] : nullptr;
    }

    /// Stands for "the machine has not said", which cannot be a value.
    ///
    /// This used to be -1, and master tune is the parameter that found the bug: it runs
    /// -99..+99, so every negative setting read as unknown and the fader dropped to the
    /// bottom.  A sentinel has to be outside the range of every parameter, not merely
    /// outside the range of most of them.
    static constexpr int kNoValue = -1000;

    /// The value the machine last reported for a control, or kNoValue.
    int rawOf(int ctl) const
    {
        const voltaire::dive::Param *p = paramFor(ctl);
        if (p == nullptr)
            return kNoValue;
        switch (p->where)
        {
        case voltaire::dive::kPart:
            return m_partHave[p->addr & 0x1f] ? int(m_partVal[p->addr & 0x1f]) : -1;
        case voltaire::dive::kCommon:
            return m_commonHave[p->addr & 0x1f] ? int(m_commonVal[p->addr & 0x1f]) : -1;
        case voltaire::dive::kRam:
        case voltaire::dive::kRamBit:
        {
            if (!m_setupKnown)
                return kNoValue;
            const int off = int(p->addr) - 0x3C00;
            if (off < 0 || off >= int(sizeof(m_setup)))
                return kNoValue;
            const uint8_t b = m_setup[off];
            if (p->where == voltaire::dive::kRamBit)
                return (b >> p->lo) & 1;
            if (p->show == voltaire::dive::kSigned)
                return int(int8_t(b));               // master tune, -99..+99
            return int(b & 0x0f);                    // control channel, one nibble
        }
        default:
            return kNoValue;
        }
    }

    static int clampRaw(const voltaire::dive::Param &p, int v)
    {
        if (p.where == voltaire::dive::kRam && p.show == voltaire::dive::kSigned)
            return v < voltaire::dive::kMasterTuneMin ? voltaire::dive::kMasterTuneMin
                 : (v > voltaire::dive::kMasterTuneMax ? voltaire::dive::kMasterTuneMax : v);
        return v < int(p.lo) ? int(p.lo) : (v > int(p.hi) ? int(p.hi) : v);
    }

    /// Push a new value at the machine, and show it straight away.
    ///
    /// The local copy is updated without waiting to be told: the round trip is a few
    /// milliseconds and a slider that only moved once the machine agreed would feel
    /// like a slider with a fault.
    void writeRaw(int ctl, int raw)
    {
        const voltaire::dive::Param *p = paramFor(ctl);
        if (p == nullptr)
            return;
        raw = clampRaw(*p, raw);
        char msg[64];
        switch (p->where)
        {
        case voltaire::dive::kPart:
            m_partVal[p->addr & 0x1f] = uint8_t(raw);
            m_partHave[p->addr & 0x1f] = true;
            std::snprintf(msg, sizeof(msg), "p%d %02x %d",
                          divePart() < 0 ? 0 : divePart(), unsigned(p->addr), raw);
            break;
        case voltaire::dive::kCommon:
            m_commonVal[p->addr & 0x1f] = uint8_t(raw);
            m_commonHave[p->addr & 0x1f] = true;
            std::snprintf(msg, sizeof(msg), "c%02x %d", unsigned(p->addr), raw);
            break;
        case voltaire::dive::kRam:
        {
            const int off = int(p->addr) - 0x3C00;
            if (off < 0 || off >= int(sizeof(m_setup)))
                return;
            const uint8_t byte = (p->show == voltaire::dive::kSigned)
                    ? uint8_t(int8_t(raw))
                    : uint8_t((m_setup[off] & 0xf0) | (raw & 0x0f));
            m_setup[off] = byte;
            std::snprintf(msg, sizeof(msg), "r%04x %d", unsigned(p->addr), byte);
            break;
        }
        case voltaire::dive::kRamBit:
        {
            const int off = int(p->addr) - 0x3C00;
            if (off < 0 || off >= int(sizeof(m_setup)))
                return;
            m_setup[off] = uint8_t(raw ? (m_setup[off] | (1u << p->lo))
                                       : (m_setup[off] & ~(1u << p->lo)));
            std::snprintf(msg, sizeof(msg), "b%04x %d %d",
                          unsigned(p->addr), int(p->lo), raw ? 1 : 0);
            break;
        }
        default:
            return;
        }
        setState("divewrite", msg);
        m_dirty = true;
    }

    /// About a second at DPF's idle rate.
    static constexpr int kDiveRetryIdles = 30;

    /// Is anything on the open page still waiting for the machine to answer?
    bool pageIncomplete() const
    {
        const int page = divePage();
        if (page < 0)
            return false;
        for (int i = voltaire::panel::kDivePageFirst[page];
             i < voltaire::panel::kDivePageFirst[page + 1]; i ++)
        {
            const voltaire::dive::Param *p = paramFor(i);
            if (p == nullptr)
                continue;
            if ((p->where == voltaire::dive::kPart
                 || p->where == voltaire::dive::kCommon
                 || p->where == voltaire::dive::kRam
                 || p->where == voltaire::dive::kRamBit) && rawOf(i) == kNoValue)
                return true;
        }
        return false;
    }

    /// Ask the DSP for everything the open page shows.
    ///
    /// Only what is on screen: the machine answers one RQ1 per parameter and there is no
    /// ranged read, so asking for all 26 of a part's parameters to fill in five sliders
    /// would cost five times what it needs to.
    void requestDiveValues()
    {
        if (!m_diveOpen)
            return;
        const int page = divePage();
        if (page < 0)
            return;
        const int part = divePart() < 0 ? 0 : divePart();
        char buf[512];
        int at = std::snprintf(buf, sizeof(buf), "%d", part);
        for (int i = voltaire::panel::kDivePageFirst[page];
             i < voltaire::panel::kDivePageFirst[page + 1]; i ++)
        {
            const voltaire::dive::Param *p = paramFor(i);
            if (p == nullptr || at >= int(sizeof(buf)) - 8)
                continue;
            if (p->where == voltaire::dive::kPart)
                at += std::snprintf(buf + at, sizeof(buf) - size_t(at), " p%02x",
                                    unsigned(p->addr));
            else if (p->where == voltaire::dive::kCommon)
                at += std::snprintf(buf + at, sizeof(buf) - size_t(at), " c%02x",
                                    unsigned(p->addr));
        }
        if (part != m_valuePart)
        {
            std::memset(m_partHave, 0, sizeof(m_partHave));
            m_valuePart = part;
        }
        setState("diveread", buf);
    }

    /// "<part> p07=7f c18=15 r3c00=7f n=Ac.Piano" -- the machine's answer.
    void applyDiveValues(const char *s)
    {
        int part = 0;
        const char *q = s;
        part = std::atoi(q);
        if (part != m_valuePart)
        {
            std::memset(m_partHave, 0, sizeof(m_partHave));
            m_valuePart = part;
        }
        while ((q = std::strchr(q, ' ')) != nullptr)
        {
            q ++;
            if (*q == 'n' && q[1] == '=')
            {
                std::memset(m_patchName, 0, sizeof(m_patchName));
                for (int i = 0; i < voltaire::dive::kNameLen && q[2 + i] != '\0'; i ++)
                    m_patchName[i] = q[2 + i];
                break;                                // the name runs to the end
            }
            unsigned addr = 0, val = 0;
            if (std::sscanf(q + 1, "%x=%x", &addr, &val) != 2)
                continue;
            if (*q == 'p' && addr < 0x20)
            { m_partVal[addr] = uint8_t(val); m_partHave[addr] = true; }
            else if (*q == 'c' && addr < 0x20)
            { m_commonVal[addr] = uint8_t(val); m_commonHave[addr] = true; }
            else if (*q == 'r' && addr >= 0x3C00 && addr < 0x3C00 + sizeof(m_setup))
            { m_setup[addr - 0x3C00] = uint8_t(val); m_setupKnown = true; }
        }
        m_dirty = true;
    }

    /// The value as it should read in the field: four or five characters, LCD style.
    void formatValue(int ctl, char *out, size_t n) const
    {
        const voltaire::dive::Param *p = paramFor(ctl);
        out[0] = '\0';
        if (p == nullptr)
            return;
        if (p->where == voltaire::dive::kUnmapped)
        { std::snprintf(out, n, "--"); return; }
        if (p->where == voltaire::dive::kName)
        { std::snprintf(out, n, "%s", m_patchName); return; }
        if (p->where == voltaire::dive::kTone)
        { std::snprintf(out, n, "%s", toneNameFor(divePart() < 0 ? 0 : divePart())); return; }
        if (p->where == voltaire::dive::kAction)
            return;

        const int raw = rawOf(ctl);
        if (raw == kNoValue)
        { std::snprintf(out, n, "..."); return; }
        formatRaw(*p, raw, out, n);
    }

    /// One value of a parameter, as the field or the list would print it.
    static void formatRaw(const voltaire::dive::Param &pp, int raw, char *out, size_t n)
    {
        const voltaire::dive::Param *p = &pp;
        switch (p->show)
        {
        case voltaire::dive::kOnOff:
            std::snprintf(out, n, raw ? "YES" : "NO");
            break;
        case voltaire::dive::kAssign:
            if (raw >= 6) std::snprintf(out, n, "OFF");
            else          std::snprintf(out, n, "%d", raw + 1);
            break;
        case voltaire::dive::kNote:
        {
            static const char *const kNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                                    "F#", "G", "G#", "A", "A#", "B" };
            std::snprintf(out, n, "%s%d", kNames[raw % 12], raw / 12 - 1);
            break;
        }
        case voltaire::dive::kSigned:
        case voltaire::dive::kPolyPress:
            std::snprintf(out, n, "%+d", voltaire::dive::display(*p, raw));
            break;
        default:
            std::snprintf(out, n, "%d", voltaire::dive::display(*p, raw));
            break;
        }
    }

    /// Where a slider's tap sits, as 0 at the bottom graticule and 1 at the top.
    float sliderFraction(int ctl) const
    {
        const voltaire::dive::Param *p = paramFor(ctl);
        const int raw = rawOf(ctl);
        if (p == nullptr || raw == kNoValue)
            return 0.0f;
        const int lo = (p->where == voltaire::dive::kRam
                        && p->show == voltaire::dive::kSigned)
                ? voltaire::dive::kMasterTuneMin : int(p->lo);
        const int hi = (p->where == voltaire::dive::kRam
                        && p->show == voltaire::dive::kSigned)
                ? voltaire::dive::kMasterTuneMax : int(p->hi);
        if (hi <= lo)
            return 0.0f;
        // Clamped, so a value outside the range this table believes in pins the tap to
        // an end of its travel instead of drawing it off the page.  That is how the LFO
        // rate looked when its range was wrong: not wrong, but ABSENT, which is a much
        // harder thing to recognise as a range error.
        const float f = float(raw - lo) / float(hi - lo);
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    /// ...and the inverse, for a drag.
    int sliderRawAt(int ctl, float fraction) const
    {
        const voltaire::dive::Param *p = paramFor(ctl);
        if (p == nullptr)
            return 0;
        const bool tune = p->where == voltaire::dive::kRam
                       && p->show == voltaire::dive::kSigned;
        const int lo = tune ? voltaire::dive::kMasterTuneMin : int(p->lo);
        const int hi = tune ? voltaire::dive::kMasterTuneMax : int(p->hi);
        const float f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
        return lo + int(std::lround(f * float(hi - lo)));
    }

    /// How far, in design units, the artwork's tap has to move to show the value.
    /// Zero is the bottom graticule and the travel's height is the top one, which is
    /// what "the tap does not exceed the first or last line" means.
    float tapOffset(int ctl) const
    {
        const auto &c = voltaire::panel::kDiveControl[ctl];
        const float want = c.travel.y + c.travel.h * (1.0f - sliderFraction(ctl));
        return want - (c.tap.y + c.tap.h * 0.5f);
    }

    /// The control under a design-space point on the open page, or -1.
    int diveControlHit(float x, float y) const
    {
        if (!m_diveOpen)
            return -1;
        const int page = divePage();
        if (page < 0)
            return -1;
        for (int i = voltaire::panel::kDivePageFirst[page];
             i < voltaire::panel::kDivePageFirst[page + 1]; i ++)
        {
            const auto &c = voltaire::panel::kDiveControl[i];
            voltaire::panel::Rect r = c.box;
            if (c.kind == voltaire::panel::DK_SLIDER)
            {
                // A slider is grabbed anywhere along its travel, and the tap is narrow,
                // so the hit box is the wider of the two with a little margin.
                r.x = c.tap.x < c.box.x ? c.tap.x : c.box.x;
                r.w = (c.tap.w > c.box.w ? c.tap.w : c.box.w);
                r.y = c.travel.y - c.tap.h;
                r.h = c.travel.h + c.tap.h * 2.0f;
            }
            if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h)
                return i;
        }
        return -1;
    }

    /// A click on a control in the drawer.
    void pressDiveControl(int ctl, float y)
    {
        const auto &c = voltaire::panel::kDiveControl[ctl];
        const voltaire::dive::Param *p = paramFor(ctl);
        if (p == nullptr || p->where == voltaire::dive::kUnmapped)
            return;

        if (c.kind == voltaire::panel::DK_SLIDER)
        {
            // Grabbing anywhere on the travel jumps there and starts a drag, which is
            // what a fader does; there is no separate "click the tap first" step.
            m_dragCtl = ctl;
            dragSliderTo(ctl, y);
            repaint();
            return;
        }

        switch (p->where)
        {
        case voltaire::dive::kName:
            // Edited in place rather than in a dialog: ten characters is not worth a
            // window, and the field is already the right shape and the right font.
            m_nameEdit = true;
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_patchName);
            trimTrailingSpaces(m_nameBuf);
            m_nameCaret = int(std::strlen(m_nameBuf));
            repaint();
            return;
        case voltaire::dive::kTone:
            m_tonePart = divePart() < 0 ? 0 : divePart();
            for (size_t g = 0; g < m_toneGroups.size(); g ++)
                if (m_toneGroups[g].media == m_partMedia[m_tonePart])
                    m_toneGroup = int(g);
            m_menu = Menu::Tone;
            m_toneHover = ToneHit();
            repaint();
            return;
        case voltaire::dive::kAction:
            runDiveAction(uint8_t(p->addr));
            repaint();
            return;
        default:
            break;
        }

        if (p->show == voltaire::dive::kOnOff)
        {
            const int raw = rawOf(ctl);
            writeRaw(ctl, raw > 0 ? 0 : 1);
            repaint();
            return;
        }

        // Everything else is a list to pick from.
        m_valueCtl = ctl;
        m_menu = Menu::Value;
        m_menuHover = -1;
        repaint();
    }

    /// The Common page's four routing presets, and the Basic page's Defaults.
    ///
    /// Left and right are Output Assign, not Output Mode: in mode 21 voice group 1 is
    /// <L31> and group 2 is <R31> (OM Output Modes, and the note under the table), so
    /// hard left is assign 1 and hard right assign 2 -- and only in a mode that has an
    /// L/R pair at all, which is why they set the mode too.  These sit in a patch-level
    /// box, so they move all six parts.
    void runDiveAction(uint8_t action)
    {
        using namespace voltaire::dive;
        switch (action)
        {
        case kActDefaults:
        {
            const int part = divePart() < 0 ? 0 : divePart();
            writePartRaw(part, 0x00, 0);      // output assign -> voice group 1
            writePartRaw(part, 0x01, 0);      // receive channel -> 1
            writePartRaw(part, 0x05, 0);      // key range low  -> the bottom
            writePartRaw(part, 0x06, 127);    // key range high -> the top
            break;
        }
        case kActPresetEfx:
            writeCommonRaw(0x18, kModeStereoEfx);
            break;
        case kActPresetDry:
            writeCommonRaw(0x18, kModeCentreDry);
            break;
        case kActPresetLeft:
        case kActPresetRight:
            writeCommonRaw(0x18, kModeStereoEfx);
            for (int part = 0; part < 6; part ++)
                writePartRaw(part, 0x00, action == kActPresetLeft ? 0 : 1);
            break;
        case kActWrite:
            // Where it goes is a choice, so it is asked before anything happens.  The
            // list is the PATCH menu -- the same 64 names, read out of the same memory --
            // because the question "which patch" already has an answer in this UI and
            // inventing a second one would be two lists to keep in step.
            m_menu = Menu::Patch;
            m_writeMode = true;
            m_menuHover = -1;
            m_writeTarget = -1;
            m_libraryNote.clear();
            if (m_libraryTab)
                libraryRescan();
            repaint();
            break;
        default:
            break;
        }
    }

    void writePartRaw(int part, uint8_t addr, int value)
    {
        char msg[48];
        std::snprintf(msg, sizeof(msg), "p%d %02x %d", part, unsigned(addr), value);
        setState("divewrite", msg);
        if (part == m_valuePart && addr < 0x20)
        { m_partVal[addr] = uint8_t(value); m_partHave[addr] = true; }
        m_dirty = true;
    }

    void writeCommonRaw(uint8_t addr, int value)
    {
        char msg[48];
        std::snprintf(msg, sizeof(msg), "c%02x %d", unsigned(addr), value);
        setState("divewrite", msg);
        if (addr < 0x20)
        { m_commonVal[addr] = uint8_t(value); m_commonHave[addr] = true; }
        m_dirty = true;
    }

    /// Set a slider from a pointer position on the page.
    void dragSliderTo(int ctl, float y)
    {
        const auto &c = voltaire::panel::kDiveControl[ctl];
        if (c.travel.h <= 0.0f)
            return;
        const float f = 1.0f - (y - c.travel.y) / c.travel.h;
        writeRaw(ctl, sliderRawAt(ctl, f));
    }

    float panelScale() const
    {
        const float sx = float(getWidth()) / voltaire::panel::kDesignWidth;
        const float sy = float(getHeight()) / designHeight();
        return sx < sy ? sx : sy;
    }

    /// Let go of every latched key.
    ///
    /// Called before a reboot, and that is not tidiness: the firmware decides what to
    /// boot into by reading the key matrix ONCE while it comes up, so a key still latched
    /// across a reset is a key held at power-on.  Latch [DEC] and ask for a plain reboot
    /// and the machine would come up in the test menu instead, which is a very confusing
    /// thing for a menu entry to do.
    void clearLatches()
    {
        for (int i = 0; i < voltaire::panel::BUTTONID_COUNT; i ++)
        {
            if (!m_latch[i])
                continue;
            m_latch[i] = false;
            const int p = mapButton(i);
            if (p >= 0)
                setParam(uint32_t(kParamButtonFirst + p), 0.0f);
        }
    }

    void setParam(uint32_t index, float value)
    {
        editParameter(index, true);
        setParameterValue(index, value);
        editParameter(index, false);
    }

    /// Panel button -> plugin parameter.  Only the six the machine actually has are
    /// here.  The others the artwork carries -- FILTER, RESET, PATCH, TONE, DIVE,
    /// CARTRIDGE MANAGER -- are the plugin's own and are handled where they are clicked,
    /// above; a switch the hardware has no contact for has no parameter to drive.
    static int mapButton(int id)
    {
        switch (id)
        {
        case voltaire::panel::BUT_PART_JUMP: return 0;
        case voltaire::panel::BUT_EDIT_EXIT: return 1;
        case voltaire::panel::BUT_LEFT:      return 2;
        case voltaire::panel::BUT_RIGHT:     return 3;
        case voltaire::panel::BUT_DEC:       return 4;
        case voltaire::panel::BUT_INC_EXIT:  return 5;
        default:                   return -1;
        }
    }

    void loadArtwork()
    {
        // A file overrides the built-in copy, so the panel can be redrawn in Inkscape and
        // reloaded without rebuilding.  Falls back to the embedded artwork.
        if (const char *path = std::getenv("VOLTAIRE_PANEL_SVG"))
        {
            m_svg = nsvgParseFromFile(path, "px", 96.0f);
            if (m_svg != nullptr)
                return;
            d_stderr2("Voltaire 110: could not parse %s, using the built-in panel", path);
        }
        std::vector<char> copy(kPanelSvg, kPanelSvg + sizeof(kPanelSvg));
        m_svg = nsvgParse(copy.data(), "px", 96.0f);   // nsvgParse modifies its input
    }

    // The artwork's raster layer, which nanosvg cannot draw: it has no <image>
    // element, so panel_export.py renders every image -- transform, clip path and all
    // -- through rsvg-convert into panel_background.png and embeds the bytes.  What
    // arrives here is already exactly the design rectangle, so it goes down with no
    // geometry of its own and cannot drift out of register with the vectors.
    //
    // Created on the first frame rather than in the constructor because it needs a
    // NanoVG context, which only exists once there is something to draw into.  If it
    // fails there is no fallback and none is wanted: the panel simply comes up on its
    // flat ground, which is what it looked like before there was a background at all.
    void drawBackground()
    {
        if (!m_backgroundTried)
        {
            m_backgroundTried = true;
            m_background = createImageFromMemory(
                    kPanelBackgroundPng, sizeof(kPanelBackgroundPng), 0);
        }
        if (!m_background.isValid())
            return;

        const float w = voltaire::panel::kDesignWidth;
        const float h = voltaire::panel::kDesignHeight;
        beginPath();
        rect(0, 0, w, h);
        fillPaint(imagePattern(0, 0, w, h, 0.0f, m_background, 1.0f));
        fill();
    }

    void drawArtwork()
    {
        if (m_svg == nullptr)
            return;

        // nanosvg resolves the document's own units -- the artwork is in MILLIMETRES, so
        // it comes back scaled by 96/25.4 and lands three and a half times too big.  The
        // geometry header is in viewBox units, because it composes transforms itself and
        // never asks nanosvg.  Normalise the artwork onto the same units rather than
        // trying to talk nanosvg out of the conversion; then the two agree by construction
        // whatever the document says its units are.
        const float k = (m_svg->width > 1.0f)
                ? (voltaire::panel::kDesignWidth / m_svg->width) : 1.0f;
        const float px = k * panelScale();
        save();
        scale(k, k);
        for (NSVGshape *sh = m_svg->shapes; sh != nullptr; sh = sh->next)
        {
            if (!(sh->flags & NSVG_FLAGS_VISIBLE))
                continue;
            // The knob pointer is drawn in code so it can rotate.  Skipping the artwork's
            // copy is what stops the old one being left behind at its zero position.
            if (std::strcmp(sh->id, voltaire::panel::kVolumeKnobPointerId) == 0)
                continue;

            // The drawer: nothing of it exists while it is shut, the body and the
            // content box are drawn here because their size follows the page, and the
            // second row of tabs is only there for a part.
            const DiveShape d = classifyDiveShape(sh->id);
            if (d.what != DiveShape::None && !m_diveOpen)
                continue;
            if (d.what == DiveShape::Body)    { drawDiveBox(sh, diveBodyRect()); continue; }
            if (d.what == DiveShape::Content) { drawDiveBox(sh, diveContentRect()); continue; }
            if (d.what != DiveShape::None
                    && voltaire::panel::kDiveTabRow[d.tab] == 1 && !row2Visible())
                continue;

            const bool lit = d.what != DiveShape::None && diveTabSelected(d.tab);
            if (lit && d.what == DiveShape::TabText)
            {
                // Brighter, and glowing, because #00a3e0 on its own does not have enough
                // headroom left to read as "selected" from across a mix window.  The glow
                // is the same path stroked twice at low alpha under the fill -- no filter,
                // which nanosvg has no notion of anyway.
                shapePath(sh, px);
                strokeColor(Color(120, 220, 255, 0.20f));
                strokeWidth(2.6f);
                stroke();
                strokeColor(Color(120, 220, 255, 0.28f));
                strokeWidth(1.2f);
                stroke();
                fillColor(Color(190, 240, 255));
                fill();
                continue;
            }

            shapePath(sh, px);
            if (sh->fill.type == NSVG_PAINT_COLOR)
            {
                // A selected tab darkens by becoming less transparent, which is the one
                // change that reads the same whatever is behind it.
                fillColor(nvgCol(sh->fill.color,
                                 lit ? sh->opacity * kDiveTabLit : sh->opacity));
                fill();
            }
            if (sh->stroke.type == NSVG_PAINT_COLOR && sh->strokeWidth > 0.0f)
            {
                strokeColor(nvgCol(sh->stroke.color, sh->opacity));
                strokeWidth(sh->strokeWidth);
                stroke();
            }
        }
        restore();
    }

    /// The open page: its raster layer, then its vectors.
    ///
    /// Each page is a separate document, already translated into panel coordinates by
    /// the exporter -- including the shift up when the second tab row is hidden -- so
    /// there is no page transform here at all.  They are parsed on first use: opening
    /// the drawer on SET should not pay for the other five.
    void drawDivePage()
    {
        if (!m_diveOpen)
            return;
        const int p = divePage();
        if (p < 0)
            return;
        if (!m_pageTried[p])
        {
            m_pageTried[p] = true;
            loadDivePage(p);
        }

        const float w = voltaire::panel::kDesignWidth;
        const float h = voltaire::panel::kDiveOpenHeight;
        if (m_pageRaster[p].isValid())
        {
            // What nanosvg cannot draw: the section frames, whose gap for the title is a
            // clip path, and the slider graticules where they are clipped by the body.
            beginPath();
            rect(0, 0, w, h);
            fillPaint(imagePattern(0, 0, w, h, 0.0f, m_pageRaster[p], 1.0f));
            fill();
        }

        NSVGimage *svg = m_pageSvg[p];
        if (svg == nullptr)
            return;
        const float k = (svg->width > 1.0f) ? (w / svg->width) : 1.0f;
        const float px = k * panelScale();
        save();
        scale(k, k);
        for (NSVGshape *sh = svg->shapes; sh != nullptr; sh = sh->next)
        {
            if (!(sh->flags & NSVG_FLAGS_VISIBLE))
                continue;

            // A slider tap is drawn where the VALUE is, not where Inkscape parked it.
            // Shifting the artwork's own shape rather than drawing a replacement keeps
            // whatever gradient and outline it was given.  Same idea as the knob pointer,
            // which is rotated instead of moved.
            const int slider = sliderForTapId(p, sh->id);
            const bool moved = slider >= 0;
            if (moved)
            {
                save();
                translate(0.0f, tapOffset(slider) / k);
            }
            shapePath(sh, px);
            if (sh->fill.type == NSVG_PAINT_COLOR)
            {
                fillColor(nvgCol(sh->fill.color, sh->opacity));
                fill();
            }
            if (sh->stroke.type == NSVG_PAINT_COLOR && sh->strokeWidth > 0.0f)
            {
                strokeColor(nvgCol(sh->stroke.color, sh->opacity));
                strokeWidth(sh->strokeWidth);
                stroke();
            }
            if (moved)
                restore();
        }
        restore();

        drawDiveValues(p);
    }

    /// Which slider owns this shape, if it is a tap.  Only the open page is searched.
    int sliderForTapId(int page, const char *id) const
    {
        if (id == nullptr || id[0] == '\0')
            return -1;
        for (int i = voltaire::panel::kDivePageFirst[page];
             i < voltaire::panel::kDivePageFirst[page + 1]; i ++)
        {
            const char *t = voltaire::panel::kDiveControl[i].tap_id;
            if (t != nullptr && std::strcmp(t, id) == 0)
                return i;
        }
        return -1;
    }

    /// The numbers and words in the LCD-style fields.
    void drawDiveValues(int page)
    {
        for (int i = voltaire::panel::kDivePageFirst[page];
             i < voltaire::panel::kDivePageFirst[page + 1]; i ++)
        {
            const auto &c = voltaire::panel::kDiveControl[i];
            if (c.kind == voltaire::panel::DK_SLIDER)
                continue;
            char text[24];
            formatValue(i, text, sizeof(text));
            if (text[0] == '\0')
                continue;
            const voltaire::dive::Param *p = paramFor(i);
            const bool dim = p != nullptr && p->where == voltaire::dive::kUnmapped;
            if (m_nameEdit && p != nullptr && p->where == voltaire::dive::kName)
            {
                // The caret is a block that blinks, the way the machine's own name
                // editor draws it.  The cell is occupied either way -- a space when the
                // caret is off -- so the text does not shuffle sideways twice a second.
                char withCaret[voltaire::dive::kNameLen + 2];
                std::snprintf(withCaret, sizeof(withCaret), "%s%c", m_nameBuf,
                              m_caretOn ? '_' : ' ');
                drawCgromText(c.box, withCaret, Color(190, 255, 190));
                beginPath();
                roundedRect(c.box.x, c.box.y, c.box.w, c.box.h, c.box.h * 0.12f);
                strokeColor(Color(120, 220, 255, 0.75f));
                strokeWidth(1.2f);
                stroke();
                continue;
            }
            drawCgromText(c.box, text, dim ? Color(70, 110, 72) : Color(150, 255, 150));
        }
    }

    /// Text in the machine's own 5 x 7 dot font, centred in a rect.
    ///
    /// The same glyph table the LCD draws from, for the same reason the panel uses it
    /// there: these fields are the machine talking, and it has one typeface.
    void drawCgromText(const voltaire::panel::Rect &r, const char *s, const Color &on)
    {
        const int n = int(std::strlen(s));
        if (n <= 0)
            return;
        const float byW = r.w * 0.84f / float(n * 6 - 1);
        const float byH = r.h * 0.60f / 8.0f;
        const float d = byW < byH ? byW : byH;
        const float tw = d * float(n * 6 - 1), th = d * 8.0f;
        const float x0 = r.x + (r.w - tw) * 0.5f;
        const float y0 = r.y + (r.h - th) * 0.5f;
        fillColor(on);
        for (int i = 0; i < n; i ++)
        {
            const unsigned char ch = (unsigned char)s[i];
            if (ch < 0x20 || ch >= 0x80)
                continue;
            const unsigned char *glyph = kU110Cgrom[ch - 0x20];
            for (int row = 0; row < 8; row ++)
            {
                const unsigned bits = glyph[row];
                for (int col = 0; col < 5; col ++)
                {
                    if (!((bits >> (4 - col)) & 1))
                        continue;
                    beginPath();
                    rect(x0 + float(i * 6 + col) * d, y0 + float(row) * d,
                         d * 0.88f, d * 0.88f);
                    fill();
                }
            }
        }
    }

    /// What the pointer is over, in words and in numbers, near the pointer.
    ///
    /// The artwork has no room for a value beside every slider -- there are eight across
    /// the LFO page -- so the number appears where you are looking instead, both while
    /// hovering and while dragging.
    void drawDiveTooltip()
    {
        const int ctl = m_dragCtl >= 0 ? m_dragCtl : m_hoverCtl;
        if (!m_diveOpen || ctl < 0)
            return;
        const auto &c = voltaire::panel::kDiveControl[ctl];
        const voltaire::dive::Param *p = paramFor(ctl);
        if (p == nullptr || p->where == voltaire::dive::kAction)
            return;

        char value[24];
        formatValue(ctl, value, sizeof(value));
        char line[64];
        std::snprintf(line, sizeof(line), "%s  %s", prettyLabel(c.label), value);

        fontFaceId(0);
        fontSize(13.0f);
        fontFace(m_font);
        Rectangle<float> box;
        textBounds(0, 0, line, nullptr, box);
        const float pad = 6.0f;
        const float w = box.getWidth() + pad * 2.0f, h = 20.0f;
        float x = m_tipX + 14.0f, y = m_tipY - h - 8.0f;
        if (x + w > float(getWidth()))  x = float(getWidth()) - w;
        if (x < 0.0f)                   x = 0.0f;
        if (y < 0.0f)                   y = m_tipY + 18.0f;

        beginPath();
        roundedRect(x, y, w, h, 4.0f);
        fillColor(Color(12, 14, 16, 0.92f));
        fill();
        strokeColor(Color(0, 163, 224, 0.55f));
        strokeWidth(1.0f);
        stroke();

        fillColor(Color(214, 222, 228));
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        text(x + pad, y + h * 0.5f, line, nullptr);
    }

    /// "SB_ENV_Attack_Rate" -> "ENV Attack Rate".
    static const char *prettyLabel(const char *label)
    {
        static char buf[48];
        const char *s = std::strchr(label, '_');
        s = (s != nullptr) ? s + 1 : label;
        size_t i = 0;
        for (; s[i] != '\0' && i < sizeof(buf) - 1; i ++)
            buf[i] = s[i] == '_' ? ' ' : s[i];
        buf[i] = '\0';
        return buf;
    }

    /// The tone a part is playing, by name, for the Basic page's field.
    const char *toneNameFor(int part) const
    {
        if (part < 0 || part >= 6)
            return "";
        for (const ToneGroup &g : m_toneGroups)
            if (g.media == m_partMedia[part] && m_partTone[part] < g.names.size())
                return g.names[m_partTone[part]].c_str();
        return "";
    }

    void loadDivePage(int p)
    {
        if (kDiveSvg[p] != nullptr)
        {
            std::string copy(kDiveSvg[p]);
            m_pageSvg[p] = nsvgParse(&copy[0], "px", 96.0f);   // nsvgParse edits its input
        }
        if (kDiveRaster[p] != nullptr
                && kDiveRasterSize[p] != 0)
            m_pageRaster[p] = createImageFromMemory(
                    const_cast<uchar *>(kDiveRaster[p]),
                    kDiveRasterSize[p], 0);
    }

    /// How close a flattened curve has to stay to the real one, in WINDOW pixels.
    ///
    /// NanoVG flattens beziers itself, and its tolerance works out at half a device
    /// pixel: `nvg__tesselateBezier` tests `(d2+d3)^2 < tessTol * |chord|^2`, which is a
    /// distance test with `tessTol` standing in for the SQUARE of the tolerance, and
    /// tessTol is 0.25.  Points are transformed before that runs, so the half pixel is a
    /// half pixel on screen.
    ///
    /// On artwork that is mostly rectangles nobody would notice.  On lettering at 22 px
    /// it is exactly visible: where a shallow curve meets a flat edge -- the top of an
    /// "S", the shoulder of a "P" -- the chord lands half a pixel proud, and the eye
    /// reads it as a notch or a stray dot.  rsvg-convert tessellates far finer and has
    /// none of them, which is what made the two renders disagree.
    ///
    /// So the curves arrive already flattened, to a tenth of a pixel.  Subdividing
    /// adaptively rather than uniformly means a nearly straight curve still costs two
    /// segments, which most of a glyph's outline is.
    static constexpr float kCurveTolerancePx = 0.1f;

    /// Split one cubic until its chord is within tolerance, emitting line segments.
    /// `tol2` is the squared tolerance in the units the points are given in.
    void flattenCubic(float x1, float y1, float x2, float y2,
                      float x3, float y3, float x4, float y4, float tol2, int level)
    {
        if (level > 10)
        {
            lineTo(x4, y4);
            return;
        }
        const float dx = x4 - x1, dy = y4 - y1;
        const float d2 = std::fabs((x2 - x4) * dy - (y2 - y4) * dx);
        const float d3 = std::fabs((x3 - x4) * dy - (y3 - y4) * dx);
        if ((d2 + d3) * (d2 + d3) < tol2 * (dx * dx + dy * dy))
        {
            lineTo(x4, y4);
            return;
        }
        const float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
        const float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
        const float x34 = (x3 + x4) * 0.5f, y34 = (y3 + y4) * 0.5f;
        const float x123 = (x12 + x23) * 0.5f, y123 = (y12 + y23) * 0.5f;
        const float x234 = (x23 + x34) * 0.5f, y234 = (y23 + y34) * 0.5f;
        const float x1234 = (x123 + x234) * 0.5f, y1234 = (y123 + y234) * 0.5f;
        flattenCubic(x1, y1, x12, y12, x123, y123, x1234, y1234, tol2, level + 1);
        flattenCubic(x1234, y1234, x234, y234, x34, y34, x4, y4, tol2, level + 1);
    }

    /// Lay a parsed SVG shape down as a NanoVG path.  Nothing is painted.
    ///
    /// `px` is how many window pixels one unit of the shape's own coordinates covers,
    /// which is what turns the pixel tolerance above into one this code can test.
    void shapePath(NSVGshape *sh, float px)
    {
        const float tol = kCurveTolerancePx / (px > 0.0f ? px : 1.0f);
        const float tol2 = tol * tol;

        // Which way round the OUTER contour runs, taken as the direction of the biggest
        // subpath.  It has to be measured rather than assumed, because the two font
        // formats disagree: TrueType winds an outer contour clockwise, CFF/OpenType
        // counter-clockwise.  See the note on pathWinding() below for why that matters
        // here of all places.
        float outer = 0.0f;
        for (NSVGpath *q = sh->paths; q != nullptr; q = q->next)
        {
            const float a = subpathArea(q);
            if (std::fabs(a) > std::fabs(outer))
                outer = a;
        }
        const bool outerPositive = outer >= 0.0f;

        beginPath();
        for (NSVGpath *p = sh->paths; p != nullptr; p = p->next)
        {
            moveTo(p->pts[0], p->pts[1]);
            for (int i = 0; i < p->npts - 1; i += 3)
            {
                const float *q = &p->pts[i * 2];
                flattenCubic(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], tol2, 0);
            }
            if (p->closed)
                closePath();

            // NanoVG FORCES every subpath to CCW unless told otherwise, so the
            // counters inside letters -- the hole in an "o" -- get reversed and the
            // glyph fills solid.  nanosvg already hands over correctly opposed
            // windings; what has to be declared is which of the two is the solid one.
            //
            // [!] Declaring each subpath by its OWN direction is not the same thing, and
            // that is what this used to do.  It looks equivalent -- the contours stay
            // opposed either way, so the fill is right and the counters stay open -- but
            // NanoVG builds its antialias fringe along the point order, so the half-pixel
            // feather lands OUTSIDE the glyph for one winding convention and INSIDE it
            // for the other.  Since TrueType and CFF wind outer contours opposite ways,
            // the same artwork in an .otf renders 31% heavier than in a .ttf: at a 16 px
            // cap height the 2 px gaps between letters close up and the lettering reads
            // as squished.  Measured against rsvg-convert at the same size, the two
            // fonts are within 2.5% of each other, so the weight was never the font's.
            //
            // Deciding solid-vs-hole RELATIVE to the outer contour is what makes this
            // independent of which format the artwork's face happens to be in.
            pathWinding((subpathArea(p) >= 0.0f) == outerPositive ? CCW : CW);
        }
    }

    // How much less transparent a selected tab's background gets.  Three times the
    // artwork's own 12% lands at a bit over a third, which is dark enough to read as
    // chosen against the panel body without becoming a black hole.
    static constexpr float kDiveTabLit = 3.0f;

    struct DiveShape
    {
        enum What { None, Body, Content, TabRect, TabText } what = None;
        int tab = 0;
    };

    /// Which piece of the drawer, if any, an SVG id belongs to.
    DiveShape classifyDiveShape(const char *id) const
    {
        DiveShape d;
        if (id == nullptr || id[0] == '\0')
            return d;
        if (std::strcmp(id, voltaire::panel::kDiveBodySvgId) == 0)
        { d.what = DiveShape::Body; return d; }
        if (std::strcmp(id, voltaire::panel::kDiveContentSvgId) == 0)
        { d.what = DiveShape::Content; return d; }
        for (int t = 0; t < voltaire::panel::DIVETABID_COUNT; t ++)
        {
            if (std::strcmp(id, voltaire::panel::kDiveTabSvgId[t]) == 0)
            { d.what = DiveShape::TabRect; d.tab = t; return d; }
            const char *tx = voltaire::panel::kDiveTabTextSvgId[t];
            if (tx != nullptr && std::strcmp(id, tx) == 0)
            { d.what = DiveShape::TabText; d.tab = t; return d; }
        }
        return d;
    }

    /// The drawer body, shortened when the second tab row is not on show.
    voltaire::panel::Rect diveBodyRect() const
    {
        voltaire::panel::Rect r = voltaire::panel::kDiveBody;
        r.h -= diveScoot();
        return r;
    }

    /// The content box, moved up into the space the hidden tab row leaves.
    voltaire::panel::Rect diveContentRect() const
    {
        voltaire::panel::Rect r = voltaire::panel::kDiveContent;
        r.y -= diveScoot();
        return r;
    }

    /// Redraw one of the artwork's own rectangles at a size the page decides, keeping
    /// the paint it was given in Inkscape.
    void drawDiveBox(NSVGshape *sh, const voltaire::panel::Rect &r)
    {
        // The artwork is drawn under a normalising scale; these rectangles are in design
        // units, so undo it for the duration.
        const float k = (m_svg->width > 1.0f)
                ? (voltaire::panel::kDesignWidth / m_svg->width) : 1.0f;
        const float px = k * panelScale();
        save();
        scale(1.0f / k, 1.0f / k);
        beginPath();
        rect(r.x, r.y, r.w, r.h);
        if (sh->fill.type == NSVG_PAINT_COLOR)
        {
            fillColor(nvgCol(sh->fill.color, sh->opacity));
            fill();
        }
        if (sh->stroke.type == NSVG_PAINT_COLOR && sh->strokeWidth > 0.0f)
        {
            strokeColor(nvgCol(sh->stroke.color, sh->opacity));
            strokeWidth(sh->strokeWidth * k);
            stroke();
        }
        restore();
    }

    /// Signed area of a flattened subpath; its sign is the winding direction.
    static float subpathArea(const NSVGpath *p)
    {
        float a = 0.0f;
        for (int i = 0, j = p->npts - 1; i < p->npts; j = i ++)
            a += p->pts[j * 2] * p->pts[i * 2 + 1] - p->pts[i * 2] * p->pts[j * 2 + 1];
        return a * 0.5f;
    }

    /// nanosvg packs a shape's colour as 0xAABBGGRR, folding fill-opacity into the top
    /// byte; sh->opacity carries the element's own opacity= on top of that.
    ///
    /// [!] DGL's Color takes RGB as 0-255 INTEGERS and alpha as a 0-1 FLOAT
    /// (Color(int, int, int, float)).  Passing the alpha byte here compiles perfectly and
    /// clamps to 1.0, which forces every translucent fill in the artwork opaque -- with
    /// no warning and nothing to see until a piece of artwork is meant to show through
    /// another.  That is exactly how it was found: the panel body is 35% dark over the
    /// background image, and the background never appeared.
    static Color nvgCol(unsigned int c, float opacity)
    {
        return Color(int(c & 0xff), int((c >> 8) & 0xff), int((c >> 16) & 0xff),
                     float((c >> 24) & 0xff) / 255.0f * opacity);
    }

    /// One line of the 16x2, centred, in the display's own character codes.
    static void lcdCentre(uint8_t *row, const char *text)
    {
        const size_t n = std::strlen(text);
        const size_t at = n >= 16 ? 0 : (16 - n) / 2;
        for (size_t i = 0; i < n && at + i < 16; i ++)
            row[at + i] = uint8_t(text[i]);
    }

    /// What to show instead of the LCD, or nullptr to show the LCD.
    ///
    /// Only ever used before the machine's first word.  Once a panel blob has arrived the
    /// display belongs to the firmware, whatever it decides to put there.
    const char *lcdStandIn() const
    {
        if (m_panelSeen)
            return nullptr;
        if (m_diagStatus == "no-program-rom")  return "NO ROM FILE";
        if (m_diagStatus == "bad-program-rom") return "BAD ROM FILE";
        if (m_diagStatus == "no-wave-rom")     return "NO WAVE ROM";
        // "ok" means the ROMs loaded and the panel is merely on its way, which is a
        // fraction of a second and not worth flashing a message about.
        if (m_diagStatus == "ok" || m_diagStatus == "not-loaded")
            return nullptr;
        if (m_diagStatus.empty())
            return m_diagWait >= kDiagWaitIdles ? "NO DSP ANSWER" : nullptr;
        return "MACHINE STOPPED";
    }

    void askForDiag()
    {
        m_diagWait = 0;
        setState("diagreq", "1");
    }

    /// The LCD, built from character codes.  Codes 0x00-0x0F come from the firmware's own
    /// CGRAM and change while it runs; everything else comes from the baked table.
    void drawLcd()
    {
        const auto &g = voltaire::panel::kLcdInner;

        // Glass and backlight.
        beginPath();
        rect(g.x, g.y, g.w, g.h);
        fillPaint(linearGradient(g.x, g.y, g.x, g.y + g.h,
                                 Color(22, 58, 24), Color(12, 38, 14)));
        fill();

        // 16 x 2 cells of 5 x 8 dots, with a one-dot gap between characters.  The pitch is
        // derived from the artwork, so the display scales with the panel.
        const float dotW = g.w / (16.0f * 6.0f - 1.0f);
        const float dotH = g.h / (2.0f * 9.0f - 1.0f);
        const float dw = dotW * 0.86f, dh = dotH * 0.86f;

        // What goes on the glass when the machine has never sent anything.  A blank LCD
        // with a working panel around it looks like a plugin that is fine and quiet; it
        // is not, and the only place a DAW user can be told so is here.
        uint8_t stand_in[32];
        const uint8_t *cells = m_lcd;
        if (const char *const msg = lcdStandIn())
        {
            std::memset(stand_in, ' ', sizeof(stand_in));
            lcdCentre(stand_in, msg);
            lcdCentre(stand_in + 16, "CLICK FOR INFO");
            cells = stand_in;
        }

        for (int cell = 0; cell < 32; cell ++)
        {
            const int col = cell % 16, row = cell / 16;
            const uint8_t code = cells[cell];
            const unsigned char *glyph = nullptr;
            if (code >= 0x20 && code < 0x80)
                glyph = kU110Cgrom[code - 0x20];
            else if (code < 8)
                glyph = &m_cgram[code * 8];

            for (int r = 0; r < 8; r ++)
            {
                const unsigned bits = glyph ? glyph[r] : 0;
                for (int c = 0; c < 5; c ++)
                {
                    const bool on = (bits >> (4 - c)) & 1;
                    const float x = g.x + (col * 6 + c) * dotW;
                    const float y = g.y + (row * 9 + r) * dotH;
                    beginPath();
                    roundedRect(x, y, dw, dh, dw * 0.22f);
                    // Unlit dots stay faintly visible, as they are on the real glass.
                    fillColor(on ? Color(150, 255, 150) : Color(26, 52, 28));
                    fill();
                }
            }
        }
    }

    void drawLeds()
    {
        // Only two of the artwork's LEDs are driven by the machine; the rest are lit only
        // when their function exists.
        drawLed(voltaire::panel::LED_PART_JUMP, (m_leds & 0x01) != 0, Color(255, 60, 40));
        drawLed(voltaire::panel::LED_EDIT_EXIT, (m_leds & 0x02) != 0, Color(255, 60, 40));
        // MIDI is the machine's own lamp, off CPU port 2 bit 6, so it blinks when the
        // hardware's does.  CLIP is the plugin's, measured after the volume control.
        drawLed(voltaire::panel::LED_MIDI, (m_leds & 0x04) != 0, Color(255, 140, 40));
        drawLed(voltaire::panel::LED_CLIP, (m_leds & 0x08) != 0, Color(255, 40, 30));
        // FILT is the measured HF correction (PLUGIN-PLAN.md section 10.2) -- the only
        // filter the machine has that is switchable.  It is a CALIBRATION rather than a
        // tone control, so the lamp means "the emulator is matched to the hardware", which
        // is the state you normally want lit.
        drawLed(voltaire::panel::LED_FILT, m_hf, Color(255, 60, 40));
    }

    void drawLed(int id, bool on, Color c)
    {
        const auto &r = voltaire::panel::kLed[id];
        beginPath();
        roundedRect(r.x, r.y, r.w, r.h, r.h * 0.3f);
        fillColor(on ? c : Color(int(c.red * 60), int(c.green * 60), int(c.blue * 60)));
        fill();
        if (on)
        {
            // A little bloom, which is cheap here and outside nanosvg's subset anyway.
            beginPath();
            rect(r.x - r.h, r.y - r.h, r.w + r.h * 2, r.h * 3);
            fillPaint(boxGradient(r.x, r.y, r.w, r.h, r.h * 0.5f, r.h,
                                  Color(c.red, c.green, c.blue, 0.45f),
                                  Color(c.red, c.green, c.blue, 0.0f)));
            fill();
        }
    }

    void drawKnob()
    {
        const auto &k = voltaire::panel::kVolumeKnob;
        // The artwork draws the knob and its pointer at zero.  Rotate from there: one
        // transform, not 128 exported frames.
        const float t = (m_volume - (-3.0f)) / (16.0f - (-3.0f));
        const float sweep = 280.0f;

        // A face under the pointer, so the control reads as one object rather than a line
        // floating over the bezel.
        beginPath();
        circle(k.cx, k.cy, k.r * 0.88f);
        fillPaint(linearGradient(k.cx, k.cy - k.r, k.cx, k.cy + k.r,
                                 Color(78, 78, 82), Color(38, 38, 42)));
        fill();

        save();
        translate(k.cx, k.cy);
        rotate((t - 0.5f) * sweep * float(M_PI) / 180.0f);
        beginPath();
        moveTo(0, 0);
        lineTo(0, -k.r * 0.82f);
        strokeColor(Color(245, 245, 245));
        strokeWidth(k.r * 0.13f);
        lineCap(ROUND);
        stroke();
        restore();
    }

    void drawButtonFeedback()
    {
        // A latching button shows its state, not a momentary press.  So does the PATCH
        // button while its menu is up.
        // Only the ones a panel button opens.  The self-check and the About box are
        // reached from the LCD and the logo, and lighting PATCH for those said the wrong
        // thing about where they came from -- as would lighting CARTRIDGE MANAGER for a
        // self-check that came from the LCD, which is why Diag is not in this list.
        const int lit = m_menu == Menu::Patch ? voltaire::panel::BUT_PATCH_MENU
                      : m_menu == Menu::Tone  ? voltaire::panel::BUT_TONE
                      : m_menu == Menu::Reset ? voltaire::panel::BUT_RESET : -1;
        if (lit >= 0)
        {
            const auto &b = voltaire::panel::kButton[lit];
            beginPath();
            roundedRect(b.x, b.y, b.w, b.h, b.h * 0.15f);
            fillColor(Color(255, 255, 255, 0.22f));
            fill();
        }
        if (m_diveOpen)
        {
            const auto &d = voltaire::panel::kButton[voltaire::panel::BUT_DIVE];
            beginPath();
            roundedRect(d.x, d.y, d.w, d.h, d.h * 0.15f);
            fillColor(Color(255, 255, 255, 0.22f));
            fill();
        }
        if (m_hf)
        {
            const auto &f = voltaire::panel::kButton[voltaire::panel::BUT_FILTER];
            beginPath();
            roundedRect(f.x, f.y, f.w, f.h, f.h * 0.08f);
            fillColor(Color(255, 255, 255, 0.14f));
            fill();
        }
        for (int i = 0; i < voltaire::panel::BUTTONID_COUNT; i ++)
        {
            if (!m_latch[i])
                continue;
            const auto &l = voltaire::panel::kButton[i];
            beginPath();
            roundedRect(l.x, l.y, l.w, l.h, l.h * 0.15f);
            fillColor(Color(255, 255, 255, 0.22f));
            fill();
            // A latch outlives the click that made it, so it is outlined as well as lit:
            // a button that is merely bright looks like one the pointer is over.
            strokeColor(Color(0, 163, 224, 0.9f));
            strokeWidth(1.5f);
            stroke();
        }

        if (m_held < 0)
            return;
        const auto &r = voltaire::panel::kButton[m_held];
        beginPath();
        roundedRect(r.x, r.y, r.w, r.h, r.h * 0.15f);
        fillColor(Color(255, 255, 255, 0.22f));
        fill();
    }

    NSVGimage *m_svg = nullptr;
    NanoImage m_background;
    bool m_backgroundTried = false;

    // ---- the DIVE drawer
    //
    // Two rows of tabs select what is shown.  Row 1 picks SET, COMMON or one of the six
    // parts; row 2 only exists for a part, and picks which group of its parameters is on
    // show.  So the page being drawn is a function of both, and which of the drawer's
    // three heights the window takes is a function of row 1 alone.
    bool m_diveOpen = false;
    int m_diveTab1 = voltaire::panel::TAB_SET;
    int m_diveTab2 = voltaire::panel::TAB_BASIC;
    NSVGimage *m_pageSvg[voltaire::panel::DIVEPAGE_COUNT] = { nullptr };
    NanoImage m_pageRaster[voltaire::panel::DIVEPAGE_COUNT];
    bool m_pageTried[voltaire::panel::DIVEPAGE_COUNT] = { false };

    // One entry per control in the artwork: which row of kParams describes it, what the
    // machine last said its value was, and whether it has said anything yet.  Resolved
    // once at startup, so a control the table has no row for is reported then rather
    // than being silently dead.
    int m_paramOf[voltaire::panel::kDiveControlCount] = { 0 };

    // Values are held by ADDRESS, not by control, because two pages share a control name
    // and mean different parameters by it -- and because that is the shape the machine
    // answers in.  m_valuePart says which part the part table describes.
    uint8_t m_partVal[0x20] = { 0 };
    bool m_partHave[0x20] = { false };
    uint8_t m_commonVal[0x20] = { 0 };
    bool m_commonHave[0x20] = { false };
    int m_valuePart = -1;
    uint8_t m_setup[4] = { 0 };
    bool m_setupKnown = false;
    char m_patchName[voltaire::dive::kNameLen + 1] = { 0 };

    bool m_nameEdit = false;     ///< the patch name field has the keyboard
    bool m_caretOn = true;
    double m_caretAt = 0.0;
    char m_nameBuf[voltaire::dive::kNameLen + 1] = { 0 };
    int m_nameCaret = 0;
    bool m_diveNeedRead = false; ///< the open page's values are stale
    int m_diveRetry = 0;         ///< idles left before asking again
    int m_dragCtl = -1;          ///< the slider being dragged, or -1
    int m_hoverCtl = -1;         ///< what the pointer is over, for the tooltip
    float m_tipX = 0.0f, m_tipY = 0.0f;
    bool m_dirty = true;
    const bool m_countFrames = std::getenv("VOLTAIRE_FPS") != nullptr;

    uint8_t m_lcd[32];
    uint8_t m_cgram[64];
    uint32_t m_cgramIn = 0;
    uint8_t m_leds = 0, m_cursorPos = 0, m_cursorFlags = 0;

    enum class Menu { None, Patch, Tone, Value, Diag, About, Reset,
                      WriteConfirm, BankPick, PresetActions, DeleteConfirm,
                      OverwriteConfirm };
    Menu m_menu = Menu::None;

    /// Which slot the WRITE about to be confirmed would overwrite, or -1.
    int m_writeTarget = -1;

    /// The PATCH menu opened to STORE the patch rather than to choose one.
    ///
    /// Same two tabs either way, because the question "where does this patch go" has the
    /// same two answers as "where do patches come from": one of the machine's 64 slots, or
    /// the library.  The DIVE common page's WRITE opens it this way, the panel's PATCH
    /// button opens it the other, and the verbs change rather than the menu.
    bool m_writeMode = false;

    // ---- the user's own library.
    //
    // The UI owns every file operation.  The DSP is handed one 116-byte record and told
    // which slot to put it in, and that is the whole of what crosses over.
    voltaire::preset::Index m_library;
    std::string m_libraryDir;
    std::string m_patchDump;      ///< the machine's current patch, 232 hex characters
    std::string m_libraryNote;    ///< what just happened, shown in the menu's header
    bool m_libraryTab = false;    ///< which half of the PATCH menu is showing
    int  m_libScroll = 0;         ///< first preset shown, for libraries bigger than a page

    /// Which bank the browser is showing.  A bank is a NAME a preset claims and nothing
    /// else -- no directory, no container, no index -- so this filter is the whole of what
    /// a bank does to the browser.
    enum class BankFilter { All, Named, Unfiled };
    BankFilter  m_bankMode = BankFilter::All;
    std::string m_bankName;
    int         m_bankScroll = 0;     ///< first chip shown, when there are more than fit

    /// The preset the bank picker is about to file, by path: an index would be stale the
    /// moment another instance saved something.
    // ---- what "Override Patch" needs to know.
    //
    // Recalling a preset, editing it and saving it back over itself is the ordinary way to
    // work on a patch, and doing that by saving a second copy and then deleting the first
    // is a chore.  So the browser remembers WHICH file the machine is playing, and offers
    // to write straight back to it -- but only while that is still true, which takes three
    // conditions rather than one.
    std::string m_loadedPath;          ///< the library file the machine is playing, or ""
    std::string m_loadedRecord;        ///< its 116 bytes as loaded, hex
    std::string m_loadedLcdName;       ///< the ten-byte name it had then
    std::string m_loadedBank;
    float       m_loadedVolume = 0.0f;
    bool        m_loadedHf = true;

    /// The preset a box is acting on, by path: an index would be stale the moment another
    /// instance saved something.  Empty in the bank box means "choose which to browse".
    std::string m_presetTargetPath;
    std::string m_presetTargetName;
    std::string m_presetTargetBank;
    /// Banks named in this UI that no patch has claimed yet.
    ///
    /// A bank exists on disk only because a patch says so, and that is what keeps the list
    /// honest -- but it made an empty bank vanish as soon as you looked away, which is
    /// exactly when somebody who has just made one is going off to find patches to put in
    /// it.  So the names live here for as long as the window is open.  Nothing is written:
    /// close the plugin and an empty bank is gone, having never been anything but an
    /// intention.
    std::vector<std::string> m_sessionBanks;
    bool m_hoverBankZone = false;   ///< the pointer is over a row's bank, not its name
    bool m_hoverFieldOk = false;    ///< the pointer is over a field's OK button
    bool m_hoverClose = false;      ///< the pointer is over the palette's X

    /// Where the X was last drawn, and for which palette.
    HeaderRect m_closeRect { 0.0f, 0.0f, 0.0f, 0.0f };
    Menu m_closeFor = Menu::None;

    /// Hover values that are not grid cells.
    static constexpr int kHoverSave = -2;
    static constexpr int kHoverOverride = -3;
    /// The one text field the library's boxes share.  Two of them need typing -- naming a
    /// bank and renaming a preset -- and one field with a purpose is better than two fields
    /// that have to be kept in step.
    enum class Typing { None, NewBank, RenamePreset };
    Typing m_typing = Typing::None;
    char   m_typeBuf[64] = { 0 };
    int    m_typeCaret = 0;

    // The About text as lines, split once at startup; m_aboutRows is those lines wrapped
    // to the window's current width, which only the renderer can know.
    std::vector<std::string> m_aboutLines;
    std::vector<std::string> m_aboutRows;
    int m_aboutScroll = 0;
    int m_aboutLast = 0;

    // ---- the self-diagnosis, and what the LCD says while the machine is silent.
    /// About two seconds at the UI's idle rate.  Long enough that a host which is simply
    /// slow to start is not accused of anything.
    static constexpr int kDiagWaitIdles = 120;
    std::vector<std::string> m_diagLines;
    std::string m_diagStatus;
    bool m_panelSeen = false;
    bool m_diagAsked = false;
    int  m_diagWait = 0;
    int  m_diagScroll = 0;
    int m_valueCtl = -1;          ///< which DIVE control the value list belongs to

    uint8_t m_patch = 0;
    std::vector<std::string> m_patchNames;
    int m_menuHover = -1;

    std::vector<ToneGroup> m_toneGroups;
    uint8_t m_partMedia[6] = { 0 }, m_partTone[6] = { 0 };
    uint8_t m_partFlags[6] = { 0 }, m_partChan[6] = { 0 };
    int m_tonePart = 0, m_toneGroup = 0;
    ToneHit m_toneHover;

    const char *m_font = "sans";

    float m_volume = 0.0f;
    bool m_hf = true;
    int m_held = -1;
    bool m_latch[voltaire::panel::BUTTONID_COUNT] = { false };
    bool m_testMode = false;
    bool m_dragKnob = false;
    double m_dragY = 0;
    float m_dragStart = 0;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Voltaire110UI)
};

UI *createUI() { return new Voltaire110UI(); }

END_NAMESPACE_DISTRHO
