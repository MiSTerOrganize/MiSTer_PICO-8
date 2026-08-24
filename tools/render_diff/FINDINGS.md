# Full-library render-diff -- findings (2026-06-19)

zepto8 (what we ship) vs PICO-8 (ground truth, local-only), 3302-cart library.

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

## INPUT-CONTROLLED (--play) finalization (2026-06-19) -- definitive
Re-ran all 1099 candidates with identical scripted gameplay input (--play; mash X to start,
then hold right + periodic jump/up), window 40-100, both engines -> pixel_window_classify.
Compared against the no-input pass (the INTERSECTION is the clean signal):
  - HIGH in BOTH passes = real bug (state-independent): Lina 100/100, On A Roll 90/93,
    Burger Age 84/84, Aurora 88/76, Mina 59/66, Coiled 53/53.
  - HIGH no-input, LOW play = STATE-DRIFT FP, resolved by input: Flip Out 59->0, Froggo
    85->28, Still a Magical Girl 33->13, Bloom Eternal 37->14.
  - LOW no-input, HIGH play = INPUT-SURFACED: real gameplay bug (Head-8n: z8 BLACK in the
    maze game, official full -> REAL, invisible to no-input!) OR input-drift FP (P3 16->60,
    RabuRabu 7->48 = gameplay state diverged from 1-frame input timing -> FP by eyeball).
  - official-black FP (Ghost Ship) stays high in both but official renders black -> excluded.

