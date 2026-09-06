#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Extract the Voltaire 110 panel geometry from the Inkscape artwork.

The SVG is the single source of truth for panel geometry: no coordinate is ever
typed into the C++.  This reads resources/graphics/overall_panel_inkscape.svg,
composes every transform down to canvas coordinates, and emits

    plugin/generated/panel_geometry.h     geometry as constexpr tables
    plugin/generated/panel_geometry.json  the same, for tooling and debugging
    plugin/generated/panel_flat.svg       the vector artwork, text flattened
    plugin/generated/panel_svg.h          the same, embedded as a C string
    plugin/generated/panel_background.png rasters, pre-rendered by rsvg-convert
    plugin/generated/panel_background.h   the same, embedded as bytes
    plugin/generated/dive_<page>_flat.svg one per DIVE page, in panel coordinates
    plugin/generated/dive_pages.h         all of them, embedded

The DIVE drawer is drawn from seven documents, not one.  The panel is the frame and
each tab's content is its own Inkscape file, so that a page can be laid out without
the other six in the way.  They are NOT merged into a single SVG: every page names
its content box `rect11` and its slider graticules `path290`, so a merge would
collide ids -- and ids are how the UI finds a shape to recolour.  Each page is
flattened separately and translated into panel coordinates by the exporter, using
the `dive_controls_max_outline` rect that appears in both documents as the anchor.

It also LINTS the artwork against nanosvg's subset.  nanosvg ignores what it
does not understand, silently, so an unsupported construct shows up as a missing
element at runtime with no error anywhere.  Catching it here is much cheaper.

Elements are recognised by their Inkscape label:

    BUT_<name>    a button          -> hit rect
    LED_<name>    an indicator      -> draw rect
    KNOB_<name>_outline   a knob    -> centre + radius
    KNOB_<name>_pointer   its needle-> shape id + pivot + zero angle
    LCD_outer / LCD_inner           -> bezel and glass
    VU_<name>     a meter           -> draw rect
    T_<name>      screenprint text  -> drawn by nanosvg; its id is exported so the
                                       UI can recolour it (a selected tab)
    M_<name>      a menu button     -> hit rect, value drawn in the LCD font
    SB_<name>     a slider body     -> the group; its graticules give the travel
    ST_<name>     a slider tap      -> the draggable part, at its zero position
    LCD_<name>    an LCD-style field-> hit/draw rect
    L_<name>      a section frame   -> rounded rect with a gap for T_<name>

Usage:
    plugin/tools/panel_export.py                 # extract
    plugin/tools/panel_export.py --check         # lint only, non-zero on error
    plugin/tools/panel_export.py --text-to-path  # also flatten text to paths
    plugin/tools/panel_export.py --background    # also pre-render the rasters

--text-to-path needs Inkscape and --background needs rsvg-convert.  Both are
needed only to REGENERATE; building from a clean tree runs them once.
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

SVG = 'http://www.w3.org/2000/svg'
INK = 'http://www.inkscape.org/namespaces/inkscape'

# ElementTree invents prefixes (ns0:svg, ns0:path) unless the default namespace is
# registered.  nanosvg matches tag names LITERALLY, so a prefixed document parses without
# error and yields zero shapes -- the panel simply does not draw, with nothing to see in
# any log.  Registering this is the whole fix.
ET.register_namespace('', SVG)
ET.register_namespace('inkscape', INK)
ET.register_namespace('sodipodi', 'http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd')
# An <image> carries its data in xlink:href.  Without this registration ElementTree
# invents a prefix, and rsvg-convert then renders a background with nothing in it.
ET.register_namespace('xlink', 'http://www.w3.org/1999/xlink')

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GFX = os.path.join(ROOT, 'resources/graphics')
SVG_PATH = os.path.join(GFX, 'overall_panel_inkscape.svg')
OUT_DIR = os.path.join(ROOT, 'plugin/generated')

# The DIVE pages, in tab order.  `scoot` says the page is shown with the second tab
# row hidden, so everything on it moves up by exactly that row's height; the two
# pages that have no per-part sub-tabs are the two that scoot.  Doing it here rather
# than in Inkscape means each page is drawn once, at one set of coordinates, and the
# artwork does not carry two copies of the same layout.
DIVE_PAGES = [
    ('set',    'dive_setup.svg',  True),
    ('common', 'dive_common.svg', True),
    ('basic',  'dive_basic.svg',  False),
    ('level',  'dive_level.svg',  False),
    ('pitch',  'dive_pitch.svg',  False),
    ('lfo',    'dive_LFO.svg',    False),
]

# The rect that appears in BOTH the panel and every page, and so fixes where a page
# lands.  Inkscape puts it at (1,1) on a page and at (2,312.26) on the panel; the
# exporter never needs to be told the offset, it measures it.
ANCHOR = 'dive_controls_max_outline'

# Layers whose contents are editing aids, not part of the rendered panel.  A layer
# whose name ends in _do_not_include is the artwork saying so itself, which is the
# convention to prefer -- the two named here predate it.
#
# 'Foreground Text as Text' used to be listed too, back when a sibling layer held
# paths built from it.  That layer is gone: the lettering is now flattened from the
# text on every export, which is what makes a label editable in Inkscape as text.
# The one thing still exempt from flattening is a hand-adjusted X_as_path pair.
SKIP_LAYERS = {'Example_LCD_Testing_only'}
SKIP_LAYER_SUFFIX = '_do_not_include'

# nanosvg understands fills, strokes and linear/radial gradients.  Everything
# here is silently dropped by it.
UNSUPPORTED_TAGS = {'filter', 'clipPath', 'mask', 'pattern', 'use', 'image',
                    'switch', 'foreignObject', 'marker', 'symbol'}
UNSUPPORTED_ATTRS = {'filter', 'mask'}

# ...with two exceptions, which this script HANDLES rather than warns about.
#
# nanosvg has no <image> element at all -- its element dispatch (plugin/src/nanosvg.h)
# knows g, path, rect, circle, ellipse, line, polyline, polygon and the gradients, and
# skips everything else without a word.  So the artwork is split in two at export time:
# every <image>, with its transform and its clip-path, is rendered ONCE by rsvg-convert
# into panel_background.png, and the vector remainder becomes panel_flat.svg.  The UI
# draws the PNG and then the vectors over it.
#
# Two consequences worth knowing.  Rasters always end up BEHIND the vector artwork
# whatever their z-order in Inkscape, because they are a separate layer by the time the
# UI sees them.  And the background is resolution-limited where the vectors are not:
# BACKGROUND_SCALE sets how much detail is kept, at roughly 660 KB per multiple.
HANDLED_TAGS = {'image', 'clipPath'}
BACKGROUND_SCALE = 2.0

