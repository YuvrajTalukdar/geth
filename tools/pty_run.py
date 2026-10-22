#!/usr/bin/env python3
"""Run geth in a pty (dev harness for the review CSVs / SIGTERM test).

Starts geth with the given args in a pty of a fixed size, with GETH_TEST=1
and fresh temp XDG_CONFIG_HOME / XDG_DATA_HOME (so it never touches the real
config or the real ~/.local/share log dir), swallows the output, and waits for
it to exit on its own (or a timeout, after which SIGTERM is sent). Prints the
exit status and the temp dirs so the caller can find the CSV(s).

Usage:
  python3 tools/pty_run.py <geth> <cols> <rows> [timeout_s] -- [geth args ...]

Example (demo, 20 ticks, logging to the temp data dir):
  python3 tools/pty_run.py build-rel/geth 250 70 120 -- \
      --demo --interval 1000 --count 20 --log
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


def main():
    argv = sys.argv[1:]
    if not argv or argv[0] == "--help":
        print(__doc__)
        return 2
    if "--" not in argv:
        print(__doc__)
        return 2
    di = argv.index("--")
    head, geth_args = argv[:di], argv[di + 1:]
    if len(head) < 3:
        print(__doc__)
        return 2
    geth, cols, rows = head[0], int(head[1]), int(head[2])
    timeout = float(head[3]) if len(head) > 3 else 300.0

    xdg_cfg = tempfile.mkdtemp(prefix="geth-pty-cfg-")
    xdg_data = tempfile.mkdtemp(prefix="geth-pty-data-")
    env = dict(os.environ)
    env["GETH_TEST"] = "1"
    env["XDG_CONFIG_HOME"] = xdg_cfg
    env["XDG_DATA_HOME"] = xdg_data
    env["TERM"] = "xterm-256color"
    env.pop("TMUX", None)
    env.pop("TERM_PROGRAM", None)
    env.pop("COLORTERM", None)

    xpixel, ypixel = cols * 13, rows * 28
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
        os.execve(geth, [geth] + geth_args, env)
        os._exit(127)

    os.close(slave)
    start = time.time()
    exit_status = None
    while exit_status is None:
        r, _, _ = select.select([master], [], [], 0.05)
        if master in r:
            try:
                os.read(master, 1 << 20)
            except OSError:
                break
        p, status, _ = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = status
        elif time.time() - start > timeout:
            print("TIMEOUT: sending SIGTERM to the geth process")
            os.kill(pid, signal.SIGTERM)
            for _ in range(100):
                p, status, _ = os.wait4(pid, os.WNOHANG)
                if p == pid:
                    exit_status = status
                    break
                time.sleep(0.05)
            break
    os.close(master)
    if exit_status is None:
        os.kill(pid, signal.SIGKILL)
        _, exit_status, _ = os.wait4(pid, 0)
        exit_status = -9

    if os.WIFEXITED(exit_status):
        code = os.WEXITSTATUS(exit_status)
        print(f"exit: {code} (clean)")
    elif os.WIFSIGNALED(exit_status):
        print(f"exit: signal {os.WTERMSIG(exit_status)}")
    else:
        print(f"exit: {exit_status}")
    print(f"XDG_DATA_HOME:   {xdg_data}")
    print(f"XDG_CONFIG_HOME: {xdg_cfg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
