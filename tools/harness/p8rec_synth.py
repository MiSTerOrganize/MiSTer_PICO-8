#!/usr/bin/env python3
"""p8rec_synth.py -- write and inspect .inp takes (P8REC container v5).

Sibling of MiSTer_OpenBOR_7533/tools/harness/mrec_synth.py, same two jobs:
input injection for hands-free driving, and building deliberately malformed
takes so the payload parser can be tested against hostile input rather than
only against files we wrote ourselves.

FORMAT (must match src/mister_main.cpp)
    "P8REC"(5)  u32 container=5  u32 engine_ver  char[256] cart
    i32 seed  u32 frame_count  u32 crc32
    identity: u16 entry_count, [u16 name_len, name, u8 sha1[20]]
    frames:   frame_count * u32   (4 players x 7 bits, packed)
    payload:  u32 file_count, [u32 name_len, name, u32 data_len, data]

Note there is NO stem field, unlike OpenBOR. PICO-8 save names come from the
cart-chosen cartdata("id"), not from the filename, so two people whose copies
of a cart are named differently still produce identical save filenames and
there is nothing to remap. (The design note originally said otherwise; it was
corrected 2026-08-02.)

The frame CRC32 is CRC-32/ISO-HDLC, matching p8_crc32.

    p8rec_synth.py write out.inp --cart NAME [--frames N] [--hold MASK@S:E]
    p8rec_synth.py inspect take.inp
"""

import argparse
import binascii
import hashlib
import struct
import sys

MAGIC = b"P8REC"
CONTAINER = 5
ENGINE_VER = 1
CART_LEN = 256
MAX_FRAMES = 2000000


def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


def build_frames(n, holds):
    """One u32 per frame. holds is [(mask, start, end)]."""
    out = bytearray()
    for f in range(n):
        w = 0
        for mask, start, end in holds:
            if start <= f < end:
                w |= mask
        out += struct.pack("<I", w)
    return bytes(out)


def build_identity(entries):
    """entries is [(display_name, sha1_bytes)]. Zero entries is legal and means
    the take does not vouch for its content -- a pre-v5 take, or one whose cart
    could not be hashed. Such a take still plays; refusing would brick every
    recording in circulation."""
    b = struct.pack("<H", len(entries))
    for nm, h in entries:
        nb = nm.encode("utf-8")
        b += struct.pack("<H", len(nb)) + nb + h
    return b


def build_payload(files):
    b = struct.pack("<I", len(files))
    for name, data in files:
        nb = name.encode("utf-8")
        b += struct.pack("<I", len(nb)) + nb + struct.pack("<I", len(data)) + data
    return b


def payload_offset(cart="TestCart", frames=4, entries=None):
    """Byte offset of the payload's file_count. The C reader arrives here by
    sequential reads; this is the Python side computing the same place so a test
    driver can seek straight to it."""
    if entries is None:
        entries = [(cart, b"\x00" * 20)]
    ident = len(build_identity(entries))
    return 5 + 4 + 4 + CART_LEN + 4 + 4 + 4 + ident + frames * 4


def write_take(path, cart="TestCart", frames=4, holds=(), seed=1,
               entries=None, payload=(), container=CONTAINER,
               engine_ver=ENGINE_VER, bad_magic=False, corrupt_crc=False,
               claim_frames=None, truncate_after=None):
    if entries is None:
        entries = [(cart, hashlib.sha1(cart.encode()).digest())]
    fr = build_frames(frames, holds)
    n_claim = claim_frames if claim_frames is not None else frames
    crc = crc32(fr)
    if corrupt_crc:
        crc ^= 0xFFFFFFFF

    hdr = (b"XXXXX" if bad_magic else MAGIC)
    hdr += struct.pack("<I", container)
    hdr += struct.pack("<I", engine_ver)
    hdr += cart.encode("utf-8")[:CART_LEN].ljust(CART_LEN, b"\0")
    hdr += struct.pack("<i", seed)
    hdr += struct.pack("<I", n_claim)
    hdr += struct.pack("<I", crc)

    blob = hdr + build_identity(entries) + fr + build_payload(list(payload))
    if truncate_after is not None:
        blob = blob[:truncate_after]
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


def inspect(path):
    d = open(path, "rb").read()
    if len(d) < 281:
        print("too short: %d bytes" % len(d))
        return 1
    print("magic       %r" % d[0:5])
    cont, ever = struct.unpack("<II", d[5:13])
    print("container   %d" % cont)
    print("engine_ver  %d" % ever)
    print("cart        %s" % d[13:269].split(b"\0")[0].decode("utf-8", "replace"))
    seed, = struct.unpack("<i", d[269:273])
    n, crc = struct.unpack("<II", d[273:281])
    print("seed        %d" % seed)
    print("frames      %d" % n)
    print("crc32       %08x" % crc)
    i = 281
    cnt, = struct.unpack("<H", d[i:i + 2]); i += 2
    print("identity    %d entr%s" % (cnt, "y" if cnt == 1 else "ies"))
    for _ in range(cnt):
        nl, = struct.unpack("<H", d[i:i + 2]); i += 2
        print("  name      %s" % d[i:i + nl].decode("utf-8", "replace")); i += nl
        print("  sha1      %s" % d[i:i + 20].hex()); i += 20
    body = d[i:i + n * 4]
    print("frame block %d bytes at %d (crc %s)"
          % (len(body), i, "OK" if crc32(body) == crc else "MISMATCH"))
    i += n * 4
    print("payload at  %d" % i)
    if i + 4 <= len(d):
        fc, = struct.unpack("<I", d[i:i + 4]); i += 4
        print("payload     %d file(s)" % fc)
        for _ in range(fc):
            if i + 4 > len(d):
                print("  <truncated>"); break
            nl, = struct.unpack("<I", d[i:i + 4]); i += 4
            nm = d[i:i + nl].decode("utf-8", "replace"); i += nl
            dl, = struct.unpack("<I", d[i:i + 4]); i += 4
            print("  %-40s %d bytes" % (nm, dl))
            i += dl
    else:
        print("payload     <absent>")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write")
    w.add_argument("out")
    w.add_argument("--cart", default="TestCart")
    w.add_argument("--frames", type=int, default=4)
    w.add_argument("--seed", type=int, default=1)
    w.add_argument("--hold", action="append", default=[], metavar="MASK@START:END")
    i = sub.add_parser("inspect")
    i.add_argument("take")
    a = ap.parse_args()
    if a.cmd == "inspect":
        return inspect(a.take)
    holds = []
    for h in a.hold:
        mask, rest = h.split("@", 1)
        start, end = rest.split(":")
        holds.append((int(mask, 0), int(start), int(end)))
    n = write_take(a.out, cart=a.cart, frames=a.frames, holds=holds, seed=a.seed)
    print("wrote %s (%d bytes)" % (a.out, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
