#!/usr/bin/env python3
"""uinput_inject.py -- frame-aligned virtual gamepad for MiSTer hybrid cores.

WHAT THIS IS FOR
----------------
Hands-free in-core input on the real hardware, through the real input path.

Our hybrid cores do NOT read SDL joystick -- MiSTer Main holds the input devices
with EVIOCGRAB. They read a joystick word out of DDR3 that the FPGA fills from
Main. So we sit DOWNSTREAM of Main's input handling, which means input injected
UPSTREAM of Main reaches every core:

    /dev/uinput -> MiSTer Main -> build_joy_mask() -> FPGA hps_io -> DDR3 -> core

Verified on hardware 2026-08-02: a uinput pad is enumerated as js0, hot-plugged
with no restart, and grabbed by MiSTer Main (PID seen holding its event node).

This matters because keyboard injection does NOT reach in-core input on either
OpenBOR build (keyboard is not wired through the SDL dummy driver there), so
OSD-level automation can drive menus but not gameplay. A virtual PAD can do both.

WHY IT IS FRAME-ALIGNED
-----------------------
Injecting on wall-clock time drops presses at arbitrary points within a frame, so
a run is not reproducible -- which defeats the purpose. MiSTer Main has a proper
vsync callback (frame_timer.cpp add_frame_callback), but it is process-internal:
it needs SPI and direct memory access, so an external tool cannot register.

We do not need it. Our cores already publish a frame counter in their DDR3
control word -- the same signal the live-fps measurement reads with devmem. This
polls that counter and advances the input timeline on real frame boundaries.

    ctrl word @ DDR3 base:  frame_counter[31:2] | active_buf[1:0]

RELATIONSHIP TO THE .inp RECORDER
---------------------------------
Complementary, not competing. A synthesised .inp is deterministic and runs with
no hardware at all, which makes it the better regression tool -- but it enters
through the recorder's own playback path, so it cannot exercise the recorder's
arm/stop, and it bypasses MiSTer Main entirely. This drives the REAL input path
end-to-end, including everything .inp deliberately skips.

MAPPING -- READ THIS BEFORE FIRST USE
-------------------------------------
MiSTer maps controllers by VID/PID into config/inputs/<CORE>_input_<VID>_<PID>_v3.map.
There are 27,525 such maps shipped, covering 26 controller types -- but NONE for
OpenBOR or PICO-8, because those cores are too new to have community defaults.

So the first run on each core needs ONE manual mapping pass through the OSD.
After that MiSTer writes the .map itself and every later run inherits it, because
this tool always presents the same VID/PID. Use --vid/--pid to impersonate a
controller you have already mapped if you prefer.

SAFETY
------
Everything is bounded. The device is destroyed on every exit path, the run has a
hard frame limit, and a stalled frame counter aborts instead of spinning -- a
remote command that outlives its session once starved sshd on this box for half
an hour.

USAGE
    uinput_inject.py --probe                 # create, report, destroy
    uinput_inject.py --script t.txt          # run a timeline
    uinput_inject.py --hold SOUTH@60:64 --frames 120

TIMELINE FORMAT (one action per line, '#' comments, frames are 0-based)
    30 34 SOUTH          # hold SOUTH from frame 30 up to (not incl.) 34
    60 64 START
    90 120 DPAD_RIGHT
"""

import argparse
import fcntl
import mmap
import os
import struct
import sys
import time

# ---- uinput ioctls: _IOC(dir,type,nr,size) = (dir<<30)|(size<<16)|(type<<8)|nr
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_SET_EVBIT = 0x40045564    # _IOW('U', 100, int)
UI_SET_KEYBIT = 0x40045565   # _IOW('U', 101, int)
UI_SET_ABSBIT = 0x40045567   # _IOW('U', 103, int)

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BUS_USB = 0x03