# A clip-path is dropped by nanosvg too, so anything wearing one goes through the same
# pre-render.  The rule is uniform on purpose -- "if nanosvg cannot clip it, rsvg draws
# it" needs no list of exceptions and cannot fall out of date.  Two constructs in the
# artwork rely on it: the section frames (L_*), whose gap for the title is an Inkscape
# power clip, and the slider graticules, which are clipped where the slider body covers
# them.  Both land behind the page's vectors, which is the z-order they already had.
#
# These are strokes rather than photographs, so they are rendered at a higher scale than
# the background: a few lines on a transparent ground cost almost nothing compressed.
FRAME_SCALE = 4.0

# Rendering the background needs a renderer that implements clip paths, which is the
# whole reason not to do it in the UI: rsvg-convert already has one.
RSVG = 'rsvg-convert'


# ------------------------------------------------------------- editing aids

def editing_aid_labels(root):
    """Labels whose element is a human-editable SOURCE, not artwork to render.

    An element labelled X is an aid when one labelled X_as_path exists: the path is
    what draws, and X is what the path was originally made from.  Keeping the pair
    is how the lettering stays editable in Inkscape.

    The rule matters in two places.  The linter must not complain that X is <text>
    nanosvg cannot draw -- it is not meant to be drawn.  And the flattener must not
    convert X to a path, because that path is already there and MAY HAVE BEEN
    ADJUSTED BY HAND since; regenerating it would silently throw the adjustment
    away.  The artwork's own naming is what says so, so nothing here is a list to
    keep up to date.
    """
    labels = {e.get(f'{{{INK}}}label') for e in root.iter()}
    return {l[:-len('_as_path')] for l in labels
            if l and l.endswith('_as_path') and len(l) > len('_as_path')}


# ---------------------------------------------------------------- transforms

IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def mat_mul(m, n):
    """Compose two SVG matrices: apply n, then m (SVG's own convention)."""
    a, b, c, d, e, f = m
    A, B, C, D, E, F = n
    return (a * A + c * B, b * A + d * B,
            a * C + c * D, b * C + d * D,
            a * E + c * F + e, b * E + d * F + f)


def mat_apply(m, x, y):
    a, b, c, d, e, f = m
    return (a * x + c * y + e, b * x + d * y + f)


def parse_transform(s):
    """Parse an SVG transform attribute into a single matrix."""
    if not s:
        return IDENTITY
    m = IDENTITY
    for name, args in re.findall(r'(\w+)\s*\(([^)]*)\)', s):
        v = [float(x) for x in re.split(r'[,\s]+', args.strip()) if x]
        if name == 'translate':
            n = (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0.0)
        elif name == 'matrix':
            n = tuple(v)
        elif name == 'scale':
            n = (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0)
        elif name == 'rotate':
            a = math.radians(v[0])
            ca, sa = math.cos(a), math.sin(a)
            n = (ca, sa, -sa, ca, 0, 0)
            if len(v) == 3:                       # rotate about a point
                cx, cy = v[1], v[2]
                n = mat_mul(mat_mul((1, 0, 0, 1, cx, cy), n),
                            (1, 0, 0, 1, -cx, -cy))
        elif name in ('skewX', 'skewY'):
            t = math.tan(math.radians(v[0]))
            n = (1, t, 0, 1, 0, 0) if name == 'skewY' else (1, 0, t, 1, 0, 0)
        else:
            continue
        m = mat_mul(m, n)
    return m


def rotation_of(s):
    """Return (angle_deg, cx, cy) of a bare rotate(), or None."""
    if not s:
        return None
    for name, args in re.findall(r'(\w+)\s*\(([^)]*)\)', s):
        if name != 'rotate':
            continue
        v = [float(x) for x in re.split(r'[,\s]+', args.strip()) if x]
        return (v[0], v[1], v[2]) if len(v) == 3 else (v[0], 0.0, 0.0)
    return None


# ----------------------------------------------------------------- path bbox

PATH_TOKENS = re.compile(r'([MmZzLlHhVvCcSsQqTtAa])|(-?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)')


def path_points(d):
    """Every control point of a path, in the path's own coordinates.

    Control points, not the true outline: a bezier is contained by its hull, so this
    can only ever be too generous, and never by much on the shapes it is asked about
    -- slider graticules, which are straight, and rounded rectangles, whose corner
    controls sit on the box.  Being exact would mean subdividing curves for no gain.
    """
    if not d:
        return []
    toks = [(c, n) for c, n in PATH_TOKENS.findall(d)]
    pts, i = [], 0
    x = y = sx = sy = 0.0
    cmd = None
    # how many numbers each command eats, and which of its pairs are coordinates
    argc = dict(M=2, L=2, H=1, V=1, C=6, S=4, Q=4, T=2, A=7, Z=0)
    while i < len(toks):
        c, n = toks[i]
        if c:
            cmd = c
            i += 1
            if cmd in 'Zz':
                x, y = sx, sy
                continue
        if cmd is None:
            return pts
        up = cmd.upper()
        rel = cmd.islower()
        k = argc[up]
        nums = []
        while len(nums) < k and i < len(toks) and not toks[i][0]:
            nums.append(float(toks[i][1]))
            i += 1
        if len(nums) < k:
            break
        if up == 'H':
            x = x + nums[0] if rel else nums[0]
        elif up == 'V':
            y = y + nums[0] if rel else nums[0]
        elif up == 'A':
            # only the endpoint is a coordinate; the radii are not points on the path
            x = x + nums[5] if rel else nums[5]
            y = y + nums[6] if rel else nums[6]
        else:
            ox, oy = x, y
            for j in range(0, k, 2):
                px = nums[j] + (ox if rel else 0.0)
                py = nums[j + 1] + (oy if rel else 0.0)
                pts.append((px, py))
                x, y = px, py
        pts.append((x, y))
        if up == 'M':
            sx, sy = x, y
            cmd = 'l' if rel else 'L'      # a second M coordinate pair is an implicit L
    return pts


