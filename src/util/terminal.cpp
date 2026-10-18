#include "util/terminal.hpp"

#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace geth::util {

namespace {

struct termios g_saved_termios{};
bool g_have_termios = false;
int g_tty_fd = -1;

// The one sequence the SIGHUP path emits: mouse tracking off (SGR, urxvt,
// any-event, button-event, normal), line wrap back on (FTXUI disables it),
// alternate screen off (restores the original screen), cursor shown — the
// same set FTXUI's own on-exit functions write for the signals it handles.
// Written to /dev/tty so it reaches the terminal even if stdout is
// redirected.
constexpr char kRestoreSeq[] =
    "\x1b[?1006l"  // SGR mouse
    "\x1b[?1005l"  // urxvt mouse
    "\x1b[?1003l"  // any-event mouse
    "\x1b[?1002l"  // button-event mouse
    "\x1b[?1000l"  // normal mouse
    "\x1b[?7h"     // re-enable line wrap
    "\x1b[?1049l"  // leave the alternate screen
    "\x1b[?25h";   // show the cursor

void on_sighup(int sig) {
  // Async-signal-safe by construction: write + tcsetattr (the universal
  // practice for this) + _exit.
  if (g_tty_fd >= 0)
    (void)!write(g_tty_fd, kRestoreSeq, sizeof(kRestoreSeq) - 1);
  if (g_have_termios)
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
  _exit(128 + sig);
}

}  // namespace

void install_sighup_restore() {
  struct termios t{};
  g_have_termios = (tcgetattr(STDIN_FILENO, &t) == 0);
  if (g_have_termios) g_saved_termios = t;
  g_tty_fd = open("/dev/tty", O_WRONLY);
  if (g_tty_fd < 0) g_tty_fd = STDOUT_FILENO;
  struct sigaction sa{};
  sa.sa_handler = on_sighup;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGHUP, &sa, nullptr);
}

void uninstall_sighup_restore() {
  struct sigaction sa{};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGHUP, &sa, nullptr);
  if (g_tty_fd > 0 && g_tty_fd != STDOUT_FILENO) close(g_tty_fd);
  g_tty_fd = -1;
  g_have_termios = false;
}

void ignore_tty_stop_signals() {
  std::signal(SIGTTIN, SIG_IGN);
  std::signal(SIGTTOU, SIG_IGN);
}

}  // namespace geth::util
