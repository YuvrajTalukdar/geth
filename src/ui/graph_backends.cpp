#include "ui/graph_backends.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
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
// Sixel (libsixel dialect: P2=1 transparent, R-run rows, height x6)
// ---------------------------------------------------------------------------

namespace {
std::string hex2(int v) {
  static const char* d = "0123456789abcdef";
  return std::string(1, d[(v >> 4) & 15]) + d[v & 15];
}
std::string sidx(std::size_t i) {
  return i < 10 ? std::string(1, (char)('0' + i)) : std::to_string(i);
}
}

std::string sixel_encode(int w, int h, const std::uint8_t* rgba, const Rgb& bg,
                         const std::vector<Rgb>& palette) {
  if (w < 1 || h < 1) return {};
  const int h6 = ((h + 5) / 6) * 6;

  // Palette: 0 = bg, 1 = the transparent slot (P2=1), then the important
  // colors, then 24 gradient steps of the fill color (the last important
  // one). Dedup from slot 2 on — slots 0/1 are pinned.
  std::vector<Rgb> pal;
  pal.push_back(bg);
  pal.push_back(rgb(0, 0, 0));
  auto add = [&](int r, int g, int b) -> int {
    for (std::size_t i = 0; i < pal.size(); ++i)
      if (pal[i].r == r && pal[i].g == g && pal[i].b == b) return (int)i;
    pal.push_back(rgb((std::uint8_t)r, (std::uint8_t)g, (std::uint8_t)b));
    return (int)pal.size() - 1;
  };
  for (const Rgb& c : palette) add(c.r, c.g, c.b);
  if (!palette.empty()) {
    const Rgb& fc = palette.back();  // the fill color
    for (int s2 = 1; s2 <= 24; ++s2) {
      const double a = 0.35 + (0.05 - 0.35) * (s2 - 1) / 23.0;
      add((int)std::lround(fc.r * a + bg.r * (1.0 - a)),
          (int)std::lround(fc.g * a + bg.g * (1.0 - a)),
          (int)std::lround(fc.b * a + bg.b * (1.0 - a)));
    }
  }
  if (pal.size() > 256) pal.resize(256);

  // Composite over the bg estimate and quantize to the nearest palette
  // entry (an exact-match cache keeps the scan a lookup).
  std::map<std::uint32_t, int> cache;
  auto map_pixel = [&](int r, int g, int b, int a) -> int {
    if (a <= 0) return 1;
    const int cr = (int)std::lround(r * a / 255.0 + bg.r * (255.0 - a) / 255.0);
    const int cg = (int)std::lround(g * a / 255.0 + bg.g * (255.0 - a) / 255.0);
    const int cb = (int)std::lround(b * a / 255.0 + bg.b * (255.0 - a) / 255.0);
    const std::uint32_t key =
        (std::uint32_t)cr << 16 | (std::uint32_t)cg << 8 | (std::uint32_t)cb;
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    // Nearest of the OPAQUE slots: slot 0 (the bg) and slots 2+ (the content
    // colors). Slot 1 is the transparent (alpha-0) slot — an opaque pixel
    // never maps there. Including slot 0 is what keeps an opaque bg pixel on
    // the bg color instead of snapping it to the nearest content color.
    int best = 0, bd = INT_MAX;
    for (std::size_t i = 0; i < pal.size(); ++i) {
      if (i == 1) continue;
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
  auto color_at = [&](int y, int x) -> int {
    if (y >= h) return 1;  // the padded rows are transparent
    const std::uint8_t* px = rgba + ((std::size_t)y * w + x) * 4;
    return map_pixel(px[0], px[1], px[2], px[3]);
  };

  std::string s = "\x1bPqP1=0,P2=1,P4=" + std::to_string(h6) + ",P5=" +
                  std::to_string(w);
  for (std::size_t i = 0; i < pal.size(); ++i)
    s += "S" + sidx(i) + "=" + hex2(pal[i].r) + hex2(pal[i].g) +
         hex2(pal[i].b);
  s += "R";  // attributes end, row data begins
  // The sixel "current color" is the last palette entry just defined (each
  // S<n>= also sets the current color), so the first run is emitted relative
  // to it — it will re-emit S0=/S1= for the usual bg/transparent first pixel.
  int cur = (int)pal.size() - 1;
  for (int y = 0; y < h6; ++y) {
    for (int x = 0; x < w;) {
      const int c = color_at(y, x);
      int x2 = x + 1;
      while (x2 < w && color_at(y, x2) == c) ++x2;
      if (c != cur) {
        s += "S" + sidx(c) + "=";
        cur = c;
      }
      s += std::to_string(x2 - x);
      x = x2;
    }
    s += "R";  // end of row
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
