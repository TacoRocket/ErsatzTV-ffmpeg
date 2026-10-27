#!/usr/bin/env python3
"""Generate the fixed HDR10 Hable table at build time; Python standard library only.

PQ, Hable and BT.1886 follow FFmpeg's LGPL OpenCL implementation. Packing follows
Intel libva-utils' 65/65/128 RGB sample. See THIRD_PARTY.md. No scene adaptation.
"""
import hashlib
import struct
import sys
from pathlib import Path

# BT.2020 -> BT.709 linear RGB, D65 white, derived from their xy primaries.
MATRIX = (
    (1.6604910021084338, -0.5876411387885496, -0.07284986331988486),
    (-0.12455047452159052, 1.1328998971259603, -0.00834942260436948),
    (-0.018150763354905234, -0.10057889800800746, 1.1187296613629127),
)
EXPECTED_SHA256 = "fe0ae520b07eefc79f26e22c97720ff441573c3dba85336665206e5fa9cfbbef"


def pq(value):
    p = value ** (32 / 2523)
    return (max(p - 3424 / 4096, 0) / max(2413 / 128 - 2392 / 128 * p, 1e-6)) ** (16384 / 2610) * 100


def hable(value):
    return (value * (value * 0.15 + 0.50 * 0.10) + 0.20 * 0.02) / (value * (value * 0.15 + 0.50) + 0.20 * 0.30) - 0.02 / 0.30


def generate():
    linear = [pq(i / 64) for i in range(65)]
    table = bytearray()
    peak = hable(10.0)  # 1000 nits / FFmpeg's 100-nit reference white.
    for red in linear:
        for green in linear:
            for blue in linear:
                rgb = [m[0] * red + m[1] * green + m[2] * blue for m in MATRIX]
                signal = max(*rgb, 1e-6)
                scale = min(hable(signal) / peak, 1) / signal
                values = [round(min(max(v * scale, 0) ** (1 / 2.4), 1) * 65535) for v in rgb]
                entry = struct.pack('<4H', *values, 0)
                table.extend(entry)
            table.extend(entry * 63)
    digest = hashlib.sha256(table).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"LUT changed: {digest}; review the transform before packaging")
    return table


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: make_lut.py OUTPUT')
    Path(sys.argv[1]).write_bytes(generate())
