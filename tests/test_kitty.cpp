// Kitty graphics-protocol tests (v1.1.0 Part A): the APC encoder produces the
// exact parameter set Konsole's handler expects, chunks the base64 payload
// correctly (<= 4096 bytes, m=1/m=0, multiple-of-4), and the chunks reassemble
// and inflate back to the original RGBA. The live ImageSet keeps one stable id
// per panel key (in-place replace) and deletes the ids whose key disappeared.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui/graph_backends.hpp"
#include "ui/graph_image.hpp"
#include "ui/term_probe.hpp"

#include "ftxui/screen/screen.hpp"

using namespace geth::ui;

namespace {

constexpr char kEsc = 0x1b;

// Builds a RasterImage (field-by-field, so the empty palette is initialized
// cleanly under -Wmissing-field-initializers).
RasterImage raster(const std::vector<std::uint8_t>& rgba, int w, int h) {
  RasterImage ri;
  ri.rgba = rgba;
  ri.w = w;
  ri.h = h;
  return ri;
}

// A pseudo-random RGBA buffer (incompressible, so zlib keeps it large enough
// to force multi-chunk transfers).
std::vector<std::uint8_t> make_rgba(int w, int h) {
  std::vector<std::uint8_t> b((std::size_t)w * h * 4);
  std::uint32_t seed = 0x1234567u;
  for (auto& x : b) {
    seed = seed * 1103515245u + 12345u;
    x = (std::uint8_t)((seed >> 16) & 0xFF);
  }
  return b;
}

// Splits a transfer stream into its per-chunk base64 payloads (the text after
// the ';' in each ESC_G ... ESC\ block), in order.
std::vector<std::string> chunk_b64s(const std::string& s) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t start = s.find(std::string(1, kEsc) + "_G", pos);
    if (start == std::string::npos) break;
    const std::size_t end = s.find(std::string(1, kEsc) + "\\", start);
    if (end == std::string::npos) break;
    const std::string content =
        s.substr(start + 3, end - (start + 3));
    const std::size_t semi = content.find(';');
    if (semi != std::string::npos)
      out.push_back(content.substr(semi + 1));
    pos = end + 2;
  }
  return out;
}

// The number of ESC_G ... ESC\ blocks in the stream.
int chunk_count(const std::string& s) {
  int n = 0;
  std::size_t pos = 0;
  while ((pos = s.find(std::string(1, kEsc) + "_G", pos)) != std::string::npos)
    ++n, pos += 3;
  return n;
}

// Round-trips a transfer: reassemble the chunks, base64-decode, zlib-inflate,
// and compare to the original RGBA.
void check_roundtrip(const std::string& stream,
                     const std::vector<std::uint8_t>& original) {
  std::string b64;
  for (const auto& c : chunk_b64s(stream)) b64 += c;
  const auto z = b64_decode(b64);
  const auto inflated = zlib_inflate(z.data(), z.size(), original.size());
  REQUIRE_EQ(inflated.size(), original.size());
  CHECK(inflated == original);
}

ftxui::Screen::Cursor park() {
  return ftxui::Screen::Cursor{5, 7, ftxui::Screen::Cursor::Hidden};
}

}  // namespace

TEST_CASE("kitty: a single-chunk transfer carries the exact param set") {
  const std::vector<std::uint8_t> rgba = make_rgba(2, 2);  // 16 bytes, tiny
  const std::string s =
      kitty_transfer(42, 2, 2, rgba.data(), rgba.size());

  // One block, and the full param list with m=0 (the only / last chunk).
  CHECK_EQ(chunk_count(s), 1);
  const std::string head =
      std::string(1, kEsc) + "_Gi=42,p=0,f=32,s=2,v=2,o=z,q=2,a=T,t=d,C=1,m=0;";
  CHECK(s.rfind(head, 0) == 0);  // starts with the head
  CHECK(s.size() > 2);
  CHECK(s.compare(s.size() - 2, 2, std::string(1, kEsc) + "\\") == 0);  // terminator
  // The head is all params; the base64 payload follows the ';'.
  check_roundtrip(s, rgba);
}

