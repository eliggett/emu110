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
#include "u110_cgrom.h"

#include <cmath>
#include <cstdio>
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
};

class Voltaire110UI : public UI
{
public:
    Voltaire110UI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT)
    {
        loadArtwork();
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
        m_patch = blob.patch;
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
    void uiIdle() override
    {
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
        const float oy = (getHeight() - voltaire::panel::kDesignHeight * s) * 0.5f;

        // Ground behind the panel, so a resized window does not show through.
        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(20, 20, 22);
        fill();

        save();
        translate(ox, oy);
        scale(s, s);

        drawArtwork();
        drawLcd();
        drawLeds();
        drawKnob();
        drawButtonFeedback();

        restore();

        // Outside the panel transform on purpose: the menu is in window pixels.
        if (m_menuOpen)
            drawMenu();

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
        // The menu is modal while it is up: it takes the click wherever it lands, so a
        // click meant to dismiss it cannot also press whatever is underneath.
        if (m_menuOpen)
        {
            if (!ev.press)
                return true;
            const int hit = menuHit(float(ev.pos.getX()), float(ev.pos.getY()));
            m_menuOpen = false;
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

        const float s = panelScale();
        const float x = (ev.pos.getX() - (getWidth() - voltaire::panel::kDesignWidth * s) * 0.5f) / s;
        const float y = (ev.pos.getY() - (getHeight() - voltaire::panel::kDesignHeight * s) * 0.5f) / s;

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
                    if (i == voltaire::panel::BUT_PATCH_MENU)
                    {
                        // Not a toggle: a click while the menu is up never reaches here,
                        // because the menu takes it first and closes on anything that is
                        // not one of its entries -- this button included.
                        m_menuOpen = true;
                        m_menuHover = -1;
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
        if (m_menuOpen)
        {
            const int hit = menuHit(float(ev.pos.getX()), float(ev.pos.getY()));
            if (hit != m_menuHover)
            { m_menuHover = hit; repaint(); }
            return true;
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
        if (m_menuOpen)
            return true;
        const auto &k = voltaire::panel::kVolumeKnob;
        const float s = panelScale();
        const float x = (ev.pos.getX() - (getWidth() - voltaire::panel::kDesignWidth * s) * 0.5f) / s;
        const float y = (ev.pos.getY() - (getHeight() - voltaire::panel::kDesignHeight * s) * 0.5f) / s;
        if (std::hypot(x - k.cx, y - k.cy) > k.r * 1.3f)
            return false;
        float v = m_volume + ev.delta.getY() * 0.5f;
        v = v < -3.0f ? -3.0f : (v > 16.0f ? 16.0f : v);
        m_volume = v; setParam(kParamVolume, v); repaint();
        return true;
    }

    bool onKeyboard(const KeyboardEvent &ev) override
    {
        if (!m_menuOpen || !ev.press || ev.key != kKeyEscape)
            return false;
        m_menuOpen = false;
        m_menuHover = -1;
        repaint();
        return true;
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

    MenuLayout menuLayout() const
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
    int menuHit(float px, float py) const
    {
        const MenuLayout m = menuLayout();
        const float gx = px - (m.x + m.rowH * 0.5f);
        const float gy = py - (m.y + m.headerH);
        if (gx < 0.0f || gy < 0.0f)
            return -1;
        const int col = int(gx / m.colW), row = int(gy / m.rowH);
        if (col >= kMenuCols || row >= kMenuRows)
            return -1;
        return col * kMenuRows + row;
    }

    void drawMenu()
    {
        const MenuLayout m = menuLayout();

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

    float panelScale() const
    {
        const float sx = float(getWidth()) / voltaire::panel::kDesignWidth;
        const float sy = float(getHeight()) / voltaire::panel::kDesignHeight;
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
            beginPath();
            for (NSVGpath *p = sh->paths; p != nullptr; p = p->next)
            {
                moveTo(p->pts[0], p->pts[1]);
                for (int i = 0; i < p->npts - 1; i += 3)
                {
                    const float *q = &p->pts[i * 2];
                    bezierTo(q[2], q[3], q[4], q[5], q[6], q[7]);
                }
                if (p->closed)
                    closePath();

                // NanoVG FORCES every subpath to CCW unless told otherwise, so the
                // counters inside letters -- the hole in an "o" -- get reversed and the
                // glyph fills solid.  nanosvg already hands over correctly opposed
                // windings, so preserving each subpath's own direction is the whole fix.
                pathWinding(subpathArea(p) >= 0.0f ? CCW : CW);
            }
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

    static Color nvgCol(unsigned int c, float opacity)
    {
        return Color(int(c & 0xff), int((c >> 8) & 0xff), int((c >> 16) & 0xff),
                     int(((c >> 24) & 0xff) * opacity));
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
        if (m_menuOpen)
        {
            const auto &b = voltaire::panel::kButton[voltaire::panel::BUT_PATCH_MENU];
            beginPath();
            roundedRect(b.x, b.y, b.w, b.h, b.h * 0.15f);
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
    bool m_dirty = true;
    const bool m_countFrames = std::getenv("VOLTAIRE_FPS") != nullptr;

    uint8_t m_lcd[32];
    uint8_t m_cgram[64];
    uint32_t m_cgramIn = 0;
    uint8_t m_leds = 0, m_cursorPos = 0, m_cursorFlags = 0;

    uint8_t m_patch = 0;
    std::vector<std::string> m_patchNames;
    bool m_menuOpen = false;
    int m_menuHover = -1;
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
