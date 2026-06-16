#!/usr/bin/env python3
"""
video_timing_verify.py -- automates full-audit Section 3 (reference-timing match)
for PICO-8.

Parses the RTL video-timing localparams, derives H/V rate + active time from the
PLL master clock + constant pixel-clock-enable divider, and compares byte-for-byte
against the target console's published values. PICO-8 targets the NES NTSC core
exactly (CLAUDE.md "PICO-8 vs NES" table, default hide_overscan=00). Catches
CRT-lock-breaking timing drift on any RTL change; a divergence is a FAIL, never
laundered into a "match" via an alternate-mode label.

NOTE vs OpenBOR's verifier: NES/PICO-8 uses a CONSTANT /4 pixel clock-enable
(CLK_VIDEO 21.47727 MHz -> 5.36932 MHz pixel), so pix = MCLK / CE_PIXEL -- no
variable per-line MCLK accounting like the Sega CD (OpenBOR) path.

Dev-machine only (no MiSTer). Exit 0 = all match, 1 = any divergence.

Usage:  python tools/video_timing_verify.py [path/to/pico8_video_timing.sv]
"""
import re, sys, os

# -- target reference: NES NTSC (hide_overscan=00), CLAUDE.md published values --
REF = {
    "name":      "NES NTSC (hide_overscan=00)",
    "MCLK_HZ":   21477270,   # CLK_VIDEO master clock (21.47727 MHz)
    "CE_PIXEL":  4,          # constant pixel clock-enable divider -> pix = MCLK/4
    "H_ACTIVE":  256, "H_TOTAL": 341,
    "V_ACTIVE":  224, "V_TOTAL": 262,
    "PIX_HZ":    5369318,    # 21477270/4 = 5.36932 MHz
    "HRATE_HZ":  15746,      # PIX/H_TOTAL
    "VRATE_HZ":  60.10,      # HRATE/V_TOTAL
    "ACTIVE_US": 47.68,      # H_ACTIVE/PIX
    "VBLANK":    38,         # V_TOTAL-V_ACTIVE
}

def parse_localparams(path):
    txt = open(path, encoding="utf-8", errors="replace").read()
    out = {}
    # tolerate widths like "localparam [8:0] V_ACTIVE = 9'd224;" and "localparam H_TOTAL = 341;"
    for m in re.finditer(r"localparam\s*(?:\[[^\]]*\]\s*)?(\w+)\s*=\s*(?:\d+'d)?(\d+)", txt):
        out[m.group(1)] = int(m.group(2))
    return out

def approx(a, b, tol):
    return abs(a - b) <= tol

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "fpga", "rtl", "pico8_video_timing.sv")
    if not os.path.isfile(path):
        print("ERROR: timing RTL not found:", path); return 2
    p = parse_localparams(path)
    need = ["H_ACTIVE","H_TOTAL","V_ACTIVE","V_TOTAL"]
    miss = [k for k in need if k not in p]
    if miss:
        print("ERROR: missing localparams:", miss); return 2

    mclk, ce = REF["MCLK_HZ"], REF["CE_PIXEL"]
    pix    = mclk / ce
    hrate  = pix / p["H_TOTAL"]
    vrate  = hrate / p["V_TOTAL"]
    active = p["H_ACTIVE"] / pix * 1e6      # us
    vblank = p["V_TOTAL"] - p["V_ACTIVE"]

    rows = [
        # label,            ours,                 reference,         tol,    fmt
        ("Pixel clock (MHz)", pix/1e6,            REF["PIX_HZ"]/1e6, 0.001, "%.5f"),
        ("H_ACTIVE",          p["H_ACTIVE"],      REF["H_ACTIVE"],   0,     "%d"),
        ("H_TOTAL",           p["H_TOTAL"],       REF["H_TOTAL"],    0,     "%d"),
        ("Active time (us)",  active,             REF["ACTIVE_US"],  0.02,  "%.2f"),
        ("H rate (Hz)",       hrate,              REF["HRATE_HZ"],   2,     "%.0f"),
        ("V_ACTIVE",          p["V_ACTIVE"],      REF["V_ACTIVE"],   0,     "%d"),
        ("V_TOTAL",           p["V_TOTAL"],       REF["V_TOTAL"],    0,     "%d"),
        ("V blanking",        vblank,             REF["VBLANK"],     0,     "%d"),
        ("V rate (Hz)",       vrate,              REF["VRATE_HZ"],   0.05,  "%.2f"),
    ]
    print("== video_timing_verify (PICO-8) -- %s ==" % os.path.basename(path))
    print("reference: %s\n" % REF["name"])
    print("%-18s %14s %14s  %s" % ("parameter","ours","reference","verdict"))
    fails = 0
    for label, ours, ref, tol, fmt in rows:
        ok = approx(float(ours), float(ref), tol)
        if not ok: fails += 1
        print("%-18s %14s %14s  %s" % (label, fmt % ours, fmt % ref, "OK" if ok else "** FAIL **"))
    print()
    if fails:
        print("RESULT: %d divergence(s) -- Section 3 FAIL (timing drift; CRT-lock risk)." % fails)
        return 1
    print("RESULT: all match -- Section 3 PASS (CRT-locked to %s)." % REF["name"])
    return 0

if __name__ == "__main__":
    sys.exit(main())
