#!/usr/bin/env python3
"""
crash_symbolize.py -- turn an PICO-8 SIGSEGV dump into "crashed in
<function>:<line>". Crashes are the most expensive debug class on this project;
this collapses the manual PC/LR offset math + addr2line into one command.

The engine's mister_crash_handler (patches/sdlport_patch.c) prints:
    === CRASH: signal N at address 0x... ===
      PC = 0x........    LR = 0x........    SP/R0/R1/R2 = ...
    Maps (first 5):
      <base>-<end> r-xp <off> ... /media/fat/games/PICO-8/PICO-8
    === END CRASH ===

The shipped binary is STRIPPED + PIE, so we need (1) the matching UNSTRIPPED
binary to resolve symbols, and (2) PC - load_base to get the file address.

Function-level resolution is pure-Python (32-bit ELF .symtab/.dynsym), so it
runs anywhere. If arm-linux-gnueabihf-addr2line is on PATH and the binary has
-g, it also prints source:line.

Usage:
    python tools/crash_symbolize.py <unstripped_binary> [dump.txt]
    ...crash dump on stdin if no dump file given.
"""
import sys, re, struct, subprocess, shutil

def read_elf_funcs(path):
    """Return sorted [(value, size, name)] for FUNC symbols in a 32-bit LE ELF."""
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        raise SystemExit("not a 32-bit ELF: " + path)
    # ELF32 header: e_shoff@32, e_shentsize@46, e_shnum@48, e_shstrndx@50
    e_shoff   = struct.unpack_from("<I", d, 32)[0]
    e_shentsz = struct.unpack_from("<H", d, 46)[0]
    e_shnum   = struct.unpack_from("<H", d, 48)[0]
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i*e_shentsz
        name, typ, flags, addr, offset, size, link, info, align, entsz = struct.unpack_from("<10I", d, off)
        secs.append({"name":name,"type":typ,"offset":offset,"size":size,"link":link,"entsz":entsz})
    # find symtab (type 2) preferred, else dynsym (type 11)
    sym = next((s for s in secs if s["type"]==2), None) or next((s for s in secs if s["type"]==11), None)
    if not sym:
        return []
    strtab = secs[sym["link"]]
    strs = d[strtab["offset"]:strtab["offset"]+strtab["size"]]
    funcs = []
    n = sym["size"]//16
    for i in range(n):
        o = sym["offset"] + i*16
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", d, o)
        if (st_info & 0xf) != 2:   # STT_FUNC
            continue
        end = strs.find(b"\x00", st_name)
        nm = strs[st_name:end].decode("ascii","replace")
        if nm and st_value:
            funcs.append((st_value, st_size, nm))
    funcs.sort()
    return funcs

def resolve(funcs, addr):
    """Nearest enclosing FUNC for a file virtual address."""
    lo, hi, best = 0, len(funcs)-1, None
    while lo <= hi:
        mid = (lo+hi)//2
        if funcs[mid][0] <= addr:
            best = funcs[mid]; lo = mid+1
        else:
            hi = mid-1
    if not best:
        return None
    val, size, nm = best
    if size and addr >= val+size:
        return None
    return "%s+0x%x" % (nm, addr-val)

def addr2line(binary, faddr):
    exe = shutil.which("arm-linux-gnueabihf-addr2line") or shutil.which("addr2line")
    if not exe:
        return None
    try:
        out = subprocess.check_output([exe,"-f","-C","-e",binary,"0x%x"%faddr],
                                      stderr=subprocess.DEVNULL, text=True).strip().splitlines()
        if len(out)>=2 and out[1] not in ("??:0","??:?"):
            return "%s  (%s)" % (out[0], out[1])
    except Exception:
        pass
    return None

def parse_dump(text, binpath):
    g = lambda pat: (re.search(pat, text) or [None,None])[1] if re.search(pat, text) else None
    sig  = g(r"signal (\d+)")
    fault= g(r"at address (0x[0-9a-fA-F]+|\(nil\))")
    regs = {}
    for r in ("PC","LR","SP","R0","R1","R2"):
        m = re.search(r"%s = (0x[0-9a-fA-F]+)" % r, text)
        if m: regs[r] = int(m.group(1),16)
    # load base: lowest mapping start for the binary basename in the Maps block
    base = None
    bn = binpath.replace("\\","/").split("/")[-1]
    for m in re.finditer(r"([0-9a-f]+)-[0-9a-f]+ ..x. ([0-9a-f]+).*?(\S+)$", text, re.M):
        start, off, path = int(m.group(1),16), int(m.group(2),16), m.group(3)
        if path.endswith(bn) or "PICO-8" in path:
            if base is None or start < base: base = start
    return sig, fault, regs, base

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    binary = sys.argv[1]
    text = open(sys.argv[2],encoding="utf-8",errors="replace").read() if len(sys.argv)>2 else sys.stdin.read()
    funcs = read_elf_funcs(binary)
    if not funcs:
        print("WARNING: no FUNC symbols in %s -- is it stripped? Use the UNSTRIPPED CI artifact." % binary)
    sig, fault, regs, base = parse_dump(text, binary)
    print("== crash_symbolize ==")
    print("binary : %s  (%d FUNC symbols)" % (binary, len(funcs)))
    print("signal : %s   fault addr: %s" % (sig, fault))
    if base is None:
        print("load base: NOT FOUND in Maps -- assuming non-PIE (file addr = runtime addr).")
        base = 0
    else:
        print("load base: 0x%x" % base)
    print()
    for r in ("PC","LR"):
        if r not in regs: continue
        runtime = regs[r]; faddr = runtime - base
        sym = resolve(funcs, faddr) if funcs else None
        line = addr2line(binary, faddr) if funcs else None
        tag = {"PC":"crashed at","LR":"called from"}[r]
        print("%-11s 0x%08x  (file 0x%x)" % (r+":", runtime, faddr))
        print("   %-9s %s" % (tag+":", sym or "<no symbol>"))
        if line: print("   line:     %s" % line)
    return 0

if __name__ == "__main__":
    sys.exit(main())
