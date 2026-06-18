#!/usr/bin/env python3
# shrinko8 side of the decode-differential. Decompresses every cart in the
# library IN-PROCESS via shrinko8's pico_cart.read_cart (the gold-standard open
# PICO-8 decoder) and records, per cart: relpath|status|charlen|md5(normalized).
# The z8headless side (our engine's --dumpcode) is gathered separately; a join
# script flags carts where our decode diverges (truncation / garbage / failure)
# -> a bug in OUR cart loader/decompressor (the Dungeon-Rider sizeof + pxa-OOB
# class), caught library-wide.
#
#   python decode_diff_shrinko.py <shrinko8_dir> <carts_root> <out_file>
import sys, os, hashlib, io, contextlib

shrinko_dir, root, out = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, shrinko_dir)
from pico_cart import read_cart  # noqa: E402

def norm(code):
    # Encoding-robust normalization: unify newlines, strip trailing blank space.
    return code.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n ")

carts = []
for dirpath, _dirs, files in os.walk(root):
    for f in files:
        fl = f.lower()
        if fl.endswith(".p8") or fl.endswith(".p8.png"):
            carts.append(os.path.join(dirpath, f))
carts.sort()

with open(out, "w", encoding="utf-8", newline="\n") as fo:
    for i, path in enumerate(carts, 1):
        rel = os.path.relpath(path, root).replace("\\", "/")
        try:
            # Suppress shrinko8's chatter so it doesn't pollute the run log.
            with contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                cart = read_cart(path)
            code = norm(cart.code or "")
            h = hashlib.md5(code.encode("utf-8", "surrogatepass")).hexdigest()[:12]
            fo.write(f"{i}|OK|{len(code)}|{h}|{rel}\n")
        except Exception as e:
            msg = str(e).replace("|", "/").replace("\n", " ")[:60]
            fo.write(f"{i}|FAIL|0|{msg}|{rel}\n")
        if i % 400 == 0:
            print(f"...shrinko decoded {i}/{len(carts)}", flush=True)
print(f"SHRINKO DECODE DONE: {len(carts)} carts -> {out}", flush=True)
