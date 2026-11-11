#!/usr/bin/env python3
import argparse
import hashlib
import struct
import sys

BLOCK = 512
PAGE_SIZE = 2048
KERNEL_ADDR = 0x10008000
RAMDISK_ADDR = 0x11000000
SECOND_ADDR = 0x10F00000
TAGS_ADDR = 0x10000100
HEADER_FORMAT = "<8s10I16s512s32s1024s"


def pad_to(data, size):
    return data + b"\0" * ((-len(data)) % size)


def make_resource(entries):
    count = len(entries)
    header = bytearray(BLOCK)
    header[0:4] = b"RSCE"
    header[8] = 1
    header[9] = 1
    header[10] = 1
    struct.pack_into("<I", header, 12, count)

    table = bytearray()
    payload = bytearray()
    next_block = 1 + count
    for name, data in entries:
        entry = bytearray(BLOCK)
        entry[0:4] = b"ENTR"
        encoded = name.encode()
        entry[4:4 + len(encoded)] = encoded
        struct.pack_into("<II", entry, 260, next_block, len(data))
        table += entry
        padded = pad_to(data, BLOCK)
        payload += padded
        next_block += len(padded) // BLOCK
    return bytes(header + table + payload)


def boot_id(kernel, ramdisk, second):
    sha = hashlib.sha1()
    for part in (kernel, ramdisk, second):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    return sha.digest().ljust(32, b"\0")


def make_boot_image(kernel, ramdisk, second, cmdline):
    if len(cmdline) >= 512:
        sys.exit("cmdline too long")
    header = struct.pack(
        HEADER_FORMAT,
        b"ANDROID!",
        len(kernel), KERNEL_ADDR,
        len(ramdisk), RAMDISK_ADDR,
        len(second), SECOND_ADDR,
        TAGS_ADDR, PAGE_SIZE, 0, 0,
        b"wallview",
        cmdline.encode(),
        boot_id(kernel, ramdisk, second),
        b"",
    )
    return b"".join(pad_to(part, PAGE_SIZE) for part in (header, kernel, ramdisk, second))


def read_vendor_id(path):
    data = open(path, "rb").read()
    fields = struct.unpack_from("<8s10I", data)
    kernel_size, ramdisk_size, second_size = fields[1], fields[3], fields[5]
    offset = PAGE_SIZE
    parts = []
    for size in (kernel_size, ramdisk_size, second_size):
        parts.append(data[offset:offset + size])
        offset += size + (-size) % PAGE_SIZE
    stored = data[576:608]
    return stored == boot_id(*parts)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify-vendor")
    parser.add_argument("--kernel")
    parser.add_argument("--ramdisk")
    parser.add_argument("--dtb")
    parser.add_argument("--cmdline", default="")
    parser.add_argument("--output")
    args = parser.parse_args()

    if args.verify_vendor:
        print("vendor id match:", read_vendor_id(args.verify_vendor))
        return

    kernel = open(args.kernel, "rb").read()
    ramdisk = open(args.ramdisk, "rb").read()
    dtb = open(args.dtb, "rb").read()
    second = make_resource([("rk-kernel.dtb", dtb)])
    image = make_boot_image(kernel, ramdisk, second, args.cmdline)
    open(args.output, "wb").write(image)
    print(f"{args.output}: {len(image)} bytes ({len(image) / 1048576:.1f} MiB)")


if __name__ == "__main__":
    main()