def node_bbox(node, mat):
    """Bounding box of a subtree in canvas coordinates, or None if it has no geometry.

    This is what lets a group be measured.  A slider is authored as a group -- its
    graticules and its body -- and neither part alone says where the thing is.
    """
    box = None

    def add(m, px, py):
        nonlocal box
        gx, gy = mat_apply(m, px, py)
        if box is None:
            box = [gx, gy, gx, gy]
        else:
            box[0], box[1] = min(box[0], gx), min(box[1], gy)
            box[2], box[3] = max(box[2], gx), max(box[3], gy)

    def geom(n, m):
        tag = n.tag.split('}')[-1]
        if tag == 'rect':
            try:
                x, y, w, h = (float(n.get(k)) for k in ('x', 'y', 'width', 'height'))
            except (TypeError, ValueError):
                return
            add(m, x, y)
            add(m, x + w, y + h)
        elif tag in ('circle', 'ellipse'):
            cx, cy = float(n.get('cx', 0)), float(n.get('cy', 0))
            rx = float(n.get('r') or n.get('rx') or 0)
            ry = float(n.get('r') or n.get('ry') or 0)
            add(m, cx - rx, cy - ry)
            add(m, cx + rx, cy + ry)
        elif tag == 'path':
            for px, py in path_points(n.get('d')):
                add(m, px, py)

    def walk(n, m, root):
        # the caller's matrix already includes this node's own transform
        if not root:
            m = mat_mul(m, parse_transform(n.get('transform')))
        geom(n, m)
        for c in n:
            walk(c, m, False)

    walk(node, mat, True)
    return box


# ------------------------------------------------------------------ scanning

class Element:
    def __init__(self, label, eid, kind, x, y, w, h):
        self.label, self.id, self.kind = label, eid, kind
        self.x, self.y, self.w, self.h = x, y, w, h

    @property
    def cx(self):
        return self.x + self.w / 2.0

    @property
    def cy(self):
        return self.y + self.h / 2.0

    def as_dict(self):
        return dict(label=self.label, id=self.id, kind=self.kind,
                    x=round(self.x, 4), y=round(self.y, 4),
                    w=round(self.w, 4), h=round(self.h, 4))


def scan(svg_path):
    """Walk the SVG, returning (elements, pivots, warnings, canvas, rasters, travel)."""
    tree = ET.parse(svg_path)
    root = tree.getroot()

    vb = root.get('viewBox')
    if vb:
        parts = [float(v) for v in re.split(r'[,\s]+', vb.strip()) if v]
        canvas = (parts[2], parts[3])
    else:
        canvas = (float(root.get('width', 0)), float(root.get('height', 0)))

    elements, pivots, warnings = [], {}, []
    aids = editing_aid_labels(root)
    rasters, travel = [], {}

    def walk(node, mat, layer, in_skipped, in_defs, in_raster):
        tag = node.tag.split('}')[-1]
        label = node.get(f'{{{INK}}}label')
        eid = node.get('id')

        if node.get(f'{{{INK}}}groupmode') == 'layer':
            layer = label or eid
            if layer in SKIP_LAYERS or (layer or '').endswith(SKIP_LAYER_SUFFIX):
                in_skipped = True

        # An element paired with an X_as_path is a source, not artwork.  Its whole
        # subtree is exempt: it is neither drawn nor measured nor complained about.
        if label in aids:
            in_skipped = True

        # A <clipPath> holds geometry that is never drawn anywhere -- it only shapes
        # something else -- so it is neither a control nor a lint subject.
        if tag in ('clipPath', 'defs'):
            in_defs = True

        mat = mat_mul(mat, parse_transform(node.get('transform')))

        # Everything nanosvg cannot draw goes to the raster pass instead: <image>,
        # which it has no element for, and anything wearing a clip-path, which it
        # parses and then ignores.  Recording the id here is what lets the flattener
        # take the same elements OUT of the vector artwork, so the two halves stay
        # disjoint and nothing is drawn twice.
        if not in_skipped and not in_defs and not in_raster:
            if tag == 'image' or node.get('clip-path'):
                rasters.append(eid)
                in_raster = True

        if not in_skipped and not in_defs and not in_raster:
            if tag in UNSUPPORTED_TAGS:
                warnings.append(f'<{tag}> id={eid}: nanosvg ignores this entirely')
            for attr in UNSUPPORTED_ATTRS:
                if node.get(attr):
                    warnings.append(f'<{tag}> id={eid}: {attr}= is dropped by nanosvg')
            if node.get('style') and 'filter:' in node.get('style'):
                warnings.append(f'<{tag}> id={eid}: filter in style= is dropped by nanosvg')

        # A rotate() on a group whose child is a knob pointer is the knob's zero
        # position, not artwork to be flattened away.
        rot = rotation_of(node.get('transform'))
        if rot is not None and tag == 'g':
            parent = mat_mul(mat, parse_transform(None))
            # pivot expressed in the coordinate system OUTSIDE this rotate
            outer = mat
            for child in node.iter():
                clab = child.get(f'{{{INK}}}label') or ''
                if clab.startswith('KNOB_') and clab.endswith('_pointer'):
                    name = clab[len('KNOB_'):-len('_pointer')]
                    # undo this node's own rotate to get the pivot in canvas space
                    ang, px, py = rot
                    base = mat_mul(mat, invert_rotate(rot))
                    gx, gy = mat_apply(base, px, py)
                    pivots[name] = dict(angle_deg=ang, x=gx, y=gy,
                                        shape_id=child.get('id'))

        # The anchor is measured even inside a guide layer.  It is not artwork --
        # it is the registration mark that says where a page sits on the panel, and
        # a guide layer is exactly where it belongs.
        # A layer is a labelled <g> like any other, and measuring one would enter it in
        # the tables as a control the size of everything it contains -- an LCD layer
        # becomes an element called LCD.  Layers are structure, not artwork.
        is_layer = node.get(f'{{{INK}}}groupmode') == 'layer'
        if label and not is_layer and (not in_skipped or label == ANCHOR) and not in_defs:
            if tag == 'rect':
                try:
                    x, y, w, h = (float(node.get(k)) for k in ('x', 'y', 'width', 'height'))
                except (TypeError, ValueError):
                    x = y = w = h = None
                if x is not None:
                    x0, y0 = mat_apply(mat, x, y)
                    x1, y1 = mat_apply(mat, x + w, y + h)
                    elements.append(Element(label, eid, 'rect',
                                            min(x0, x1), min(y0, y1),
                                            abs(x1 - x0), abs(y1 - y0)))
            elif tag == 'circle':
                cx, cy = float(node.get('cx')), float(node.get('cy'))
                r = float(node.get('r'))
                gx, gy = mat_apply(mat, cx, cy)
                # uniform scale assumed; report the mean if it is not
                sx = math.hypot(mat[0], mat[1])
                sy = math.hypot(mat[2], mat[3])
                rr = r * (sx + sy) / 2.0
                elements.append(Element(label, eid, 'circle',
                                        gx - rr, gy - rr, 2 * rr, 2 * rr))
            elif tag == 'ellipse':
                cx, cy = float(node.get('cx')), float(node.get('cy'))
                rx, ry = float(node.get('rx')), float(node.get('ry'))
                gx, gy = mat_apply(mat, cx, cy)
                elements.append(Element(label, eid, 'ellipse',
                                        gx - rx, gy - ry, 2 * rx, 2 * ry))
            elif tag in ('g', 'path'):
                b = node_bbox(node, mat)
                if b is not None:
                    elements.append(Element(label, eid, tag,
                                            b[0], b[1], b[2] - b[0], b[3] - b[1]))
            elif tag == 'text':
                # A text box needs font metrics to measure and the exporter has none.
                # Its anchor is enough: what the UI wants from a label is its id, so
                # that a selected tab can be redrawn in a brighter colour.
                try:
                    gx, gy = mat_apply(mat, float(node.get('x')), float(node.get('y')))
                    elements.append(Element(label, eid, 'text', gx, gy, 0.0, 0.0))
                except (TypeError, ValueError):
                    pass

            # A slider is a group: graticules plus a body.  The ten graticules are
            # what bounds the travel -- the artwork's own statement of where the tap
            # may go -- and they are the only part of it the UI cannot guess.
            if label.startswith('SB_'):
                for kid in node.iter():
                    if kid.get(f'{{{INK}}}label') == 'slider_lines':
                        b = node_bbox(kid, mat_mul(mat, parse_transform(kid.get('transform'))))
                        if b is not None:
                            travel[label] = b
                        break

        for child in node:
            walk(child, mat, layer, in_skipped, in_defs, in_raster)

    walk(root, IDENTITY, None, False, False, False)

    # off-canvas check
    W, H = canvas
    for e in elements:
        if e.x < -0.5 or e.y < -0.5 or e.x + e.w > W + 0.5 or e.y + e.h > H + 0.5:
            warnings.append(f'{e.label}: extends outside the canvas '
                            f'({e.x:.1f},{e.y:.1f} {e.w:.1f}x{e.h:.1f})')

    return elements, pivots, warnings, canvas, rasters, travel


