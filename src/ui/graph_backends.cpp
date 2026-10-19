#include "ui/graph_backends.hpp"

#include <bit>
#include <climits>
#include <cmath>
#include <map>

#include <zlib.h>

namespace geth::ui {

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

namespace {
constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string b64_encode(const std::uint8_t* d, std::size_t n) {
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  for (std::size_t i = 0; i < n; i += 3) {
    const int b0 = d[i];
    const int b1 = i + 1 < n ? d[i + 1] : 0;
    const int b2 = i + 2 < n ? d[i + 2] : 0;
    const int t = (b0 << 16) | (b1 << 8) | b2;
    out += kB64[(t >> 18) & 63];
    out += kB64[(t >> 12) & 63];
    out += (i + 1 < n) ? kB64[(t >> 6) & 63] : '=';
    out += (i + 2 < n) ? kB64[t & 63] : '=';
  }
  return out;
}

std::vector<std::uint8_t> b64_decode(const std::string& s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<std::uint8_t> out;
  int buf = 0, bits = 0;
  for (char c : s) {
    if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
    const int v = val(c);
    if (v < 0) return {};
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((std::uint8_t)((buf >> bits) & 0xFF));
    }
  }
  return out;
}

std::string zlib_compress(const std::uint8_t* d, std::size_t n, int level) {
  z_stream zs{};
  if (deflateInit(&zs, level) != Z_OK) return {};
  zs.next_in = const_cast<Bytef*>(d);
  zs.avail_in = static_cast<uInt>(n);
  std::string out;
  out.resize(deflateBound(&zs, static_cast<uLong>(n)));
  zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
  zs.avail_out = static_cast<uInt>(out.size());
  const int r = deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  if (r != Z_STREAM_END) return {};
  return out;
}

std::vector<std::uint8_t> zlib_inflate(const std::uint8_t* d, std::size_t n,
                                       std::size_t expect) {
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) return {};
  zs.next_in = const_cast<Bytef*>(d);
  zs.avail_in = static_cast<uInt>(n);
  std::vector<std::uint8_t> out;
  out.resize(expect > 0 ? expect : n * 4 + 64);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  int r;
  while ((r = inflate(&zs, Z_NO_FLUSH)) == Z_OK) {
    out.resize(out.size() * 2);
    zs.next_out = out.data() + zs.total_out;
    zs.avail_out = static_cast<uInt>(out.size() - zs.total_out);
  }
  inflateEnd(&zs);
  if (r != Z_STREAM_END) return {};
  out.resize(zs.total_out);
  return out;
}

// ---------------------------------------------------------------------------
// Kitty
// ---------------------------------------------------------------------------

std::string kitty_transfer(int id, int w, int h, const std::uint8_t* rgba,
                           std::size_t n) {
  const std::string z = zlib_compress(rgba, n, Z_BEST_SPEED);
  const std::string b = b64_encode(
      reinterpret_cast<const std::uint8_t*>(z.data()), z.size());
  const std::string head = "\x1b_Gi=" + std::to_string(id) + ",p=0,f=32,s=" +
                           std::to_string(w) + ",v=" + std::to_string(h) +
                           ",o=z,q=2,a=T,t=d,C=1";
  const std::size_t kChunk = 4096;  // the protocol's max base64 chunk
  std::string out;
  std::size_t off = 0;
  bool first = true;
  while (off < b.size()) {
    const bool last = off + kChunk >= b.size();
    const std::size_t len = last ? b.size() - off : kChunk;
    // base64 length is always a multiple of 4, and so is the chunk size.
    if (first) {
      out += head;
      // m is a PARAM (before the ';' that ends the param list): single chunk
      // is m=0, a longer transfer's first chunk is m=1.
      out += last ? ",m=0;" : ",m=1;";
      out += b.substr(off, len);
      out += "\x1b\\";
    } else {
      out += "\x1b_G";
      out += last ? "m=0;" : "m=1;";
      out += b.substr(off, len);
      out += "\x1b\\";
    }
    off += len;
    first = false;
  }
  return out;
}

