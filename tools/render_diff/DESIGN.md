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

## Milestone 2 — wrapper generator (real carts): ✅ DONE 2026-06-18
`gen_wrapper.py` — `.p8` direct; `.p8.png` decompressed via **shrinko8** to a full
`.p8` (preserves __gfx__/__map__/__sfx__/__music__). Injects the harness by
**PREPENDING** (right after `__lua__`, before the cart's code).

KEY DESIGN (better than the M1 append approach): the harness **overrides `flip()`** —
the UNIVERSAL per-frame hook. Verified: the engine calls global `flip()` to present
each frame for modern `_update`/`_draw` carts AND old top-level flip-loop carts call
it manually — so one hook catches every cart structure, and `flip` == one display
frame so cadence aligns automatically (no `_update` 30/60 issue). The harness also
overrides `t()`/`time()` to be frame-based (`__rd_f/60`) so time-driven carts run the
same #frames on both engines, and `srand(1)`/`btn`/`btnp` for determinism. It hashes
`0x6000` at each flip checkpoint and `stop()`s past the last one.

VALIDATED: synthetic modern `_update60` cart — full match. **campfire.p8.png**
(flip-loop + time-driven) — all 8 checkpoints (f1/f2/f8/f30/f60/f120/f240/f300)
**identical** between official PICO-8 and z8headless = conformant.

RUN CONVENTION: pass z8headless `--frames` GENEROUSLY (e.g. 2000) — flip-loop carts
flip < once per engine-frame, so z8 needs extra engine-frames to reach the checkpoint
flip-count; the cart's `stop()` bounds it once flips hit the target, so over-
provisioning is free. official `-x` is frame-based 1:1.

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
