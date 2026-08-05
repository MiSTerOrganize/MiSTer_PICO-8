# Virtual-pad core-level maps (`uinput_inject.py`, VID/PID `1209:beef`)

Deploy to `/media/fat/config/inputs/` on the MiSTer. These are **core-level** map
files for the *virtual* pad only. They are keyed by VID/PID, so they cannot
collide with a real controller's map and they never touch it.

## The rule that makes these necessary

MiSTer reads the two map scopes in **different orders**, which is not obvious and
cost several wrong predictions before it was measured:

| Scope | File | Slot -> DDR3 bit |
|---|---|---|
| menu-level | `input_<vid>_<pid>_v3.map` | translated through the core's `jn` -- measured on PICO-8 as `bit4<-slot5`, `bit5<-slot4`, `bit6<-slot6` |
| core-level | `<Core>_input_<vid>_<pid>_v3.map` | **identity**: `bit4<-slot4`, `bit5<-slot5`, ... |

So a core-level map is written directly in the core's own button order. Fitting a
menu-level map and assuming a core-level one behaves the same is wrong.

Slot encoding (128 bytes = 64x uint16 LE, slot N at index 2N):
buttons are the raw evdev code (`0x130` = BTN_SOUTH = Xbox A); directions are
`0x300 + axis*2 + dir`, so `0x0321` = axis 16 (`ABS_HAT0X`) positive = RIGHT.
The tail beyond slot 11 is deliberately **zeroed** -- carrying over a real pad's
analog/axis definitions put a stray bit 23 on the joystick word.

## Verified on hardware 2026-08-03

PICO-8 (`J1,O,X,Pause` / `jn,B,Y,Start`) -- all 7 inputs the core declares:

| Xbox | bit | control |
|---|---|---|
| d-pad up/down/left/right | `0x08/0x04/0x02/0x01` | movement |
| A | `0x10` | O |
| X | `0x20` | X |
| Start | `0x40` | Pause |

OpenBOR_7533 (`jn,A,B,X,Y,L,R,Start`) -- all 11 inputs, every one exact:

| Xbox | bit | control |
|---|---|---|
| d-pad up/down/left/right | `0x08/0x04/0x02/0x01` | movement |
| B | `0x10` | Attack |
| A | `0x20` | Jump |
| Y | `0x40` | Special |
| X | `0x80` | Attack2 |
| LB | `0x100` | Attack3 |
| RB | `0x200` | Attack4 |
| Start | `0x400` | Start |

The identity rule predicted all eleven on the first attempt, so the model is
confirmed rather than merely fitted to PICO-8.

## Two traps when sweeping inputs

1. **Put `START` LAST.** It is the Pause bit on both cores, so anything pressed
   after it navigates the pause menu instead of being measured -- that is how a
   sweep silently Reset the running binary mid-run.
2. **A core restart invalidates the run.** The injector paces off the DDR3 frame
   counter; a restart resets it, the delta goes negative, and the run grinds to
   its `--max-seconds` ceiling reporting nonsense (`-772 fps`) instead of failing
   fast.
