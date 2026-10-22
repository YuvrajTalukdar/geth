#!/usr/bin/env python3
"""SIGTERM mid-run test for the CSV logger (dev tool).

Starts `geth --demo --log` in a pty (1 Hz, no --count, so it runs until
signalled), lets it log for a few ticks, then sends SIGTERM. Verifies the
process exits and the CSV it wrote is complete: header + one row per tick,
every row parseable. This exercises the "terminal restored / process killed"
path — the logger's unbuffered per-row writes must leave a well-formed file.

Usage:
  python3 tools/sigterm_test.py <build-rel/geth> [ticks_before_signal]
"""
import csv
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


def main():
    geth = sys.argv[1] if len(sys.argv) > 1 else "build-rel/geth"
    ticks = int(sys.argv[2]) if len(sys.argv) > 2 else 5

    xdg_cfg = tempfile.mkdtemp(prefix="geth-sig-cfg-")
    xdg_data = tempfile.mkdtemp(prefix="geth-sig-data-")
    log_dir = os.path.join(xdg_data, "logs")
    env = dict(os.environ)
    env["GETH_TEST"] = "1"
    env["XDG_CONFIG_HOME"] = xdg_cfg
    env["XDG_DATA_HOME"] = xdg_data
    env["TERM"] = "xterm-256color"
    env.pop("TMUX", None)
    env.pop("TERM_PROGRAM", None)
    env.pop("COLORTERM", None)

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 2600, 1400))

    args = [geth, "--demo", "--interval", "1000", "--log", "--log-dir", log_dir]
    pid = os.fork()
    if pid == 0:
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

    # Let it run ~ticks seconds (1 row/s), pumping the pty.
    deadline = time.time() + ticks
    while time.time() < deadline:
        r, _, _ = select.select([master], [], [], 0.1)
        if master in r:
            try:
                os.read(master, 1 << 20)
            except OSError:
                pass

    print(f"sending SIGTERM after ~{ticks} s ...")
    os.kill(pid, signal.SIGTERM)
    exit_status = None
    for _ in range(200):
        p, status, _ = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = status
            break
        time.sleep(0.05)
    os.close(master)
    if exit_status is None:
        os.kill(pid, signal.SIGKILL)
        _, exit_status, _ = os.wait4(pid, 0)
    if os.WIFSIGNALED(exit_status):
        print(f"exit: signal {os.WTERMSIG(exit_status)}")
    elif os.WIFEXITED(exit_status):
        print(f"exit: {os.WEXITSTATUS(exit_status)}")

    # Find the CSV and validate it is complete and parseable.
    files = []
    if os.path.isdir(log_dir):
        files = sorted(os.path.join(log_dir, f) for f in os.listdir(log_dir)
                       if f.endswith(".csv"))
    if not files:
        print("FAIL: no CSV file was written")
        return 1
    path = files[-1]
    with open(path, "r", encoding="utf-8", newline="") as f:
        rows = list(csv.reader(f))
    ncols = len(rows[0]) if rows else 0
    ok = True
    bad = [i for i, r in enumerate(rows) if len(r) != ncols]
    data_rows = len(rows) - 1
    print(f"CSV: {path}")
    print(f"  columns: {ncols}   data rows: {data_rows}  (expected ~{ticks})")
    print(f"  header: {rows[0][:3]} ...")
    if bad:
        ok = False
        print(f"  FAIL: {len(bad)} row(s) with wrong field count: {bad}")
    if data_rows < 1:
        ok = False
        print("  FAIL: no data rows")
    print("  " + ("OK: complete and parseable" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