def invert_rotate(rot):
    ang, cx, cy = rot
    a = math.radians(-ang)
    ca, sa = math.cos(a), math.sin(a)
    n = (ca, sa, -sa, ca, 0, 0)
    return mat_mul(mat_mul((1, 0, 0, 1, cx, cy), n), (1, 0, 0, 1, -cx, -cy))


# -------------------------------------------------------------------- output

def c_ident(name):
    return re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_').upper()


# Which prefixes name a DIVE control, and what the UI does with each.
DIVE_KIND = {'BUT': 'DK_BUTTON', 'M': 'DK_MENU', 'SB': 'DK_SLIDER', 'LCD': 'DK_LCD'}


def emit_header(path, elements, pivots, canvas, svg_rel, pages=(), row2_h=0.0):
    by_prefix = {}
    for e in elements:
        pre = e.label.split('_', 1)[0]
        by_prefix.setdefault(pre, []).append(e)

    by_label = {e.label: e for e in elements}
    drawer = by_label.get('dive_with_tabs_max_outline')
    content = by_label.get(ANCHOR)

    # A button below the drawer's top edge is a tab, not a panel button.  The
    # artwork already says which is which by where it put them, so nothing here
    # needs a list of tab names to keep in step with Inkscape.
    top = drawer.y if drawer is not None else float('inf')
    # Rounded to a tenth of a millimetre before sorting: two tabs in the same row
    # can sit a ten-thousandth apart in the artwork, which is not a row boundary but
    # is enough to shuffle the enum from one export to the next.
    tabs = sorted((e for e in by_prefix.get('BUT', []) if e.y >= top - 0.01),
                  key=lambda e: (round(e.y, 1), e.x))
    rows = sorted({round(e.y, 1) for e in tabs})

    buttons = sorted((e for e in by_prefix.get('BUT', []) if e.y < top - 0.01),
                     key=lambda e: (e.y, e.x))
    leds = sorted(by_prefix.get('LED', []), key=lambda e: (e.y, e.x))
    meters = sorted(by_prefix.get('VU', []), key=lambda e: (e.y, e.x))
    lcds = {e.label: e for e in by_prefix.get('LCD', [])}
    knobs = {e.label: e for e in by_prefix.get('KNOB', [])}

    L = []
    A = L.append
    A('// GENERATED FILE -- do not edit.')
    A(f'// Produced by plugin/tools/panel_export.py from {svg_rel}')
    A('//')
    A('// Every panel coordinate lives in the Inkscape artwork.  To move a control,')
    A('// move it in Inkscape and re-run the exporter; nothing here is authored by hand.')
    A('')
    A('#pragma once')
    A('')
    A('namespace voltaire {')
    A('namespace panel {')
    A('')
    A('// Design-space units are the SVG user units (millimetres).  The UI scales')
    A('// the whole panel by one factor; no code should assume a pixel size.')
    A(f'inline constexpr float kDesignWidth  = {canvas[0]:.5f}f;')
    A(f'inline constexpr float kDesignHeight = {canvas[1]:.5f}f;')
    A('')
    A('struct Rect { float x, y, w, h; };')
    A('struct Knob { float cx, cy, r; float zero_deg; };')
    A('')

    def emit_table(name, enum, items, extra=None):
        if not items:
            A(f'// (no {name} in the artwork)')
            A('')
            return
        A(f'enum {enum} : int {{')
        for e in items:
            A(f'    {c_ident(e.label)},')
        A(f'    {enum.upper()}_COUNT')
        A('};')
        A('')
        A(f'inline constexpr Rect k{name}[{enum.upper()}_COUNT] = {{')
        for e in items:
            A(f'    {{ {e.x:9.4f}f, {e.y:9.4f}f, {e.w:8.4f}f, {e.h:8.4f}f }},'
              f'  // {e.label}')
        A('};')
        A('')
        A(f'inline constexpr const char *k{name}Name[{enum.upper()}_COUNT] = {{')
        for e in items:
            A(f'    "{e.label}",')
        A('};')
        A('')
        A(f'// SVG element ids, for looking shapes up in the parsed artwork.')
        A(f'inline constexpr const char *k{name}SvgId[{enum.upper()}_COUNT] = {{')
        for e in items:
            A(f'    "{e.id}",')
        A('};')
        A('')

    emit_table('Button', 'ButtonId', buttons)
    emit_table('Led', 'LedId', leds)
    emit_table('Meter', 'MeterId', meters)

    for label in ('LCD_outer', 'LCD_inner'):
        if label in lcds:
            e = lcds[label]
            A(f'inline constexpr Rect k{c_ident(label).title().replace("_","")} = '
              f'{{ {e.x:.4f}f, {e.y:.4f}f, {e.w:.4f}f, {e.h:.4f}f }};')
    A('')

    for name, piv in sorted(pivots.items()):
        outline = knobs.get(f'KNOB_{name}_outline')
        if outline is None:
            continue
        A(f'// The pointer shape "{piv["shape_id"]}" is drawn by the SVG; rotate it about')
        A(f'// (cx, cy).  zero_deg is where the artwork already points, so a value of')
        A(f'// 0.0 needs no rotation at all.')
        A(f'inline constexpr Knob k{name.title()}Knob = '
          f'{{ {outline.cx:.4f}f, {outline.cy:.4f}f, {outline.w / 2:.4f}f, '
          f'{piv["angle_deg"]:.4f}f }};')
        A(f'inline constexpr const char *k{name.title()}KnobPointerId = "{piv["shape_id"]}";')
        A('')

    emit_dive(A, canvas, tabs, rows, drawer, content, by_label, pages, row2_h,
              by_label.get('overall_outline'))

    A('} // namespace panel')
    A('} // namespace voltaire')
    A('')

    with open(path, 'w') as f:
        f.write('\n'.join(L))


