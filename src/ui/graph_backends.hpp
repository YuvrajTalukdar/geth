// The terminal image encoders and the live image set (v1.1.0, Part A).
//
// Kitty graphics protocol (the primary backend, Konsole-verified): an APC
// `ESC_G ... ESC\` transferring the panel's RGBA8 image, zlib-compressed and
// base64-chunked. Konsole's handler (processGraphicsToken, audited and live
// tested): it renders a=T transfers pixel-exact at the cursor cell (above
// the text), ignores a=r entirely and rejects t=f — so the transfer uses
// a=T, t=d. Same id + explicit p=0 replaces the image in place (no
// flicker); q=2 asks for no reply (verified silent on Konsole); C=1 keeps
// the cursor where the CUP put it.
//
// Sixel (the fallback): the CLASSIC DEC form — `ESC P 9;1q` (no space
// before the 'q' final byte: 0x20 is an intermediate byte and tmux/Konsole
// drop the whole DCS when any intermediate is present), the "1;1;W;H6"
// size, `#c;2;r;g;b` register declarations (0-100), 6-bit column masks with
// `!n` repeats, `-` row advances, `ESC \`. That is the dialect
// tmux 3.8 parses (image-sixel.c) and re-encodes to the attached terminal
// when it advertised sixel in DA1, and the dialect Konsole's own
// processSixel accepts (it anchors the image at the cursor cell and defines
// index 0 as the background — opaque, since Konsole's sixel is indexed-8
// with no alpha). Semi-transparent pixels are composited over the theme's
// bg estimate (register 0); each 1x6 column is painted with one color
// (series > grid > fill priority). Height is padded to a multiple of 6,
// re-emitted after every frame.
//
// ImageSet drives the live loop: one stable kitty id per panel (keyed by
// panel id), round() emits the full byte stream for a tick — deletes of the
// images whose panel disappeared, the CUP + transfer of each live one, and
// the restore of FTXUI's cursor park at the end (its next frame preamble is
// relative from there).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ftxui/screen/screen.hpp"  // Screen::Cursor (the park)

#include "ui/graph_image.hpp"
#include "ui/term_probe.hpp"  // ImageProto
#include "ui/theme.hpp"

namespace geth::ui {

// -- Encoding helpers (unit-tested, also used by the --render-png path) -----

// Standard base64 (with '=' padding).
std::string b64_encode(const std::uint8_t* d, std::size_t n);
// The inverse (ignores whitespace); empty on a malformed tail.
std::vector<std::uint8_t> b64_decode(const std::string& s);

// zlib (the same format PNG uses; level per the zlib API).
std::string zlib_compress(const std::uint8_t* d, std::size_t n, int level);
// Inflates; empty on a corrupt stream. `expect` (0 = unknown) sanity-checks
// the declared size.
std::vector<std::uint8_t> zlib_inflate(const std::uint8_t* d, std::size_t n,
                                       std::size_t expect);

// -- Kitty ------------------------------------------------------------------

// One transfer, chunked when the base64 payload exceeds 4096 bytes: the
// first chunk carries the full parameters (i=<id>, p=0, f=32 = RGBA32,
// o=z = zlib, q=2 = no reply, a=T = transfer+draw, t=d = don't clear,
// C=1 = keep the cursor), m=1 chunks after it, m=0 on the last.
std::string kitty_transfer(int id, int w, int h, const std::uint8_t* rgba,
                           std::size_t n);
// Delete one image by id: `a=d, d=i, i=<id>`.
std::string kitty_delete(int id);
// Delete every image (the belt over the per-id deletes).
std::string kitty_delete_all();

// -- Sixel ------------------------------------------------------------------

// Encodes one image in the classic DEC sixel form (see the file header),
// height padded to a multiple of 6. `bg` is the compositing color for
// semi-transparent pixels AND register 0 (the image's opaque background —
// the terminal's real background is unknown; the theme's bg is the best
// estimate). Registers: 0 = bg, then the important colors from `palette`
// (the rasterizer's list: grid, series in draw order, fill), then 24
// gradient steps of the fill color; everything else quantizes to the
// nearest register. A 1x6 column takes the highest-priority register it
// contains (series > grid > fill steps); fill-step columns merge their
// steps so the fill stays solid.
std::string sixel_encode(int w, int h, const std::uint8_t* rgba, const Rgb& bg,
                         const std::vector<Rgb>& palette);

// -- The live set -----------------------------------------------------------

// One panel graph to place this round: `key` (the panel id) addresses the
// stable kitty id, (x,y) the graph rect's top-left in cells.
struct ImagePlace {
  std::string key;
  int x = 0, y = 0;
  RasterImage img;
};

// The placed-image bookkeeping. round() builds the byte stream of one tick
// (deletes + CUP/transfers + the cursor-park restore); the stream is written
// to stdout from the UI thread, byte-ordered after FTXUI's frame.
class ImageSet {
 public:
  ImageProto proto = ImageProto::kNone;
  // The compositing color for sixel (the active theme's bg, set each round).
  const Rgb* bg = nullptr;

  int active() const { return static_cast<int>(ids_.size()); }
  int id_of(const std::string& key) const;  // -1 when absent

  // The stream of one round: deletes of the ids whose key is gone (kitty),
  // then for every place a CUP to its rect and the encoded image (a NEW key
  // gets the next id, a known key keeps its id = in-place replace), and
  // finally the restore of FTXUI's cursor park `park` (0-based x/y/shape).
  std::string round(const std::vector<ImagePlace>& places,
                    const ftxui::Screen::Cursor& park);
  // Every live id in one burst (normal exit; the SIGHUP/SIGTERM handlers
  // write a snapshot of this ahead of the terminal restore).
  std::string delete_all() const;
  // A protocol/renderer change: drop all bookkeeping (the caller emits
  // delete_all() first when images were on screen).
  void reset() {
    ids_.clear();
    next_id_ = 1;
  }

 private:
  std::map<std::string, int> ids_;  // key -> kitty id
  int next_id_ = 1;
};

}  // namespace geth::ui
