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
    T_<name>      screenprint text  -> ignored here, drawn by nanosvg

Usage:
    plugin/tools/panel_export.py                 # extract
    plugin/tools/panel_export.py --check         # lint only, non-zero on error
    plugin/tools/panel_export.py --text-to-path  # regenerate the paths layer
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

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SVG_PATH = os.path.join(ROOT, 'resources/graphics/overall_panel_inkscape.svg')
OUT_DIR = os.path.join(ROOT, 'plugin/generated')

# Layers whose contents are editing aids, not part of the rendered panel.
# 'Foreground Text as Text' is the layer the human edits; the paths layer built
# from it is what actually renders, since nanosvg has no text support at all.
SKIP_LAYERS = {'Foreground Text as Text', 'Example_LCD_Testing_only'}

# nanosvg understands fills, strokes and linear/radial gradients.  Everything
# here is silently dropped by it.
UNSUPPORTED_TAGS = {'filter', 'clipPath', 'mask', 'pattern', 'use', 'image',
                    'switch', 'foreignObject', 'marker', 'symbol'}
UNSUPPORTED_ATTRS = {'filter', 'clip-path', 'mask'}


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
    """Walk the SVG, returning (elements, knob_pivots, warnings, canvas)."""
    tree = ET.parse(svg_path)
    root = tree.getroot()

    vb = root.get('viewBox')
    if vb:
        parts = [float(v) for v in re.split(r'[,\s]+', vb.strip()) if v]
        canvas = (parts[2], parts[3])
    else:
        canvas = (float(root.get('width', 0)), float(root.get('height', 0)))

    elements, pivots, warnings = [], {}, []

    def walk(node, mat, layer, in_skipped):
        tag = node.tag.split('}')[-1]
        label = node.get(f'{{{INK}}}label')
        eid = node.get('id')

        if node.get(f'{{{INK}}}groupmode') == 'layer':
            layer = label or eid
            in_skipped = layer in SKIP_LAYERS

        mat = mat_mul(mat, parse_transform(node.get('transform')))

        if not in_skipped:
            if tag in UNSUPPORTED_TAGS:
                warnings.append(f'<{tag}> id={eid}: nanosvg ignores this entirely')
            for attr in UNSUPPORTED_ATTRS:
                if node.get(attr):
                    warnings.append(f'<{tag}> id={eid}: {attr}= is dropped by nanosvg')
            if node.get('style') and 'filter:' in node.get('style'):
                warnings.append(f'<{tag}> id={eid}: filter in style= is dropped by nanosvg')
            if tag == 'text':
                warnings.append(f'<text> id={eid} in layer {layer!r}: nanosvg cannot '
                                'render text -- convert to path or move it to an '
                                'editing-aid layer')

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

        if label and not in_skipped:
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

        for child in node:
            walk(child, mat, layer, in_skipped)

    walk(root, IDENTITY, None, False)

    # off-canvas check
    W, H = canvas
    for e in elements:
        if e.x < -0.5 or e.y < -0.5 or e.x + e.w > W + 0.5 or e.y + e.h > H + 0.5:
            warnings.append(f'{e.label}: extends outside the canvas '
                            f'({e.x:.1f},{e.y:.1f} {e.w:.1f}x{e.h:.1f})')

    return elements, pivots, warnings, canvas


def invert_rotate(rot):
    ang, cx, cy = rot
    a = math.radians(-ang)
    ca, sa = math.cos(a), math.sin(a)
    n = (ca, sa, -sa, ca, 0, 0)
    return mat_mul(mat_mul((1, 0, 0, 1, cx, cy), n), (1, 0, 0, 1, -cx, -cy))


# -------------------------------------------------------------------- output

def c_ident(name):
    return re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_').upper()


def emit_header(path, elements, pivots, canvas, svg_rel):
    by_prefix = {}
    for e in elements:
        pre = e.label.split('_', 1)[0]
        by_prefix.setdefault(pre, []).append(e)

    buttons = sorted(by_prefix.get('BUT', []), key=lambda e: (e.y, e.x))
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

    A('} // namespace panel')
    A('} // namespace voltaire')
    A('')

    with open(path, 'w') as f:
        f.write('\n'.join(L))