std::string kitty_delete(int id) {
  return "\x1b_Gi=" + std::to_string(id) + ",a=d,d=i\x1b\\";
}

std::string kitty_delete_all() { return "\x1b_Ga=d,d=a\x1b\\"; }

// ---------------------------------------------------------------------------
// Sixel (classic DEC form: the dialect tmux 3.8 parses and re-encodes, and
// the dialect Konsole's processSixel accepts)
// ---------------------------------------------------------------------------

namespace {
// A palette component in the protocol's 0-100 range (rounded).
int p100(int v) { return (v * 100 + 127) / 255; }
}

std::string sixel_encode(int w, int h, const std::uint8_t* rgba, const Rgb& bg,
                         const std::vector<Rgb>& palette) {
  if (w < 1 || h < 1) return {};
  const int h6 = ((h + 5) / 6) * 6;

  // Registers: 0 = the bg, then the important colors from `palette` (the
  // grid first, then the series in draw order, the fill color), then 24
  // gradient steps of the fill color. Each register carries a PRIORITY for
  // the per-column color choice — a sixel column (1 px wide, 6 px tall) is
  // painted with ONE color: 0 = the series/line colors (topmost), 1 = the
  // grid, 2 = the fill gradient steps, 3 = the bg (never chosen).
  //
  // Sixel has no per-pixel alpha: the image's background IS register 0
  // (Konsole's sixel is an opaque indexed image whose index 0 we define as
  // the theme bg; tmux re-emits register 0 as `#0;2;...` too). Unpainted
  // cells decode to index 0 on both ends.
  struct Pal {
    int r, g, b;
    int cls;
  };
  std::vector<Pal> pal;
  pal.push_back({bg.r, bg.g, bg.b, 3});
  auto add = [&](int r, int g, int b, int cls) -> int {
    for (std::size_t i = 0; i < pal.size(); ++i)
      if (pal[i].r == r && pal[i].g == g && pal[i].b == b) return (int)i;
    pal.push_back({r, g, b, cls});
    return (int)pal.size() - 1;
  };
  for (std::size_t i = 0; i < palette.size(); ++i)
    add(palette[i].r, palette[i].g, palette[i].b, i == 0 ? 1 : 0);
  if (!palette.empty()) {
    const Rgb& fc = palette.back();  // the fill color
    for (int s2 = 1; s2 <= 24; ++s2) {
      const double a = 0.35 + (0.05 - 0.35) * (s2 - 1) / 23.0;
      add((int)std::lround(fc.r * a + bg.r * (1.0 - a)),
          (int)std::lround(fc.g * a + bg.g * (1.0 - a)),
          (int)std::lround(fc.b * a + bg.b * (1.0 - a)), 2);
    }
  }
  if (pal.size() > 256) pal.resize(256);

  // Composite over the bg estimate and quantize to the nearest register (an
  // exact-match cache keeps the scan a lookup). Index 0 (the bg) = "no ink".
  std::map<std::uint32_t, int> cache;
  auto map_pixel = [&](int r, int g, int b, int a) -> int {
    if (a <= 0) return 0;
    const int cr = (int)std::lround(r * a / 255.0 + bg.r * (255.0 - a) / 255.0);
    const int cg = (int)std::lround(g * a / 255.0 + bg.g * (255.0 - a) / 255.0);
    const int cb = (int)std::lround(b * a / 255.0 + bg.b * (255.0 - a) / 255.0);
    const std::uint32_t key =
        (std::uint32_t)cr << 16 | (std::uint32_t)cg << 8 | (std::uint32_t)cb;
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    int best = 0, bd = INT_MAX;
    for (std::size_t i = 0; i < pal.size(); ++i) {
      const int dr = cr - pal[i].r, dg = cg - pal[i].g, db = cb - pal[i].b;
      const int dist = dr * dr + dg * dg + db * db;
      if (dist < bd) {
        bd = dist;
        best = (int)i;
      }
    }
    cache.emplace(key, best);
    return best;
  };
  // The quantized grid (rows y >= h, the x6 padding, are bg).
  std::vector<std::int16_t> idx((std::size_t)w * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const std::uint8_t* px = rgba + ((std::size_t)y * w + x) * 4;
      idx[(std::size_t)y * w + x] =
          (std::int16_t)map_pixel(px[0], px[1], px[2], px[3]);
    }
  auto reg_at = [&](int y, int x) -> int {
    return y < h ? (int)idx[(std::size_t)y * w + x] : 0;
  };

  // One column's (register, mask): the highest-priority register it contains
  // (series > grid > fill steps; ties: more rows, then the lower index),
  // with the mask of that register's rows — except when a fill step wins, in
  // which case ALL fill rows share the winning step (the gradient varies a
  // percent or two inside one 6 px band, so merging keeps the fill solid
  // instead of leaving 1 px holes). A column with no ink returns {-1, 0}.
  auto column = [&](int y0, int x) -> std::pair<int, int> {
    int greg[6], gmask[6];
    int ng = 0;
    for (int i = 0; i < 6; ++i) {
      const int r = reg_at(y0 + i, x);
      if (r == 0) continue;
      int gi = -1;
      for (int k = 0; k < ng; ++k)
        if (greg[k] == r) {
          gi = k;
          break;
        }
      if (gi < 0) {
        greg[ng] = r;
        gmask[ng] = 0;
        gi = ng++;
      }
      gmask[gi] |= 1 << i;
    }
    if (ng == 0) return {-1, 0};
    int best = 0, bestcls = pal[greg[0]].cls;
    int bestbits = std::popcount((unsigned)gmask[0]);
    for (int k = 1; k < ng; ++k) {
      const int cls = pal[greg[k]].cls;
      const int bits = std::popcount((unsigned)gmask[k]);
      if (cls < bestcls ||
          (cls == bestcls &&
           (bits > bestbits || (bits == bestbits && greg[k] < greg[best])))) {
        best = k;
        bestcls = cls;
        bestbits = bits;
      }
    }
    int mask = gmask[best];
    if (bestcls == 2) {
      mask = 0;
      for (int k = 0; k < ng; ++k)
        if (pal[greg[k]].cls == 2) mask |= gmask[k];
    }
    return {greg[best], mask};
  };

  // The stream. ESC P 9;1q"1;1;W;H6 ... — NO SPACE between the params and
  // the 'q' final byte: 0x20 is an INTERMEDIATE byte in the ANSI state
  // machine, and both tmux (input_dcs_dispatch: buf[0]=='q' &&
  // interm_len==0) and Konsole (hook: _nIntermediate==0) drop the ENTIRE
  // DCS when any intermediate is present. The 'q' final byte itself is
  // stored (input_input) and must be the first payload byte sixel_parse
  // expects. '9' is the classic first param, '1' is P2 (tmux reads it from
  // the param list, index 1, and re-emits it; Konsole ignores the DCS
  // params entirely). Then the "1;1;W;H6" size — REQUIRED, an image without
  // it is invalid — the register declarations (#c;2;r;g;b, components
  // 0-100), and the data: 6-bit column masks (bytes 0x3f-0x7e), !n
  // repeats, #c register switches, '-' row advances. tmux's parser
  // (image-sixel.c sixel_parse) drops the WHOLE image on any other payload
  // byte — digits, spaces, '=' included — so every byte must be classic
  // grammar.
  std::string s = "\x1bP9;1q\"1;1;" + std::to_string(w) + ";" +
                  std::to_string(h6);
  for (std::size_t i = 0; i < pal.size(); ++i)
    s += "#" + std::to_string(i) + ";2;" + std::to_string(p100(pal[i].r)) +
         ";" + std::to_string(p100(pal[i].g)) + ";" +
         std::to_string(p100(pal[i].b));

  int cur = -1;
  const int kRunMin = 4;  // !n from here on (3 bytes beats 4+ literals)
  for (int y0 = 0; y0 < h6; y0 += 6) {
    int x = 0;
    while (x < w) {
      const auto [color, mask] = column(y0, x);
      int run = 1;
      while (x + run < w) {
        const auto r2 = column(y0, x + run);
        if (r2.first != color || r2.second != mask) break;
        ++run;
      }
      if (color >= 0 && color != cur) {
        s += "#" + std::to_string(color);
        cur = color;
      }
      // ARITHMETIC, not OR: 0x3f already has all six data bits set, so
      // 0x3f|mask is a no-op — the 6-bit value rides in the carry (byte
      // 0x3f+mask spans 0x3f-0x7e; both tmux and Konsole decode with
      // `byte - 0x3f`).
      const char byte = (char)(0x3f + (mask & 0x3f));
      if (run >= kRunMin) {
        // !n repeats the following byte n times (3 bytes beats 4+ literals).
        s += "!" + std::to_string(run);
        s += byte;
      } else {
        // Short runs: the literal byte once PER COLUMN — a run of N identical
        // columns paints N columns, so N < 4 emits N bytes.
        for (int k = 0; k < run; ++k) s += byte;
      }
      x += run;
    }
    if (y0 + 6 < h6) s += "-";
  }
  s += "\x1b\\";
  return s;
}

