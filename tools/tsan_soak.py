#!/usr/bin/env python3
"""5-minute ThreadSanitizer pty soak for geth.

Starts geth (`--demo --interval 100`) in a fixed-size pty with GETH_TEST=1 and
fresh temp XDG_CONFIG_HOME / XDG_DATA_HOME (so it never touches the real config
or the real ~/.local/share log dir), then drives a fixed key cycle for the given
duration and exits cleanly with `q`. The point is to keep the timer thread and
the UI thread (sampling + rendering + CSV) in constant motion — cycling the
graph renderer, window, theme, the panel manager, CSV logging, pause and the
sampling interval — so TSan has a long, varied window in which to observe any
cross-thread data race.

stdout (the terminal stream) goes to <out_dir>/soak_screen.txt; stderr (where
TSan writes its reports) goes to <out_dir>/soak_tsan.txt, kept SEPARATE from the
terminal stream so a report is unambiguous.

The key cycle is one key every 300 ms:
  g  t  T  e  ESC  l  space  +  -
ESC is the manager-close (the panel manager swallows every key while open, so
ESC there closes it instead of quitting; ESC only quits when the manager is
closed).

Usage:
  python3 tools/tsan_soak.py <geth> <duration_s> <out_dir> [cols rows]

Exit status: 0 when geth exited cleanly AND no TSan report was produced,
1 otherwise (so tsan_check.sh can fail the build on a report).
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

# The soak's key cycle, one key every KEY_PERIOD seconds (one per 300 ms).
KEYS = [b"g", b"t", b"T", b"e", b"\x1b", b"l", b" ", b"+", b"-"]
KEY_PERIOD = 0.300


def main():
    argv = sys.argv[1:]
    if len(argv) < 3 or argv[0] in ("--help", "-h"):
        print(__doc__)
        return 2
    geth, duration_s, out_dir = argv[0], float(argv[1]), argv[2]
    cols = int(argv[3]) if len(argv) > 3 else 200
    rows = int(argv[4]) if len(argv) > 4 else 50
    os.makedirs(out_dir, exist_ok=True)
    tsan_out = os.path.join(out_dir, "soak_tsan.txt")
    screen_out = os.path.join(out_dir, "soak_screen.txt")

    xdg_cfg = tempfile.mkdtemp(prefix="geth-soak-cfg-")
    xdg_data = tempfile.mkdtemp(prefix="geth-soak-data-")
    env = dict(os.environ)
    env["GETH_TEST"] = "1"
    env["XDG_CONFIG_HOME"] = xdg_cfg
    env["XDG_DATA_HOME"] = xdg_data
    env["TERM"] = "xterm-256color"
    for k in ("TMUX", "TERM_PROGRAM", "COLORTERM"):
        env.pop(k, None)

    xpixel, ypixel = cols * 13, rows * 28
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                struct.pack("HHHH", rows, cols, xpixel, ypixel))

    # stderr -> the TSan file (clean reports), stdout+stdin -> the pty.
    tsan_fd = os.open(tsan_out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    pid = os.fork()
    if pid == 0:  # child
        os.setsid()
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(tsan_fd, 2)
        for fd in (slave, tsan_fd):
            if fd > 2:
                os.close(fd)
        os.close(master)
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        os.execve(geth, [geth, "--demo", "--interval", "100"], env)
        os._exit(127)

    os.close(slave)
    screen_f = open(screen_out, "wb")
    start = time.time()
    end = start + duration_s
    key_i = 0
    next_key = start + KEY_PERIOD
    exit_status = None
    forced_kill = False
    while time.time() < end and exit_status is None:
        r, _, _ = select.select([master], [], [], 0.02)
        if master in r:
            try:
                screen_f.write(os.read(master, 1 << 20))
            except OSError:
                pass  # child closed its end; keep draining the pipe + reaping
        # Send the next key in the cycle, one every KEY_PERIOD seconds.
        while time.time() >= next_key:
            os.write(master, KEYS[key_i % len(KEYS)])
            key_i += 1
            next_key += KEY_PERIOD
            if next_key < start:  # never rewind across a very slow iteration
                next_key = time.time()
        p, status, _ = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = status
    # Soak finished (or geth exited early): ask it to quit, then reap.
    try:
        os.write(master, b"q")
    except OSError:
        pass
    deadline = time.time() + 15
    while exit_status is None and time.time() < deadline:
        r, _, _ = select.select([master], [], [], 0.05)
        if master in r:
            try:
                screen_f.write(os.read(master, 1 << 20))
            except OSError:
                pass
        p, status, _ = os.wait4(pid, os.WNOHANG)
        if p == pid:
            exit_status = status
    if exit_status is None:  # it did not quit: escalate, then give up
        os.kill(pid, signal.SIGTERM)
        for _ in range(100):
            p, status, _ = os.wait4(pid, os.WNOHANG)
            if p == pid:
                exit_status = status
                break
            time.sleep(0.05)
        if exit_status is None:
            os.kill(pid, signal.SIGKILL)
            forced_kill = True
            _, exit_status, _ = os.wait4(pid, 0)
    screen_f.close()
    os.close(master)

    # Drain any residual TSan output flushed on the child's final exit.
    time.sleep(0.2)
    tsan = open(tsan_out, "rb").read()
    clean_exit = os.WIFEXITED(exit_status) and os.WEXITSTATUS(exit_status) == 0
    if forced_kill:
        print("exit: SIGKILL (forced after soak; geth did not quit cleanly)")
    elif clean_exit:
        print("exit: 0 (clean)")
    elif os.WIFSIGNALED(exit_status):
        print(f"exit: signal {os.WTERMSIG(exit_status)}")
    else:
        print(f"exit: {exit_status}")
    print(f"keys sent: {key_i} over {time.time() - start:.1f}s "
          f"(cycle: {' '.join(repr(k) for k in KEYS)})")
    print(f"XDG_DATA_HOME:   {xdg_data}")
    print(f"XDG_CONFIG_HOME: {xdg_cfg}")
    print(f"TSan report:     {tsan_out}")
    has_report = b"WARNING: ThreadSanitizer" in tsan
    print("TSan report:     NONE" if not has_report
          else f"TSan report:     {len(tsan)} bytes (SEE {tsan_out})")
    return 0 if (clean_exit and not has_report) else 1


if __name__ == "__main__":
    sys.exit(main())
