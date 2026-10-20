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

#include <chrono>
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

// -- The re-send policy -------------------------------------------------------
//
// A panel's raster at tick N+1 is its raster at tick N slid by one sample.
// While the data is quiet (the machine is idle), that slide is invisible:
// the chart looks pixel-identical. Re-uploading it anyway keeps the
// terminal (and the compositor) redrawing megapixel images every tick —
// measured 47-80% sustained iGPU on a Ryzen 7 260 APU while an idle geth
// sat in pixels, 2-8% in text. The skip must fire on QUIET data, and that
// rules out comparing rasters: a line chart carries almost no ink, so even
// the ±1-3 %-of-scale jitter real sensors produce every tick displaces a
// large share of the only pixels present — and the L1 distance of a pure
// translation is independent of the distance moved, so no raster-space
// threshold separates jitter from a real move.
//
// The gate is on the DATA instead, where the change is directly measurable.
// Between two ticks the window slides one sample, so the line at each column
// moves by that column's adjacent-sample delta (|s_k - s_{k-1}|), in units of
// 1/vmax. The metric is the FRACTION of the panel's columns whose line moved
// more than kColMoveEps — computed by the rasterizer's caller (frame.cpp) and
// carried on the ImagePlace. A quiet window moves ~0 columns (skipped, up to
// kMaxLagMs); a real change moves most columns (sent on the next tick).
//
// Why the fraction, not the max: a line chart is a thin stroke on a mostly
// empty background, and a spike that is merely SCROLLING (the chart sliding
// by one sample) keeps the *max* column delta large for the whole ~60 s the
// spike sits in the window — a max-delta gate would re-upload the entire
// image just to shift that spike one column left. But the scroll only moves
// the columns around the spike (a few percent); a real change (a broad move,
// a jump, a new sustained level) moves most columns. The fraction separates
// the two: a few % -> skip, most % -> send.
//
// A panel re-sends when ANY of: first placement, the rect moved or
// resized, vmax changed (the sticky auto-scale rescaled), the in-window
// gap count changed, more than kChangeFrac of its columns moved, or kMaxLagMs
// passed since the last send. THIS QUIET-SKIP IS KITTY-ONLY: a kitty image
// persists in the terminal's separate image buffer (same id, p=0, replaced in
// place), so a skipped tick still shows the last real picture. SIXEL RE-EMITS
// EVERY FRAME — a sixel image (the tmux+Konsole path) does NOT persist: tmux
// attaches it to the screen cells it covers and FTXUI's full-screen rewrite
// (graph cells are plain spaces in pixels mode) detaches it on the next frame,
// so a skipped sixel panel would vanish from the screen. A theme switch is
// invisible to the gate (colors only); the live loop force-resets the set when
// it happens.

// A column counts as "moved" when the line at that column changed by more
// than this fraction of the panel's height this tick (a smooth slide moves
// the line sub-pixel per column, far below this; a real local move does not).
constexpr double kColMoveEps = 0.03;
// Send when more than this fraction of the panel's columns moved. A scrolling
// spike moves only the columns around it (~5 %); a real broad change moves
// most of them.
constexpr double kChangeFrac = 0.25;
// Hard cap: every panel is re-sent at least this often, so slow drifts and
// sub-eps jitter cannot accumulate unbounded staleness.
constexpr int kMaxLagMs = 10000;

// -- The live set -----------------------------------------------------------

// One panel graph to place this round: `key` (the panel id) addresses the
// stable kitty id, (x,y) the graph rect's top-left in cells. `vmax`,
// `change` and `nan_count` feed the re-send policy (above).
struct ImagePlace {
  std::string key;
  int x = 0, y = 0;
  RasterImage img;
  // This tick's y scale (a change forces a re-send — the sticky auto-scale
  // rescaled).
  double vmax = 0;
  // The fraction (0..1) of the decimated columns whose line moved more than
  // kColMoveEps this tick (all series) — how much of the chart moved.
  double change = 0;
  // In-window gap (NaN) count; a gap opening or closing changes the shape.
  int nan_count = 0;
};

// The placed-image bookkeeping. round() builds the byte stream of one tick
// (deletes + CUP/transfers + the cursor-park restore); the stream is written
// to stdout from the UI thread, byte-ordered after FTXUI's frame. The
// re-send policy (kChangeFrac / kMaxLagMs) skips quiet panels — KITTY ONLY,
// where the placed image persists in the terminal's buffer. SIXEL re-emits
// every panel every frame (its image does not persist — FTXUI's rewrite
// detaches it), so that path is byte-identical to the pre-optimization
// behavior.
class ImageSet {
 public:
  ImageProto proto = ImageProto::kNone;
  // The compositing color for sixel (the active theme's bg, set each round).
  const Rgb* bg = nullptr;

  int active() const { return static_cast<int>(ids_.size()); }
  int id_of(const std::string& key) const;  // -1 when absent

  // The stream of one round: deletes of the ids whose key is gone (kitty),
  // then for every place that re-sends (policy above) a CUP to its rect and
  // the encoded image (a NEW key gets the next id, a known key keeps its
  // id = in-place replace), and finally the restore of FTXUI's cursor park
  // `park` (0-based x/y/shape). `now` drives the kMaxLagMs cap.
  std::string round(const std::vector<ImagePlace>& places,
                    const ftxui::Screen::Cursor& park,
                    std::chrono::steady_clock::time_point now);
  // Every live id in one burst (normal exit; the SIGHUP/SIGTERM handlers
  // write a snapshot of this ahead of the terminal restore).
  std::string delete_all() const;
  // A protocol/renderer/theme change: drop all bookkeeping (the caller
  // emits delete_all() first when images were on screen).
  void reset() {
    ids_.clear();
    next_id_ = 1;
  }

 private:
  struct Entry {
    int id = 0;
    int x = 0, y = 0;    // last-sent position (a move always re-sends)
    int w = 0, h = 0;    // last-sent raster size (a resize re-sends)
    double vmax = 0;     // last-sent y scale (a rescale re-sends)
    int nan_count = 0;   // last-sent gap count (a gap appearing re-sends)
    std::chrono::steady_clock::time_point last_send{};
  };
  std::map<std::string, Entry> ids_;  // key -> the last-sent state
  int next_id_ = 1;
};

}  // namespace geth::ui
