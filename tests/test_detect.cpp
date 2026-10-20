// Probe-parsing tests (v1.1.0 Part A): parse_probe_reply recognizes the kitty
// APC answer and the DA1 '4' (sixel) attribute from canned reply bytes,
// kitty_reply_rejects flags a non-OK transfer status, and inside_multiplexer
// detects tmux/screen. The multiplexer flag is informational only — the
// probe still runs in a pane and the multiplexer's own replies decide the
// outcome (tmux 3.8 answers DA1 with the sixel attribute and forwards
// re-encoded sixel to a sixel-capable attached terminal).
#include <doctest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "ui/term_probe.hpp"

using namespace geth::ui;

namespace {

ProbeReply parse(const std::string& s) {
  return parse_probe_reply(reinterpret_cast<const std::uint8_t*>(s.data()),
                           s.size());
}
bool rejects(const std::string& s) {
  return kitty_reply_rejects(reinterpret_cast<const std::uint8_t*>(s.data()),
                             s.size());
}

// A typical terminal DA1 reply.
const std::string kDa1Sixel = "\x1b[?1;2;4;6;7;8;12;c";  // has the '4'
const std::string kDa1NoSixel = "\x1b[?62;c";  // no '4'
// The kitty protocol query's answer (an APC — any answer proves the protocol).
const std::string kKittyReply = "\x1b_G i=31;OK\x1b\\";

// Saves and restores TMUX/TERM around a test case.
struct EnvGuard {
  std::string tmux, term;
  bool had_tmux = false, had_term = false;
  EnvGuard() {
    if (const char* t = std::getenv("TMUX")) {
      had_tmux = (t[0] != '\0');
      tmux = t;
    }
    if (const char* r = std::getenv("TERM")) {
      had_term = (r[0] != '\0');
      term = r;
    }
  }
  ~EnvGuard() {
    if (had_tmux)
      setenv("TMUX", tmux.c_str(), 1);
    else
      unsetenv("TMUX");
    if (had_term)
      setenv("TERM", term.c_str(), 1);
    else
      unsetenv("TERM");
  }
};

}  // namespace

TEST_CASE("detect: a kitty APC answer plus a sixel DA1") {
  const ProbeReply r = parse(kKittyReply + kDa1Sixel);
  CHECK(r.kitty);
  CHECK(r.sixel);
  CHECK(r.da1);
}

TEST_CASE("detect: a sixel-only DA1 (no kitty answer)") {
  const ProbeReply r = parse(kDa1Sixel);
  CHECK(!r.kitty);
  CHECK(r.sixel);
  CHECK(r.da1);
}

TEST_CASE("detect: a DA1 without the sixel attribute") {
  const ProbeReply r = parse(kDa1NoSixel);
  CHECK(!r.kitty);
  CHECK(!r.sixel);
  CHECK(r.da1);
}

TEST_CASE("detect: no reply at all") {
  const ProbeReply r = parse("");
  CHECK(!r.kitty);
  CHECK(!r.sixel);
  CHECK(!r.da1);
}

TEST_CASE("detect: an unrelated reply is not a DA1") {
  // A CSI cursor-position reply (ESC [ r ; c H) is not a DA1 (no '?').
  const ProbeReply r = parse("\x1b[10;20H");
  CHECK(!r.kitty);
  CHECK(!r.sixel);
  CHECK(!r.da1);
}

TEST_CASE("kitty: an OK transfer reply is not a rejection") {
  CHECK(!rejects("\x1b_G i=99;OK\x1b\\"));
}

TEST_CASE("kitty: an error transfer reply is a rejection") {
  CHECK(rejects("\x1b_G i=99;ENOTSUPPORTED:\x1b\\"));
  CHECK(rejects("\x1b_G i=99;EINVALID\x1b\\"));
  CHECK(rejects("\x1b_G i=99;ENOENT\x1b\\"));
}

TEST_CASE("kitty: a bare status separator and no-APC bytes are not rejections") {
  CHECK(!rejects("\x1b_G i=99;\x1b\\"));  // ';' with no status word
  CHECK(!rejects("\x1b[?62;c"));  // no APC at all
  CHECK(!rejects(""));
}

TEST_CASE("detect: inside_multiplexer via TMUX and TERM") {
  EnvGuard guard;
  // Neither set -> not a multiplexer.
  unsetenv("TMUX");
  setenv("TERM", "xterm-256color", 1);
  CHECK(!inside_multiplexer());
  // TMUX set -> multiplexer.
  setenv("TMUX", "/tmp/tmux-1000/default,12345,0", 1);
  CHECK(inside_multiplexer());
  unsetenv("TMUX");
  // TERM starting with "screen" -> multiplexer.
  setenv("TERM", "screen-256color", 1);
  CHECK(inside_multiplexer());
  setenv("TERM", "screen", 1);
  CHECK(inside_multiplexer());
  // A plain terminal -> not a multiplexer.
  setenv("TERM", "linux", 1);
  CHECK(!inside_multiplexer());
}

// Sixel (the tmux path) is EXPERIMENTAL: its image does not persist, so it
// re-emits every frame (no re-send policy, higher iGPU). It is therefore NOT
// the `auto` default: `auto` in a sixel-only multiplexer stays on the
// optimized text renderer and points at the experimental option; an explicit
// `pixels` request selects sixel. Inside tmux the pane's tty is owned by tmux
// (3.8+, built with sixel), which answers DA1 with the '4' attribute and
// forwards re-encoded sixel to a sixel-capable attached terminal.
TEST_CASE("resolve: sixel is experimental — opt-in via explicit pixels") {
  ProbeResult p;
  p.tty = true;
  p.multiplexer = true;  // $TMUX is set
  p.sixel = true;         // tmux's DA1 carried the '4'
  p.cell_w = 7;           // the pane's TIOCGWINSZ pixel size
  p.cell_h = 18;
  // `auto` stays on text (the optimized path), not the experimental sixel,
  // and names the experimental option in its note.
  std::string w;
  CHECK(resolve_protocol("auto", p, &w) == ImageProto::kNone);
  CHECK(w.find("EXPERIMENTAL") != std::string::npos);
  CHECK(w.find("pixels") != std::string::npos);
  // An explicit pixels request selects sixel (no warning).
  w.clear();
  CHECK(resolve_protocol("pixels", p, &w) == ImageProto::kSixel);
  CHECK(w.empty());
  CHECK(resolve_protocol("text", p) == ImageProto::kNone);

  // kitty beats sixel and IS the `auto` default (the optimized path).
  p.kitty = true;
  w.clear();
  CHECK(resolve_protocol("auto", p, &w) == ImageProto::kKitty);
  CHECK(w.empty());
  CHECK(resolve_protocol("pixels", p, &w) == ImageProto::kKitty);
}

// Regression: the multiplexer warning is only reached when NO protocol is
// detected (e.g. plain screen answers DA1 without '4').
TEST_CASE("resolve: no protocol in a multiplexer warns about the multiplexer") {
  ProbeResult p;
  p.tty = true;
  p.multiplexer = true;
  p.cell_w = 7;
  p.cell_h = 18;
  std::string w;
  CHECK(resolve_protocol("pixels", p, &w) == ImageProto::kNone);
  CHECK(w.find("inside the multiplexer") != std::string::npos);
  // Outside a multiplexer the warning names the terminal instead.
  p.multiplexer = false;
  w.clear();
  CHECK(resolve_protocol("pixels", p, &w) == ImageProto::kNone);
  CHECK(w.find("inside the multiplexer") == std::string::npos);
}
