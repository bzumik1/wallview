#!/usr/bin/env python3
"""Show or change CST226SE image configuration fields and recompute the checksum.

    cstimg.py show <image>
    cstimg.py set <in> <out> <offset>=<value>[:u16] ...

The output must be a new file (never the input or an existing file), so the
vendor image cannot be overwritten. Offsets are file offsets (for example 0x166a for cfg[0x66]); see
docs/touch-controller.md, "Report rate". The version is left unchanged, so
neither the vendor kernel nor anyone reading 0xD208 mistakes it for a newer
release. Never commit images: they stay in the git-ignored local/firmware/.
"""
import hashlib
import os
import struct
import sys

SIZE = 7680
FIELDS = [  # (file offset, width, description)
    (0x1668, 1, "cfg[0x64] -> 0x40001088 lo"),
    (0x1669, 1, "cfg[0x65] -> 0x4000108a lo"),
    (0x166A, 1, "cfg[0x66] -> 0x40001088 hi"),
    (0x166B, 1, "cfg[0x67] -> 0x4000106e"),
    (0x166C, 1, "cfg[0x68] -> 0x4000106e"),
    (0x16A0, 1, "cfg[0x9c] -> 0x4000108a hi"),
    (0x16B8, 2, "cfg[0xb4] -> 0x40001086"),
]


def checksum(d):
    return (0x55 + sum(struct.unpack("<1919I", d[:0x1DFC]))) & 0xFFFFFFFF


def load(path):
    d = bytearray(open(path, "rb").read())
    if len(d) != SIZE:
        sys.exit(f"{path}: {len(d)} bytes, expected {SIZE}")
    if checksum(d) != struct.unpack_from("<I", d, 0x1DFC)[0]:
        sys.exit(f"{path}: checksum does not match its trailer")
    return d


VENDOR_CHECKSUM = 0x99630E41
KNOWN = {off for off, _, _ in FIELDS} | {0x16B9}


def show(d):
    ver, csum = struct.unpack_from("<II", d, 0x1DF8)
    kind = "vendor image" if csum == VENDOR_CHECKSUM else "MODIFIED image"
    print(f"version 0x{ver:08x} checksum 0x{csum:08x} ({kind}) sha256 {hashlib.sha256(d).hexdigest()}")
    for off, width, desc in FIELDS:
        val = d[off] if width == 1 else struct.unpack_from("<H", d, off)[0]
        print(f"0x{off:04x} {val:5d} (0x{val:0{width * 2}x})  {desc}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "show":
        show(load(sys.argv[2]))
        return
    if len(sys.argv) < 5 or sys.argv[1] != "set":
        sys.exit(__doc__)
    src, dst = sys.argv[2], sys.argv[3]
    if os.path.exists(dst) or os.path.abspath(src) == os.path.abspath(dst):
        sys.exit(f"{dst}: exists or is the input; write a new file")
    d = load(src)
    print(f"input  {src}")
    show(d)
    for arg in sys.argv[4:]:
        spec, _, width = arg.partition(":")
        off, _, val = spec.partition("=")
        off, val = int(off, 0), int(val, 0)
        size = 2 if width == "u16" else 1
        if not (0x1604 <= off and off + size <= 0x16CC):
            sys.exit(f"0x{off:04x}: only the main configuration table 0x1604-0x16cb may change")
        if not 0 <= val < (1 << (8 * size)):
            sys.exit(f"value {val} does not fit {size} byte(s)")
        if off not in KNOWN:
            print(f"warning: 0x{off:04x} is not one of the suspected front-end fields", file=sys.stderr)
        if width == "u16":
            struct.pack_into("<H", d, off, val)
        elif width == "":
            d[off] = val
        else:
            sys.exit(f"unknown width {width}")
    struct.pack_into("<I", d, 0x1DFC, checksum(d))
    with open(dst, "xb") as f:
        f.write(d)
    print(f"output {dst}")
    show(d)


if __name__ == "__main__":
    main()