# Friendly name -> (kind, code). Names follow the pad's physical position, not
# any core's labelling, because the two OpenBOR builds and PICO-8 map them
# differently and a positional name stays true across all of them.
BUTTONS = {
    "SOUTH": 0x130, "EAST": 0x131, "NORTH": 0x133, "WEST": 0x134,
    "TL": 0x136, "TR": 0x137, "SELECT": 0x13A, "START": 0x13B,
}
ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y = 0x00, 0x01, 0x10, 0x11
# Directions ride ABS_X/ABS_Y, NOT ABS_HAT0X/Y, because that is what the map
# binds them to: the map stores directions as "ABS axis 0/1", i.e. the stick.
# Emitting on the hat would leave every direction unbound.
#
# ABS_X/ABS_Y are declared 0..255, so a direction is 0 or 255 and REST IS 128 --
# not 0. The hat's rest was 0 because its range is -1..1; reusing that here would
# park the stick hard-left forever and jam a direction on permanently.
# Back on the HAT. Moving directions to ABS_X/ABS_Y (to match what the map
# binds) killed them outright: MiSTer's built-in default handles a hat but not
# a raw stick. Keep the hat, which is measured to work, and treat the map's
# stick-bound directions as a separate problem.
DPAD = {
    "DPAD_LEFT": (ABS_HAT0X, -1), "DPAD_RIGHT": (ABS_HAT0X, 1),
    "DPAD_UP": (ABS_HAT0Y, -1), "DPAD_DOWN": (ABS_HAT0Y, 1),
}
AXIS_REST = {ABS_X: 128, ABS_Y: 128, ABS_HAT0X: 0, ABS_HAT0Y: 0}
AXES = [ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y]

DDR3_BASE = 0x3A000000       # ctrl word: frame_counter[31:2] | active_buf[1:0]
PAGE = 0x1000

DEFAULT_VID, DEFAULT_PID, DEFAULT_VER = 0x1209, 0xBEEF, 0x0001
DEFAULT_NAME = b"MiSTerFrontier Virtual Pad"


class Pad(object):
    """A uinput gamepad. Always destroyed -- see __exit__."""

    def __init__(self, name, vid, pid, ver):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        for ev in (EV_KEY, EV_ABS, EV_SYN):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        for code in BUTTONS.values():
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        for a in AXES:
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, a)

        absmax, absmin = [0] * 64, [0] * 64
        absmax[ABS_X] = absmax[ABS_Y] = 255
        absmax[ABS_HAT0X] = absmax[ABS_HAT0Y] = 1
        absmin[ABS_HAT0X] = absmin[ABS_HAT0Y] = -1
        # legacy uinput_user_dev: simpler than UI_DEV_SETUP and supported
        # everywhere this will ever run
        dev = struct.pack("<80s4HI", name.ljust(80, b"\0"), BUS_USB, vid, pid, ver, 0)
        dev += struct.pack("<64i", *absmax)
        dev += struct.pack("<64i", *absmin)
        dev += struct.pack("<64i", *([0] * 64))
        dev += struct.pack("<64i", *([0] * 64))
        assert len(dev) == 1116, len(dev)
        os.write(self.fd, dev)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.state = {}

    def __enter__(self):
        return self

    def __exit__(self, *a):
        try:
            self.release_all()
        except Exception:
            pass
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        except Exception:
            pass
        os.close(self.fd)

    def _emit(self, etype, code, value):
        # input_event on 32-bit ARM: timeval(2 longs) + u16 + u16 + s32 = 16 bytes
        os.write(self.fd, struct.pack("<llHHi", 0, 0, etype, code, value))

    def centre(self):
        """Park the analog axes at rest. Without this the device comes up with
        ABS_X/ABS_Y at 0, which on a 0..255 axis is a stuck full-left/full-up --
        MiSTer would see a direction held down from the moment it enumerates."""
        for axis, rest in AXIS_REST.items():
            self._emit(EV_ABS, axis, rest)
        self.sync()

    def set(self, name, on):
        """Idempotent: only emits when the state actually changes, so a held
        button produces one press event, not one per frame."""
        if self.state.get(name, 0) == (1 if on else 0):
            return False
        self.state[name] = 1 if on else 0
        if name in BUTTONS:
            self._emit(EV_KEY, BUTTONS[name], 1 if on else 0)
        elif name in DPAD:
            axis, val = DPAD[name]
            # release goes to the axis's REST value, not 0 -- see AXIS_REST
            self._emit(EV_ABS, axis, val if on else AXIS_REST[axis])
        else:
            raise KeyError(name)
        return True

    def sync(self):
        self._emit(EV_SYN, SYN_REPORT, 0)

    def release_all(self):
        changed = False
        for name in list(self.state):
            changed |= self.set(name, False)
        if changed:
            self.sync()

    def describe(self):
        """Our block from /proc/bus/input/devices, plus who holds the node."""
        try:
            txt = open("/proc/bus/input/devices").read()
        except IOError:
            return None, None, []
        for block in txt.split("\n\n"):
            if DEFAULT_NAME.decode() in block or "Virtual Pad" in block:
                ev = js = None
                for line in block.split("\n"):
                    if line.startswith("H: Handlers="):
                        for h in line.split("=", 1)[1].split():
                            if h.startswith("event"):
                                ev = h
                            if h.startswith("js"):
                                js = h
                return ev, js, block.strip().split("\n")
        return None, None, []


