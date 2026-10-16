// Terminal restoration for the one signal FTXUI does not handle: SIGHUP.
//
// FTXUI's ScreenInteractive installs its own handlers for SIGINT, SIGTERM,
// SIGSEGV, SIGILL, SIGABRT and SIGFPE during the loop (RecordSignal); those
// turn into a normal loop exit, and FTXUI's on-exit functions restore the
// cursor, alternate screen, mouse tracking and termios. SIGHUP (closing the
// terminal / login session) is NOT on that list, so it would kill the process
// with the terminal left in raw mode, on the alternate screen, with mouse
// tracking on.
//
// install_sighup_restore() — called right before the UI loop takes over the
// terminal, while the termios is still the user's normal one — saves that
// termios, opens /dev/tty, and installs a SIGHUP handler that writes the
// same restore sequence FTXUI would have written (mouse off, line wrap back
// on, alternate screen off, cursor on), restores the saved termios, and
// exits 128+SIGHUP.
//
// uninstall_sighup_restore() — after a clean UI exit — puts SIGHUP back to
// the default so a later SIGHUP gets the usual behavior.
#pragma once

namespace geth::util {

void install_sighup_restore();
void uninstall_sighup_restore();

}  // namespace geth::util
