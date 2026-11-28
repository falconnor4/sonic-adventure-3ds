#!/usr/bin/env python3
"""what the GPU was given in the picture it never finished: sa3ds/hang.bin,
which the 3DS writes where a freeze stops the drawing in C3D_FrameBegin
(backend.cpp's sa3ds_dump_hang). it reports the batches, then the triangles
whose clip space or screen place lies furthest out, as draw.v.pica carries
each corner: a world batch's through the rows, any other's as it is

usage: hang.py HANG.BIN [COUNT]"""
import math
import struct
import sys

BATCH_SCREEN = 16
NEAR = 1.0
FORMATS = {0: "RGBA8", 1: "RGB8", 2: "RGBA5551", 3: "RGB565", 4: "RGBA4", 5: "LA8", 7: "L8",
           8: "A8", 9: "LA4", 10: "L4", 11: "A4", 12: "ETC1", 13: "ETC1A4", 255: "none"}


def read(path):
    data = open(path, "rb").read()
    if data[:8] != b"SAHANG1\0":
        sys.exit(f"{path}: not a hang dump")
    at = 8
    corners, tops, unders, status = struct.unpack_from("<4I", data, at)
    at += 16
    rows = struct.unpack_from("<16f", data, at)
    at += 64
    camera = struct.unpack_from("<3f", data, at)
    at += 12
    (slider,) = struct.unpack_from("<f", data, at)
    at += 4
    batches = []
    for i in range(tops + unders):
        first, count, flags, texture, sample = struct.unpack_from("<5I", data, at)
        blend, src, dst, depth, write, cull, fmt, _ = struct.unpack_from("<8B", data, at + 20)
        width, height = struct.unpack_from("<2H", data, at + 28)
        at += 32
        batches.append(dict(top=i < tops, first=first, count=count, flags=flags, texture=texture,
                            sample=sample, blend=blend, src=src, dst=dst, depth=depth, write=write,
                            cull=cull, fmt=fmt, width=width, height=height))
    points = []
    for i in range(corners):
        x, y, z, w, u, v, color = struct.unpack_from("<6fI", data, at + i * 28)
        points.append((x, y, z, w, u, v, color))
    return dict(corners=corners, status=status, rows=rows, camera=camera, slider=slider,
                batches=batches, points=points)


def clip(rows, p, world):
    if not world:
        return p[:4]
    return tuple(rows[4 * r] * p[0] + rows[4 * r + 1] * p[1] + rows[4 * r + 2] * p[2] +
                 rows[4 * r + 3] * p[3] for r in range(4))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dump = read(sys.argv[1])
    show = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    rows = dump["rows"]
    print(f"{dump['corners']} corners, {len(dump['batches'])} batches, the GPU's status "
          f"{dump['status']:#010x}, the slider {dump['slider']:.2f}")
    print("camera", " ".join(f"{c:.6g}" for c in dump["camera"]))
    for r in range(4):
        print("row", r, " ".join(f"{v:.6g}" for v in rows[4 * r:4 * r + 4]))
    found = []
    for n, b in enumerate(dump["batches"]):
        world = b["top"] and not b["flags"] & BATCH_SCREEN
        for i in range(b["first"], b["first"] + b["count"] - 2, 3):
            tri = [clip(rows, dump["points"][i + k], world) for k in range(3)]
            uvs = [dump["points"][i + k][4:6] for k in range(3)]
            values = [abs(c) for t in tri for c in t] + [abs(c) for uv in uvs for c in uv]
            if any(math.isnan(c) or math.isinf(c) for c in values):
                found.append((math.inf, n, i, tri, uvs, "not a number"))
                continue
            # where all three lie before the near plane, the screen's place of each
            if all(t[3] >= NEAR for t in tri):
                screen = max(max(abs(t[0] / t[3]), abs(t[1] / t[3])) for t in tri)
            else:
                screen = 0.0
            found.append((max(max(values), screen), n, i, tri, uvs,
                          f"most {max(values):.4g}, on the screen {screen:.4g}"))
    found.sort(key=lambda f: -f[0])
    print(f"\nthe {show} triangles furthest out:")
    for _, n, i, tri, uvs, why in found[:show]:
        b = dump["batches"][n]
        print(f"batch {n} ({'top' if b['top'] else 'bottom'}, flags {b['flags']:#x}, texture "
              f"{b['texture']} {FORMATS.get(b['fmt'], b['fmt'])} {b['width']}x{b['height']}, "
              f"blend {b['blend']} {b['src']}/{b['dst']}, depth {b['depth']}, cull {b['cull']}) "
              f"corner {i}: {why}")
        for t, uv in zip(tri, uvs):
            print("    clip", " ".join(f"{c:.6g}" for c in t), " uv", " ".join(f"{c:.6g}" for c in uv))


if __name__ == "__main__":
    main()
