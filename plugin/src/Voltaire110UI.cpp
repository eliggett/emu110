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
#include <vector>

START_NAMESPACE_DISTRHO

// Kept in step with the plugin by hand; there are only a few and they are checked by the
// selftest, which fails loudly if the port layout moves.
enum Params
{
    kParamVolume = 0, kParamHfCorrection,
    kParamButtonFirst,
    kParamOutFirst = kParamButtonFirst + 6,
    kParamOutLcd0 = kParamOutFirst,
    kParamOutLcdLast = kParamOutLcd0 + 10,
    kParamOutStatus, kParamOutCgramIndex, kParamOutCgramA, kParamOutCgramB,
    kParamCount
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
        if (index >= kParamOutLcd0 && index <= kParamOutLcdLast)
        {
            const uint32_t v = uint32_t(value);
            for (uint32_t k = 0; k < 3; k ++)
            {
                const uint32_t c = (index - kParamOutLcd0) * 3 + k;
                if (c < 32)
                    m_lcd[c] = uint8_t((v >> (8 * k)) & 0xff);
            }
            repaint();
        }
        else if (index == kParamOutStatus)
        {
            const uint32_t v = uint32_t(value);
            m_leds = uint8_t(v & 0xff);
            m_cursorPos = uint8_t((v >> 8) & 0xff);
            m_cursorFlags = uint8_t((v >> 16) & 0xff);
        }
        else if (index == kParamOutCgramIndex) { m_cgramIn = uint32_t(value) & 7; }
        else if (index == kParamOutCgramA)
        {
            const uint32_t v = uint32_t(value);
            for (uint32_t r = 0; r < 4; r ++)
                m_cgram[m_cgramIn * 8 + r] = uint8_t((v >> (5 * r)) & 0x1f);
        }
        else if (index == kParamOutCgramB)
        {
            const uint32_t v = uint32_t(value);
            for (uint32_t r = 0; r < 4; r ++)
                m_cgram[m_cgramIn * 8 + 4 + r] = uint8_t((v >> (5 * r)) & 0x1f);
        }
        else if (index == kParamVolume) { m_volume = value; repaint(); }
        else if (index == kParamHfCorrection) { m_hf = value > 0.5f; repaint(); }
    }

    // ---- drawing --------------------------------------------------------------------

    void onNanoDisplay() override
    {
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
    }

    // ---- input ----------------------------------------------------------------------

    bool onMouse(const MouseEvent &ev) override
    {
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

private:
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
        drawLed(voltaire::panel::LED_MIDI, false, Color(255, 60, 40));
        drawLed(voltaire::panel::LED_CLIP, false, Color(255, 60, 40));
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
        // A latching button shows its state, not a momentary press.
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

    uint8_t m_lcd[32];
    uint8_t m_cgram[64];
    uint32_t m_cgramIn = 0;
    uint8_t m_leds = 0, m_cursorPos = 0, m_cursorFlags = 0;

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
