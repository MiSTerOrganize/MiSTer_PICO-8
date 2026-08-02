#!/usr/bin/env python3
"""test_snap_payload.py -- exercise the REAL p8snap_read parser, natively.

Sibling of MiSTer_OpenBOR_7533/tools/harness/test_snap_extract.py, and the same
argument applies: p8snap_read turns bytes written by a stranger into filenames,
and the .p8d.txt extension check is a security control rather than a nicety.

The threat is specific and already documented in the function: the bare-name
check ALONE accepted an entry called <cart>.p8, which is where cstore overlays
live -- so vm::load_cart would splice a stranger's bytes over the cart's ROM
(spritesheet, map, sfx, music) before the first frame. That is contained to the
data region, so not code execution, but it renders arbitrary attacker content
while the viewer believes they are watching the real cart. This project has
already seen a cart pack track geometry into its sfx region, so "data" here is
control flow inside the cart.

Like the OpenBOR one, this does NOT reimplement the parser. It cuts
struct P8SnapFile and p8snap_read out of src/mister_main.cpp and compiles them
natively with a test driver. PICO-8 needs no patcher, so the cut is from the
real source directly -- if the function changes shape, this stops compiling
rather than quietly testing a stale copy.

Scope, stated honestly: this covers p8snap_read (the payload parser), not
p8rec_load's header/identity handling, which is entangled with globals and the
notice renderer. The payload is the part that parses untrusted names.

    python3 test_snap_payload.py [--keep]
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import p8rec_synth  # noqa: E402

PROLOGUE = r"""
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
"""

TEST_MAIN = r"""
/* Driver: seek to the payload offset given on argv, then call the real reader.
 * Exit codes are DISTINCT -- 2 is a driver/usage error, 0 accepted, 1 refused --
 * so a driver mistake can never be mistaken for a refusal. That happened in the
 * OpenBOR sibling: eleven refusal checks passed while executing nothing, because
 * they only asserted "rc != 0" and the driver was exiting 2. */
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <take> <payload_offset>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    if (fseek(f, atol(argv[2]), SEEK_SET) != 0) { fclose(f); return 2; }
    std::vector<P8SnapFile> v;
    bool ok = p8snap_read(f, v);
    fclose(f);
    if (!ok) { printf("REFUSED\n"); return 1; }
    printf("ACCEPTED %d\n", (int)v.size());
    for (size_t i = 0; i < v.size(); i++)
        printf("  %s %d\n", v[i].name.c_str(), (int)v[i].data.size());
    return 0;
}
"""

RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond), detail))
    print("  %-56s %s%s" % (name, "PASS" if cond else "FAIL",
                            ("  -- " + detail) if detail and not cond else ""))


def cut_source():
    src = open(os.path.join(REPO, "src", "mister_main.cpp"),
               encoding="utf-8", errors="replace").read()
    st = src.find("struct P8SnapFile")
    if st < 0:
        raise SystemExit("struct P8SnapFile not found in mister_main.cpp")
    struct_decl = src[st:src.find("\n", st) + 1]

    fs = src.find("static bool p8snap_read")
    if fs < 0:
        raise SystemExit("p8snap_read not found in mister_main.cpp")
    # the function ends at the first line that is exactly "}" at column 0
    fe = src.find("\n}\n", fs)
    if fe < 0:
        raise SystemExit("could not find the end of p8snap_read")
    return struct_decl + "\n" + src[fs:fe + 3]


def compile_probe(work, body):
    csrc = os.path.join(work, "probe.cpp")
    open(csrc, "w", encoding="utf-8", newline="\n").write(
        PROLOGUE + "#include <cstdlib>\n" + body + TEST_MAIN)
    exe = os.path.join(work, "probe")
    r = subprocess.run(["g++", "-O1", "-g", "-std=c++17", "-o", exe, csrc],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-4000:])
        raise SystemExit("the extracted p8snap_read did not compile")
    return exe


def main():
    keep = "--keep" in sys.argv
    work = tempfile.mkdtemp(prefix="p8snap_")
    print("workdir:", work)
    try:
        exe = compile_probe(work, cut_source())
        print("p8snap_read compiled natively from src/mister_main.cpp\n")

        def drive(name, payload, **kw):
            p = os.path.join(work, name)
            frames = kw.pop("frames", 4)
            cart = kw.pop("cart", "TestCart")
            p8rec_synth.write_take(p, cart=cart, frames=frames, payload=payload, **kw)
            off = p8rec_synth.payload_offset(cart=cart, frames=frames)
            r = subprocess.run([exe, p, str(off)], capture_output=True, text=True)
            return r.returncode, (r.stdout + r.stderr).strip()

        # ---- the allowed shape ---------------------------------------------
        rc, out = drive("ok.inp", [("mygame.p8d.txt", b"CARTDATA"),
                                   ("other_id.p8d.txt", b"MORE")])
        check("valid payload: accepted, both entries", rc == 0 and "ACCEPTED 2" in out, out)

        rc, out = drive("empty.inp", [])
        check("empty payload: accepted, zero entries", rc == 0 and "ACCEPTED 0" in out, out)

        # ---- the whitelist: this is the control ----------------------------
        for label, entry in [
            ("ROM overlay <cart>.p8", "mygame.p8"),
            ("cart image .p8.png", "mygame.p8.png"),
            ("path traversal ../", "../escape.p8d.txt"),
            ("absolute path", "/etc/passwd.p8d.txt"),
            ("backslash separator", "..\\escape.p8d.txt"),
            ("subdir prefix", "sub/mygame.p8d.txt"),
            ("dot", "."),
            ("dotdot", ".."),
            ("empty name", ""),
            ("bare extension only", ".p8d.txt"),
            ("wrong extension .txt", "mygame.txt"),
            ("shell script", "evil.sh"),
            ("no extension", "mygame"),
        ]:
            rc, out = drive("bad.inp", [(entry, b"X")])
            check("refuse: %s" % label, rc == 1, "rc=%d %s" % (rc, out))

        # A rejected entry must refuse the WHOLE payload. Playing on with fewer
        # save files than were recorded is a desync dressed as a warning.
        rc, out = drive("mixed.inp", [("good.p8d.txt", b"G"), ("evil.p8", b"E")])
        check("refuse: one bad entry rejects the ENTIRE payload",
              rc == 1, "rc=%d %s" % (rc, out))

        # order must not matter -- the bad entry first is the same verdict
        rc, out = drive("mixed2.inp", [("evil.p8", b"E"), ("good.p8d.txt", b"G")])
        check("refuse: bad entry FIRST also rejects everything",
              rc == 1, "rc=%d %s" % (rc, out))

        # ---- resource guards ------------------------------------------------
        # A shared take must not be able to drive allocation on a 1 GB board that
        # shares its RAM with the FPGA.
        import struct as _s
        blob = _s.pack("<I", 1) + _s.pack("<I", 14) + b"mygame.p8d.txt" + _s.pack("<I", 32 << 20)
        p = os.path.join(work, "huge.inp")
        p8rec_synth.write_take(p, frames=2, payload=[])
        with open(p, "r+b") as fh:
            fh.seek(p8rec_synth.payload_offset(frames=2)); fh.write(blob)
        r = subprocess.run([exe, p, str(p8rec_synth.payload_offset(frames=2))],
                           capture_output=True, text=True)
        check("refuse: per-entry size cap (32 MiB entry)", r.returncode == 1,
              (r.stdout + r.stderr).strip())

        blob = _s.pack("<I", 4097)
        with open(p, "r+b") as fh:
            fh.seek(p8rec_synth.payload_offset(frames=2)); fh.write(blob)
        r = subprocess.run([exe, p, str(p8rec_synth.payload_offset(frames=2))],
                           capture_output=True, text=True)
        check("refuse: file-count cap (4097 files)", r.returncode == 1,
              (r.stdout + r.stderr).strip())

        # ---- truncation is refusal, not "an older take" ---------------------
        p2 = os.path.join(work, "trunc.inp")
        off = p8rec_synth.payload_offset(frames=2)
        p8rec_synth.write_take(p2, frames=2,
                               payload=[("mygame.p8d.txt", b"X" * 64)])
        with open(p2, "r+b") as fh:
            fh.truncate(off + 10)
        r = subprocess.run([exe, p2, str(off)], capture_output=True, text=True)
        check("refuse: payload truncated mid-record", r.returncode == 1,
              (r.stdout + r.stderr).strip())

        with open(p2, "r+b") as fh:
            fh.truncate(off)
        r = subprocess.run([exe, p2, str(off)], capture_output=True, text=True)
        check("refuse: payload count absent (truncation, not 'no payload')",
              r.returncode == 1, (r.stdout + r.stderr).strip())

    finally:
        if keep:
            print("\nkept:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)

    bad = [n for n, ok, _ in RESULTS if not ok]
    print("\n%d/%d checks passed" % (len(RESULTS) - len(bad), len(RESULTS)))
    if bad:
        print("FAILED:")
        for n in bad:
            print("  -", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
