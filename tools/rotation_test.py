#!/usr/bin/env python3
"""Rotation test for the CSV logger (dev tool, pty-driven).

Starts `geth --demo` in a pty (1 Hz, no --log), then drives it with real keys:
  l          start logging            -> file 1 (base.csv)
  (wait ~3 ticks)
  e          open the panel manager
  <space>    hide the selected panel  -> page change -> rotation
  Escape     close the manager
  (wait ~3 ticks)                     -> file 2 (base-2.csv)
  q          quit

Verifies TWO csv files appear in the log dir, that file 1 has MORE columns
than file 2 (the hidden panel's columns are gone), and that both are
parseable. Copies both to a target dir (default review/log_rotation).

Usage:
  python3 tools/rotation_test.py <build-rel/geth> [dest_dir]
"""
import csv
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time


def pump(master, secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.05)
        if master in r:
            try:
                os.read(master, 1 << 20)
            except OSError:
                return


def main():
    geth = sys.argv[1] if len(sys.argv) > 1 else "build-rel/geth"
    dest = sys.argv[2] if len(sys.argv) > 2 else "review/log_rotation"

    xdg_cfg = tempfile.mkdtemp(prefix="geth-rot-cfg-")
    xdg_data = tempfile.mkdtemp(prefix="geth-rot-data-")
    env = dict(os.environ)
    env["GETH_TEST"] = "1"
    env["XDG_CONFIG_HOME"] = xdg_cfg
    env["XDG_DATA_HOME"] = xdg_data
    env["TERM"] = "xterm-256color"
    env.pop("TMUX", None)
    env.pop("TERM_PROGRAM", None)
    env.pop("COLORTERM", None)

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                struct.pack("HHHH", 50, 200, 2600, 1400))
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
        os.execve(geth, [geth, "--demo", "--interval", "1000"], env)
        os._exit(127)
    os.close(slave)

    pump(master, 3)          # startup + settle
    os.write(master, b"l")   # start logging -> file 1
    pump(master, 3)          # ~3 rows into file 1
    os.write(master, b"e")   # open manager
    pump(master, 0.6)
    os.write(master, b" ")   # hide the selected (first) panel -> rotate
    pump(master, 0.6)
    os.write(master, b"\x1b")  # Escape: close manager
    pump(master, 3)          # ~3 rows into file 2
    os.write(master, b"q")   # quit
    pump(master, 3)

    exit_status = None
    for _ in range(100):
        p, st, _ = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = st
            break
        time.sleep(0.05)
    if exit_status is None:
        os.kill(pid, signal.SIGKILL)
        _, exit_status, _ = os.wait4(pid, 0)
    os.close(master)

    log_dir = os.path.join(xdg_data, "geth", "logs")
    files = sorted(os.path.join(log_dir, f) for f in
                   (os.listdir(log_dir) if os.path.isdir(log_dir) else [])
                   if f.endswith(".csv"))
    print(f"exit: {os.WEXITSTATUS(exit_status) if os.WIFEXITED(exit_status) else 'sig'}")
    print(f"csv files: {len(files)}")
    ok = len(files) == 2
    # Identify file 1 (base, no -N suffix) and file 2 (-2): by name, not by
    # sort order ("...-2.csv" sorts BEFORE "...csv" because '-' < '.').
    base = [p for p in files if not any(
        os.path.basename(p).endswith(f"-{n}.csv") for n in range(2, 99))]
    rot = [p for p in files if p not in base]
    if len(base) != 1 or len(rot) != 1:
        ok = False
        print(f"  FAIL: expected 1 base + 1 rotated, got {len(base)} + {len(rot)}")
    ncols = {}
    for path in files:
        with open(path, "r", encoding="utf-8", newline="") as f:
            rows = list(csv.reader(f))
        n = len(rows[0]) if rows else 0
        ncols[os.path.basename(path)] = n
        bad = [i for i, r in enumerate(rows) if len(r) != n]
        print(f"  {os.path.basename(path)}: {n} cols, {len(rows)-1} data rows"
              f"{'  BAD ROWS: %s' % bad if bad else ''}")
        if bad:
            ok = False
    if ok:
        b = ncols[os.path.basename(base[0])]
        r = ncols[os.path.basename(rot[0])]
        print(f"  columns: file1 (base) {b}  ->  file2 (-2) {r}  "
              f"({b - r} fewer after hiding a panel)")
        if not (r < b):
            ok = False
            print("  FAIL: file 2 should have fewer columns than file 1")

    # Copy the two files to the destination.
    if ok:
        os.makedirs(dest, exist_ok=True)
        for path in files:
            shutil.copy(path, os.path.join(dest, os.path.basename(path)))
        print(f"  copied both to {dest}/")
    print("  " + ("OK: rotation produced two well-formed files" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
