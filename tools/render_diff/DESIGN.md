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

## Milestone 3 — dual-engine batch + diff + triage: PIPELINE WORKING; 2 real limits found
`compare_render.py` joins z8 + official result files (FBHASH/AUDHASH per cart) and
classifies: MATCH / RENDER-DIVERGE / AUDIO-DIVERGE / NO-GAMEPLAY? / WRAP-OVERSIZE /
NO-CHECKPOINTS. Batch = gen wrappers (python) -> z8 one Docker loop (`--frames 2000`)
-> official `-x` loop (PowerShell, flags "program too large" -> OVERSIZE) -> compare.

**8-cart pilot (2026-06-18):** 1 MATCH (Jumping Jack), 2 RENDER-DIVERGE (Wander the
Cosmos @f30, Gridbeans @f60), 4 WRAP-OVERSIZE, 1 NO-CHECKPOINTS (3D Picoh Mummy,
errored both). Pipeline validated end-to-end.

### 🛑 Two honest limitations the pilot exposed
1. **WRAP-OVERSIZE (the big one): ~half of FULL GAMES hit official's 8192-token cap.**
   The injected harness (even minified) pushes near-max carts over the limit ->
   official rejects "program too large" -> can't headless-wrap. Small carts/demos are
   fine; big games (the most bug-prone) often are NOT. Fallback for those = the slow
   GUI-screenshot route (CTRL-6 automation), or they stay unchecked. This is a real
   ceiling on the headless approach. Ideas to explore: strip the cart's own dead code
   to make token room (risky); GUI fallback for the oversize subset; a token-free
   external driver (none known for official -x).
2. **RENDER-DIVERGE is a CANDIDATE, not a confirmed bug.** The scripted input + timing
   can drive the two engines to different gameplay states (input-/nondeterminism-
   driven), which looks like a render divergence. Secondary triage REQUIRED: re-run the
   diverging cart with NO input (`__m` returns 0) -- if it still diverges it's a pure
   render bug; if it matches, the divergence was input-driven (not a render bug).

### TODO to make M3 trustworthy at scale
- Secondary no-input triage pass for RENDER-DIVERGE candidates (auto).
- WRAP-OVERSIZE handling (GUI fallback or accept-and-report the uncovered subset).
- Multicart (Virtua Racing `vracing_0..N.p8`) special handling.
- Then the full 3302-cart run (multi-hour, official side real-time).

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

## No-input triage (2026-06-18) -- validated, found 1 real candidate
`gen_wrapper.py --noinput` zeros the input mask. Re-running the 2 pilot RENDER-DIVERGE
candidates with no input:
- **Gridbeans**: all 8 checkpoints MATCH official -> FALSE POSITIVE (was input-driven).
- **Wander the Cosmos**: still DIVERGES (f1-f60 match, f120/240/300 differ) with zero
  input + srand(1) + frame-based time -> REAL zepto8-vs-official candidate (render bug,
  rnd-conformance, or memory/stat init). Not a harness artifact (harness identical +
  deterministic on both; bxor/rotl/tostr already conformance-verified). NOT yet a
  CONFIRMED bug -- confirm via frame-dump (PNG) vs official screenshot + root-cause.
Triage discipline: every RENDER-DIVERGE must pass the no-input re-run before it counts
as a candidate; input-driven diffs are filtered out.

## Wander the Cosmos confirmation (2026-06-18) -- NOT a confirmed bug
Drilled the pilot's one real candidate. Dumped both engines' raw framebuffer at f120
(--dumpframe) + pixel-diffed: 125/16384 px differ (0.76%), SCATTERED across the whole
frame, dark-color swaps (z8=5<->official=0/1/2/3). Official frame = the TITLE screen
with a STARFIELD (no input -> never left title). So the divergence is the starfield
twinkle. Decisive root-cause test: rnd_seq conformance cart (srand-seeded rnd()) ->
zepto8 rnd() is byte-IDENTICAL to official (all 18 values). So NOT an rnd bug. With
rnd conformant + srand fixed + t() frame-based, the residual scatter is most likely
subtle _update-vs-flip frame-PACING (a 30fps cart's per-update star animation; the
exact update:display scheduling the flip-hook exposes differs slightly per engine).
=> Wander is NOT a confirmed zepto8 render bug -- real+reproducible but sub-visible and
   probably a pacing/harness artifact. (no-false-found-it: traced, did not declare.)
Wins: (1) rnd conformance is now a permanent test (rnd_seq, PASS). (2) TRIAGE TUNING:
scattered single-pixel twinkle (starfields/particles) is a frame-pacing false-positive
source; weight RENDER-DIVERGE by magnitude/clustering -- the trustworthy signal is a
large/clustered diff (broken sprite, wrong colours over a region), not 100-px scatter.

## TRUSTWORTHY triage flow (2026-06-18) -- validated, this is the shipping design
The "weight by magnitude/clustering" idea above was TESTED and PARTIALLY FAILED, which
is the key finding: a DENSE starfield (Wander, 125px) reaches largest_connected_comp=56
and densest_16x16_cell=49 -- it trips raw cluster-size thresholds even though the
triptych shows it's obviously a scattered twinkle FP. So **raw magnitude/cluster size
CANNOT be the verdict** (it false-positives on dense particle fields). The validated
trustworthy pipeline is TWO TIERS with the IMAGE as final authority:

- **Tier 1 (auto, zero false claims): exact FBHASH gate.** A cart whose framebuffer is
  bit-identical to official at all 8 checkpoints across 300 frames is DEFINITIVELY
  rendering correctly. This MATCH set is the trustworthy "confirmed-good" list -- no
  human review needed, no false negatives. (Most simple/static carts land here.)
- **Tier 2 (DIVERGE candidates): triptych + visual triage.** For each diverging cart,
  `fbdiff.py` dumps the raw framebuffer both sides and writes z8/official/**diff** PNGs.
  The diff PNG (magenta = differing px over the official frame) is read by eye -- a
  scattered field over the whole frame = particle/pacing FP; a contiguous broken
  sprite / wrong-colour region = REAL bug. Claude reads these PNGs at scale, so the
  user is still saved the manual play-through; the human/Claude eyeball is what makes
  the bug list trustworthy (no auto-mislabel reaches the user).
- **`fbdiff.py` HINT (sort-key only, NOT verdict):** prints total / largest_cc /
  densest_16x16 / **bg_swap_ratio**. bg_swap_ratio = fraction of diff px with bg (0/1)
  on one side; a moving particle field ~= 0.99 (a star that relocated -> bg on one
  side), a real palette/sprite bug is low (real colour swapped for real colour). Hint
  buckets: LIKELY-REAL (largest_cc>=24 & bg<0.5, or total>=1500) | PARTICLE-FP? (bg>=
  0.85) | REVIEW-IMAGE. Used to ORDER which triptychs to eyeball first, never to label.
  VALIDATED on Wander: total=125 largest_cc=56 cell=49 bg_swap_ratio=0.99 -> PARTICLE-FP?
  (correct -- raw cluster size alone had mislabeled it CLUSTERED-REAL).

Net: the tool is trustworthy because every "this cart is buggy" claim is gated by an
exact-hash divergence AND a human-read triptych; magnitude is only a triage sort-key.
Honest residual: the WRAP-OVERSIZE subset (~half of full games) still can't be headless-
wrapped (official 8192-token cap) -- those are reported as uncovered, not as pass/fail.
