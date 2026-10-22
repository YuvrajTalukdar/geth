#!/usr/bin/env python3
"""pty-harness test for `geth --count N`: it must EXIT ON ITS OWN.

The old dev lore said "`--count` stops sampling but never exits" — that was a
harness artifact: the pty read loop broke on the EIO (child closed its end of
the pty) *before* reaping the zombie, lost the real status, and reported a
bogus "signal 119". This test drives `geth --demo --count N --log` in a pty
and asserts the CORRECT behavior:

  1. geth exits SPONTANEOUSLY (no SIGTERM/SIGKILL sent) with exit code 0,
     promptly after the Nth tick — terminal restored, log closed.
  2. The CSV it wrote is complete: a header plus exactly N data rows, every
     row the same (parseable) width.

Usage:
  python3 tools/count_exit_test.py [build-rel/geth] [count] [interval_ms]
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
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    interval = int(sys.argv[3]) if len(sys.argv) > 3 else 1000

    xdg_cfg = tempfile.mkdtemp(prefix="geth-cnt-cfg-")
    xdg_data = tempfile.mkdtemp(prefix="geth-cnt-data-")
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
    args = [geth, "--demo", "--interval", str(interval), "--count", str(count),
            "--log", "--log-dir", log_dir]
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

    # Expected wall time: N ticks at `interval`, plus a generous margin for
    # start-up and the post-tick exit. NO signal is sent — a spontaneous exit
    # is exactly what we are testing.
    start = time.time()
    deadline = start + (count * interval / 1000.0) + 15.0
    exit_status = None
    t_exit = None
    while time.time() < deadline:
        r, _, _ = select.select([master], [], [], 0.05)
        if master in r:
            try:
                os.read(master, 1 << 20)
            except OSError:
                pass  # child closed its end; keep waiting to reap below
        p, status, _ = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = status
            t_exit = time.time()
            # give the pty a moment to drain, then stop
            time.sleep(0.1)
            break
    os.close(master)

    spontaneous = exit_status is not None
    if spontaneous and os.WIFEXITED(exit_status) and \
            os.WEXITSTATUS(exit_status) == 0:
        print(f"PASS: spontaneous exit code 0 after {t_exit - start:.2f}s "
              f"(no signal sent; --count {count} @ {interval} ms)")
    elif spontaneous:
        if os.WIFSIGNALED(exit_status):
            print(f"FAIL: exited by signal {os.WTERMSIG(exit_status)}, not a clean code 0")
        else:
            print(f"FAIL: spontaneous exit code {os.WEXITSTATUS(exit_status)}, expected 0")
        return 1
    else:
        # It never exited on its own — the old false premise. Clean up.
        print(f"FAIL: no spontaneous exit within "
              f"{count * interval / 1000.0 + 15.0:.0f}s (--count {count})")
        os.kill(pid, signal.SIGKILL)
        os.wait4(pid, 0)
        return 1

    # The CSV must be complete: header + exactly `count` data rows, uniform.
    files = sorted(os.path.join(log_dir, f) for f in
                   (os.listdir(log_dir) if os.path.isdir(log_dir) else [])
                   if f.endswith(".csv"))
    if not files:
        print("FAIL: no CSV file was written")
        return 1
    path = files[-1]
    with open(path, "r", encoding="utf-8", newline="") as f:
        rows = list(csv.reader(f))
    ncols = len(rows[0]) if rows else 0
    bad = [i for i, r in enumerate(rows) if len(r) != ncols]
    data_rows = len(rows) - 1
    print(f"  CSV: {os.path.basename(path)}  columns: {ncols}  "
          f"data rows: {data_rows}  (expected {count})")
    if bad:
        print(f"  FAIL: {len(bad)} row(s) with wrong field count: {bad}")
        return 1
    if data_rows != count:
        print(f"  FAIL: expected {count} data rows, got {data_rows}")
        return 1
    print("PASS: CSV is complete (header + one row per tick, uniform width)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