def emit_dive(A, canvas, tabs, rows, drawer, content, panel_by_label, pages, row2_h,
              outline):
    """The DIVE drawer: its tabs, its three heights, and each page's controls."""
    if not tabs or drawer is None or content is None:
        A('// (no DIVE drawer in the artwork)')
        A('')
        return

    A('// ------------------------------------------------------------------ DIVE')
    A('//')
    A('// The drawer lives below the panel in the same artwork, so every coordinate')
    A('// here is in the same design space as the panel above it.  It has three')
    A('// heights: shut, open on a page with one tab row, and open on a page with')
    A('// two.  A page that shows no second row is drawn shifted up by exactly that')
    A('// row\'s height, and the geometry below already has the shift applied.')
    A('')
    # Shut, the window ends below the panel outline with the same margin the artwork
    # leaves at its left edge -- which is what the outline's 4-unit stroke needs to
    # not be sliced in half by the window edge.
    shut = (outline.y + outline.h + outline.x) if outline is not None else drawer.y
    A(f'inline constexpr float kPanelShutHeight = {shut:.5f}f;')
    A(f'inline constexpr float kDiveRow2Height  = {row2_h:.5f}f;')
    A(f'inline constexpr float kDiveOpenHeight  = {canvas[1]:.5f}f;')
    A(f'inline constexpr float kDiveOpenHeight1Row = {canvas[1] - row2_h:.5f}f;')
    A('')
    A('// The drawer body and its content box are the two shapes whose size depends')
    A('// on which page is open, so the UI draws them itself and skips the artwork\'s')
    A('// copies.  Same pattern as the knob pointer.')
    A(f'inline constexpr const char *kDiveBodySvgId    = "{drawer.id}";')
    A(f'inline constexpr const char *kDiveContentSvgId = "{content.id}";')
    A(f'inline constexpr Rect kDiveBody    = {{ {drawer.x:.4f}f, {drawer.y:.4f}f, '
      f'{drawer.w:.4f}f, {drawer.h:.4f}f }};')
    A(f'inline constexpr Rect kDiveContent = {{ {content.x:.4f}f, {content.y:.4f}f, '
      f'{content.w:.4f}f, {content.h:.4f}f }};')
    A('')

    # ---- the tabs
    A('enum DiveTabId : int {')
    for e in tabs:
        A(f'    TAB_{c_ident(e.label[4:])},')
    A('    DIVETABID_COUNT')
    A('};')
    A('')
    A('inline constexpr Rect kDiveTab[DIVETABID_COUNT] = {')
    for e in tabs:
        A(f'    {{ {e.x:9.4f}f, {e.y:9.4f}f, {e.w:8.4f}f, {e.h:8.4f}f }},  // {e.label}')
    A('};')
    A('')
    A('// 0 is the always-visible row; 1 only appears once a part tab is chosen.')
    A('inline constexpr int kDiveTabRow[DIVETABID_COUNT] = {')
    for e in tabs:
        A(f'    {rows.index(round(e.y, 1))},')
    A('};')
    A('')
    for what, get in (('SvgId', lambda e: e.id),
                      ('TextSvgId', lambda e: (panel_by_label[t].id
                                               if (t := text_id_for(panel_by_label, e.label))
                                               else None)),
                      ('Name', lambda e: e.label[4:])):
        A(f'inline constexpr const char *kDiveTab{what}[DIVETABID_COUNT] = {{')
        for e in tabs:
            v = get(e)
            A(f'    {chr(34) + v + chr(34) if v else "nullptr"},')
        A('};')
        A('')

    # ---- the pages
    A('enum DivePage : int {')
    for p in pages:
        A(f'    DIVE_{p.name.upper()},')
    A('    DIVEPAGE_COUNT')
    A('};')
    A('')
    A('inline constexpr const char *kDivePageName[DIVEPAGE_COUNT] = {')
    for p in pages:
        A(f'    "{p.name}",')
    A('};')
    A('')
    A('// The pages with no per-part sub-tabs: their second row is hidden and their')
    A('// content is already shifted up by kDiveRow2Height.')
    A('inline constexpr bool kDivePageHidesRow2[DIVEPAGE_COUNT] = {')
    for p in pages:
        A(f'    {"true" if p.scoot else "false"},')
    A('};')
    A('')
    A('enum DiveKind : int { DK_BUTTON, DK_MENU, DK_SLIDER, DK_LCD };')
    A('')
    A('// One row per control.  `travel` is where a slider tap\'s CENTRE may go --')
    A('// the artwork draws ten graticules and the tap stays between the first and')
    A('// the last -- and `tap` is the tap\'s own size, taken from where it was')
    A('// parked in Inkscape.  Both are zero for anything that is not a slider.')
    A('struct DiveControl {')
    A('    const char *label;')
    A('    const char *id;')
    A('    const char *tap_id;')
    A('    const char *text_id;')
    A('    int page;')
    A('    int kind;')
    A('    Rect box;')
    A('    Rect travel;')
    A('    Rect tap;')
    A('};')
    A('')

    rows_out, starts = [], []
    for pi, p in enumerate(pages):
        starts.append(len(rows_out))
        controls = [e for e in p.elements
                    if e.label.split('_', 1)[0] in DIVE_KIND]
        for e in sorted(controls, key=lambda e: (e.y, e.x)):
            kind = DIVE_KIND[e.label.split('_', 1)[0]]
            travel = (0.0, 0.0, 0.0, 0.0)
            tap = (0.0, 0.0, 0.0, 0.0)
            tap_id = None
            if kind == 'DK_SLIDER':
                b = p.travel.get(e.label)
                if b is None:
                    continue
                travel = (b[0], b[1], b[2] - b[0], b[3] - b[1])
                t = p.by_label.get('ST_' + e.label[3:])
                if t is None:
                    continue
                tap = (t.x, t.y, t.w, t.h)
                # The tap's own id, so the UI can draw the artwork's tap at the value
                # rather than where Inkscape parked it.  Same idea as the knob pointer.
                tap_id = t.id
            tid = text_id_for(p.by_label, e.label)
            rows_out.append((e, pi, kind, travel, tap, tap_id,
                             p.by_label[tid].id if tid else None))
    starts.append(len(rows_out))

    A('inline constexpr DiveControl kDiveControl[] = {')
    for e, pi, kind, travel, tap, tap_id, tid in rows_out:
        A('    { "%s", "%s", %s, %s, %d, %s,'
          % (e.label, e.id,
             '"%s"' % tap_id if tap_id else 'nullptr',
             '"%s"' % tid if tid else 'nullptr', pi, kind))
        A('      { %9.4ff, %9.4ff, %8.4ff, %8.4ff },' % (e.x, e.y, e.w, e.h))
        A('      { %9.4ff, %9.4ff, %8.4ff, %8.4ff },' % travel)
        A('      { %9.4ff, %9.4ff, %8.4ff, %8.4ff } },' % tap)
    A('};')
    A(f'inline constexpr int kDiveControlCount = {len(rows_out)};')
    A('')
    A('// Half-open range of kDiveControl belonging to each page.')
    A('inline constexpr int kDivePageFirst[DIVEPAGE_COUNT + 1] = {')
    for s in starts:
        A(f'    {s},')
    A('};')
    A('')