def holders_of(evnode):
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        d = "/proc/%s/fd" % pid
        try:
            for fd in os.listdir(d):
                try:
                    if os.readlink(os.path.join(d, fd)).endswith("/" + evnode):
                        try:
                            c = open("/proc/%s/comm" % pid).read().strip()
                        except IOError:
                            c = "?"
                        out.append("%s(%s)" % (pid, c))
                        break
                except OSError:
                    continue
        except OSError:
            continue
    return out


class FrameSource(object):
    """Reads the core's frame counter out of the DDR3 control word.

    This is the same signal the live-fps measurement reads. MiSTer Main has its
    own vsync callback, but it is process-internal (SPI + direct memory), so an
    external tool reads the counter our own core already publishes instead."""

    def __init__(self):
        self.f = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
        self.m = mmap.mmap(self.f, PAGE, mmap.MAP_SHARED,
                           mmap.PROT_READ, offset=DDR3_BASE)

    def close(self):
        self.m.close()
        os.close(self.f)

    def frame(self):
        self.m.seek(0)
        return struct.unpack("<I", self.m.read(4))[0] >> 2   # drop active_buf[1:0]


def parse_timeline(lines):
    """-> [(start, end, name)] ; '#' comments and blank lines ignored."""
    out = []
    for n, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3:
            raise SystemExit("timeline line %d: expected 'START END BUTTON', got %r" % (n, line))
        s, e, name = int(parts[0]), int(parts[1]), parts[2].upper()
        if name not in BUTTONS and name not in DPAD:
            raise SystemExit("timeline line %d: unknown button %r (known: %s)"
                             % (n, name, ", ".join(sorted(list(BUTTONS) + list(DPAD)))))
        if e <= s:
            raise SystemExit("timeline line %d: END must be greater than START" % n)
        out.append((s, e, name))
    return out


# MiSTer's "Define buttons" prompt order, read from Main_MiSTer menu.cpp
# (joy_button_map) -- NOT inferred:
#
#   RIGHT, LEFT, DOWN, UP, BUTTON A, BUTTON B, BUTTON X, BUTTON Y,
#   BUTTON L, BUTTON R, SELECT, START, KBD TOGGLE, MENU, <stick/mouse tilts>
#
# The first 12 are the core gamepad entries. `jn` in CONF_STR is SNES naming,
# but every control our cores document is written in XBOX terms (see each
# per-core README, "Controls (Xbox wireless controller default mapping)"), so
# the virtual pad must be mapped to REPRODUCE an Xbox pad -- otherwise the pad
# is internally consistent while silently meaning something different from the
# docs, and every test timeline written from a README is wrong.
#
#   jn A = Xbox B    jn B = Xbox A    jn X = Xbox Y    jn Y = Xbox X
#
MAP_XBOX_SEQUENCE = [
    ("RIGHT",    "DPAD_RIGHT"),
    ("LEFT",     "DPAD_LEFT"),
    ("DOWN",     "DPAD_DOWN"),
    ("UP",       "DPAD_UP"),
    ("BUTTON A", "EAST"),    # jn A -> Xbox B
    ("BUTTON B", "SOUTH"),   # jn B -> Xbox A
    ("BUTTON X", "NORTH"),   # jn X -> Xbox Y
    ("BUTTON Y", "WEST"),    # jn Y -> Xbox X
    ("BUTTON L", "TL"),      # jn L -> Xbox LB
    ("BUTTON R", "TR"),      # jn R -> Xbox RB
    ("SELECT",   "SELECT"),
    ("START",    "START"),
]

