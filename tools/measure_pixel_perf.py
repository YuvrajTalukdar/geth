#!/usr/bin/env python3
"""Measure geth's render cost in a fake terminal (dev tool, not in ctest).

Runs `geth --demo` in a pty at a fixed size and 1 Hz for a fixed number of
ticks, against a fake terminal that (a) answers the startup probe so the
pixel path is exercised and (b) swallows all output while counting bytes.
Reports wall time, CPU % (rusage of the geth process) and bytes/s.

Budget (v1.1.0): 1 tick/s must cost ~<= 2 % of a core (= 20 ms of CPU per
second) for the pixel renderer.

Usage:
  python3 tools/measure_pixel_perf.py [build/geth] [rows x cols] [ticks]

Runs the pixel renderer, then the text renderer, and prints both.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time


def run_once(geth, rows, cols, ticks, renderer):
    # Cell size 13x28 px (the embedded glyph metrics) — the probe reads it
    # from TIOCGWINSZ (ws_xpixel/cols).
    xpixel, ypixel = cols * 13, rows * 28
    env = dict(os.environ)
    env["GETH_TEST"] = "1"
    env["XDG_CONFIG_HOME"] = "/tmp/geth-cfg-perf"
    env["TERM"] = "xterm-256color"
    env.pop("TMUX", None)
    env.pop("TERM_PROGRAM", None)
    env.pop("COLORTERM", None)

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                struct.pack("HHHH", rows, cols, xpixel, ypixel))

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
        os.execve(geth,
                  [geth, "--demo", "--interval", "1000",
                   "--renderer", renderer, "--count", str(ticks)], env)
        os._exit(127)

    os.close(slave)
    total_bytes = 0
    transfers = deletes = 0
    tail = b""  # rolling window for query detection (may split chunks)
    replied_query = replied_test = False
    rusage = None
    start = time.time()
    exit_status = None
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
            transfers += chunk.count(b"a=T,t=d,C=1")
            deletes += chunk.count(b"a=d,d=i")
            tail = (tail + chunk)[-256:]
            if not replied_query and b"\x1b_Gi=31" in tail:
                # kitty query: answer OK, DA1 without '4' (kitty, no sixel).
                os.write(master, b"\x1b_Gi=31;OK\x1b\\\x1b[?62;c")
                replied_query = True
            if not replied_test and b"\x1b_Gi=99" in tail:
                os.write(master, b"\x1b_Gi=99;OK\x1b\\")
                replied_test = True
        p, status, ru = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = status
            rusage = ru
    # The read may have hit EOF just after the child exited — reap before
    # concluding anything (WNOHANG in the loop only sees it on a later pass).
    if exit_status is None:
        for _ in range(100):
            p, status, ru = os.wait4(pid, os.WNOHANG)
            if p == pid:
                exit_status, rusage = status, ru
                break
            time.sleep(0.05)
    if exit_status is None:  # it hung — kill and report
        os.kill(pid, signal.SIGKILL)
        _, exit_status, rusage = os.wait4(pid, 0)
        exit_status = -9
    # Drain anything still buffered.
    while True:
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
        transfers += chunk.count(b"a=T,t=d,C=1")
        deletes += chunk.count(b"a=d,d=i")
    elapsed = time.time() - start
    os.close(master)

    # CPU time of the geth process (rusage captured by the final wait4).
    cpu_s = (rusage.ru_utime + rusage.ru_stime) if rusage is not None else None
    return dict(renderer=renderer, wall=elapsed, bytes=total_bytes,
                transfers=transfers, deletes=deletes, exit=exit_status,
                cpu_s=cpu_s)


def main():
    geth = sys.argv[1] if len(sys.argv) > 1 else "build/geth"
    rows_cols = sys.argv[2] if len(sys.argv) > 2 else "70x250"
    ticks = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    rows, cols = (int(v) for v in rows_cols.lower().split("x"))
    for renderer in ("pixels", "text"):
        r = run_once(geth, rows, cols, ticks, renderer)
        cpu_pct = 100.0 * r["cpu_s"] / r["wall"] if r["cpu_s"] is not None else -1
        st = r["exit"]
        if st == 0:
            exit_str = "0 (clean)"
        elif st < 0:
            exit_str = f"killed (hang)"
        elif os.WIFSIGNALED(st):
            exit_str = f"signal {os.WTERMSIG(st)}"
        else:
            exit_str = str(os.WEXITSTATUS(st))
        print(f"geth --demo --renderer {r['renderer']}  {cols}x{rows}  "
              f"1 Hz, {ticks} ticks")
        print(f"  wall:     {r['wall']:.1f} s   exit: {exit_str}")
        print(f"  cpu:      {cpu_pct:.2f} %  (budget ~2 % = 20 ms/s)")
        print(f"  bytes/s:  {r['bytes'] / r['wall']:.0f} B/s  "
              f"(master side, all traffic)")
        print(f"  images:   transfers={r['transfers']} deletes={r['deletes']}")
        print()


if __name__ == "__main__":
    main()
