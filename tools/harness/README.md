# PICO-8 differential bug-finding harness

Find broken carts across a whole library **without playing them**, off-device.

## Pieces
- `tools/z8headless.cpp` — engine-only (no SDL/MiSTer) frame dumper. Loads a
  cart, runs N frames (optional `--hold MASK` / `--input SCRIPT`), dumps frames
  as PNG. Built by `.github/workflows/diff_harness.yml` (Linux static + smoke).
- `tools/harness/scan_library.sh` — runs z8headless over every cart under a root
  with a timeout; logs `index|exitcode|md5_early|md5_late|relpath`.
- `tools/harness/analyze_scan.py` — classifies the scan into Crash / Hang /
  No-render / Black / Static / OK and prints a ranked watch-list.

## Signal reliability
- **CRASH (exit≠0) and HANG (exit 124): reliable** — a robust engine should
  never segfault/hang on any cart, so these are real bugs regardless of input.
- **BLACK / STATIC: noisy** without input — many carts sit on a black/static
  screen until a button is pressed (e.g. Zokorimoro shows black at frame 5/59
  but works fine). Re-run with `--hold` (e.g. mask 16 = O, 32 = X) to advance
  past intros before trusting black/stuck signals.

## Run it (via WSL or any Linux)
1. Dispatch `Diff Harness` CI; download the `z8headless-linux` artifact
   (`z8headless` + `bios.p8`).
2. Stage into a Linux env (WSL example):
   ```sh
   mkdir -p /tmp/z8 && cp z8headless bios.p8 /tmp/z8/ && chmod +x /tmp/z8/z8headless
   ```
3. Scan a library (carts reachable from the Linux env, e.g. /mnt/host/c/...):
   ```sh
   sh tools/harness/scan_library.sh "/path/to/Carts"        # no input
   sh tools/harness/scan_library.sh "/path/to/Carts" 32     # hold X to pass titles
   ```
4. Analyze on the host: `python tools/harness/analyze_scan.py scan_results.txt`

## Drill into a flagged cart
```sh
/tmp/z8/z8headless --cart "/path/to/Cart.p8.png" --frames 60 --dump 5,30,59 --out out
```
Then view the PNGs. To reproduce a deep-state bug, use `--input SCRIPT`
(`FRAME MASK` lines) to script the controller.

## Notes
- `.p8.png` decode needs the real engine (the proven Linux/ARM build); the MinGW
  Windows build mis-decodes them (LLP64 / portability), so use Linux/WSL.
- Multicart *data banks* (`bank_*.p8`, sub-carts) render black standalone — those
  are false positives in the BLACK list (they're data, not entry carts).
