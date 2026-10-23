#!/usr/bin/env python3
"""Timer-thread tests for geth (v1.5.3).

Verifies the condition_variable timer (wait_until(next_deadline), re-anchored
on quit and on interval change, steady_clock deadlines) with four pty
measurements against the built geth binary:

  1. `q` at a 5000 ms interval exits in < 200 ms (the old sleep_for loop
     waited out the whole interval, so a quit could take up to 5 s).
  2. After each of `+` (1000 -> 500 ms, FASTER) and `-` (500 -> 1000, then
     1000 -> 2000 ms, SLOWER), the NEXT tick fires within the new interval
     +/- 50 ms of the keypress (the old code applied the new interval only
     after the current sleep finished).
  3. A 5000 -> 250 ms change (four `+` keys) blanks NO rates: the tick right
     after the change is not treated as a suspend gap, so its cpu.usage stays
     finite. (Without reset_gap_reference() on the change, up to 5000 ms +
     250 ms > 5 x 250 ms looks like a suspend and blanks the rates.)
  4. Over 60 ticks at a 1000 ms interval, cumulative drift < 50 ms
     (steady_clock deadlines accumulate no sleep_for error).

Ticks are observed via `--trace cpu.usage`, which prints one flushed
`[tick NNNN] store cpu.usage = <value>` line to stdout every tick (demo mode
always has a cpu.usage value; a blanked one prints as `nan`). The pty harness
records each line's arrival time. A blanked rate is detected as the value
reading `nan`.

Usage:
  python3 tools/timer_test.py <geth> [timeout_s]

Exit 0 iff all four tests pass, 1 otherwise.
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import tempfile
import termios
import time

TICK_RE = re.compile(rb"\[tick (\d+)\] store\s+cpu\.usage = (\S+)")


class PtyGet:
    """Runs one geth in a pty; records --trace tick lines with arrival times."""

    def __init__(self, geth, args, cols=200, rows=50):
        self.geth = geth
        self.args = args
        xdg_cfg = tempfile.mkdtemp(prefix="geth-timer-cfg-")
        xdg_data = tempfile.mkdtemp(prefix="geth-timer-data-")
        self.xdg_data = xdg_data
        self.xdg_cfg = xdg_cfg
        env = dict(os.environ)
        env["GETH_TEST"] = "1"
        env["XDG_CONFIG_HOME"] = xdg_cfg
        env["XDG_DATA_HOME"] = xdg_data
        env["TERM"] = "xterm-256color"
        for k in ("TMUX", "TERM_PROGRAM", "COLORTERM"):
            env.pop(k, None)

        xpixel, ypixel = cols * 13, rows * 28
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, xpixel, ypixel))

        self.pid = os.fork()
        if self.pid == 0:  # child
            os.setsid()
            os.dup2(slave, 0)
            os.dup2(slave, 1)
            os.dup2(slave, 2)
            if slave > 2:
                os.close(slave)
            os.close(self.master)
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            os.execve(geth, [geth] + args, env)
            os._exit(127)

        os.close(slave)
        self.ticks = []  # (arrival_time, tick_number, value_str)
        self.seen = set()
        self.tail = b""
        self.exited = False
        self.exit_status = None
        self.exit_time = None

    def _drain(self):
        r, _, _ = select.select([self.master], [], [], 0)
        while r:
            try:
                data = os.read(self.master, 1 << 20)
            except OSError:
                return
            if not data:
                return
            self._consume(data)
            r, _, _ = select.select([self.master], [], [], 0)

    def _consume(self, data):
        chunk = self.tail + data
        for m in TICK_RE.finditer(chunk):
            tick_no = int(m.group(1))
            if tick_no not in self.seen:
                self.seen.add(tick_no)
                self.ticks.append(
                    (time.time(), tick_no, m.group(2).decode()))
        # Keep a small tail: a trace line is < 64 bytes and is one atomic
        # write, so at most one line can straddle a read boundary.
        self.tail = chunk[-64:]

    def poll(self, timeout):
        self._drain_partial(timeout)
        p, status, _ = os.wait4(self.pid, os.WNOHANG)
        if p == self.pid:
            self.exited = True
            self.exit_status = status
            self.exit_time = time.time()

    def _drain_partial(self, timeout):
        r, _, _ = select.select([self.master], [], [], timeout)
        if self.master in r:
            try:
                data = os.read(self.master, 1 << 20)
            except OSError:
                return
            if data:
                self._consume(data)

    def send(self, key):
        os.write(self.master, key)

    def wait_for(self, pred, timeout):
        """Poll until pred() is true or timeout; return True if pred held."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.poll(0.01)
            if pred():
                return True
        self.poll(0.01)
        return pred()

    def tick_count(self):
        return len(self.ticks)

    def last_tick(self):
        return self.ticks[-1] if self.ticks else None

    def kill(self):
        if not self.exited:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
        try:
            os.close(self.master)
        except OSError:
            pass


def wait_ticks(g, count, timeout):
    return g.wait_for(lambda: g.tick_count() >= count, timeout)


def is_nan(val):
    v = val.lower()
    return v in ("nan", "-nan", "+nan")


def test1_quit_latency(g):
    """`q` at 5000 ms exits in < 200 ms."""
    # Wait for the first tick so the timer is in a steady 5000 ms sleep.
    if not wait_ticks(g, 1, 15.0):
        return False, "no tick within 15 s (startup?)"
    t0 = time.time()
    g.send(b"q")
    if not g.wait_for(lambda: g.exited, 5.0):
        g.kill()
        return False, "did not exit within 5 s of q"
    dt = (g.exit_time or time.time()) - t0
    ok = dt < 0.200
    return ok, "q->exit %.1f ms (limit 200)" % (dt * 1000)


