#!/usr/bin/env python3
"""Build the table pack for the per-scene probe (tonemap_vaapi_lut=scene=1).

The tables are not computed here. Each one is sampled from the tone mapper it has to
match: a 65-point identity grid is tagged with a peak brightness, run through libplacebo,
and the result is the table for that peak. See ../README.md for the FFmpeg commands.

  scene_pack.py grid grid.gbrp          write the identity grid (gbrp12le, 4225x65)
  scene_pack.py ladder                  print the peaks to sample, in nits
  scene_pack.py pack DIR out.bin        pack DIR/x-<nits>.gbrp16 (gbrp16le, 4225x65)

Grid and sampled tables hold red down the rows and green*65+blue along the columns.
The pack is "ETVSCLUT", u32 version 1, u32 count, u32 65, u32 0, count u32 peaks in nits,
then count tables of 65*65*65 RGB triples of u16, red outermost. Little-endian throughout.
"""
import struct
import sys
from array import array
from pathlib import Path

N = 65
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq_to_nits(v):
    p = v ** (1 / M2)
    return 10000 * (max(p - C1, 0) / (C2 - C3 * p)) ** (1 / M1)


def ladder():
    # No tone mapping happens at or below 203 nits, so 203 is a node and nothing blends across it.
    peaks, v = [100, 120, 145, 172, 203], 0.60
    while v < 0.999:
        peaks.append(round(pq_to_nits(v)))
        v += 0.02
    return peaks + [10000]


def grid(path):
    level = [round(i * 4095 / (N - 1)) for i in range(N)]
    r = array("H", (level[i // (N * N)] for i in range(N**3)))
    g = array("H", (level[i // N % N] for i in range(N**3)))
    b = array("H", (level[i % N] for i in range(N**3)))
    Path(path).write_bytes(g.tobytes() + b.tobytes() + r.tobytes())


def pack(source, path):
    files = sorted(Path(source).glob("x-*.gbrp16"), key=lambda f: int(f.stem[2:]))
    if len(files) < 2:
        sys.exit("need at least two sampled tables")
    with open(path, "wb") as out:
        out.write(b"ETVSCLUT" + struct.pack("<IIII", 1, len(files), N, 0))
        out.write(struct.pack(f"<{len(files)}I", *(int(f.stem[2:]) for f in files)))
        for f in files:
            planes = array("H", f.read_bytes())
            if len(planes) != 3 * N**3:
                sys.exit(f"{f}: expected {3 * N**3} samples")
            table = array("H", bytes(2 * 3 * N**3))
            table[0::3] = planes[2 * N**3 :]
            table[1::3] = planes[: N**3]
            table[2::3] = planes[N**3 : 2 * N**3]
            out.write(table.tobytes())


if __name__ == "__main__":
    if sys.byteorder != "little":
        sys.exit("little-endian host required")
    if sys.argv[1:2] == ["grid"] and len(sys.argv) == 3:
        grid(sys.argv[2])
    elif sys.argv[1:] == ["ladder"]:
        print(" ".join(map(str, ladder())))
    elif sys.argv[1:2] == ["pack"] and len(sys.argv) == 4:
        pack(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
