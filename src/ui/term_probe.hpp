// Terminal image-protocol detection (v1.1.0).
//
// The pixel renderer needs a terminal that speaks the Kitty graphics
// protocol or Sixel. At startup the app asks the terminal a few questions
// and waits at most 150 ms for each answer:
//
//   1. A Kitty graphics PROTOCOL query (an a=q image directive, an APC):
//      a terminal that implements the protocol answers with its own APC
//      (OK or an error message — either proves the protocol exists); one
//      that does not ignores the sequence.
//   2. If the query got an OK, a real 1x1 f=24 (RGB) image TRANSFER with
//      a=T (transfer+draw) and q=0 (reply requested): a compliant
//      terminal renders the pixel and replies `i=99;OK`. Konsole is the
//      warning case (decoded from its source): it renders a=T transfers
//      but ignores a=r ones entirely and answers ENOTSUPPORTED to t=f —
//      so a=T t=d q=0 is the only diagnostic form, and a reply whose
//      status is not OK means "no usable renderer" (kitty off).
//   3. DA1 (ESC [ c): the device-attributes reply; the '4' parameter marks
//      Sixel support.
//
// The probe runs inside a multiplexer too: the pane's /dev/tty is owned by
// the multiplexer, and its replies are the honest answer. tmux (3.8+, built
// with sixel) answers DA1 with the '4' attribute and never answers the
// kitty query — which resolves to pixels (sixel), and tmux forwards those
// images when the attached terminal itself supports them. screen answers
// DA1 without '4' — text. No reply within the timeout is also "text". The
// cell's pixel size (needed to size the images) comes from TIOCGWINSZ
// (ws_xpixel/ws_ypixel — tmux fills these in for the pane) with the
// CSI 16 t query as a fallback; when it cannot be determined the text
// renderer is used too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace geth::ui {

// What the terminal can do with terminal images.
enum class ImageProto { kNone, kKitty, kSixel };

// One pass over a captured reply stream (unit-tested with recorded bytes).
struct ProbeReply {
  bool kitty = false;  // an ESC_G ... ESC\ answer was seen
  bool sixel = false;  // the DA1 answer carried the '4' attribute
  bool da1 = false;    // a DA1 answer was seen at all
};

// The query burst (the kitty protocol query, then DA1), ready to write.
std::string probe_query();

// Scans `b` for the APC answer and the DA1 reply. Pure and allocation-free.
ProbeReply parse_probe_reply(const std::uint8_t* b, std::size_t n);

// The kitty test transfer: one 1x1 black f=24 (RGB) pixel, a=T (draw),
// q=0 (reply requested), C=1, p=0. A compliant terminal renders it (one
// dark pixel) and replies `ESC_G i=99;OK ESC\`. A protocol handler that
// cannot render a=T replies an error instead (e.g. Konsole answers
// `i=99;ENOTSUPPORTED:` to t=f; it ignores a=r transfers altogether —
// neither of those forms can discriminate, which is why the test uses
// a=T t=d).
std::string kitty_test_transfer();

// True when `b` holds a kitty reply that rejects a transfer: an ESC_G APC
// whose status is anything but OK (ENOTSUPPORTED, ENOENT, EINVALID, ...).
// A successful transfer is either silent (q=1) or answered `i=N;OK`
// (q=0) — neither is a rejection; any other status is. Pure.
bool kitty_reply_rejects(const std::uint8_t* b, std::size_t n);

// True when running inside a multiplexer: $TMUX is set, or TERM starts
// with "screen". Informational — the probe still runs; the multiplexer's
// own replies decide the outcome (see the file header).
bool inside_multiplexer();

struct ProbeResult {
  bool tty = false;        // a tty was available to probe
  bool multiplexer = false;  // inside tmux/screen (the probe still runs)
  bool kitty = false;       // the raw detections (kitty: query OK AND the
                            // test transfer was not rejected)
  bool kitty_rejected = false;  // the query said OK, the transfer errored
  bool sixel = false;
  int cell_w = 0, cell_h = 0;  // the cell's pixel size (0 = unknown)
  std::string note;  // a human-readable detail ("" when nothing to say)
};

// The full startup probe: multiplexer check, /dev/tty open, raw-mode query,
// the 150 ms reply window, the cell pixel size (TIOCGWINSZ, CSI 16 t
// fallback). Never throws; a missing tty is reported, not fatal.
ProbeResult probe_terminal(int timeout_ms = 150);

// Resolves the [general].graph_renderer intent against the probe:
// "auto"   -> pixels when a protocol AND the cell size are known, else text
// "pixels" -> pixels when available, else text (warning says why)
// "text"   -> text
// Returns the protocol to use (kNone = the text renderer).
ImageProto resolve_protocol(const std::string& intent, const ProbeResult& p,
                            std::string* warning = nullptr);

// "text", "pixels (kitty)" or "pixels (sixel)".
std::string renderer_name(ImageProto p);

// The --probe-terminal report: the protocols, the cell size, and the
// renderer that will be chosen.
std::string probe_report_text(const ProbeResult& r, const std::string& intent,
                              bool cli_override);

}  // namespace geth::ui
