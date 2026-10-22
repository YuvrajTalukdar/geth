#!/usr/bin/env python3
"""Measure geth's CPU cost of CSV logging in a fake terminal (dev tool).

Runs `geth --demo` in a pty at a fixed size and 1 Hz for a fixed number of
ticks, twice per renderer: once WITHOUT --log and once WITH --log (to a temp
dir). Reports wall time and CPU % (rusage of the geth process) for each, and
the delta. The point: at 1 Hz, logging must not measurably change the CPU
cost (the zero-alloc steady state is proven separately by the unit tests).

Usage:
  python3 tools/measure_log_perf.py [build-rel/geth] [rows x cols] [ticks]
"""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time


def run_once(geth, rows, cols, ticks, renderer, log_dir):
    # Cell size 13x28 px (the embedded glyph metrics) — the probe reads it
    # from TIOCGWINSZ (ws_xpixel/cols).
    xpixel, ypixel = cols * 13, rows * 28
    env = dict(os.environ)
    env["GETH_TEST"] = "1"
    env["XDG_CONFIG_HOME"] = "/tmp/geth-cfg-perf"
    env["XDG_DATA_HOME"] = "/tmp/geth-data-perf"
    env["TERM"] = "xterm-256color"
    env.pop("TMUX", None)
    env.pop("TERM_PROGRAM", None)
    env.pop("COLORTERM", None)

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                struct.pack("HHHH", rows, cols, xpixel, ypixel))

    args = [geth, "--demo", "--interval", "1000", "--renderer", renderer,
            "--count", str(ticks)]
    if log_dir:
        args += ["--log", "--log-dir", log_dir]

    pid = os.fork()
    if pid == 0:  # child
        os.setsid()
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        if slave > 2:
            os.close(slave)
        os.close(master)
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        os.execve(geth, args, env)
        os._exit(127)

    os.close(slave)
    start = time.time()
    exit_status = None
    rusage = None
    total_bytes = 0
    while exit_status is None:
        r, _, _ = select.select([master], [], [], 0.01)
        if master in r:
            try:
                chunk = os.read(master, 1 << 20)
            except OSError:
                break
            if not chunk:
                break
            total_bytes += len(chunk)
        p, status, ru = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = status
            rusage = ru
    if exit_status is None:
        for _ in range(200):
            p, status, ru = os.wait4(pid, os.WNOHANG)
            if p == pid:
                exit_status, rusage = status, ru
                break
            time.sleep(0.05)
    if exit_status is None:  # it hung — kill and report
        os.kill(pid, signal.SIGKILL)
        _, exit_status, rusage = os.wait4(pid, 0)
        exit_status = -9
    while True:  # drain
        r, _, _ = select.select([master], [], [], 0.05)
        if master not in r:
            break
        try:
            chunk = os.read(master, 1 << 20)
        except OSError:
            break
        if not chunk:
            break
        total_bytes += len(chunk)
    elapsed = time.time() - start
    os.close(master)
    cpu_s = (rusage.ru_utime + rusage.ru_stime) if rusage is not None else None
    return dict(wall=elapsed, bytes=total_bytes, exit=exit_status, cpu_s=cpu_s)


def main():
    geth = sys.argv[1] if len(sys.argv) > 1 else "build-rel/geth"
    rows_cols = sys.argv[2] if len(sys.argv) > 2 else "70x250"
    ticks = int(sys.argv[3]) if len(sys.argv) > 3 else 45
    rows, cols = (int(v) for v in rows_cols.lower().split("x"))

    results = {}
    for renderer in ("pixels", "text"):
        for log in (False, True):
            log_dir = (tempfile.mkdtemp(prefix="geth-log-perf-") if log else None)
            r = run_once(geth, rows, cols, ticks, renderer, log_dir)
            cpu_pct = 100.0 * r["cpu_s"] / r["wall"] if r["cpu_s"] is not None else -1
            results[(renderer, log)] = (r, cpu_pct)
            tag = "log ON " if log else "log OFF"
            print(f"geth --demo --renderer {renderer:6s} {cols}x{rows}  1 Hz, "
                  f"{ticks} ticks  [{tag}]")
            print(f"  wall: {r['wall']:.1f}s   exit: {r['exit']}   "
                  f"cpu: {cpu_pct:.2f}%   bytes: {r['bytes']:.0f}")
            print()

    print("CPU % (log ON - log OFF) per renderer:")
    for renderer in ("pixels", "text"):
        off = results[(renderer, False)][1]
        on = results[(renderer, True)][1]
        print(f"  {renderer:6s}:  {on:6.2f}%  -  {off:6.2f}%  =  {on - off:+.2f}%")


if __name__ == "__main__":
    main()
