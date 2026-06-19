# Full-library render-diff -- findings (2026-06-19)

zepto8 (what we ship) vs official PICO-8 (ground truth, local-only), 3302-cart library.

## Funnel
- 3302 carts -> 3275 wrappable (27 skipped: 25 POOM data-only sub-carts w/ no __lua__, 2 malformed PNGs).
- Tier-1 (input + 8 checkpoints): 928 MATCH | 1099 RENDER-DIVERGE | 250 AUDIO-DIVERGE |
  89 NO-GAMEPLAY? | 674 WRAP-OVERSIZE | 235 NO-CHECKPOINTS.
- Tier-2 SEQ (no-input, every-frame, on the 1099): 712 PACING-FP (demoted) | 347 REAL-CANDIDATE |
  35 REVIEW-IMAGE | 5 NO-SEQ.
- Eyeball pass (382 candidates -> 364 with a nonzero no-input diff): ranked by largest_cc*(1-bg_swap),
  scanned ALL via 16 contact-sheet montages + 13 full-res confirmations.

## Confirmed REAL render bugs: 3 (2 root causes)
1. REM - Dream Generator (id 00344) -- PALETTE/COLOUR bug. Identical 3D corridor geometry +
   "LAYER 0" text, but zepto8 renders washed-out pink/grey where official shows vivid
   orange/yellow textured walls. pal()/secret-palette class (cf. oblivion_eve fix).
2. Mina and the Misty Forest (id 01174) -- BACKGROUND-TILE bug. Title/character/text in identical
   positions; zepto8 fails to render the decorative forest border (flat vs dense flower/leaf tiles).
3. Bomba Cum Laude (id 00923) -- SAME background-tile bug as #2; same author (KATIUSZA 2022).
   zepto8 drops the decorative tiled background (flat orange vs patterned).
=> #2 + #3 are almost certainly ONE root cause (one fix helps both + likely more KATIUSZA carts).

## Everything else = false positive (correctly NOT reported)
361 of the 364 nonzero-diff candidates are animation/scroll/particle/timing PHASE differences
(same content + colours, shifted position/phase). Validated signature: REAL = wrong colours OR a
missing region while everything else is pixel-identical; FP = same content at a different position.

## Trust / coverage (honest)
- 928 MATCH = clean (validated H3). AUDIO-DIVERGE = input-driven, not bugs (H2). NO-CHECKPOINTS
  z8-empty (84) = flip-hook coverage gap, not bugs (H1). WRAP-OVERSIZE (674) = UNCOVERED.
- BLIND SPOT: gameplay-gated render bugs (Virtua Racing track) render fine at the title and are
  NOT caught here -- confirmed: vracing_title's attract track renders identically, only demo-car
  positions differ (animation). Needs per-cart deterministic input (next work item).
- High precision (every reported bug eyeball-confirmed), incomplete recall (oversize + gameplay-gated).

## CORRECTION (2026-06-19) -- the montage was unreliable; pixel-window classifier supersedes it
The 46px montage only caught GROSS bugs and falsely cleared the tail (user caught this).
Rebuilt with a resolution-independent **pixel-window classifier** (dump frames 30-90 both
engines; for 5 anchors take min full-pixel Hamming to the other engine's whole window;
<2% = anchor byte-reproduced = proven phase-clean; >=2% = eyeball). Validated: REM 94% /
Mina 59% / Bomba 44% (real) vs Ape 0.8% / Thine 0.04% (clean) -- clean separation.

ARCHITECTURE cross-check (x86 z8headless vs ARM z8headless, the shipped target): built
z8headless in the shipped arm32v7/bullseye toolchain, ran under QEMU. Frame-level x86!=ARM
for FLOAT-heavy carts (3D/trig: gfx.cpp line()/oval() use double/float -> FMA rounding),
IDENTICAL for integer carts. BUT the classifier VERDICTS are arch-invariant (phase-robust):
REM 94.5/94.4, Mina 58.9/58.9, Bomba 44/44, Ape 0.8/0.8 -- x86 results ARE shipped-
representative. The 3 known bugs confirmed on the ARM render.

Full 1099 run through the classifier: 88 carts >=5%, 140 >=2%, 891 <1% proven-clean.
Full-res eyeball of all 140 (6 native-128px sheets) + per-frame nonblack analysis:

CONFIRMED REAL (high confidence -- same-state wrong-pixels OR persistent near-black):
  REM (00344) wrong palette/colours | Mina (01174) missing border tiles |
  Bomba (00923) missing bg tiles | Skelethrone (00375) wrong bg colour |
  Lina (01149) z8 renders black (168px vs 16384) | Burger Age (01514) z8 black (128px) |
  Medusa-Aegis (01662) z8 black (235px)
PROBABLE REAL (z8 persistently under-renders; root-cause TBD real-vs-missing-API):
  Froggo (01586) missing bg | Coiled (01531) missing character |
  Aurora Railway (00489) sparse | On A Roll (02139) board not drawn | Aconcagua / Pumpkin Slasher
FALSE POSITIVE classes IDENTIFIED:
  - official `-x` renders BLACK for some carts (Ghost Ship 00605: official 54px const) ->
    z8 has content, official black => NOT a z8 bug.
  - NO-INPUT state drift: z8 on title, official auto-advanced to gameplay (Rest In Pyrite,
    Flip Out, TMNT Prevenge text-reveal) => different state, not a render bug.
  - ~120 lower-residual BOTH-FULL = animation/phase (matching content shifted) -> FP.

So: ~7 confirmed + ~5 probable real render divergences (NOT 3). The montage missed them.
HONEST residual: no-input cannot separate "z8 fails to render" from "engines at different
auto-advance state" for every cart -> the probable set needs input-controlled / matched-state
follow-up. Same root limitation as the gameplay-gated (VR) gap.

## OVERSIZE (674 carts) coverage -- PROTOTYPE PROVEN (2026-06-19)
The 674 WRAP-OVERSIZE carts can't take an injected harness (cart already near the 8192-token
limit; e.g. 13 Jumps = 8007 tokens). shrinko8 --minify barely helps (PICO-8 counts TOKENS,
not chars; minify freed only 114 tokens on 13 Jumps). SOLUTION (proven): capture official's
framebuffer token-free via the GUI.
  official side: `pico8 -run "<UNMODIFIED cart>" -width 512 -height 512 -windowed 1`
    -> OS window client-area grab (512x512 = clean 4x of 128px) -> /4 downsample
    -> RGB->palette-index. Demonstrated on 13 Jumps (oversize): 16383/16384 px map EXACTLY
       to PICO-8 palette indices (99.99%) -> classifier-ready.
  z8 side: z8headless --dump on the RAW cart (no token limit, no harness, token-free).
  compare: feed both into the pixel-window classifier (grab a few frames at different wall-
    clock points for phase-robustness).
Caveats: GUI (window pops, needs the dev display, local-only), sequential ~7s/cart (~80 min
for 674). NOT for CI. But it CLOSES the 28% -- oversize is testable, not "untested forever".
Same window-grab also fixes the NO-CHECKPOINTS hook-gap carts (z8 native --dump + official grab).

## Next
- Root-cause + fix the 3 confirmed bugs in zepto8 (palette path for #1; background-tile/map path
  for #2/#3 -- check how many other carts share it).
- VR-class gap: per-cart deterministic input to reach gameplay for the racing/action carts.