def test2_interval_change(g):
    """After `+` (1000 -> 500) and `-` (500 -> 1000 -> 2000), each next tick
    is within the new interval +/- 50 ms of the keypress."""
    if not wait_ticks(g, 3, 15.0):
        return False, "no steady ticks within 15 s"
    changes = ((b"+", 0.500), (b"-", 1.000), (b"-", 2.000))
    for key, want in changes:
        before = g.tick_count()
        t0 = time.time()
        g.send(key)
        if not g.wait_for(lambda: g.tick_count() > before, want + 1.0):
            g.kill()
            return False, ("no next tick within %.0f ms of the %s change"
                           % (want * 1000, key.decode()))
        dt = (g.last_tick()[0]) - t0
        if abs(dt - want) >= 0.050:
            g.kill()
            return False, ("change->next-tick %.1f ms (want %.0f +/- 50 "
                           "after %s)" % (dt * 1000, want * 1000, key.decode()))
        time.sleep(0.1)
    return True, ("3 changes (+, -, -): each next tick within the new "
                  "interval +/- 50 ms")


def test3_no_blank_on_change(g):
    """5000 -> 250 ms (four `+`) must not blank the next tick's rates."""
    if not wait_ticks(g, 2, 20.0):
        return False, "no steady ticks within 20 s"
    before = g.tick_count()
    # 5000 -> 2000 -> 1000 -> 500 -> 250, one `+` key each (faster).
    for _ in range(4):
        g.send(b"+")
        time.sleep(0.05)
    # The tick immediately after the change must NOT be a blank (nan) gap.
    # Wait for one new tick at the 250 ms cadence and inspect its value.
    if not g.wait_for(lambda: g.tick_count() > before, 3.0):
        g.kill()
        return False, "no tick within 3 s of the change"
    # The FIRST new tick is the one that would be blanked by a stale gap
    # reference. Check it (and the next, for good measure).
    new = g.ticks[before:]
    checked = new[:2]
    bad = [t for t in checked if is_nan(t[2])]
    if checked and not bad:
        return True, ("next %d tick(s) after 5000->250 all finite "
                      "(values: %s)" % (len(checked),
                                        ", ".join(t[2] for t in checked)))
    if not checked:
        return False, "no new tick value to inspect"
    return False, ("blanked tick(s) after the change: %s"
                   % ", ".join(t[2] for t in bad))


def test4_drift(g):
    """Over 60 ticks at 1000 ms, cumulative drift < 50 ms."""
    # Let the ticks run (the process exits after --count 60).
    if not g.wait_for(lambda: g.exited or g.tick_count() >= 60, 90.0):
        g.kill()
        return False, "did not reach 60 ticks / exit in 90 s"
    ts = [t[0] for t in g.ticks]
    if len(ts) < 60:
        g.kill()
        return False, "only %d tick lines (need 60)" % len(ts)
    # Use the first and 60th tick line; the span should be 59 x 1000 ms.
    first, last = ts[0], ts[59]
    span = last - first
    drift = abs(span - 59.0 * 1.000)
    ok = drift < 0.050
    return ok, ("60 ticks spanned %.1f ms (want 59000 +/- 50); "
                "drift %.1f ms" % (span * 1000, drift * 1000))


def run_one(name, geth, args, check, timeout):
    print("=== %s ===" % name)
    print("args: %s" % " ".join(args))
    g = PtyGet(geth, args)
    try:
        ok, detail = check(g)
    except Exception as e:  # a harness crash is a test failure, not a hang
        ok, detail = False, "harness error: %r" % e
    finally:
        g.kill()
    print("  -> %s: %s" % ("PASS" if ok else "FAIL", detail))
    return ok


def main():
    argv = sys.argv[1:]
    if len(argv) < 1 or argv[0] in ("--help", "-h"):
        print(__doc__)
        return 2
    geth = argv[0]
    timeout = float(argv[1]) if len(argv) > 1 else 300.0

    results = []
    results.append(run_one(
        "1. quit latency (q at 5000 ms exits < 200 ms)", geth,
        ["--demo", "--interval", "5000", "--trace", "cpu.usage"],
        test1_quit_latency, timeout))
    results.append(run_one(
        "2. interval change (next tick within new interval +/- 50 ms)", geth,
        ["--demo", "--interval", "1000", "--trace", "cpu.usage"],
        test2_interval_change, timeout))
    results.append(run_one(
        "3. no rate blanking on 5000 -> 250 ms change", geth,
        ["--demo", "--interval", "5000", "--trace", "cpu.usage"],
        test3_no_blank_on_change, timeout))
    results.append(run_one(
        "4. drift over 60 ticks at 1000 ms (< 50 ms)", geth,
        ["--demo", "--interval", "1000", "--trace", "cpu.usage",
         "--count", "60"],
        test4_drift, timeout))

    print()
    print("=" * 60)
    passed = sum(1 for r in results if r)
    print("TIMER TESTS: %d/4 passed" % passed)
    for r, name in zip(results, [
            "1. quit latency", "2. interval change",
            "3. no blanking on change", "4. drift over 60 ticks"]):
        print("  %s  %s" % ("PASS" if r else "FAIL", name))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
