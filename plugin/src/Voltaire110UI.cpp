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
#include "DiveParams.h"

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

    void uiIdle() override
    {
        if (m_nameEdit)
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

        drawBackground();
        drawArtwork();
        drawDivePage();
        drawLcd();
        drawLeds();
        drawKnob();
        drawButtonFeedback();

        restore();

        // Outside the panel transform on purpose: the menus are in window pixels.
        if (m_menu == Menu::Patch)
            drawPatchMenu();
        else if (m_menu == Menu::Tone)
            drawToneMenu();
        else if (m_menu == Menu::Value)
            drawValueMenu();
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
        // A menu is modal while it is up: it takes the click wherever it lands, so a
        // click meant to dismiss it cannot also press whatever is underneath.
        if (m_menu == Menu::Patch)
        {
            if (!ev.press)
                return true;
            const int hit = patchHit(float(ev.pos.getX()), float(ev.pos.getY()));
            m_menu = Menu::None;
            m_menuHover = -1;
            if (hit >= 0 && size_t(hit) < m_patchNames.size())
            {
                char n[8];
                std::snprintf(n, sizeof(n), "%d", hit);
                setState("patchsel", n);
            }
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
                        m_menuHover = -1;
                        m_toneHover = ToneHit();
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
        if (m_menu == Menu::Patch)
        {
            const int hit = patchHit(float(ev.pos.getX()), float(ev.pos.getY()));
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

    bool onKeyboard(const KeyboardEvent &ev) override
    {
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
        m_menu = Menu::None;
        m_menuHover = -1;
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

    struct MenuLayout { float x, y, w, h, rowH, colW, headerH, fontSize; };

    MenuLayout patchLayout() const
    {
        MenuLayout m;
        m.rowH = (float(getHeight()) - 12.0f) / float(kMenuRows + 2);
        m.rowH = m.rowH < 9.0f ? 9.0f : (m.rowH > 26.0f ? 26.0f : m.rowH);
        m.fontSize = m.rowH * 0.68f;
        m.colW = m.fontSize * 9.0f;
        m.headerH = m.rowH * 1.6f;
        m.w = kMenuCols * m.colW + m.rowH;               // half a row of margin each side
        m.h = m.headerH + kMenuRows * m.rowH + m.rowH * 0.5f;
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
        m.rowH = (float(getHeight()) - 12.0f) / float(kMenuRows + 2);
        m.rowH = m.rowH < 9.0f ? 9.0f : (m.rowH > 26.0f ? 26.0f : m.rowH);
        m.fontSize = m.rowH * 0.68f;
        m.colW = m.fontSize * 5.0f;
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

        fontFace(m_font);
        fontSize(m.fontSize * 1.05f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(Color(235, 235, 235));
        text(m.x + m.rowH * 0.5f, m.y + m.headerH * 0.5f, "PATCH", nullptr);

        fontSize(m.fontSize * 0.8f);
        textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
        fillColor(Color(150, 155, 165));
        // The machine's own 64.  A bank of the user's own is the other half of this menu
        // and is not written yet; saying which list this is now means the second one can
        // arrive without the first changing meaning.
        text(m.x + m.w - m.rowH * 0.5f, m.y + m.headerH * 0.5f,
             m_patchNames.empty() ? "waiting for the machine" : "internal patches", nullptr);

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
        m.rowH = (float(getHeight()) - 12.0f) / float(kToneRows + 5);
        m.rowH = m.rowH < 9.0f ? 9.0f : (m.rowH > 24.0f ? 24.0f : m.rowH);
        m.fontSize = m.rowH * 0.68f;
        m.colW = m.fontSize * 9.0f;
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
    void resizeToDrawer()
    {
        const uint w = getWidth();
        const uint h = uint(std::lround(double(w) * designHeight()
                                        / voltaire::panel::kDesignWidth));
        if (h != getHeight())
            setSize(w, h);
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
            // Storing the temporary patch is the machine's own WRITE procedure, driven
            // from the front panel; there is no SysEx for it.  Not built yet, and saying
            // so beats appearing to have saved something.
            d_stderr("DIVE: WRITE is not wired up yet");
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

    void setParam(uint32_t index, float value)
    {
        editParameter(index, true);
        setParameterValue(index, value);
        editParameter(index, false);
    }

    /// Panel button -> plugin parameter.  The artwork carries controls the machine does
    /// not have (FILTER, RESET, PATCH, TONE, DIVE, CARTRIDGE MANAGER); those are drawn and
    /// clickable but do nothing yet, which is what was asked for.
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
            // windings, so preserving each subpath's own direction is the whole fix.
            pathWinding(subpathArea(p) >= 0.0f ? CCW : CW);
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

        for (int cell = 0; cell < 32; cell ++)
        {
            const int col = cell % 16, row = cell / 16;
            const uint8_t code = m_lcd[cell];
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
        if (m_menu != Menu::None)
        {
            const auto &b = voltaire::panel::kButton[m_menu == Menu::Tone
                    ? voltaire::panel::BUT_TONE : voltaire::panel::BUT_PATCH_MENU];
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

    enum class Menu { None, Patch, Tone, Value };
    Menu m_menu = Menu::None;
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
    bool m_dragKnob = false;
    double m_dragY = 0;
    float m_dragStart = 0;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Voltaire110UI)
};

UI *createUI() { return new Voltaire110UI(); }

END_NAMESPACE_DISTRHO