def emit_json(path, elements, pivots, canvas, svg_rel, pages=()):
    doc = dict(source=svg_rel,
               design_width=round(canvas[0], 5),
               design_height=round(canvas[1], 5),
               elements=[e.as_dict() for e in elements],
               knobs={k: {kk: (round(vv, 4) if isinstance(vv, float) else vv)
                          for kk, vv in v.items()} for k, v in pivots.items()},
               dive=[dict(name=p.name, source=os.path.basename(p.path),
                          hides_row2=p.scoot,
                          offset=[round(v, 5) for v in p.offset],
                          elements=[e.as_dict() for e in p.elements],
                          travel={k: [round(v, 4) for v in b]
                                  for k, b in p.travel.items()})
                     for p in pages])
    with open(path, 'w') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')


# ------------------------------------------------------- document rewriting

def reframe(root, offset, canvas):
    """Move a document's artwork by `offset` and give it `canvas`'s frame.

    Every DIVE page is authored on its own sheet, positioned by the content box it
    shares with the panel.  Baking the offset in here means the page is drawn at
    panel coordinates by whoever draws it, and -- because the frame is rewritten to
    the panel's too -- every document normalises to the same scale in the UI.  A
    page that has to be shifted at draw time is a page whose hit boxes and artwork
    can disagree; this way they cannot.
    """
    kids = [c for c in list(root) if c.tag != f'{{{SVG}}}defs']
    if offset != (0.0, 0.0):
        g = ET.Element(f'{{{SVG}}}g')
        g.set('transform', 'translate(%.6f,%.6f)' % offset)
        for c in kids:
            root.remove(c)
            g.append(c)
        root.append(g)
    root.set('width', '%.5fmm' % canvas[0])
    root.set('height', '%.5fmm' % canvas[1])
    root.set('viewBox', '0 0 %.5f %.5f' % canvas)


# ---------------------------------------------------------------- fonts

# Warned about at most once per family per run, since seven SVGs name the same faces.
_font_warned = set()


def check_fonts(root):
    """Warn if a font the artwork names is not installed.

    Inkscape substitutes a missing face IN SILENCE and exports paths anyway, so the
    panel comes out in whatever fontconfig picked and the build reports success.  It
    does not look like an error; it looks like somebody changed the artwork.  The
    faces are not in the repository -- they are installed locally -- so this is the
    normal experience of a fresh clone, not a rare one.

    fc-match answers with the family it would ACTUALLY use, which is the question.
    The pattern has to be escaped first: unescaped, fontconfig reads the '-' in
    'Earth-Mod' as the start of a style and cheerfully answers 'Earth' -- a wrong
    answer that looks like a right one.
    """
    families = set()
    for e in root.iter():
        for m in re.finditer(r'font-family:\s*([^;]+)', e.get('style') or ''):
            families.add(m.group(1).strip().strip('\'"'))
        attr = e.get('font-family')
        if attr:
            families.add(attr.strip().strip('\'"'))

    for fam in sorted(families - _font_warned):
        pattern = fam.replace('-', r'\-')
        try:
            got = subprocess.run(['fc-match', '-f', '%{family}', pattern],
                                 capture_output=True, text=True).stdout.strip()
        except FileNotFoundError:
            return                              # no fontconfig; nothing to check
        if got.split(',')[0] != fam:
            _font_warned.add(fam)
            sys.stderr.write(
                f'panel_export: warning: font "{fam}" is not installed -- fontconfig '
                f'would use "{got}" instead.  The lettering in the exported panel is '
                f'NOT the artwork.\n')


# ---------------------------------------------------------------- text->path

