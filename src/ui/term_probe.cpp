#include "ui/term_probe.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <vector>

namespace geth::ui {

// The kitty graphics protocol query (an a=q image: a 1x1 24-bit pixel,
// base64 "AAAA"), then DA1. A supporting terminal answers the query with an
// APC; only the DA1 answer (with or without the '4' attribute) otherwise.
std::string probe_query() {
  return std::string("\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\") + "\x1b[c";
}

// One black 1x1 pixel, transferred AND drawn (a=T), reply requested
// (q=0). a=T t=d is the diagnostic form — decoded from Konsole's handler
// (src/Vt102Emulation.cpp, processGraphicsToken): Konsole renders a=T
// transfers (Kitty placement, above the text) and replies `i=99;OK`, but
// it ignores a=r transfers entirely and answers ENOTSUPPORTED to t=f, so
// neither of those forms can discriminate. A compliant terminal renders
// the pixel and replies `i=99;OK`; a handler that cannot render it
// replies an error (ENOTSUPPORTED, ENOENT, ...).
std::string kitty_test_transfer() {
  return "\x1b_Gi=99,p=0,f=24,s=1,v=1,a=T,t=d,q=0,C=1;AAAA\x1b\\";
}

bool kitty_reply_rejects(const std::uint8_t* b, std::size_t n) {
  for (std::size_t i = 0; i + 2 < n; ++i) {
    if (b[i] != 0x1b || b[i + 1] != '_' || b[i + 2] != 'G') continue;
    // Inside the APC: look for a `;<STATUS>` field (statuses are
    // letter/digit words, e.g. OK, ENOTSUPPORTED, EINVALID).
    for (std::size_t j = i + 3; j + 1 < n; ++j) {
      if (b[j] != ';') continue;
      std::size_t k = j + 1;
      while (k < n && std::isalnum(b[k])) ++k;
      if (k == j + 1) continue;  // a bare ';' (no status word)
      const bool ok = (k - (j + 1) == 2 && b[j + 1] == 'O' && b[j + 2] == 'K');
      if (!ok) return true;
      j = k;
    }
  }
  return false;
}

ProbeReply parse_probe_reply(const std::uint8_t* b, std::size_t n) {
  ProbeReply r;
  for (std::size_t i = 0; i + 2 < n; ++i) {
    // The APC answer: ESC _ G ... (the terminator is not needed for the
    // decision — any answer to the query proves the protocol exists).
    if (b[i] == 0x1b && b[i + 1] == '_' && b[i + 2] == 'G') {
      r.kitty = true;
      continue;
    }
    // DA1: ESC [ ? <params> c
    if (b[i] == 0x1b && b[i + 1] == '[' && i + 2 < n && b[i + 2] == '?') {
      std::size_t j = i + 3;
      while (j < n && b[j] != 'c') ++j;
      if (j < n) {
        r.da1 = true;
        int param = 0;
        bool in_num = false;
        for (std::size_t p = i + 3; p < j; ++p) {
          if (std::isdigit(b[p])) {
            param = param * 10 + (b[p] - '0');
            in_num = true;
          } else if (in_num) {
            if (param == 4) r.sixel = true;
            param = 0;
            in_num = false;
          }
        }
        if (in_num && param == 4) r.sixel = true;
        i = j;
      }
    }
  }
  return r;
}

bool inside_multiplexer() {
  if (const char* tmux = std::getenv("TMUX"); tmux && tmux[0]) return true;
  if (const char* term = std::getenv("TERM"); term && term[0])
    if (std::strncmp(term, "screen", 6) == 0) return true;
  return false;
}

namespace {

// Read up to `limit_ms` from `fd` into `out` (appended), stopping early when
// `done` reports the reply is complete.
void read_window(int fd, int limit_ms, std::vector<std::uint8_t>& out,
                 const std::function<bool()>& done) {
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (done()) return;
    const auto left_ms = static_cast<long long>(limit_ms) -
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    if (left_ms <= 0) return;
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int pr = poll(&pfd, 1, static_cast<int>(left_ms));
    if (pr <= 0) return;
    uint8_t tmp[256];
    const ssize_t nread = read(fd, tmp, sizeof tmp);
    if (nread <= 0) return;
    out.insert(out.end(), tmp, tmp + nread);
  }
}

// The CSI 16 t fallback: the cell's pixel size (height then width).
bool query_cell_pixels(int fd, int timeout_ms, int& w, int& h) {
  const char q[] = "\x1b[16t";
  if (write(fd, q, sizeof q - 1) != static_cast<ssize_t>(sizeof q - 1)) return false;
  std::vector<std::uint8_t> buf;
  read_window(fd, timeout_ms, buf, [] { return false; });
  for (std::size_t i = 0; i + 3 < buf.size(); ++i) {
    if (buf[i] != 0x1b || buf[i + 1] != '[' || buf[i + 2] != '1' ||
        buf[i + 3] != '6')
      continue;
    if (i + 4 >= buf.size() || buf[i + 4] != ';') continue;
    int vals[2] = {0, 0};
    int vi = 0, num = 0;
    bool in_num = false;
    for (std::size_t p = i + 5; p < buf.size() && vi < 2; ++p) {
      if (std::isdigit(buf[p])) {
        num = num * 10 + (buf[p] - '0');
        in_num = true;
      } else if (buf[p] == ';' || buf[p] == 't') {
        if (in_num && vi < 2) vals[vi++] = num;
        num = 0;
        in_num = false;
        if (buf[p] == 't') break;
      }
    }
    if (vi == 2 && vals[0] > 0 && vals[1] > 0) {
      h = vals[0];
      w = vals[1];
      return true;
    }
  }
  return false;
}

}  // namespace

ProbeResult probe_terminal(int timeout_ms) {
  ProbeResult r;
  r.multiplexer = inside_multiplexer();
  if (r.multiplexer) r.note = "inside tmux/screen";

  const int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
  if (fd < 0) {
    if (r.note.empty()) r.note = "no tty available";
    return r;
  }
  r.tty = true;

  struct termios saved{};
  bool have_termios = tcgetattr(fd, &saved) == 0;
  if (have_termios) {
    struct termios raw = saved;
    cfmakeraw(&raw);
    if (tcsetattr(fd, TCSANOW, &raw) != 0) have_termios = false;
  }

  if (have_termios) {
    // The query runs inside a multiplexer too: the pane's /dev/tty is
    // owned by the multiplexer, and its replies are the honest answer.
    // tmux (3.8+, built with sixel) answers DA1 with the '4' attribute
    // and never answers the kitty query — which resolves to pixels
    // (sixel), and tmux forwards those images to the attached terminal
    // when that terminal itself supports them. screen answers DA1
    // without '4' — text. (The kitty test transfer below is only sent
    // when the query got a kitty reply, so a multiplexer that drops it
    // costs nothing.)
    // Drop anything already queued (a stray paste would confuse the
    // parser) and ask the two questions in one burst.
    (void)tcflush(fd, TCIFLUSH);
    const std::string q = probe_query();
    (void)!write(fd, q.data(), q.size());
    std::vector<std::uint8_t> buf;
    read_window(fd, timeout_ms, buf, [&] {
      const ProbeReply p = parse_probe_reply(buf.data(), buf.size());
      return p.kitty && p.da1;  // both answers in: stop waiting
    });
    const ProbeReply p = parse_probe_reply(buf.data(), buf.size());
    r.kitty = p.kitty;
    r.sixel = p.sixel;
    // An OK to the protocol query is not proof of a renderer: Konsole
    // answers i=31 with OK, renders a=T transfers, but ignores a=r ones
    // and rejects t=f with ENOTSUPPORTED. Verify with the a=T t=d test
    // image — a renderer replies i=99;OK (or stays silent), a handler
    // that cannot render it replies an error and kitty flips off. The
    // window is 400 ms (a 150 ms query window once raced a slow
    // rejection reply) — paid only when the query got a reply at all.
    if (r.kitty) {
      const std::string t = kitty_test_transfer();
      (void)!write(fd, t.data(), t.size());
      std::vector<std::uint8_t> tbuf;
      read_window(fd, std::max(timeout_ms, 400), tbuf, [&] {
        return kitty_reply_rejects(tbuf.data(), tbuf.size());
      });
      if (kitty_reply_rejects(tbuf.data(), tbuf.size())) {
        r.kitty = false;
        r.kitty_rejected = true;
        r.note = r.note.empty()
                     ? "kitty answered the protocol query but rejected the "
                           "test image — treated as unsupported"
                     : r.note +
                           "; kitty answered the protocol query but rejected "
                           "the test image — treated as unsupported";
      }
    }
    // Swallow whatever the terminal still owes us (a late DA1 must not
    // reach the UI's input parser). When kitty is on and no rejection
    // reply was consumed, a late reply may still be in flight (observed
    // up to ~350 ms) — drain for as long as the transfer window did.
    if (have_termios) (void)tcsetattr(fd, TCSANOW, &saved);
    std::vector<std::uint8_t> drain;
    read_window(fd, r.kitty ? std::max(timeout_ms, 400) : 30, drain,
                [] { return false; });
  } else {
    if (have_termios) (void)tcsetattr(fd, TCSANOW, &saved);
  }

  // The cell pixel size: TIOCGWINSZ first (the common case), CSI 16 t as the
  // fallback for terminals that do not fill in ws_xpixel/ws_ypixel.
  winsize ws{};
  if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel > 0 &&
      ws.ws_ypixel > 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    r.cell_w = ws.ws_xpixel / ws.ws_col;
    r.cell_h = ws.ws_ypixel / ws.ws_row;
  }
  if ((r.cell_w <= 0 || r.cell_h <= 0) && have_termios) {
    struct termios raw = saved;
    cfmakeraw(&raw);
    if (tcsetattr(fd, TCSANOW, &raw) == 0) {
      (void)query_cell_pixels(fd, timeout_ms, r.cell_w, r.cell_h);
      (void)tcsetattr(fd, TCSANOW, &saved);
    }
  }
  if (r.cell_w <= 0 || r.cell_h <= 0) {
    r.note = r.note.empty()
                 ? "cell pixel size unknown (TIOCGWINSZ and CSI 16 t gave nothing)"
                 : r.note + "; " +
                       "cell pixel size unknown (TIOCGWINSZ and CSI 16 t gave nothing)";
  }
  close(fd);
  return r;
}

ImageProto resolve_protocol(const std::string& intent, const ProbeResult& p,
                            std::string* warning) {
  // "text" and "steps" are always text-rendered (the step-line chart uses
  // box-drawing glyphs, not an image protocol), so they resolve to kNone.
  if (intent == "text" || intent == "steps") return ImageProto::kNone;
  // Only "pixels" wants an IMAGE renderer (kitty > sixel). `explicit_image`
  // gates the sixel opt-in.
  const bool explicit_image = (intent == "pixels");
  const bool sized = p.cell_w > 0 && p.cell_h > 0;
  // Kitty is the OPTIMIZED, fully-supported renderer: the placed image
  // persists in the terminal, so the re-send policy (the iGPU optimization)
  // applies. `auto` selects it whenever it is available.
  if (sized && p.kitty) return ImageProto::kKitty;
  // Sixel is the EXPERIMENTAL renderer (the tmux+Konsole path): its image does
  // not persist (tmux attaches it to the cells it covers and FTXUI's rewrite
  // detaches it), so it re-emits EVERY frame — no re-send policy, higher iGPU.
  // It is therefore NOT selected under `auto`; it is used only when pixels are
  // EXPLICITLY requested (graph_renderer="pixels" / --renderer pixels) and
  // kitty is unavailable — exactly the tmux+Konsole case.
  if (sized && p.sixel) {
    if (explicit_image) return ImageProto::kSixel;
    // `auto` in a sixel-only multiplexer: stay on the optimized text
    // renderer, and point at the experimental option.
    if (warning)
      *warning = "sixel is EXPERIMENTAL and not the default — set "
                 "graph_renderer=\"pixels\" to use it (it re-emits every "
                 "frame, higher GPU load; kitty/text is the optimized path)";
    return ImageProto::kNone;
  }
  if (warning && explicit_image) {
    if (p.multiplexer)
      *warning = "an image renderer was requested, but no image protocol was "
                 "detected inside the multiplexer — using text";
    else if (!sized)
      *warning = "an image renderer was requested, but the cell pixel size is "
                 "unknown — using text";
    else
      *warning = "an image renderer was requested, but this terminal has no "
                 "image protocol (kitty/sixel) — using text";
  }
  return ImageProto::kNone;
}

std::string renderer_name(ImageProto p) {
  switch (p) {
    case ImageProto::kKitty: return "pixels (kitty)";
    case ImageProto::kSixel: return "pixels (sixel)";
    default: return "text";
  }
}

std::string probe_report_text(const ProbeResult& r, const std::string& intent,
                              bool cli_override) {
  std::string s;
  s += "terminal probe:\n";
  s += "  tty:                ";
  s += r.tty ? "yes" : "no (no tty available)";
  s += "\n";
  s += "  inside multiplexer: ";
  s += r.multiplexer ? "yes (tmux/screen)" : "no";
  s += "\n";
  s += "  kitty protocol:     ";
  s += r.tty ? (r.kitty ? "yes" : "no") : "not probed";
  s += "\n";
  s += "  sixel:              ";
  s += r.tty ? (r.sixel ? "yes" : "no") : "not probed";
  s += "\n";
  s += "  cell pixel size:    ";
  s += (r.cell_w > 0 && r.cell_h > 0)
           ? (std::to_string(r.cell_w) + "x" + std::to_string(r.cell_h) + " px")
           : "unknown";
  s += "\n";
  const ImageProto res = resolve_protocol(intent, r);
  s += "  renderer:           ";
  s += renderer_name(res);
  s += "\n";
  s += "  (graph_renderer = \"" + intent + "\"";
  s += cli_override ? ", --renderer override" : " (from the config)";
  s += ")\n";
  if (!r.note.empty()) {
    s += "  note:               ";
    s += r.note;
    s += "\n";
  }
  return s;
}

}  // namespace geth::ui
