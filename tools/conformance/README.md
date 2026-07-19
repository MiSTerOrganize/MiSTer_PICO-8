# z8conformance — zepto8 vs PICO-8 differential

Closes the PICO-8 **render/behaviour-correctness gap** in the diff harness: catches
the recurring "zepto8 behaves differently from PICO-8" bug class (oblivion_eve
secret palette, `tline` OOB wrap, extended-memory `peek` shorthand, Virtua Racing
fix32) — **systematically**, instead of one user report at a time.

## How it works
Each conformance cart (our content) exercises one tricky API, then emits its result
via `printh`:
- **`CONFVAL label=...`** — a value test (peek/pget/fix32 result). Reliable headless.
- **`CONFHASH=...`** — an in-cart hash of the raw framebuffer (`0x6000..0x7fff`).

Both PICO-8 engines run the **same cart** and print the **same** lines if conformant:
1. **Ground truth** = PICO-8 headless: `pico8 -x cart.p8` → `printh` → captured (LOCAL only).
2. **Under test** = `z8headless` (zepto8) → `printh` mirrors to stdout/stderr → captured.
3. **Diff** → any difference = a zepto8 conformance bug, pinned to the cart's API.

The official outputs are frozen into `goldens.txt` (license-safe — our carts' numbers,
NOT the PICO-8 binary), so `run_conformance.sh` diffs z8headless against the goldens
**without needing PICO-8** (CI-able).

## Conformance matrix (gen_matrix.py, 2026-07-19)

`gen_matrix.py` generates the `m_*.p8` cart families that sweep the API surface
(shapes incl. OOB/inverted/degenerate coords, fillp/clip/camera combos, spr/sspr/
palt, map/tline + mask registers, palette pipeline, print/cursor, P8SCII escapes
(isolated — a lexer gap can't mask other carts), fix32 math edges, memory ops,
strings). Rationale: carts can only misbehave through engine primitives, so
pinning the primitive surface protects carts nobody has played. Regenerate carts
with `python3 gen_matrix.py`, goldens with `gen_goldens.ps1` (local, Windows).

Cart boot code that exceeds one frame's CPU budget is suspended and resumed on
later frames — `run_conformance.sh` therefore runs `--frames 60` (one frame
truncates the matrix carts' screen-hash loops mid-cart).

**Status 2026-07-19: 15/18 carts conformant.** 3 known zepto8 divergences
(bug-tracked; goldens carry the reference truth to converge on):
- `m_shapes` — line rasterization (OOB-endpoint clipping + trig endpoints)
- `m_sprites` — sspr anisotropic scale rounding + negative dw/dh flips
- `m_strings` — fix32→string formatting (PICO-8 truncates at 4 decimals,
  zepto8 rounds: `"z"..-0.0001` → `z-0` vs `z-0.0001`)

## 🛑 License (PICO-8 is a LOCAL reference only)
PICO-8 (`#PICO-8_Official/`) is **never** committed/shipped/CI'd — it runs
only on the dev machine to regenerate goldens. We ship **zepto8** only. See
`feedback_pico8_license_compliance.md` + `feedback_official_pico8_reference_only.md`.

## Run (z8headless side — CI-able, no PICO-8 needed)
In a container/WSL with `z8headless` + `bios.p8` + the carts + `goldens.txt`:
```
Z8=./z8headless DATADIR=. sh run_conformance.sh
```
PASS/FAIL per cart; exits non-zero on any divergence.

## Regenerate goldens (LOCAL only, after an intentional change / new cart)
On the dev machine with PICO-8:
```
for c in *.p8; do pico8 -x "$c" -home /tmp/p8h ; done   # capture the "CONF*" printh lines
```
(strip the `INFO: ` prefix, `sort -u` per cart, write into `goldens.txt`). Then re-run
`run_conformance.sh` to confirm z8headless still matches.

## Adding a conformance cart
1. Write `<api>.p8` exercising the API; emit `CONFVAL`/`CONFHASH` via `printh`.
   Prefer `CONFVAL` (peek/pget) — the framebuffer-hash channel is unreliable for some
   draw ops in headless flat-cart mode (`mech_check` works; complex draw sequences may
   hash empty — use pget value tests instead).
2. Add the cart name to `CARTS` in `run_conformance.sh`.
3. Regenerate goldens (above) + commit cart + goldens.

## Carts (2026-06-18)
| Cart | Class it guards | Channel |
|---|---|---|
| `mech_check` | baseline draw ops + framebuffer-hash mechanism | CONFHASH |
| `fix32_math` | 16.16 fix32 arithmetic (Virtua Racing class) — 25 ops | CONFVAL |
| `peek_extmem` | extended-memory (0x8000+) peek/poke + `$`/`%`/`@` shorthand | CONFVAL |
| `pal_secret` | draw-pal / screen-pal / secret-palette region / fillp (oblivion_eve) | CONFVAL |
| `tline_map` | `tline` OOB mx/my must SKIP not wrap (oblivion_eve) | CONFVAL |