def flatten(svg_path, out_path, drop_ids=(), offset=(0.0, 0.0), canvas=None,
            verbose=True):
    """Build the flattened artwork the renderer actually loads.

    Four things have to happen, in this order:

      1. Drop the editing-aid layers -- 'Example_LCD_Testing_only' is scaffolding,
         and anything named *_do_not_include is the artwork saying so itself.  This
         must happen BEFORE Inkscape runs, because --export-plain-svg strips
         inkscape:label and the layers become unidentifiable afterwards.
      2. Drop every element paired with an X_as_path, for the same reason and one
         more: Inkscape would convert it to a path, and that path is ALREADY in
         the artwork, possibly adjusted by hand since it was made.  Flattening the
         source again would quietly replace the adjusted one with a fresh copy.
         Drop the raster pass's elements too, so no shape is drawn twice.
      3. Let Inkscape convert what remains to paths.  nanosvg has no text support
         whatsoever.  A single-line label comes back as one <path> WITH THE SAME
         id, which is what lets the UI recolour a selected tab.
      4. Drop anything that is still <text>, and every <image>.  nanosvg would
         ignore both silently, so leaving them in would make rsvg-convert (our
         reference renderer) disagree with what the plugin actually draws -- which
         defeats the point of having a reference.
    """
    tree = ET.parse(svg_path)
    root = tree.getroot()

    aids = editing_aid_labels(root)
    drop = set(drop_ids)
    dropped = []

    def prune(parent):
        for child in list(parent):
            label = child.get(f'{{{INK}}}label') or ''
            is_layer = child.get(f'{{{INK}}}groupmode') == 'layer'
            if ((is_layer and (label in SKIP_LAYERS
                               or label.endswith(SKIP_LAYER_SUFFIX)))
                    or label.endswith('_duplicate') or label in aids
                    or child.get('id') in drop):
                dropped.append(label or child.get('id'))
                parent.remove(child)
            else:
                prune(child)

    prune(root)
    check_fonts(root)
    if canvas is not None:
        reframe(root, offset, canvas)

    out_dir = os.path.dirname(out_path)
    os.makedirs(out_dir, exist_ok=True)
    # Named per process: two exporters running at once must not delete each other's.
    tmp = os.path.join(out_dir, '.flatten_in.%d.svg' % os.getpid())
    tree.write(tmp, encoding='utf-8', xml_declaration=True)

    cmd = ['inkscape', tmp,
           '--export-type=svg', '--export-plain-svg',
           '--export-text-to-path', f'--export-filename={out_path}']
    r = subprocess.run(cmd, capture_output=True, text=True)
    os.unlink(tmp)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return None

    # Step 4: anything Inkscape could not convert.
    tree = ET.parse(out_path)
    root = tree.getroot()
    leftover = []

    def strip_text(parent):
        for child in list(parent):
            if child.tag == f'{{{SVG}}}text':
                leftover.append(child.get('id') or '?')
                parent.remove(child)
            elif child.tag == f'{{{SVG}}}image':
                parent.remove(child)          # drawn from the raster pass instead
            else:
                strip_text(child)

    strip_text(root)
    tree.write(out_path, encoding='utf-8', xml_declaration=True)
    for eid in leftover:
        sys.stderr.write(f'panel_export: warning: <text> id={eid} survived '
                         'text-to-path and was removed from the flat SVG; '
                         'check it in Inkscape\n')

    # nanosvg finds shapes by literal tag name, so verify the output is unprefixed
    # before anyone tries to draw it.  This is cheap and it is the failure that has
    # no symptom.
    with open(out_path) as f:
        head = f.read(4096)
    if '<svg' not in head:
        sys.stderr.write('panel_export: ERROR: %s has namespace-prefixed tags; nanosvg '
                         'will parse it and find nothing to draw\n' % out_path)

    if verbose and dropped:
        sys.stderr.write('panel_export: flattened without '
                         + ', '.join(repr(d) for d in dropped) + '\n')
    return out_path


# ------------------------------------------------------------ raster layer

def render_raster(svg_path, out_png, canvas, keep_ids, scale,
                  offset=(0.0, 0.0), verbose=True):
    """Pre-render the elements nanosvg cannot draw, into one transparent PNG.

    Why a pre-render rather than support in the UI: these are not just shapes, they
    are shapes under a transform AND a clip path, and rsvg already implements both
    correctly.  Reimplementing clipping against NanoVG to save one build-time
    dependency would be trading a solved problem for an unsolved one.

    The result is exactly the canvas rectangle, transparent wherever nothing was
    drawn, so the UI blits it into the design rect with no geometry of its own --
    nothing here has to agree with anything there.
    """
    tree = ET.parse(svg_path)
    root = tree.getroot()
    keep = set(keep_ids)
    if not keep:
        return None

    def prune(node):
        """Remove every branch that does not lead to a kept id.  Ancestors stay,
        because their transforms are part of where the kept element lands."""
        wanted = False
        for child in list(node):
            if child.tag == f'{{{SVG}}}defs':
                continue                       # the clip paths live here
            if child.get('id') in keep:
                wanted = True
                continue
            if prune(child):
                wanted = True
            else:
                node.remove(child)
        return wanted

    if not prune(root):
        return None

    reframe(root, offset, canvas)

    out_dir = os.path.dirname(out_png)
    os.makedirs(out_dir, exist_ok=True)
    tmp = os.path.join(out_dir, '.raster_in.%d.svg' % os.getpid())
    tree.write(tmp, encoding='utf-8', xml_declaration=True)

    width = int(round(canvas[0] * scale))
    r = subprocess.run([RSVG, '-w', str(width), tmp, '-o', out_png],
                       capture_output=True, text=True)
    os.unlink(tmp)
    if r.returncode != 0:
        sys.stderr.write(r.stderr or f'panel_export: {RSVG} failed\n')
        return None
    return out_png


def embed_bytes(f, name, data, comment=''):
    if comment:
        f.write(comment)
    f.write('static const unsigned char %s[] = {\n' % name)
    for i in range(0, len(data), 16):
        f.write('    ' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ',\n')
    f.write('};\n\n')


def embed_svg(f, name, text):
    f.write('static const char %s[] =\n' % name)
    for line in text.splitlines():
        esc = line.replace('\\', '\\\\').replace('"', '\\"')
        f.write('    "%s\\n"\n' % esc)
    f.write('    ;\n\n')


# --------------------------------------------------------------------- main

class Page:
    """One DIVE tab's content, measured and moved into panel coordinates."""

    def __init__(self, name, filename, scoot, panel_anchor, row2_h):
        self.name = name
        self.path = os.path.join(GFX, filename)
        self.scoot = scoot
        (self.elements, _, self.warnings,
         self.canvas, self.rasters, self.travel) = scan(self.path)

        anchor = next((e for e in self.elements if e.label == ANCHOR), None)
        if anchor is None:
            self.warnings.append(f'no {ANCHOR} rect -- there is nothing to line '
                                 'the page up with')
            self.offset = (0.0, 0.0)
        else:
            # The page is placed by the box it shares with the panel, and shifted up
            # by the second tab row when that row is not shown on this page.
            self.offset = (panel_anchor.x - anchor.x,
                           panel_anchor.y - anchor.y - (row2_h if scoot else 0.0))

        dx, dy = self.offset
        for e in self.elements:
            e.x += dx
            e.y += dy
        for k, b in self.travel.items():
            self.travel[k] = [b[0] + dx, b[1] + dy, b[2] + dx, b[3] + dy]
        self.by_label = {e.label: e for e in self.elements}


