# F1 — PICO-8 mouse (`stat(32,33,34)`) — SCOPE (2026-08-15)

Status: **scoped, not built.** No code written. This document exists because the backlog
row said *"open, unscoped"*, and scoping it changed the size of the job.

---

## 🛑 The premise in the feature matrix is WRONG, and the job is smaller than it says

`CLAUDE.md` Section 6e records:

> | Mouse | ❌ no (zepto8 hasn't wired PICO-8 `stat(32,33,34)`) |

**zepto8 has wired it.** Verified against the current source, not assumed:

| what | where | state |
|---|---|---|
| `stat(32/33/34)` — x, y, buttons | `src/pico8/vm.cpp:1919-1921` | ✅ implemented |
| `stat(36/37/38/39)` — wheel, activity, relative x/y | `:1923-1926` | ✅ implemented |
| devkit gate (`poke(0x5f2d,1)`) | `:1892-1893` via `m_ram.draw_state.mouse_flags` | ✅ implemented |
| `vm::mouse(coords, relative, buttons, scroll)` — the setter, incl. wheel deltas, the activity heuristic and mouse-as-button-4/5 mapping | `:728-748` | ✅ implemented |
| **anything that CALLS `vm::mouse()`** | — | ❌ **nothing, anywhere** |

So the API is complete and the **input source is missing**. F1 is not "implement the mouse
API"; it is **"deliver mouse packets to an API that is already waiting for them"**.

Grep that settles it — the setter has zero callers in the tree:

```
grep -rn "mouse_input\|update_mouse\|set_mouse\|->mouse(" src/     # -> no hits
```

---

## What has to be built

### 1. FPGA — `hps_io` already carries the mouse; the core does not wire it

`fpga/sys/hps_io.sv:106-107` exposes:

```
output reg [24:0] ps2_mouse     = 0,   // [24] strobe, [23:16] dy, [15:8] dx, [7:0] PS/2 status
output reg [15:0] ps2_mouse_ext = 0,   // [15:8] extra buttons, [7:0] wheel
```

`fpga/PICO8.sv:347` instantiates `hps_io` and connects `ps2_key` only — **neither mouse port
is connected**, so MiSTer Main is already delivering packets that fall on the floor.

Work: connect both; on each toggle of `ps2_mouse[24]`, sign-extend `dx`/`dy` using the PS/2
status byte's sign bits, accumulate into a position clamped to **0..127** in each axis
(PICO-8's screen is 128x128 and `stat(32/33)` is in screen pixels), latch buttons from
status `[2:0]`, accumulate the wheel from `ps2_mouse_ext[7:0]`, and publish.

🛑 **Accumulate in the FPGA, not the ARM.** PS/2 packets arrive asynchronously and faster
than the ARM's frame loop; integrating them ARM-side would drop motion between frames and
make the pointer rate depend on frame rate — exactly the class of bug the
record-and-force-the-timestep rule exists for elsewhere.

### 2. DDR3 — one new qword

Free slots in the current map (`src/native_video_writer.h`): `0x14`, `0x1C`, `0x24`, `0x2C`,
`0x34`, `0x3C`, `0x44`, `0x4C`, and `0x50-0xFF` before `BUF0` at `0x100`.

**Proposal: `NV_MOUSE_OFFSET 0x00000050`**, a fresh qword rather than a spare upper half.
The payload is x(8) + y(8) + buttons(3) + wheel(8) + a change counter — it does not fit
comfortably beside an existing field, and `0x50` is clear of every current consumer.

🛑 **Do NOT reuse `0x4C`** (the upper half of the savestate qword at `0x48`) just to save a
transaction. The replay-slot picker could ride a spare half because its payload was 4 bits;
this one is not that.

### 3. ARM — one call per frame

In `mister_main.cpp`'s frame loop, read the word and call the setter that already exists:

```c
g_vm->mouse(lol::ivec2(x, y), lol::ivec2(dx, dy), buttons, wheel);
```

`vm::mouse` does the rest — including `m_state.mouse.ac`, the wheel deltas, and the
optional mouse-buttons-as-buttons-4/5 mapping when `mouse_flags.buttons` is set.

🛑 **Read the FPGA's accumulated position for `coords`, and a per-frame delta for
`relative`.** They are different values and `stat(38/39)` returns the relative pair.

### 4. Nothing needed in CONF_STR

MiSTer Main routes a USB mouse to `ps2_mouse` with no core-side option. Carts opt in
themselves with `poke(0x5f2d,1)`, which the devkit gate already honours — so a cart that
does not ask still sees `stat(32/33/34) = 0`, unchanged from today.

---

## 🛑 Ship constraint — this is a new DDR3 word, so RBF and ARM binary ship TOGETHER

Same trap as the OSD replay-slot picker and the Attack3/Attack4 CONF_STR change. With the
**old** RBF, `0x50` is untouched dead space holding whatever the previous core left in DDR3,
so a new ARM binary polling it reads **garbage** — here, a mouse that jitters or holds a
button down. Build the RBF first, then push RBF + `version.txt` + the four `.summary` files
+ the ARM binary in one go.

Budget for the SEED lottery: `pll_hdmi` is the tightest path on this device and any new
clk_sys-domain logic pressures it. **SEED is the only lever** — never add `pll_hdmi` to the
async clock-groups SDC.

---

## Verification

| # | Check | How |
|---|---|---|
| 1 | Packets reach DDR3 | move a USB mouse, `devmem 0x3A000050 32` changes; buttons set the expected bits |
| 2 | Position is clamped and absolute | sweep to each edge; the word saturates at 0 and 127 in both axes, no wrap |
| 3 | A cart sees it | a 5-line test cart: `poke(0x5f2d,1)` then `print(stat(32)..","..stat(33)..","..stat(34))` |
| 4 | A cart that does **not** opt in is unchanged | any existing cart — `stat(32/33/34)` must still read 0 |
| 5 | No regression with no mouse attached | the word stays 0; every current cart behaves exactly as today |
| 6 | Golden traces still pass | the full-corpus frame/audio hash net must be unchanged — mouse state must not enter any hashed output |

🛑 **Check 6 is the one that can bite.** If mouse state ever reaches a recorded or hashed
path, every golden trace shifts. It must not enter the `.inp` either — an `.inp` is
self-contained and a replay must not depend on a mouse being present on the receiving
machine.

---

## Cost and priority

Small: one RTL block, one DDR3 word, one ARM call — the engine side is free. The real cost
is a Quartus compile plus its SEED lottery, and a hardware pass.

**Priority: low.** Nothing in the shipped cart library is known to require it; it is a
capability gap, not a defect. Worth doing when an RBF is being compiled for another reason,
so the compile cost is shared.

**Also update when it lands:** `CLAUDE.md` Section 6e's Mouse row (currently wrong in its
stated *reason*, as above), the per-core feature matrix, and `docs/PICO-8/README.md`.
