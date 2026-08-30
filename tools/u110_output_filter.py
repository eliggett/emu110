#!/usr/bin/env python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later
"""The U-110's analog reconstruction filter (IC30-35, service notes Fig. 4).

Two equal-R Sallen-Key lowpass sections plus the output RC.  Component values from the
schematic reproduce the printed corner frequencies, so the filter is fully specified:

    section 1   R39/R40 10k, C53 8200p, C52 680p   ->  6741 Hz, Q 1.74
    section 2   R41/R42 10k, C55 3300p, C54 560p   -> 11708 Hz, Q 1.21
    output RC   R78 10k, C56 2200p                 ->  7234 Hz, 1 pole

For an equal-R Sallen-Key: f0 = 1/(2*pi*R*sqrt(C1*C2)),  Q = 0.5*sqrt(C1/C2).
"""
import numpy as np

R = 10e3
SECTIONS = [(8200e-12, 680e-12), (3300e-12, 560e-12)]
RC_R, RC_C = 10e3, 2200e-12


def sk_params(c1, c2, r=R):
    f0 = 1.0 / (2 * np.pi * r * np.sqrt(c1 * c2))
    q = 0.5 * np.sqrt(c1 / c2)
    return f0, q


def biquad_lp(f0, q, fs):
    """RBJ cookbook lowpass, bilinear-transformed."""
    w0 = 2 * np.pi * f0 / fs
    a = np.sin(w0) / (2 * q)
    c = np.cos(w0)
    b = np.array([(1 - c) / 2, 1 - c, (1 - c) / 2])
    a_ = np.array([1 + a, -2 * c, 1 - a])
    return b / a_[0], a_ / a_[0]


def _apply_biquad(x, b, a):
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b[0]*v + b[1]*x1 + b[2]*x2 - a[1]*y1 - a[2]*y2
        x2, x1 = x1, v
        y2, y1 = y1, o
        y[i] = o
    return y


def apply_filter(x, fs):
    """Run a mono signal through the full 5th-order chain."""
    y = np.asarray(x, dtype=float)
    for c1, c2 in SECTIONS:
        f0, q = sk_params(c1, c2)
        b, a = biquad_lp(f0, q, fs)
        y = _apply_biquad(y, b, a)
    k = 1.0 - np.exp(-2 * np.pi * (1.0 / (2 * np.pi * RC_R * RC_C)) / fs)
    out = np.zeros_like(y); acc = 0.0
    for i, v in enumerate(y):
        acc += k * (v - acc); out[i] = acc
    return out


if __name__ == '__main__':
    for c1, c2 in SECTIONS:
        print("Sallen-Key %6.0f Hz  Q %.2f" % sk_params(c1, c2))
    print("output RC  %6.0f Hz  1 pole" % (1/(2*np.pi*RC_R*RC_C)))