def text_id_for(page_labels, label):
    """The screenprint that names a control: M_output_mode is titled T_output_mode.

    Matched without case because the artwork spells a name the way it reads on the
    panel -- LCD_Patch_Name over T_patch_name -- and the pairing is about which
    words they share, not how they are capitalised.
    """
    suffix = label.split('_', 1)[1].lower() if '_' in label else ''
    for other in page_labels:
        if other.startswith('T_') and other[2:].lower() == suffix:
            return other
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--svg', default=SVG_PATH)
    ap.add_argument('--out-dir', default=OUT_DIR)
    ap.add_argument('--check', action='store_true',
                    help='lint only; exit non-zero if the artwork has problems')
    ap.add_argument('--text-to-path', action='store_true',
                    help='also write the flattened SVGs the UI actually draws')
    ap.add_argument('--background', action='store_true',
                    help='also pre-render what nanosvg cannot draw')
    ap.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()

    elements, pivots, warnings, canvas, rasters, _ = scan(args.svg)
    rel = os.path.relpath(args.svg, ROOT)
    by_label = {e.label: e for e in elements}

    anchor = by_label.get(ANCHOR)
    row2 = [by_label[l] for l in ('BUT_basic', 'BUT_level', 'BUT_pitch', 'BUT_LFO')
            if l in by_label]
    row2_h = row2[0].h if row2 else 0.0

    pages = []
    if anchor is not None:
        for name, filename, scoot in DIVE_PAGES:
            p = Page(name, filename, scoot, anchor, row2_h)
            warnings.extend(f'{filename}: {w}' for w in p.warnings)
            pages.append(p)
    else:
        warnings.append(f'the panel has no {ANCHOR} rect; the DIVE pages have '
                        'nothing to line up with and were skipped')

    for w in warnings:
        sys.stderr.write(f'panel_export: warning: {w}\n')

    n_controls = sum(len(p.elements) for p in pages)
    if args.check:
        if not args.quiet:
            print(f'{len(elements)} named elements, {len(pivots)} knob(s), '
                  f'{len(rasters)} raster(s), {len(pages)} DIVE page(s) with '
                  f'{n_controls} named elements, {len(warnings)} warning(s)')
        return 1 if warnings else 0

    os.makedirs(args.out_dir, exist_ok=True)
    h = os.path.join(args.out_dir, 'panel_geometry.h')
    j = os.path.join(args.out_dir, 'panel_geometry.json')
    emit_header(h, elements, pivots, canvas, rel, pages, row2_h)
    emit_json(j, elements, pivots, canvas, rel, pages)
    written = [h, j]

    if args.background:
        png = os.path.join(args.out_dir, 'panel_background.png')
        if render_raster(args.svg, png, canvas, rasters, BACKGROUND_SCALE,
                         verbose=not args.quiet):
            data = open(png, 'rb').read()
            hdr = os.path.join(args.out_dir, 'panel_background.h')
            with open(hdr, 'w') as f:
                f.write('// GENERATED by plugin/tools/panel_export.py -- do not edit.\n'
                        '//\n'
                        "// The artwork's raster layer, already transformed and clipped,\n"
                        '// %d bytes of PNG at %d px wide (%.1fx the design width).\n'
                        '#pragma once\n\n' % (len(data),
                                              int(round(canvas[0] * BACKGROUND_SCALE)),
                                              BACKGROUND_SCALE))
                embed_bytes(f, 'kPanelBackgroundPng', data)
            written += [png, hdr]
        else:
            sys.stderr.write('panel_export: no raster layer in the panel\n')

    flat = None
    if args.text_to_path:
        flat = flatten(args.svg, os.path.join(args.out_dir, 'panel_flat.svg'),
                       drop_ids=rasters, verbose=not args.quiet)
        if flat:
            hdr = os.path.join(args.out_dir, 'panel_svg.h')
            with open(hdr, 'w') as f:
                f.write('// GENERATED by plugin/tools/panel_export.py -- do not edit.\n'
                        '#pragma once\n\n')
                embed_svg(f, 'kPanelSvg', open(flat).read())
            written += [flat, hdr]

    # The DIVE pages: flattened and rasterised the same way, but each translated
    # into panel coordinates first, and all embedded in one header.
    if pages and (args.text_to_path or args.background):
        page_svg, page_png = {}, {}
        for p in pages:
            if args.text_to_path:
                out = os.path.join(args.out_dir, 'dive_%s_flat.svg' % p.name)
                if flatten(p.path, out, drop_ids=p.rasters, offset=p.offset,
                           canvas=canvas, verbose=False):
                    page_svg[p.name] = open(out).read()
                    written.append(out)
            if args.background:
                out = os.path.join(args.out_dir, 'dive_%s_raster.png' % p.name)
                if render_raster(p.path, out, canvas, p.rasters, FRAME_SCALE,
                                 offset=p.offset, verbose=False):
                    page_png[p.name] = open(out, 'rb').read()
                    written.append(out)
        hdr = os.path.join(args.out_dir, 'dive_pages.h')
        with open(hdr, 'w') as f:
            f.write('// GENERATED by plugin/tools/panel_export.py -- do not edit.\n'
                    '//\n'
                    '// One DIVE page per tab, each already translated into panel\n'
                    "// coordinates, so all of them normalise to the panel's own scale.\n"
                    '// A page with no raster has a zero-length array.\n'
                    '#pragma once\n\n')
            for p in pages:
                if p.name in page_svg:
                    embed_svg(f, 'kDiveSvg_%s' % p.name, page_svg[p.name])
            for p in pages:
                data = page_png.get(p.name, b'')
                embed_bytes(f, 'kDiveRaster_%s' % p.name, data or b'\x00')
                if not data:
                    f.write('#define kDiveRaster_%s_EMPTY 1\n\n' % p.name)
            f.write('static const char *const kDiveSvg[] = {\n')
            for p in pages:
                f.write('    %s,\n' % ('kDiveSvg_%s' % p.name
                                       if p.name in page_svg else 'nullptr'))
            f.write('};\n\n')
            f.write('static const unsigned char *const kDiveRaster[] = {\n')
            for p in pages:
                f.write('    %s,\n' % ('kDiveRaster_%s' % p.name
                                       if p.name in page_png else 'nullptr'))
            f.write('};\n\n')
            f.write('static const unsigned int kDiveRasterSize[] = {\n')
            for p in pages:
                f.write('    %du,\n' % len(page_png.get(p.name, b'')))
            f.write('};\n')
        written.append(hdr)

    if not args.quiet:
        print(f'panel {canvas[0]:.2f} x {canvas[1]:.2f}, {len(elements)} elements, '
              f'{len(rasters)} raster(s), {len(pages)} DIVE page(s)')
        for w in written:
            size = os.path.getsize(w)
            print('  -> %-44s %6d KB' % (os.path.relpath(w, ROOT), size // 1024)
                  if size >= 1024 else
                  '  -> %-44s %6d B ' % (os.path.relpath(w, ROOT), size))
    return 0


if __name__ == '__main__':
    sys.exit(main())