def main():
    # Line-buffer stdout. A backgrounded run once died without flushing and left
    # a 0-byte log, so there was no record of whether it had injected anything --
    # the same blindness as grep-filtering a tool while diagnosing it.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hold-open", type=float, default=0.0, metavar="SECONDS",
                    help="create the pad and just HOLD it, injecting nothing, so a human "
                         "can map it in the OSD. Ends early if /tmp/uinput_stop appears.")
    ap.add_argument("--map-xbox", action="store_true",
                    help="walk MiSTer's Define-buttons flow, pressing the button that "
                         "reproduces an XBOX pad for each prompt (see MAP_XBOX_SEQUENCE)")
    ap.add_argument("--map-gap", type=float, default=1.5, metavar="SECONDS",
                    help="wall-clock gap between --map-xbox presses (default 1.5)")
    ap.add_argument("--wait-go", type=float, default=0.0, metavar="SECONDS",
                    help="before --map-xbox, hold the pad and WAIT for /tmp/uinput_go. "
                         "Keeps ONE pad alive across the human navigating the OSD -- "
                         "starting a second process instead would make the device vanish "
                         "and reappear mid-flow, forcing another 25s re-enumeration.")
    ap.add_argument("--map-tail", type=float, default=90.0, metavar="SECONDS",
                    help="after the 12 presses, keep the pad ALIVE this long so the "
                         "trailing prompts (KBD TOGGLE, MENU, stick/mouse) auto-advance "
                         "on MiSTer's timeout with the device still present")
    ap.add_argument("--probe", action="store_true",
                    help="create the pad, report enumeration + who grabbed it, destroy")
    ap.add_argument("--script", help="timeline file")
    ap.add_argument("--hold", action="append", default=[], metavar="NAME@START:END")
    ap.add_argument("--frames", type=int, default=0,
                    help="total frames to run (default: last timeline frame + 30)")
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PID)
    ap.add_argument("--name", default=DEFAULT_NAME.decode())
    # 3 s was too short on real hardware: the probe reported "grabbed by: NOBODY"
    # every time, which looks exactly like the tool not working. MiSTer Main
    # hotplug-detects on a slow poll -- 25 s got a clean "grabbed by: 535(MiSTer)"
    # on the same box, same core, no other change. Default raised so the first
    # run someone tries does not produce a false negative.
    ap.add_argument("--settle", type=float, default=25.0,
                    help="seconds to let MiSTer Main enumerate the pad "
                         "(measured: 3 s is NOT enough, 25 s works)")
    ap.add_argument("--min-fps", type=int, default=0, metavar="N",
                    help="wait until the core is actually producing N+ fps before "
                         "injecting (0 = off). A core with no content still ticks "
                         "the counter at ~7 fps via the keepalive, so 'counter is "
                         "advancing' does NOT mean content is running. Use ~20.")
    ap.add_argument("--wait-content", type=float, default=180.0, metavar="SECONDS",
                    help="how long --min-fps may wait before giving up (default 180; "
                         "PAK load times span ~2s to ~69s across the library)")
    ap.add_argument("--max-seconds", type=float, default=120.0,
                    help="hard wall-clock ceiling; nothing may outlive this")
    a = ap.parse_args()

    timeline = []
    if a.script:
        timeline += parse_timeline(open(a.script).read().split("\n"))
    for h in a.hold:
        name, rng = h.split("@", 1)
        s, e = rng.split(":")
        timeline += parse_timeline(["%s %s %s" % (s, e, name)])

    total = a.frames or (max([e for _, e, _ in timeline] or [0]) + 30)

    t_start = time.time()
    with Pad(a.name.encode(), a.vid, a.pid, DEFAULT_VER) as pad:
        pad.centre()          # before MiSTer reads it, not after
        time.sleep(a.settle)
        ev, js, block = pad.describe()
        if not ev:
            print("FAIL: pad did not appear in /proc/bus/input/devices")
            return 1
        print("pad: event=%s js=%s vid=%04x pid=%04x" % (ev, js, a.vid, a.pid))
        held = holders_of(ev)
        print("grabbed by: %s" % (", ".join(held) if held else "NOBODY"))
        if not held:
            print("  WARNING: MiSTer Main is not holding it, so input will not reach a core.")
            print("  Usually means Main was busy at hotplug -- retry, or check it is running.")

        if a.probe:
            for l in block:
                print("  " + l)
            return 0

        if a.hold_open > 0:
            # Hold the device open so a human can map it in the OSD. Injects
            # nothing -- the pad only has to EXIST while the mapping flow runs.
            # Bounded two ways: the --hold-open ceiling, and a stop file, so it
            # can never outlive the session that started it.
            limit = min(a.hold_open, a.max_seconds)
            print("holding the pad open for up to %.0fs "
                  "(touch /tmp/uinput_stop to end early)" % limit)
            t0 = time.time()
            while time.time() - t0 < limit:
                if os.path.exists("/tmp/uinput_stop"):
                    print("stop file seen, releasing")
                    os.unlink("/tmp/uinput_stop")
                    break
                time.sleep(0.5)
            print("held %.1fs; pad about to be destroyed" % (time.time() - t0))
            return 0

        if a.map_xbox and a.wait_go > 0:
            print("waiting up to %.0fs for /tmp/uinput_go (pad held open meanwhile)" % a.wait_go)
            t_w = time.time()
            while not os.path.exists("/tmp/uinput_go"):
                if time.time() - t_w > a.wait_go:
                    print("no go signal within %.0fs -- releasing without mapping" % a.wait_go)
                    return 2
                if os.path.exists("/tmp/uinput_stop"):
                    os.unlink("/tmp/uinput_stop"); print("stop file seen"); return 2
                time.sleep(0.5)
            os.unlink("/tmp/uinput_go")
            print("go received after %.0fs" % (time.time() - t_w))

        if a.map_xbox:
            # Walk Define-buttons. WALL-CLOCK paced, deliberately: the OSD flow
            # advances on a press, not per frame, and the frame counter is not a
            # reliable clock while the OSD is up. The prompt ORDER comes from
            # menu.cpp; the button per prompt comes from the per-core README's
            # Xbox table.
            print("mapping to reproduce an XBOX pad; %d prompts, %.1fs apart"
                  % (len(MAP_XBOX_SEQUENCE), a.map_gap))
            print("  the OSD must ALREADY be on the first prompt (RIGHT) before this runs")
            for prompt, button in MAP_XBOX_SEQUENCE:
                if time.time() - t_start > a.max_seconds:
                    print("  max-seconds reached, stopping partway")
                    return 1
                print("  %-10s <- %s" % (prompt, button))
                pad.set(button, True); pad.sync()
                time.sleep(0.12)
                pad.set(button, False); pad.sync()
                time.sleep(a.map_gap)
            # The remaining prompts (KBD TOGGLE, MENU, stick/mouse tilts) are
            # skipped by simply not pressing: menu.cpp advances on a timeout
            # (CheckTimer(joy_map_timeout) -> MENU_JOYDIGMAP1). So the user
            # presses NOTHING -- but the pad must stay alive while those
            # timeouts elapse, or the device vanishes mid-flow.
            print("12 sent. Holding %.0fs while the trailing prompts time out" % a.map_tail)
            print("  (KBD TOGGLE, MENU, stick/mouse -- skipped by not pressing)")
            t_tail = time.time()
            while time.time() - t_tail < a.map_tail:
                if os.path.exists("/tmp/uinput_stop"):
                    os.unlink("/tmp/uinput_stop"); break
                if time.time() - t_start > a.max_seconds:
                    print("  max-seconds reached during tail"); break
                time.sleep(0.5)
            print("mapping run complete")
            return 0

        if not timeline:
            print("nothing to inject (no --script and no --hold)")
            return 2

        fs = FrameSource()
        try:
            base = fs.frame()
            t0 = time.time()
            # A stalled counter means no core is driving video. Bail rather than
            # spin: this is the class of loop that must never outlive its session.
            spin = 0
            while fs.frame() == base:
                time.sleep(0.05)
                spin += 1
                if spin > 40:
                    print("FAIL: frame counter is not advancing (base=%d)." % base)
                    print("  No hybrid core is running, or it is not writing DDR3.")
                    return 1

            # A live counter is NOT the same as content running. A core with no
            # cart/PAK mounted still advances it via the 150ms keepalive (~6.7
            # fps), so "advancing" alone will happily inject a whole timeline
            # into a loading screen -- which is exactly what happened on
            # 2026-08-03: a fixed 45s sleep was assumed generous because ATOV
            # loads in <2s, but PAK load times across the library span ~2s to
            # ~69s, so ANY fixed delay is wrong for some content.
            # Gate on OBSERVED production instead of a guessed delay.
            if a.min_fps > 0:
                waited = 0.0
                while True:
                    f0 = fs.frame(); time.sleep(0.5); fps = (fs.frame() - f0) * 2
                    if fps >= a.min_fps:
                        print("content running (%d fps >= --min-fps %d) after %.1fs"
                              % (fps, a.min_fps, waited))
                        break
                    waited += 0.5
                    if waited >= a.wait_content:
                        print("FAIL: only %d fps after %.0fs (--min-fps %d)."
                              % (fps, waited, a.min_fps))
                        print("  ~7 fps = keepalive only: no cart/PAK mounted, or still loading.")
                        print("  Injecting now would spend the timeline on a loading screen.")
                        return 3
            print("frame counter live (base=%d); injecting %d frames" % (base, total))

            start = fs.frame()
            last = -1
            emitted = 0
            reset = False
            while True:
                if time.time() - t0 > a.max_seconds:
                    print("stopping: hit --max-seconds ceiling")
                    break
                raw = fs.frame()
                # A core restart resets the frame counter, so `raw - start` goes
                # NEGATIVE and every later comparison is nonsense: the run then
                # grinds to --max-seconds and reports garbage (measured: a ~12s
                # job burned 120s and printed "-92639 frames ... -772.0 fps").
                # Any backwards movement means the thing we are pacing against
                # restarted, so the timeline is void -- fail fast and say why.
                # This is NOT rare: Record and Play both reset the PAK, so any
                # timeline crossing them must be split into separate runs.
                if raw < start:
                    reset = True
                    print("FAIL: frame counter went BACKWARDS (%d -> %d) -- the core "
                          "restarted mid-run, so this timeline is void." % (start, raw))
                    print("      Split the timeline: anything that resets the core "
                          "(Record / Play / Reset) must end its run there.")
                    break
                cur = raw - start
                if cur == last:
                    time.sleep(0.001)
                    continue
                last = cur
                if cur >= total:
                    break
                changed = False
                for s, e, name in timeline:
                    changed |= pad.set(name, s <= cur < e)
                if changed:
                    pad.sync()
                    emitted += 1
            elapsed = time.time() - t0
            if reset:
                # EXIT CODES -- must stay DISTINCT, or a harness cannot act on
                # them and ends up parsing prose (or, worse, treating any
                # non-zero as the same thing and reporting a driver error as a
                # real result):
                #   0 = the timeline ran
                #   1 = no frame counter at all (no core running)
                #   2 = usage / nothing to inject
                #   3 = content not running (only keepalive) -- see --min-fps
                #   4 = the core RESTARTED mid-run; the timeline is void
                return 4
            print("done: %d frames, %d state changes, %.1fs (%.1f fps observed)"
                  % (last, emitted, elapsed, last / elapsed if elapsed else 0))
        finally:
            fs.close()
    print("pad destroyed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