def emit_json(path, elements, pivots, canvas, svg_rel):
    doc = dict(source=svg_rel,
               design_width=round(canvas[0], 5),
               design_height=round(canvas[1], 5),
               elements=[e.as_dict() for e in elements],
               knobs={k: {kk: (round(vv, 4) if isinstance(vv, float) else vv)
                          for kk, vv in v.items()} for k, v in pivots.items()})
    with open(path, 'w') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')


# ---------------------------------------------------------------- text->path

def text_to_path(svg_path, verbose=True):
    """Build the flattened artwork the renderer actually loads.

    Three things have to happen, in this order:

      1. Drop the editing-aid layers.  'Foreground Text as Text' is the human's
         copy and 'Example_LCD_Testing_only' is scaffolding; both would draw on
         top of the real artwork.  This must happen BEFORE Inkscape runs, because
         --export-plain-svg strips inkscape:label and the layers become
         unidentifiable afterwards.
      2. Let Inkscape convert the remaining text to paths.  nanosvg has no text
         support whatsoever.
      3. Drop anything that is still <text>.  nanosvg would ignore it silently,
         so leaving it in would make rsvg-convert (our reference renderer)
         disagree with what the plugin actually draws -- which defeats the point
         of having a reference.
    """
    tree = ET.parse(svg_path)
    root = tree.getroot()

    dropped = []

    def prune(parent):
        for child in list(parent):
            label = child.get(f'{{{INK}}}label') or ''
            is_layer = child.get(f'{{{INK}}}groupmode') == 'layer'
            if (is_layer and label in SKIP_LAYERS) or label.endswith('_duplicate'):
                dropped.append(label or child.get('id'))
                parent.remove(child)
            else:
                prune(child)

    prune(root)

    os.makedirs(OUT_DIR, exist_ok=True)
    tmp = os.path.join(OUT_DIR, '.panel_pruned.svg')
    tree.write(tmp, encoding='utf-8', xml_declaration=True)

    out = os.path.join(OUT_DIR, 'panel_flat.svg')
    cmd = ['inkscape', tmp,
           '--export-type=svg', '--export-plain-svg',
           '--export-text-to-path', f'--export-filename={out}']
    r = subprocess.run(cmd, capture_output=True, text=True)
    os.unlink(tmp)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return None

    # Step 3: anything Inkscape could not convert.
    tree = ET.parse(out)
    root = tree.getroot()
    leftover = []

    def strip_text(parent):
        for child in list(parent):
            if child.tag == f'{{{SVG}}}text':
                leftover.append(child.get('id') or '?')
                parent.remove(child)
            else:
                strip_text(child)

    strip_text(root)
    if leftover:
        tree.write(out, encoding='utf-8', xml_declaration=True)
        for eid in leftover:
            sys.stderr.write(f'panel_export: warning: <text> id={eid} survived '
                             'text-to-path and was removed from the flat SVG; '
                             'check it in Inkscape\n')

    if verbose and dropped:
        sys.stderr.write('panel_export: flattened without '
                         + ', '.join(repr(d) for d in dropped) + '\n')
    return out


# --------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--svg', default=SVG_PATH)
    ap.add_argument('--out-dir', default=OUT_DIR)
    ap.add_argument('--check', action='store_true',
                    help='lint only; exit non-zero if the artwork has problems')
    ap.add_argument('--text-to-path', action='store_true',
                    help='also write generated/panel_flat.svg with text flattened')
    ap.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()

    elements, pivots, warnings, canvas = scan(args.svg)
    rel = os.path.relpath(args.svg, ROOT)

    for w in warnings:
        sys.stderr.write(f'panel_export: warning: {w}\n')

    if args.check:
        if not args.quiet:
            print(f'{len(elements)} named elements, {len(pivots)} knob(s), '
                  f'{len(warnings)} warning(s)')
        return 1 if warnings else 0

    os.makedirs(args.out_dir, exist_ok=True)
    h = os.path.join(args.out_dir, 'panel_geometry.h')
    j = os.path.join(args.out_dir, 'panel_geometry.json')
    emit_header(h, elements, pivots, canvas, rel)
    emit_json(j, elements, pivots, canvas, rel)

    flat = text_to_path(args.svg, not args.quiet) if args.text_to_path else None

    if not args.quiet:
        print(f'panel {canvas[0]:.2f} x {canvas[1]:.2f}, {len(elements)} elements')
        print(f'  -> {os.path.relpath(h, ROOT)}')
        print(f'  -> {os.path.relpath(j, ROOT)}')
        if flat:
            print(f'  -> {os.path.relpath(flat, ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