DEFINITIVE confirmed real render bugs (eyeballed; high in both / persistent / not FP class):
  REM (palette) | Mina (border tiles) | Bomba (bg tiles) | Skelethrone (bg colour) |
  Froggo (bg) | Coiled (missing character) | Lina (z8 black) | Burger Age (z8 black) |
  Medusa (z8 black) | On A Roll (board not drawn) | Aurora Railway (under-render) |
  Head-8n (z8 black in gameplay)  => ~12 confirmed (the montage's "3" was badly low).
PROBABLE (z8 won't advance to gameplay / under-renders -- eyeball or input-tune):
  Arena Shooter, Road Fever, Rest In Pyrite, Aconcagua, Pumpkin Slasher.

HONEST endpoint: no single automated metric gives a zero-eyeball verdict -- real bugs AND
persistent-animation/input-drift FPs all sustain residual. The TOOLKIT (no-input + --play
pixel-window classifier, intersection, per-frame nonblack auto-class for z8-black/official-
black, ARM cross-check) RANKS + auto-classifies the clear cases; the eyeball confirms the
BOTH-FULL middle. Multicart (VR) still needs z8headless --input/--dump + official GUI grab.

## FIX root-cause progress (2026-06-19)
Cluster hypotheses DISSOLVED on inspection (no fix-one-helps-many shortcuts):
  - cartdata stub = misleading diagnostic print (private.cpp:152); cartdata IS implemented.
  - "tline" in Mina/Bomba = ou*tline*_text substring; they use map()+sspr, not tline.
  - pal(table) bulk-set IS handled (bios.p8 pal_args_1/2 iterate the table).
=> the ~12 bugs are INDIVIDUAL subtle z8-vs-official divergences; each is its own root-cause.

REM (00344) root-cause IN PROGRESS -- ruled out cleanly (minimal-repro + dual-engine, no
guesses, all minimal-repro + dual-engine verified): RULED OUT as conformant on z8 -->
  - pal(c0,c1)  -> 12 (both)        - single poke(0x5f00+i,12) -> 12 (both)
  - spr draw-palette apply -> 12 (both)   - variadic poke4(0x5f00,..) byte readback = 1..16 (both)
  - end-of-frame palette regs 0x5f00-0x5f1f byte-IDENTICAL (both)
CONCLUSION: REM's wrong framebuffer indices (same geometry) come from **tline** -- the only
render op REM uses that's left, and the only confirmed-bug cart using real tline. REM's floor:
tline(0,ye,127,ye,x,z,(ca*rz)>>(6-scale),(-sa*rz)>>(6-scale)) -- fix32 multiply + VARIABLE
right-shift for the texel dx/dy step. basic tline (tline_map.p8 conformance) PASSES, so the
sub-bug is in tline with REM's specific params: likely the fix32 >> by (6-scale) in dx/dy
(texel-step) diverging, OR tline's draw-palette application mid-fog. NEXT (focused): replicate
REM's exact tline params (extract runtime x/z/ca/sa/rz/scale) into a conformance cart to pin
texel-step-vs-palette, then patch src/pico8 gfx.cpp tline + regression-test via render-diff.
REM is the HARDEST confirmed bug (3D mode-7 floor). Froggo (secret-palette 0x5f10, the
oblivion_eve family) is the recommended faster first LANDED fix.

## FIX #1 LANDED (2026-06-19): sin/cos peak off-by-one -> fixes Head-8n
trigtables.h sintable had 4096 entries but sin_helper indexes up to 4096 (a>>2, a max
0x4001) -> OOB read gave +-1.58 at the +-1 peaks (cos0/cos.5/sin.25/sin.75). Verified vs
official (cos(0) 0x0001.9510 -> 0x0001.0000). Added 4097th entry 0x0000. REGRESSION (fixed
z8 vs official, render-diff): Head-8n 99.9% -> 0.0% FIXED; all other trig values unchanged;
no conformance regression. High-leverage (any cart hitting exact peak trig angles + explains
REM's x86!=ARM since OOB is UB). Found via the render-diff trig conformance probe. SHIP: push
src -> GitHub Actions rebuilds ARM binary -> DB -> users.

REM NOT fixed by this (still 94.4%) -- user correctly flagged "wrong colours != trig". REM's
colour bug is SEPARATE and remains UN-ISOLATED after exhaustive conformance testing (pal/poke/
spr/tline draw-palette, variadic poke4, poke4+tline, fix32 shifts, sub-texel, trig -- ALL
conformant on z8). REM = hardest confirmed bug (3D mode-7 + per-band fog); next approach =
feature-bisect REM's draw code (progressively disable render features to localise).

## REM feature-bisect result (2026-06-19): procedural dream-THEME divergence
Spatial per-row diff: divergence is UNIFORM across the whole screen (~860-1024 of 1024
px per row, sky+walls+floor alike) -> NOT a single render call. Colour histogram f60:
z8 = pale theme (col 7=10682, 6=4540); official = warm theme (col 9=7581, 4=2273, a=1962).
So REM (a "Dream GENERATOR") produces an ENTIRELY DIFFERENT procedural dream/fog-palette
THEME on z8 vs official -- a generation-path divergence, not a core render op (pal/poke/
spr/tline/poke4/shift/sub-texel/trig all proven conformant; trig fix didn't help; basic
rnd conformance-verified). User's "wrong colours = palette" instinct CONFIRMED: it's the
fog-palette THEME selection in REM's generation. Function-noop bisect failed (noop changes
REM's flip cadence -> 0 frames). NEXT (dedicated session): trace REM's theme/mem_pals
selection (the op feeding set_fog's pals[]) with peek-comparison instrumentation; may be a
"different-but-valid procedural output" rather than corruption. REM deferred as hardest.

## REM theme-selection trace (2026-06-19): rnd-COUNT desync in generation
Wrapped rnd to count calls + logged the chosen theme (fog=fogs[rnd_range(0,#fogs)], line 731).
RESULT: by frame 1 z8 made rc=986 rnd calls, official rc=14 (~70x); after init z8 does ~3
rnd/frame, official 0/frame (flat 14). So z8's dream GENERATION consumes vastly more rnd ->
fog theme picked from a totally different rnd state -> z8 pale (col 7,6) vs official warm
(9,4,a). The rnd consumer is carve() (the maze DFS, ~4 rnd/cell via get_rnd_dirs); maze dims
mw/mh come from the world chosen by rnd(worlds) -> the 986-vs-14 cascades from an EARLIER
rnd-count divergence. rnd VALUES are conformant (rnd_seq), so the divergence is the NUMBER of
rnd calls = a generation loop/branch running more on z8 (control-flow, not values). EXACT
branch NOT pinned: needs per-phase rnd-count instrumentation inside generate_level, but REM
resists behavior-altering wrappers (noop-set_fog and --play both yield 0 frames -- REM's flip
cadence changes). UNRESOLVED whether this is a real generation control-flow bug OR a no-input
state-drift artifact (official may sit at a near-static intro at 14 rnd while z8 auto-runs the
animated dream). REM is the single hardest of the 12 -- deferred; needs a dedicated session
with a working in-generation probe (or testing REM with real hardware input).

## Next
- Root-cause + fix the 3 confirmed bugs in zepto8 (palette path for #1; background-tile/map path
  for #2/#3 -- check how many other carts share it).
- VR-class gap: per-cart deterministic input to reach gameplay for the racing/action carts.

## ENTROPY-SEEDED FALSE POSITIVES (2026-06-19) -- REM + Froggo off the list
HARDWARE test (user): REM renders FINE on MiSTer -- warm/correct -- it just generates a
RANDOM scene each startup. So REM's 94.4% headless divergence was a FALSE POSITIVE: REM
seeds its RNG from real-time entropy; z8headless and official-x supply different headless
time -> different dream -> flagged. On hardware both use the RTC -> random-but-fine. NOT a bug.
Run-twice test: all 12 are deterministic WITHIN an engine (run1==run2) -- so the FP class is
entropy-SEEDING (cross-engine time differs), not run-to-run randomness; my harness srand(1)
is OVERRIDDEN by the cart's own srand. Detector = cart-side srand(stat-time).
FOUND: Froggo srand(stat(95)) x3 (RTC seed) -> same class -> FP-suspect, off the list.
Skelethrone/Coiled use time() (animation phase) -- need a look. The other 7 (Mina, Bomba,
Lina, Burger Age, Medusa, On A Roll, Aurora) have NO cart-side srand -> harness srand(1)
controls them -> genuinely diffable -> still trustworthy real candidates.
GENERAL LESSON: the render-diff must EXCLUDE entropy-seeded carts (grep cart for srand(stat..))
-- they are non-comparable cross-engine and produce false positives even though deterministic
within an engine + fine on hardware. Add to the harness/triage as a pre-filter.
REM center-line (transparent vertical seam during pan): deferred -- almost certainly a
raycaster center-ray (vanishing-point) artifact present on PC too (= REM design); user to
confirm via PC pan. Render-diff missed it (1px ~0.8% sub-threshold + motion-only).

## ENTROPY-FP SCAN of all 140 candidates (2026-06-19)
Grepped each candidate's cart for srand(stat()/time()/t()). 4 of 140 are entropy-seeded
(+ REM = 5 FPs total, off the list): Froggo srand(stat(95)), Zarchy engine srand(time()),
Nano4x-Supremacy srand(stat/time), Mansion Bros srand(t()). Only 4/140 -> the list is NOT
dominated by entropy FPs. PRE-FILTER RULE (bake into render-diff triage): a cart calling
srand(stat..)/srand(time())/srand(t()) seeds from the RTC -> z8headless vs official-x get
different headless time -> non-comparable cross-engine -> EXCLUDE (renders fine on hardware).
Detector: grep -qE "srand\(stat\(|srand\(time\(|srand\(t\(\)" <decompressed cart>.
Remaining trustworthy real candidates (NOT entropy-seeded): Mina, Bomba, Lina, Burger Age,
Medusa, On A Roll, Aurora (+ Skelethrone/Coiled use time() for ANIMATION not seed -- check).

## FIX #2 LANDED (2026-06-19): stat(102) returns number 0 (standalone), not nil
Target: Lina - A Fishy Quest (01149) -- THE #1 candidate (100% residual no-input + --play +
pixel-window; z8 drew 40 px, official 16384/16384 every frame). Root cause: Lina's entire
_draw is gated on check_url(), which returns true only if stat(102)==0 (number) or a host
string. We returned nil (a 2026-05-21 oblivion_eve fix), so nil==0 -> false -> _draw skipped
-> black screen (the 40 px = the `else: print(stat(102),0,64)` branch).
GROUND TRUTH (measured, PICO-8 0.2.7a6 -x standalone): stat(102) = NUMBER 0.
The old nil matched the BBS WEB player (host string), not standalone -- wrong reference.
Fix: src/pico8/vm.cpp api_stat id 102 -> (int16_t)0. Commit 1bc5047.
VERIFIED on locally-built z8headless: Lina 40 px -> 16384/16384 (matches official). oblivion_eve
NOT regressed (still renders 2241-8498 px animated title; "Quit" now shows = correct standalone
behavior, works via extcmd("shutdown") handler). Burger/Medusa/On-A-Roll do NOT use stat(102)
(checked) -- separate root causes, still open.
GENERAL: BBS-fingerprinting carts (stat(102)/stat(120)/stat(121)) must be answered as STANDALONE
on MiSTer -- measure the official standalone value, don't infer from the web-player context.

## FIX #3 LANDED (2026-06-19): cursor() negative coords wiped the screen via print-scroll
Target: Burger Age (01514) -- z8 BLACK (0 px), official ~13700. Root cause is a GENERAL
zepto8 engine bug (not cart-specific), likely affecting multiple candidates.
Chain: cart's textout() -> cursor(x, wavepos()) where wavepos=1.5*sin(...) is sometimes
NEGATIVE -> api_cursor took uint8_t so neg y wrapped to ~255 -> print() auto-scroll computed
final_y = 255 + height - 128 = +133 -> scrool_screen(133) memmove/memset PAST the framebuffer
-> entire screen cleared to black, every frame.
GROUND TRUTH (PICO-8 0.2.7a6 -x, measured sweep): cursor y = -8/-1/0/60 -> NO scroll
(fb 16384); 120->15616, 124->15365, 128->14868, 130->14612, 200->5652, 255->20. So PC CLAMPS
negative cursor coords to 0 on set (cursor(_,-1) == cursor(_,0)); positive scrolls proportionally.
Fix: api_cursor args uint8_t -> int16_t (preserve sign), clamp negatives to 0 before storing the
byte. src/pico8/{gfx.cpp,vm.h}. Commit 7b3d3a5.
VERIFIED on z8headless vs official: neg-y print no longer erases; sweep -8..200 matches official
exactly (255 is 0 vs official 20 -- pre-existing extreme-edge scroll-clamp, no real-cart impact);
Burger Age 0 -> ~13700 (matches official); Lina (FIX #2) still 16384, no regression.
GENERAL IMPACT: any cart drawing text at a sometimes-negative y (wavy/bobbing text via sin) was
having its frame wiped on z8. RE-RUN the render-diff after this ships -- expect several other
black/under-render candidates (and high-residual carts with animated HUD text) to clear.

## RE-RUN vs the July-fixed engine (2026-07-19) -- dumpwin 30-90 + wide-window 5-200 phase test
Engine = current main (conformance 18/18: line/sspr/format/oval fixes + June trig/stat102/cursor).
A/B against a June-state build (0cfeed8): z8 dumps BYTE-IDENTICAL across builds for every
candidate -> the July primitive fixes are boot-window-neutral for these carts (no sweep, no
regression). NOTE: the June "re-triage cleared Mina/Bomba" note does NOT reproduce with this
pipeline -- both still diverge at their original residuals; treat them as OPEN.

| cart | residual (5-anchor) | wide-window (5-200) min | verdict |
|---|---|---|---|
| Lina (01149) | 1.5% | full 16384/16384 render both engines | FIXED (June stat102) ✓ residue = animation drift |
| Burger Age (01514) | 0.0% | -- | FIXED (June cursor) ✓ |
| Aurora Railway (00489) | 0.0% | -- | FIXED ✓ |
| Skelethrone (00375b) | 83.7% | collapses to 4.7-12.8% (ref f40 ~= z8 f95) | PHASE-FP at boot (title identical incl. bg colour; residue = sparkle anim). No bug visible headless. |
| Froggo plain (01586c) | 8.0% | full scene renders both | REVIEW-low (traffic-position drift) |
| Froggo Hop (01586b) | -- | srand(stat(95)) | ENTROPY-FP (June class) -- excluded |
| REM (00344) | 94.4% | -- | ENTROPY-FP (June hardware-confirmed fine) -- excluded |
| Mina (01174) | 58.9% | IRREDUCIBLE (~58.9% vs every frame) | REAL -- border/bg tiles missing (visual: flat green vs flower tiling; sprites+text identical) |
| Bomba (00923) | 44.0% | IRREDUCIBLE | REAL -- bg tiles missing (flat orange vs brick pattern) |
| Coiled (01531) | 53.6% | IRREDUCIBLE (z8 static from ~f7) | REAL -- big character art missing (z8 shows level layout instead) |
| Medusa (01662) | 28.8% | z8 avg 147 non-black vs ref 4867 | REAL -- all scenery missing, only text renders |
| On A Roll (02139) | 90.6% | z8 724 vs ref 15604 | REAL -- board/map missing, sprites+text render |
| Skelethrone 2022 (00375a) | NO-DUMP both engines | -- | harness doesn't engage this cart shape; inconclusive |
| Froggo1k (01586a) | NO-DUMP on z8 (ref dumps fine) | -- | z8 never reaches the flip hook -- investigate separately |

SHARED SIGNATURE (hypothesis, not yet verified): all 5 REAL carts render sprites+text correctly
but lose a LARGE background/tile/art layer (Mina/Bomba borders, Medusa scenery, On A Roll board,
Coiled character art). Suggests ONE shared zepto8 path (map()/large-blit under some parameter
class the conformance matrix doesn't cover). Next: bisect one _draw (On A Roll or Mina) per the
proven June method to identify the failing op, then extend the conformance matrix with it.
Work dir: scratchpad rd19 (win_wrap/win_off/win_z8/win_z8june/wide_z8/png).

## FIX #5 (2026-07-19): map() celw/celh full-map defaults -- ALL FOUR missing-layer carts + 8 silent victims
The shared missing-layer signature WAS one bug. Measured (m_map_size conformance cart, 21 checks):
omitted or NIL celw/celh each independently default to the FULL map incl. shared rows 32..63
(old "defaults to 128,32" doc is wrong); explicit 0/negative draws nothing; nil stays present-0
for other opt args (rnd/btn/peek/pal) so only map() got a nil-aware binding (nilopt<>).
Cleared to pixel-exact: On A Roll 90.6%->0, Mina 58.9%->0, Bomba 44%->0, Medusa 28.8%->0.
Corpus: exactly 12 goldens changed = those 4 + 8 silent victims (Halloween Cavern, Rest In
Pyrite, Aconcagua Hack, Time For Lunch, visje, Slimeblast, Timmy's Return, snowfight.io);
7 of 8 verified pixel-exact vs reference (Halloween Cavern = WRAP-OVERSIZE, verified statically:
bare map(0,0)); other 3,290 goldens byte-identical. Commit 7bed5c7.

## FIX #6 (2026-07-19): rnd(table) element selection + ord(nil) -- Coiled pixel-exact
Coiled's title character art (str2image RLE) is gated on title_cnt=rnd({0,1})%2==0. z8's BIOS
used t[flr(rnd(#t))+1]; the reference picks 0-based index (prng_a >> 8) % #t after ONE update
(pinned via bit-slice scan on n=16 + verified on 38 draws x 2 seeds; m_rnd_ord conformance cart).
Same stream, different slice -> wrong element under the same seed. New __rndi private binding.
Also measured: ord(nil)/ord()/ord("")/oob return NO values (z8's "nil -> N zeros" guess removed).
Coiled 53.6% -> 0.0%. Commit 3069c8f. OPEN (task #19): tostr decimal ROUNDING-vs-truncation
divergence found en route (ref prints 0.5933 for 0x.97e2; July truncation finding was measured
on non-discriminating cases) -- display-only, needs its own probe round.

RENDER-DIFF LIST STATUS after fixes #5+#6: every confirmed-real cart from the June campaign is
now pixel-exact (Lina, Burger, Aurora, Mina, Bomba, Medusa, On A Roll, Coiled) or reclassified
(REM + Froggo entropy-FP, Skelethrone phase-FP). Remaining REVIEW-low: Froggo-plain 8% traffic
drift, Lina 1.5% animation residue.

## HARNESS FIX (2026-07-19): --test path LENGTH selects a behavior mode on layout-sensitive carts
Snak + CluePix Halloween went NONDET after the rnd/ord rebuild -- but each invocation style was
internally deterministic: trace paths <=15 chars (std::string SSO, no heap alloc) gave mode M,
longer paths mode W, reproduced on pure tmpfs (9p mount latency was a red herring; the trace
FILE is fully buffered). Chain: path length -> one extra heap allocation -> every later address
shifts -> pointer-keyed pairs() order changes (ASLR-off makes layout deterministic, NOT
path-invariant) -> these carts' object-key iteration diverges (audio-only from f92, video static).
trace_worker.sh compared run A (long golden path) vs run B (short mktemp) = two modes = permanent
false NONDET. Fix: both runs write to SAME-LENGTH mktemp paths, mv into the golden slot after.
Verified 2 rounds DET; full corpus 3,302/3,302 DET. Inherent cart sensitivity (the reference's
object-key iteration is address-dependent too) -- a harness-discipline rule, not an engine bug.
Component-4 hardware runs must use consistent -test path lengths for the same reason.

## FIX #7 (2026-07-19): decimal formatting = round-half-up with zero-digit suppression
Task #19 measurement round (34 probe values, m_tostr conformance cart): the reference's
lua_number2str-equivalent is d4 = |frac|*10000 >> 16, ROUND HALF-UP on the low 16 bits, but
ONLY when d4 > 0 -- a round that would manufacture a digit out of ".0000" is suppressed
regardless of the integer part (0x0.0006 -> "0", 0x0005.0006 -> "5", yet 0x0.000a -> "0.0002",
0x0.97e2 -> "0.5933"); carries into the integer (0x0.ffff -> "1"); sign kept ("-0"); tostr and
concat share one path (measured equal). SUPERSEDES the July pure-truncation reading -- every
July golden was fit to values where the two rules agree, and all stay valid. Suite 22/22;
corpus 3,302/3,302 DET; 12 goldens changed (score/HUD digits). Commit c1442db.
Process note: a PowerShell Set-Content golden append left a lone trailing CR that made two
visually identical lines diff -- strip CRs when appending goldens from Windows captures.

## FIX #8 (2026-07-19): DO accepted in place of THEN for if/elseif -- 22 silently-broken carts + user report
Community report (Tempest 2000 alpha, shrinko8-minified): "syntax error at startup" on the core,
fine on PC. Root cause: PICO-8 accepts `if cond do ... end` / `elseif cond do` (measured, m_ifdo
conformance cart: block form, paren form `if (c) do..end`, per-clause then/do mixing, nesting all
accepted; `while cond then` is STILL a syntax error -- the shorthand is if/elseif-only) and
shrinko8's minifier emits it heavily; z8lua only knew THEN -> "'then' expected near 'do'" ->
black screen. Fix: lparser.c accepts TK_DO where TK_THEN is expected (never triggers short-if).
Suite 23/23. Corpus blast radius: **22 library carts changed goldens = all previously
black-screening on this same syntax** (Infinitroid, Domain Conflict, Zombie Horde, RogueRis x2,
Berzerk Realm, Town Simulator, ...) -- spot-verified Infinitroid now boots to title. 3,302/3,302
DET. NOTE: these 22 were invisible to the render-diff campaign because a syntax-error screen is
deterministic + the June candidate list came from divergence-vs-reference sampling, not load
success -- a "cart loads at all" differential (error-text detector on frame dumps) would have
caught them; the golden-trace blast-radius diff is what finally surfaced them.

## VIRTUA RACING RE-RUN (2026-07-19, task #9): track corruption NOT reproducible on current engine
First-ever VR GAMEPLAY differential (June's "VR-class gap" -- deterministic input to reach racing).
Method: wrap the 15-cart multicart in a throwaway copy -- title gets an INPUT-ONLY harness
(srand/frame-t/btn-mask, no dump), vracing_main gets dumpwin+play; numbered carts are pure
reload() data, untouched. z8headless runs it directly (cwd = cart dir). REFERENCE GOTCHAS
discovered: (1) **`-x` mode silently NO-OPs `load()`** (minimal 2-cart probe: cart B never
boots, execution falls through) -- headless reference CANNOT run multicarts at all; (2) the
workaround is **windowed `-run` mode** (load() works; printh still captured via redirected
stdout; wrapper input-override neutralizes the real keyboard; carts must sit in `{home}/carts`).
RESULT: splash window (main f30-120): 70/91 frames PIXEL-EXACT, residue = clock offset once
animation starts. Race window (f200-320): both engines render CLEAN, structurally-correct
tracks (road/striping/scenery/HUD verified by eyeball at f250 + f312) -- NO corruption on z8.
Residual divergence = accumulating car-STATE drift, best-offset -4 -- attributable to the
reference's real-time windowed loop dropping/doubling updates under 3D load vs z8's stepped
execution (the splash matched exactly until motion began), NOT provably an engine divergence;
this harness cannot separate the two for real-time-only reference runs. VERDICT: the June
corruption class (fix32/line 3D geometry) appears swept by the July fixes (line raster,
trig, oval, sspr); awaiting the user's hardware eyeball to close. If hardware still shows
corruption, next step: savestate-near-corruption + frame-step compare per the methodology doc.

## 2026-07-21: reset() implemented + TWO latent memory-safety bugs + VR bridge differential
User report: Run Run Rudolph froze at title with tutorial ON. Cause: init_tutorial()
calls the built-in reset() -- zepto8 had NO reset() at all (not implemented, bound,
or stubbed) -> "attempt to call a nil value" -> cart coroutine dead -> frozen title.
Implemented from a reference marker-poke probe (m_reset conformance cart, suite 23/23):
resets 0x5f00..0x5f7f draw state EXCEPT 0x5f24 + cursor 0x5f26-27 (measured untouched);
PRNG reseeded from entropy (Z8_TEST_SEED-deterministic in trace mode). Corpus scan of
built-in-reset() callers: 18 library carts were silently dying wherever that path fired.

The post-fix corpus scan then caught "Oust (Demo)" SIGBUS (heap-layout shift from the
new sandbox key) -> ASan pinned TWO PRE-EXISTING bugs: (1) cart::load_png read the
whole 64K+5 set_bin buffer from a 32,800-pixel image = ~131KB OOB heap READ on every
.p8.png load (garbage only ever landed in rom >= 0x8020, unobservable; now clamped +
zero-filled); (2) vm::render honored multiscreen (_map_display) by writing 128*msx x
128*msy pixels into callers' 128x128 buffers = heap WRITE overflow on every multiscreen
cart, on hardware too (render() now bounds-aware; single-screen buffer -> screen 0 only).
Corpus after: 3,302/3,302 DET, 0 crashes. Goldens re-archived (23 changed: boot-reachable
reset() callers + Snak/CluePix _ENV-layout pair).

## 2026-07-21: VR yellow-bridge differential -- headless engine renders the section CLEAN
User hardware retest: track corruption persists "before the yellow bridge" (big forest,
first track). The July race differential only covered f200-320 (~2-5s); the bridge is
23-42s in -- never reached. New harness: input-script through title menu (O, O; NOTE
buttons must be RELEASED after starting the race -- the load() button-mask eats a
held button forever, headless-only artifact), then the player's control() swapped for
the cart's own NPC AI driver in a throwaway copy -> full clean AI lap. RESULT: the
whole lap incl. the canyon approach + yellow truss bridge renders COHERENT geometry
on x86 headless at the exact race-clock times of the user's corruption photos.
=> The corruption is MiSTer-side only. Note: VR maps SECRET palette colors at _init
(pal(14,128,1) / pal(15,131,1)) -- the photos' purple/white patchwork may be correct
geometry in scrambled colors, not shattered vertices. NEXT: live DDR3 framebuffer
capture on the MiSTer while the corruption is on screen (devmem dump of the ARM-written
RGB565 buffer) to split engine-render-side vs present/FPGA-side.

---

## 2026-08-22 -- 5 carts throw a runtime error in our engine and NOT in the reference

Found while auditing the PICO-8 collection, not while looking for engine bugs.
All 114 multi-cart folder entries were booted under z8headless for 300 frames:
114 ran, 0 crashed, 0 hung. Seven emitted a caught Lua error while still
completing, and each named something that should never be nil.

Running the same seven through the local reference binary (0.2.7a6, `-x`, 25s,
from each cart's own folder so siblings resolve identically) splits them:

| cart | error | reference |
|---|---|---|
| Hide and Seek | `attempt to index local 'pt'` | **same error** |
| Space 8 | line 309 `compare number with nil` | **same error, same line** |
| Celeste Theo's Return | line 138 `sin` is nil | clean |
| Samurise | line 89 `#c` on a number | clean |
| Into Ruins (v1.06) | line 695 `flr` is nil | clean |
| Libryinth | line 101 concatenate a table | clean |
| PICO-BALL | line 1407 `circfill` is nil | clean |

Two are genuine cart behaviour. **Five run clean in the reference and error in
ours** -- an engine divergence, and three of them fail because a CORE API name
(`sin`, `flr`, `circfill`) is nil, which cannot be true of a working sandbox. A
three-line probe calling all three ran clean, so the API exists; those carts are
reaching a context where it is not bound.

**The mechanism is not proven.** `load()` is implicated and the evidence is
partial. A two-cart probe -- A loads B from inside `_update` at frame 20 --
shows z8headless re-executing **A's top-level chunk a second time** and never
entering B:

```
A: start, flr=1      <- twice
A: loading b         <- twice
(B never entered)
```

The reference prints each once and also never enters B, because `-x` stops
there. So `-x` cannot arbitrate this path, and the doubled re-entry in ours is
an anomaly worth explaining but is not yet established as the cause.

🛑 Do NOT record this as "load() is broken". What is measured: five carts error
in our engine and not in the reference; our engine re-executes a cart's
top-level chunk on that path and the reference does not. Closing it needs a
headless mode that survives the load -- the wrapper approach this directory
already uses -- or a run on the device.

Also worth noting for whoever picks this up: an earlier attempt to explain the
seven by "they all call `load()`" was true (7/7) and worthless -- **99 of the
104 entries call `load()`**. The base rate was computed before the claim was
made, which is the only reason it did not ship as an explanation.

## 2026-08-23 -- the 5-cart divergence RESOLVED: `_ENV`-swap loses the API. Two red herrings killed.

Closing out the 2026-08-22 entry above. Its two open questions are answered: the
mechanism is **not** `load()`, and the "top-level chunk runs twice" anomaly was
never real.

### The mechanism (measured, not inferred)

PICO-8 accepts a function parameter literally named `_ENV`, which rebinds the
environment for that function's body -- a widely-used cart idiom for
struct-field shorthand. All three "core API is nil" carts use it:

```lua
function draw_snow(_ENV)          -- Celeste Theo's Return, entry cart line 135
 ...
 x += _g.level_id==4 and spd*3 or sin(off)*spd-_g.cam_spdx   -- line 138: sin
end
foreach(fx_snow_alt, draw_snow)   -- called from _update, frame 0
```

The cart author knows globals are lost -- hence `local _g=_ENV` at line 4 and
`_g.level_id` -- but writes bare `sin`, `circ`, `spr`. Those work on the
reference and are nil for us.

Probe `probe/envsem.p8` (`f(_ENV)` called with a plain table, no metatable):

| inside the swapped `_ENV` | reference | ours (before) | agree? |
|---|---|---|---|
| `sin` (API name) | **function** | **nil** | **NO** |
| `myglobal` (cart's own global) | nil | nil | yes |
| `mytab` (cart's own global table) | nil | nil | yes |
| `x` (field of the passed table) | 42 | 42 | yes |
| write `wtest = 123` | goes to the passed table | same | yes |

So the **only** divergence is API names -- user globals correctly do *not* fall
back, which rules out "the reference chains `_ENV` to the globals table".

`probe/envovr.p8` pins what the binding actually is:

```
1 pre_override sin0=0        2 post_override sin0=OVR     <- `sin = f` takes effect
3 env_sin_type=function      4 env_sin_call=0             <- but _ENV.sin is UNCHANGED
5 inswap sin0=OVR                                         <- the swap sees the override
7 inswap_add=function foreach=nil printh=nil
8 inswap_pairs=nil tostr=function setmetatable=nil        <- only a SUBSET survives
```

Assignment and read both bypass `_ENV` and survive an `_ENV` swap: these names
behave as **chunk-scope locals (upvalues)**, not table fields. Enumerating all
127 names in `api::functions` (`probe/envenum.p8`) gives exactly **58** that
survive -- hot inner-loop functions (math, bitops, peek/poke, drawing
primitives, `add`/`del`/`deli`, `tostr`/`tonum`/`sub`) -- while `foreach`,
`printh`, `pairs`, `cls`, `camera`, `btn`, `stat`, `rnd`, `sqrt`, `abs`,
`cocreate`, `flip` and the rest stay ordinary globals. Full list:
`probe/visible.txt`.

### The fix, and why it is exactly right

Prepend those 58 names as chunk locals to the cart chunk. Two lines in
`src/pico8/bios.p8`:

```lua
__z8_api_prelude = "local max,min,mid,...,deli=max,min,mid,...,deli;"
...
local code, ex = __z8_load_code(__z8_api_prelude..cart_code..glue_code, nil, nil, sandbox)
```

No trailing newline (line numbers are preserved -- verified: Libryinth's error
stays at line 101), and a trailing `;` so a cart starting with `(` cannot be
parsed as a call continuation.

Verification:

* `envsem.p8` and `envovr.p8` output is now **byte-identical** to the
  reference, including the subtle rows 3/4 (override does not touch `_ENV.sin`)
  and 7/8 (exact membership).
* All **127** API names match the reference exactly under an `_ENV` swap
  (`diff` of the enumeration = empty).
* Celeste Theo's Return, Into Ruins (v1.06) and PICO-BALL: **CLEAN** -- and
  they genuinely RUN rather than merely stopping the error. Distinct video
  hashes over 200 frames (`--test`), stock vs patched:

  | cart | stock | patched |
  |---|---|---|
  | Celeste Theo's Return | **1** | 101 |
  | Into Ruins (v1.06) | **1** | 22 |
  | PICO-BALL | **1** | 4 |

  One distinct frame across 200 is a dead cart re-erroring every tick; the
  patched runs animate.

**Blast radius, measured.** The prelude costs 58 local slots, so the cart's own
top-level `local` budget drops from 200 to 142. That sounds like a regression
risk; measured, it is the opposite -- the reference's own budget is **141**
(`locals/loc141.p8` OK, `loc142.p8` -> "syntax error line 142"). The reference
burns the same ~58 slots, which independently confirms the chunk-local
mechanism, and leaves us one slot *more* permissive than the reference rather
than less. Scanning the full **8,508**-cart library for syntax errors:

```
patched syntax-error carts: 104
stock   syntax-error carts:  104   (the same 104)
NEWLY_BROKEN_COUNT=0
```

**Runtime blast radius, measured with the golden gate.** The locally built
z8headless first reproduced the goldens exactly on the 51-cart subset (51/51
MATCH), which validates it as a baseline rather than assuming it.

Full corpus with the prelude: **3,249 / 3,302 MATCH, 53 DIFF**. Per the gate's
own instruction every DIFF was classified rather than assumed --
`trace_worker.sh` over all 53:

```
=== class histogram ===
     53 DET
=== any NONDET / CRASH / HANG ===
(none -- all DET)
```

All 53 are reproducible-but-stale goldens: the documented heap-shift class
(z8lua hashes object keys by POINTER, so extra chunk text moves `pairs()` order
on object-keyed carts). Zero NONDET, zero crashes, zero hangs. This is the same
class and roughly triple the size of the 2026-08-16 re-baseline, where 171 lines
of BIOS growth moved 19 traces -- expected from *any* change of this breadth, and
it means **53 goldens need re-baselining with the reason recorded**, per the
golden README's rule.

### Two red herrings, both killed

**`load()` is not involved.** None of the three fixed carts needed it. The
2026-08-22 note that all 7 called `load()` was already flagged as worthless
against a 99/104 base rate; it is now positively excluded.

**"A's top-level chunk executed twice" never happened.** `printh` writes to
**both stdout and stderr**, so any `2>&1` capture shows every cart line twice.
Splitting the streams shows each line exactly once. There is no double
execution and no anomaly to explain.

**Harness note:** z8headless loads `bios.p8` from the *current working
directory* and only *warns* if it is missing (`bios.cpp` logs and continues).
Running from a cart's own folder therefore produces a BIOS-less VM whose every
frame reports a bare `attempt to call a nil value` -- superficially similar to a
real cart error but with no chunk name or line number. Copy `bios.p8` next to
the binary and pass the cart by absolute path, as `batch_z8.sh` does.

### Second divergence found on the way: `deli()` clamps an out-of-range index

Unrelated to `_ENV`, found while tracing the two carts that survived the first
fix. Measured (`probe/deli.p8`, `probe/deli2.p8`):

| call (`t={"a","b","c"}`) | reference | ours |
|---|---|---|
| `deli(t,4)` | nil, table **untouched** | `"c"`, and it is **deleted** |
| `deli(t,0)` / `deli(t,-1)` | nil, untouched | deletes an element |
| `deli(t,3)`, `deli(t,3.9)`, `deli(t)` | as expected | agree |
| `add(t,v,i)` for `i` in 1..#t+1 | inserts | agree |
| `add(t,v,0/5/9/-1)` | **runtime error** "position out of bounds" | silently clamps |

Cause: `i=i and mid(1,i\1,#c) or #c` in the BIOS clamps the index into range
instead of rejecting it, so an out-of-range `deli` silently deletes a live
element. Fix (`deli` only so far) returns nil and leaves the table alone;
`probe/deli.p8` then matches the reference byte-for-byte, and the 51-cart subset
gate is 51/51 MATCH with both fixes applied. `add`'s out-of-bounds case is
characterised above but **not yet changed** -- making it raise is a behaviour
change that needs its own corpus pass.

This one is **reached by real carts**, so it is not theoretical. Instrumenting
the BIOS to log out-of-range indices, full **8,508**-cart scan at 300
frames/cart: **17** carts hit out-of-range `deli`, **0** hit out-of-range `add`.
`Snake 1999 (TweetTweet Jam 9)` hits it **173,206 times** in 300 frames -- a hot
path -- then `visje` (300), `Drafting Table` (300), `Rewob` (216), `Ace
Hiragana` (184), `worm` (120), `Picodex Dual` (40), and ten more between 2 and
12. Every one of those is currently losing a live element on each such call.

Because `add` is never reached out-of-bounds by any cart in the corpus, making
it raise (to match the reference) is a zero-blast-radius change on this library
-- but it is still a change from "silently succeeds" to "aborts the cart", so it
should not ride along with the `deli` fix unexamined.

### STILL OPEN: Samurise and Libryinth are a *third*, separate cause

Both survive **both** fixes, and both are confirmed reference-clean:

* **Samurise** -- `attempt to get length of local 'c' (a number value)`. The
  failing frame is the **BIOS's own `count()`** (chunk line 89 = `local cnt,
  max = 0, #c`, verified by dumping the BIOS chunk, not by arithmetic).
  Instrumented: it is called as `count(6)`. The caller is the cart's embedded
  Lisp compiler -- DSL source line 165, `(count elem)`, where `elem` comes from
  `(inext exp i)` and should be a table. `count(<number>)` raises on the
  reference too, so the divergence is upstream: some value is a number for us
  and a table there.
* **Libryinth** -- `attempt to concatenate local 'n' (a table value)` at cart
  line 101, inside a minified `nK()`.

Ruled out by direct measurement, all byte-identical to the reference:
`tonum` (31 tokens incl. hex/binary/whitespace forms), `split` (incl. the
numeric-conversion rule and `split(s,".")`), `sub` (negative/over-range),
`unpack`, `inext`, `select` (incl. negative indices), `rawget`, string
indexing `s[i]`, `all`/`count` over strings/tables/nil, `deli`/`add`/`del`.

Next step for whoever picks this up: the cart's AST is built by
`return tonum(e) or e` in `parse()`, so a token that becomes a number for us
and a symbol there (or vice versa) would explain it -- but `tonum` itself is
clean, so the difference is more likely in `consume()`'s tokenisation inputs.
Wrap a throwaway copy of the cart (never edit the user's file) and dump the AST
at the `builtin.table` call.

**Useful vehicle:** Samurise's DSL is **Parens-8**, a Lisp-in-PICO-8 that also
ships standalone as `[Tools]/Parens-8 Repl.p8.png` -- vastly easier to work with
than a minified game. Caveat, measured rather than assumed: the REPL cart runs
**CLEAN on both stock and patched**, so it is not an automatic reproduction; it
idles without evaluating the forms Samurise uses. Feed it the failing form
(`builtin.table` / `(count elem)` path) rather than expecting it to fail on boot.
It does independently hit the out-of-range `deli` above (twice), which is how it
surfaced.

### What shipping needs

The `_ENV` prelude is a **BIOS-only** change (`bios.p8`), so it needs no binary
rebuild -- but it changes every cart's chunk, so before it ships: the
conformance suite, a full golden-trace corpus run with a blast-radius diff (a
change this broad should be expected to move goldens), and the usual hardware
pass. The `deli` fix is likewise BIOS-only and needs the same. Prototypes live
in the session scratchpad (`biosfix/`, `biosdeli/`); nothing has been committed.

Two consequences that follow from the project's own rules and are easy to miss:

1. **Bump the recorder's game-logic version stamp** (`P8REC` / `MREC_ENGINE_VER`).
   The rule is to bump only for a shipped game-LOGIC change that would desync
   existing takes -- and this is one, twice over: cart-visible name resolution
   changes, and the chunk-text growth shifts the Lua heap, which moves
   `pairs()` order on object-keyed carts (the same mechanism that moved Snak's
   golden). Takes recorded before the change should be refused, not played
   best-effort.
2. **Exercise save states.** `persist()`/`unpersist()` key the eris perms table
   on the API function OBJECTS taken from `_ENV`, and the prelude's locals hold
   those same objects, so mapping *should* still work -- but that is reasoning,
   not a measurement, and save/restore is not covered by any headless harness
   here. Verify it on hardware before shipping.
