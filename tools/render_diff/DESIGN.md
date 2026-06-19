# Full-library PICO-8 render-diff (zepto8 vs official PICO-8) — design + status

GOAL: auto-triage the whole cart library (3302 carts) — flag every cart where zepto8
(what we ship) renders differently from official PICO-8 (ground truth). Turns
"play 3302 carts by hand" into "review the flagged ~dozens." This is why official
PICO-8 was bought. Will flag + then fix the Virtua Racing track corruption (task #13).

## Milestone 1 — DRIVE mechanism: ✅ VALIDATED 2026-06-18
A cart driven frame-by-frame, framebuffer-hashed at checkpoints, produces IDENTICAL
hashes on official PICO-8 (`-x` headless) and z8headless. Proven apples-to-apples on
`_mechcheck_driven.p8`: f1=0xd70e.d70c, f30=0xf264.e32b, f120=0x4001.4ea4,
f300=0x8dd9.96fd — all 4 match on both engines.

Validated recipe (the harness to inject into each cart):
- `srand(<fixed>)` + override `btn`/`btnp` to a scripted mask → determinism.
- Force **60fps** with `_update60` (so `_draw` is 1:1 with display frames; a 30fps
  `_update`-only cart desyncs because z8headless counts 60fps display frames while
  `_update` runs at 30fps → counter reaches only half).
- Count frames in `_update60`; in `_draw`, at checkpoint frames hash `0x6000..0x7fff`
  (`h=bxor(rotl(h,3),@a)`) and `printh("FBHASH f"..f.."="..tostr(h,true))`.
- z8headless: `--frames N` (N ≥ last checkpoint + margin). official: `-x` runs the
  loop; `stop()` past the last checkpoint quits cleanly.
- Capture: zepto8 `printh`→stdout+stderr; official `printh`→terminal (strip `INFO: `).

## Milestone 2 — wrapper generator (real carts): TODO
Per cart: (1) get the cart's full content — `.p8` direct; `.p8.png` decompress→`.p8`
(shrinko8 / official `pico8` load+save, LOCAL). Must preserve __gfx__/__map__/__sfx__,
not just code. (2) Inject the harness: wrap the cart's `_init`/`_update`/`_update60`/
`_draw` so the wrapper forces 60fps + counts + checkpoint-hashes (without breaking the
cart's own loop). (3) Throwaway wrapper — never written back to the user's cart.

## Milestone 3 — dual-engine batch + diff + triage: TODO
For each cart: run wrapper on official PICO-8 `-x` (LOCAL goldens) + z8headless; diff
the per-checkpoint FBHASH lines; emit a triaged report: MATCH / DIVERGE / errored /
non-deterministic. Multicart (e.g. Virtua Racing `vracing_0..N.p8`) handled specially
(wrap entry + drive into the buggy sub-cart).

## Honest limits (state in the report)
- Non-deterministic carts (rnd-from-time/entropy) → false positives; `srand` fixes
  most, the rest are FLAGGED FOR REVIEW, not auto-failed.
- Carts needing tailored input to reach the buggy scene (generic "hold X" gets most
  past titles; some need per-cart input).
- Catches RENDER divergences only (crashes/hangs already covered by the diff harness;
  audio would be a separate diff). Forcing 60fps changes a 30fps cart's behaviour but
  both engines run the SAME forced sequence, so the diff stays valid.
- Outcome = automated TRIAGE (big time-save), not zero-false-positive magic.

## License
Official PICO-8 (`#PICO-8_Official/`) runs LOCAL only — never CI/committed. Goldens
(our carts' output numbers) are committable; the binary is not. See
feedback_pico8_license_compliance.md + feedback_official_pico8_reference_only.md.