// ---------------------------------------------------------------------------
// The live set
// ---------------------------------------------------------------------------

namespace {
// Absolute CUP (1-based).
std::string cup(int x, int y) {
  return "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
}
}

int ImageSet::id_of(const std::string& key) const {
  const auto it = ids_.find(key);
  return it == ids_.end() ? -1 : it->second;
}

std::string ImageSet::round(const std::vector<ImagePlace>& places,
                            const ftxui::Screen::Cursor& park) {
  std::string s;
  if (proto == ImageProto::kNone) return s;

  // 1. Deletes first: the kitty ids whose key is no longer placed.
  if (proto == ImageProto::kKitty) {
    for (auto it = ids_.begin(); it != ids_.end();) {
      bool live = false;
      for (const auto& pl : places)
        if (pl.key == it->first) {
          live = true;
          break;
        }
      if (!live) {
        s += kitty_delete(it->second);
        it = ids_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // 2. The live images: CUP to the rect, then the transfer. A new key gets
  //    the next id; a known key KEEPS its id (Konsole replaces id+pid in
  //    place — no flicker).
  for (const auto& pl : places) {
    if (!pl.img.valid()) continue;
    auto it = ids_.find(pl.key);
    if (it == ids_.end()) it = ids_.emplace(pl.key, next_id_++).first;
    s += cup(pl.x, pl.y);
    if (proto == ImageProto::kKitty)
      s += kitty_transfer(it->second, pl.img.w, pl.img.h, pl.img.rgba.data(),
                          pl.img.rgba.size());
    else if (proto == ImageProto::kSixel && bg != nullptr)
      s += sixel_encode(pl.img.w, pl.img.h, pl.img.rgba.data(), *bg,
                        pl.img.palette);
  }

  // 3. Restore FTXUI's cursor park: its next frame preamble is relative from
  //    here (the spike verified the relative math lines up exactly).
  s += cup(park.x, park.y);
  s += (park.shape == ftxui::Screen::Cursor::Hidden) ? "\x1b[?25l"
                                                     : "\x1b[?25h";
  return s;
}

std::string ImageSet::delete_all() const {
  if (proto != ImageProto::kKitty || ids_.empty()) return "";
  std::string s;
  for (const auto& e : ids_) s += kitty_delete(e.second);
  return s;
}

}  // namespace geth::ui