TEST_CASE("kitty: a large transfer chunks at 4096 with m=1 then m=0") {
  const std::vector<std::uint8_t> rgba = make_rgba(200, 150);  // 120 KB
  const std::string s = kitty_transfer(7, 200, 150, rgba.data(), rgba.size());

  const auto b64s = chunk_b64s(s);
  CHECK(b64s.size() >= 2);  // definitely multi-chunk
  for (std::size_t i = 0; i < b64s.size(); ++i) {
    CHECK_LE(b64s[i].size(), 4096);
    CHECK_EQ(b64s[i].size() % 4, 0u);  // base64 is a multiple of 4
  }
  // The first chunk carries the full params plus m=1 (more to follow); every
  // chunk but the last is m=1, the last is m=0.
  CHECK(s.find("f=32") != std::string::npos);
  CHECK(s.find(",m=1;") != std::string::npos);
  CHECK(s.find("m=0;") != std::string::npos);
  // Exactly one block carries the full param head (the first).
  int heads = 0;
  std::size_t pos = 0;
  while ((pos = s.find("f=32", pos)) != std::string::npos) ++heads, pos += 4;
  CHECK_EQ(heads, 1);
  check_roundtrip(s, rgba);
}

TEST_CASE("kitty: delete commands use a=d, d=i, i=<id>") {
  const std::string d = kitty_delete(9);
  CHECK(d == std::string(1, kEsc) + "_Gi=9,a=d,d=i" + std::string(1, kEsc) + "\\");
  const std::string all = kitty_delete_all();
  CHECK(all == std::string(1, kEsc) + "_Ga=d,d=a" + std::string(1, kEsc) + "\\");
}

TEST_CASE("kitty ImageSet: stable id per key, delete on disappearance") {
  const auto imgA = make_rgba(4, 4);
  const auto imgB = make_rgba(4, 4);

  ImageSet set;
  set.proto = ImageProto::kKitty;

  ImagePlace pA;
  pA.key = "cpu";
  pA.x = 10;
  pA.y = 5;
  pA.img = raster(imgA, 4, 4);
  ImagePlace pB;
  pB.key = "mem";
  pB.x = 12;
  pB.y = 9;
  pB.img = raster(imgB, 4, 4);

  // Round 1: two panels -> ids 1 (cpu) and 2 (mem).
  const std::string s1 = set.round({pA, pB}, park());
  CHECK_EQ(set.id_of("cpu"), 1);
  CHECK_EQ(set.id_of("mem"), 2);
  CHECK(s1.find("i=1,p=0,f=32") != std::string::npos);
  CHECK(s1.find("i=2,p=0,f=32") != std::string::npos);

  // Round 2: only cpu remains -> mem's id (2) is deleted, cpu KEEPS id 1
  // (in-place replace, no new id, no flicker).
  const std::string s2 = set.round({pA}, park());
  CHECK_EQ(set.id_of("cpu"), 1);
  CHECK_EQ(set.id_of("mem"), -1);
  CHECK(s2.find("i=2,a=d,d=i") != std::string::npos);  // delete mem
  CHECK(s2.find("i=1,p=0,f=32") != std::string::npos);  // replace cpu in place
  CHECK(s2.find("i=3,p=0") == std::string::npos);  // no new id was minted
  CHECK_EQ(set.active(), 1);
}

TEST_CASE("kitty ImageSet: delete_all covers every live id") {
  const auto img = make_rgba(3, 3);
  ImageSet set;
  set.proto = ImageProto::kKitty;
  ImagePlace pa;
  pa.key = "cpu";
  pa.img = raster(img, 3, 3);
  ImagePlace pb;
  pb.key = "mem";
  pb.img = raster(img, 3, 3);
  set.round({pa, pb}, park());
  const std::string del = set.delete_all();
  CHECK(del.find("i=1,a=d,d=i") != std::string::npos);
  CHECK(del.find("i=2,a=d,d=i") != std::string::npos);
  // A none-protocol set has nothing to delete.
  ImageSet empty;
  empty.proto = ImageProto::kNone;
  CHECK(empty.delete_all().empty());
}
