// Unit tests for the Phase 3 responsive layout engine (ui/layout.{hpp,cpp}).
//
// The engine is a pure function, so these tests drive it directly with two
// representative pages and a spread of terminal sizes, asserting the invariants
// the renderer relies on: every panel gets a non-overlapping rect inside the
// page, each panel meets the minimum size for its mode, the column count never
// exceeds the minimum-width bound, scrolling is active exactly when the page is
// taller than the content area, and the result is deterministic.

#include <doctest.h>

#include <algorithm>
#include <vector>

#include "ui/layout.hpp"
#include "ui/panel_spec.hpp"

using namespace geth::ui;

namespace {

const double GiB = 1024.0 * 1024.0 * 1024.0;
const double MiB = 1024.0 * 1024.0;

// The 10-panel default page for THIS machine's shape: one dGPU + one iGPU.
std::vector<PanelSpec> make_page_10() {
  Hardware hw;
  hw.cpu_name = "AMD Ryzen 5 5600H with Radeon Graphics";
  hw.ram_total_bytes = 32.0 * GiB;
  GpuInfo d;
  d.bus_id = "0000:01:00.0";
  d.name = "NVIDIA GeForce RTX 3050 Laptop GPU";
  d.vendor = geth::GpuVendor::kNvidia;
  d.is_integrated = false;
  d.vram_total_bytes = 4.0 * GiB;
  d.reports_power = true;
  GpuInfo a;
  a.bus_id = "0000:07:00.0";
  a.name = "Cezanne [Radeon Vega Series / Radeon Vega Mobile Series]";
  a.vendor = geth::GpuVendor::kAmd;
  a.is_integrated = true;
  a.vram_total_bytes = 512.0 * MiB;
  a.reports_power = false;
  hw.gpus = {d, a};
  return build_default_page(hw);
}

// The 8-panel default page: one dGPU, no iGPU (6 fixed panels + one usage
// and one VRAM panel per GPU).
std::vector<PanelSpec> make_page_8() {
  Hardware hw;
  hw.cpu_name = "AMD Ryzen 5 5600H with Radeon Graphics";
  hw.ram_total_bytes = 32.0 * GiB;
  GpuInfo d;
  d.bus_id = "0000:01:00.0";
  d.name = "NVIDIA GeForce RTX 3050 Laptop GPU";
  d.vendor = geth::GpuVendor::kNvidia;
  d.is_integrated = false;
  d.vram_total_bytes = 4.0 * GiB;
  d.reports_power = true;
  hw.gpus = {d};
  return build_default_page(hw);
}

// A 6-panel "desktop" page: one dGPU, no iGPU. Built directly so the panel
// count is exactly six (CPU, one GPU, Memory, Temperature, Power, Network).
std::vector<PanelSpec> make_page_6() {
  std::vector<PanelSpec> page;

  // value_fn is nullptr in every fixture series: the layout engine never
  // calls it, and a null std::function keeps the designated initializers
  // warning-free under -Wmissing-field-initializers.
  auto s = [](const char* id, const char* label, int ci = 0, bool in_legend = true) {
    SeriesSpec ss;
    ss.id = id;
    ss.label = label;
    ss.color_index = ci;
    ss.in_legend = in_legend;
    return ss;
  };

  PanelSpec cpu;
  cpu.id = "cpu";
  cpu.title = "CPU";
  cpu.series = {s("cpu.usage", "CPU", 0, /*in_legend=*/false)};
  cpu.info_rows = {{"Total Usage", nullptr, 0}, {"Average CPU Frequency", nullptr, -1}};
  cpu.y_scale.kind = ScaleSpec::Kind::kPercent;
  page.push_back(cpu);

  PanelSpec gpu;
  gpu.id = "gpu.0000:01:00.0.usage";
  gpu.title = "RTX 3050";
  gpu.gpu_bus = "0000:01:00.0";
  gpu.series = {s("gpu.0000:01:00.0.usage", "RTX 3050", 1, /*in_legend=*/false)};
  gpu.info_rows = {{"RTX 3050 Usage", nullptr, 1}, {"Core Frequency", nullptr, -1}};
  gpu.y_scale.kind = ScaleSpec::Kind::kPercent;
  page.push_back(gpu);

  PanelSpec mem;
  mem.id = "mem";
  mem.title = "Memory";
  mem.series = {s("mem.ram_used", "RAM", 0), s("mem.swap_used", "Swap", 2)};
  mem.y_scale.kind = ScaleSpec::Kind::kAbsolute;
  mem.y_scale.absolute_max = 16.0 * GiB;
  page.push_back(mem);

  PanelSpec temps;
  temps.id = "temps";
  temps.title = "Temperature";
  temps.series = {s("cpu.temp", "CPU")};
  temps.y_scale.kind = ScaleSpec::Kind::kTemperature;
  page.push_back(temps);

  PanelSpec power;
  power.id = "power";
  power.title = "Power";
  power.series = {s("cpu.power", "CPU Package"), s("gpu.0000:01:00.0.power", "RTX 3050", 1)};
  power.y_scale.kind = ScaleSpec::Kind::kWatts;
  power.y_scale.floor_ = 30.0;
  page.push_back(power);

  PanelSpec net;
  net.id = "net";
  net.title = "Network";
  net.series = {s("net.total.rx", "Download"), s("net.total.tx", "Upload", 2)};
  net.y_scale.kind = ScaleSpec::Kind::kBytesPerSec;
  net.y_scale.floor_ = 64.0 * 1024.0;
  page.push_back(net);

  return page;
}

// The minimum-width bound on the column count, mirroring compute_layout:
// the largest c in [1..n] whose narrowest (base) column is >= every panel's
// Tight (the smallest mode's) minimum width.
int column_bound(int term_w, const std::vector<PanelSpec>& panels) {
  int need = 0;
  for (const auto& p : panels)
    need = std::max(need, min_panel_width(p, PanelMode::kTight));
  return std::max(1, std::min((int)panels.size(), need > 0 ? term_w / need : (int)panels.size()));
}

bool rects_overlap(const LayoutRect& a, const LayoutRect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Every invariant the renderer relies on, for one computed layout.
void check_invariants(const Layout& lo, const std::vector<PanelSpec>& panels, int W, int H) {
  CHECK_EQ((int)lo.panels.size(), (int)panels.size());
  REQUIRE((int)lo.panels.size() == (int)panels.size());

  // Every panel got a real rect.
  for (int i = 0; i < (int)lo.panels.size(); ++i) {
    const auto& r = lo.panels[i].rect;
    CHECK(r.w > 0);
    CHECK(r.h > 0);
  }

  // No two rects overlap.
  for (int i = 0; i < (int)lo.panels.size(); ++i)
    for (int j = i + 1; j < (int)lo.panels.size(); ++j)
      CHECK_FALSE(rects_overlap(lo.panels[i].rect, lo.panels[j].rect));

  // Every rect is within the terminal width. Vertically: a non-scrolling rect
  // sits inside the page (== the content height). A scrolling rect is the
  // offset-0 REFERENCE form (rows below the window sit lower than the viewport
  // and are re-placed by the renderer per offset), so only y >= 0 is checked
  // there — the tiling is checked by check_no_wasted_space.
  for (int i = 0; i < (int)lo.panels.size(); ++i) {
    const auto& r = lo.panels[i].rect;
    CHECK(r.x >= 0);
    CHECK(r.x + r.w <= W);
    CHECK(r.y >= 0);
    if (!lo.scrolling) CHECK(r.y + r.h <= lo.page_height);
  }

  // Every panel meets the minimum size for its mode.
  for (int i = 0; i < (int)lo.panels.size(); ++i) {
    const PanelSpec& p = panels[i];
    const auto& pl = lo.panels[i];
    const auto& r = pl.rect;
    CHECK(r.w >= min_panel_width(p, PanelMode::kTight));
    if (pl.mode == PanelMode::kFull)
      CHECK(r.h >= full_height(p, r.w));
    else if (pl.mode == PanelMode::kCompact)
      CHECK(r.h >= compact_height());
    else  // kTight
      CHECK(r.h >= tight_height());
  }

  // The column count never exceeds the minimum-width bound.
  CHECK(lo.columns <= column_bound(W, panels));
  CHECK(lo.columns >= 1);

  // Scrolling is active exactly when the grid has more rows than the viewport
  // window (total_rows > k visible rows).
  const bool expect_scroll = lo.total_rows > lo.rows();
  CHECK_EQ(lo.scrolling, expect_scroll);
  // The visible area always fills: the row heights sum to the content height.
  {
    int sum = 0;
    for (int rh : lo.row_heights) sum += rh;
    CHECK_EQ(sum, lo.content_height);
  }
  CHECK_EQ(lo.content_height, H - 2);  // one header row + one footer row
}

// The v1.0.2 invariant: the VISIBLE panels tile the content area EXACTLY —
// no blank rows or columns, no gaps, no overlaps — at every size and mode.
//   not scrolling: every row is visible and the union of the panel rects is
//     precisely [0,W) x [0,content_height).
//   scrolling: the k visible rows stretch to fill content_height (each >= the
//     Tight minimum height), and each of those rows tiles the full width.
void check_no_wasted_space(const Layout& lo, int W, int H) {
  const int content_h = H - 2;
  CHECK_EQ(lo.content_height, content_h);
  const int k = lo.rows();

  if (!lo.scrolling) {
    CHECK_EQ(lo.total_rows, k);
    long long area = 0;
    for (const auto& pl : lo.panels) {
      const auto& r = pl.rect;
      CHECK(r.x >= 0);
      CHECK(r.x + r.w <= W);
      CHECK(r.y >= 0);
      CHECK(r.y + r.h <= content_h);
      area += static_cast<long long>(r.w) * r.h;
    }
    // Contained + non-overlapping (checked above) + areas sum to the region's
    // area  =>  the union is exactly the region (no gaps, no blanks).
    CHECK_EQ(area, static_cast<long long>(W) * content_h);
  } else {
    // The k visible rows fill content_height exactly, each at least the Tight
    // minimum height, and there ARE hidden rows.
    CHECK(lo.total_rows > k);
    for (int i = 0; i < k; ++i) CHECK(lo.row_heights[i] >= tight_height());
    int sum = 0;
    for (int i = 0; i < k; ++i) sum += lo.row_heights[i];
    CHECK_EQ(sum, content_h);
    // The scroll window bounds: offset 0..(total_rows-k), k rows per page.
    CHECK_EQ(max_top_row(lo, content_h), lo.total_rows - k);
    CHECK_EQ(rows_fitting(lo, content_h), k);
    // Each visible row (offset-0 form) tiles the full terminal width.
    for (int r = 0; r < k; ++r) {
      int rowmin = 1 << 30, rowmax = 0, rowsum = 0;
      bool any = false;
      for (const auto& pl : lo.panels) {
        const auto& rc = pl.rect;
        if (rc.row != r) continue;
        any = true;
        rowmin = std::min(rowmin, rc.x);
        rowmax = std::max(rowmax, rc.x + rc.w);
        rowsum += rc.w;
      }
      CHECK(any);
      CHECK_EQ(rowmin, 0);
      CHECK_EQ(rowmax, W);
      CHECK_EQ(rowsum, W);  // + non-overlap (checked) => no horizontal gaps
    }
  }
}

}  // namespace

TEST_SUITE("layout") {

TEST_CASE("the three fixture pages have the expected panel counts") {
  const auto p10 = make_page_10();
  const auto p8 = make_page_8();
  const auto p6 = make_page_6();
  CHECK_EQ((int)p10.size(), 10);
  CHECK_EQ((int)p8.size(), 8);
  CHECK_EQ((int)p6.size(), 6);
}

TEST_CASE("every size: all three pages satisfy the layout invariants") {
  const std::vector<std::pair<int, int>> sizes = {
      {60, 20},  {80, 24},  {100, 30}, {120, 40}, {160, 48},
      {200, 60}, {250, 70}, {320, 90}, {80, 100}, {300, 30}};
  const auto p10 = make_page_10();
  const auto p8 = make_page_8();
  const auto p6 = make_page_6();
  const std::vector<PanelSpec>* pages[] = {&p10, &p8, &p6};
  for (const auto& [W, H] : sizes)
    for (const auto* page : pages) {
      const Layout lo = compute_layout(W, H, *page, LayoutPrefs{});
      check_invariants(lo, *page, W, H);
      check_no_wasted_space(lo, W, H);
    }
}

// The v1.0.2 sweep: no wasted space at ANY size. Widths 40..320 (step 7) x
// heights 12..99 (step 3) = 41 x 30 = 1230 sizes, all three pages.
TEST_CASE("sweep: no wasted space at every size (10/8/6-panel pages)") {
  const auto p10 = make_page_10();
  const auto p8 = make_page_8();
  const auto p6 = make_page_6();
  const std::vector<PanelSpec>* pages[] = {&p10, &p8, &p6};
  int sizes = 0;
  for (int W = 40; W <= 320; W += 7)
    for (int H = 12; H <= 100; H += 3) {
      ++sizes;
      for (const auto* page : pages) {
        const Layout lo = compute_layout(W, H, *page, LayoutPrefs{});
        check_invariants(lo, *page, W, H);
        check_no_wasted_space(lo, W, H);
      }
    }
  CHECK_EQ(sizes, 41 * 30);
}

TEST_CASE("large terminals: all Full and no scrolling (10-panel page)") {
  const auto p10 = make_page_10();
  // 160x48 and 250x70 are the standing non-regression sizes: they must keep
  // the all-Full, no-scrolling layout.
  for (const auto& [W, H] : {std::pair{160, 48}, std::pair{200, 60},
                             std::pair{250, 70}, std::pair{320, 90}}) {
    const Layout lo = compute_layout(W, H, p10, LayoutPrefs{});
    CHECK_FALSE(lo.scrolling);
    bool all_full = true;
    for (const auto& pl : lo.panels)
      if (pl.mode != PanelMode::kFull) all_full = false;
    CHECK(all_full);
  }
}

TEST_CASE("repro size 210x19: no scrolling, no blank rows (10-panel page)") {
  const auto p10 = make_page_10();
  // 210x19 was the reported bug: the old engine scrolled here and left blank
  // rows. With Tight + the stretched window, every panel fits at every height
  // down to content 8 rows, so no scrolling at all.
  for (int H = 10; H <= 20; ++H) {
    const Layout lo = compute_layout(210, H, p10, LayoutPrefs{});
    CHECK_FALSE(lo.scrolling);
    check_invariants(lo, p10, 210, H);
    check_no_wasted_space(lo, 210, H);
  }
}

TEST_CASE("very short terminal falls back to scrolling (10-panel page)") {
  const auto p10 = make_page_10();
  // 60x20: content height 18 rows cannot hold ten panels even at the Tight
  // minimum height in one column, so the engine must scroll.
  const Layout lo = compute_layout(60, 20, p10, LayoutPrefs{});
  CHECK(lo.scrolling);
  CHECK(lo.total_rows > lo.rows());
  check_invariants(lo, p10, 60, 20);
  check_no_wasted_space(lo, 60, 20);
}

TEST_CASE("scroll window: k stretched rows, offset 0..total-k (60x20, 10 panels)") {
  const auto p10 = make_page_10();
  const int W = 60, H = 20, content_h = H - 2;  // 18
  const Layout lo = compute_layout(W, H, p10, LayoutPrefs{});
  CHECK(lo.scrolling);
  const int k = content_h / tight_height();  // 18 / 4 = 4 visible rows
  CHECK_EQ(k, 4);
  CHECK_EQ(lo.rows(), k);
  CHECK_EQ(lo.total_rows, 10);
  CHECK_EQ(max_top_row(lo, content_h), lo.total_rows - k);  // 6
  CHECK_EQ(rows_fitting(lo, content_h), k);                 // 4
  int sum = 0;
  for (int rh : lo.row_heights) sum += rh;
  CHECK_EQ(sum, content_h);  // the window fills the viewport exactly
}

TEST_CASE("same input, same output (determinism)") {
  const auto p10 = make_page_10();
  const auto p6 = make_page_6();
  for (const auto& [W, H] : {std::pair{100, 30}, std::pair{160, 48}, std::pair{80, 100}}) {
    const Layout a = compute_layout(W, H, p10, LayoutPrefs{});
    const Layout b = compute_layout(W, H, p10, LayoutPrefs{});
    CHECK_EQ(a.columns, b.columns);
    CHECK_EQ(a.page_height, b.page_height);
    CHECK(a.scrolling == b.scrolling);
    CHECK_EQ((int)a.panels.size(), (int)b.panels.size());
    for (int i = 0; i < (int)a.panels.size(); ++i) {
      CHECK(a.panels[i].rect == b.panels[i].rect);
      CHECK(a.panels[i].mode == b.panels[i].mode);
    }
    const Layout c = compute_layout(W, H, p6, LayoutPrefs{});
    const Layout d = compute_layout(W, H, p6, LayoutPrefs{});
    CHECK_EQ(c.columns, d.columns);
    for (int i = 0; i < (int)c.panels.size(); ++i) CHECK(c.panels[i].rect == d.panels[i].rect);
  }
}

TEST_CASE("--columns 2 at 250x70 gives exactly 2 columns (both pages)") {
  const auto p10 = make_page_10();
  const auto p6 = make_page_6();
  LayoutPrefs prefs;
  prefs.columns = 2;
  CHECK_EQ(compute_layout(250, 70, p10, prefs).columns, 2);
  CHECK_EQ(compute_layout(250, 70, p6, prefs).columns, 2);
}

TEST_CASE("legend columns step at 56 and 96 interior columns") {
  CHECK_EQ(legend_cols(55), 1);
  CHECK_EQ(legend_cols(56), 2);
  CHECK_EQ(legend_cols(95), 2);
  CHECK_EQ(legend_cols(96), 3);
  CHECK_EQ(legend_cols(200), 3);
}

TEST_CASE("panel minimum widths match the graph-width rules") {
  PanelSpec p;
  p.y_scale.kind = ScaleSpec::Kind::kPercent;
  // graph_w at the minimum width is exactly the mode's minimum graph width
  // (32 for Full/Compact, 24 for Tight); the minimum width always uses the
  // abbreviated axis (the narrowest case).
  CHECK_EQ(graph_w(p, min_panel_width(p, PanelMode::kFull), true), 32);
  CHECK_EQ(graph_w(p, min_panel_width(p, PanelMode::kCompact), true), 32);
  CHECK_EQ(graph_w(p, min_panel_width(p, PanelMode::kTight), true), 24);
  p.y_scale.kind = ScaleSpec::Kind::kBytesPerSec;  // widest axis
  CHECK_EQ(graph_w(p, min_panel_width(p, PanelMode::kFull), true), 32);
  CHECK_EQ(graph_w(p, min_panel_width(p, PanelMode::kTight), true), 24);
  // Mode minimum heights (borders included).
  CHECK_EQ(compact_height(), 5);  // border(2) + graph(3)
  CHECK_EQ(tight_height(), 4);    // border(2) + graph(2)
  // Abbreviated axis widths: the narrowest budget per scale kind.
  CHECK_EQ(abbrev_axis_width(ScaleSpec::Kind::kPercent), 4);      // "100%"
  CHECK_EQ(abbrev_axis_width(ScaleSpec::Kind::kTemperature), 4);  // "100°"
  CHECK_EQ(abbrev_axis_width(ScaleSpec::Kind::kAbsolute), 4);     // "24G"
  CHECK_EQ(abbrev_axis_width(ScaleSpec::Kind::kWatts), 5);        // "1.5kW"
  CHECK_EQ(abbrev_axis_width(ScaleSpec::Kind::kBytesPerSec), 6);  // "1.5M/s"
  // The abbreviated form kicks in below 60 INNER columns, in any mode.
  CHECK(uses_abbrev_labels(59));
  CHECK_FALSE(uses_abbrev_labels(60));
}

TEST_CASE("best_mode: the largest mode that fits, false below Tight") {
  PanelSpec p;
  p.y_scale.kind = ScaleSpec::Kind::kPercent;
  p.info_rows = {{"a", nullptr, 0}};  // full_height = 2 + 4 + 0 + 1 = 7
  PanelMode m;
  // Wide panel (gw 91): the mode is decided by height alone.
  CHECK(best_mode(p, 100, 7, m));
  CHECK_EQ(m, PanelMode::kFull);
  CHECK(best_mode(p, 100, 6, m));
  CHECK_EQ(m, PanelMode::kCompact);
  CHECK(best_mode(p, 100, 5, m));
  CHECK_EQ(m, PanelMode::kCompact);
  CHECK(best_mode(p, 100, 4, m));
  CHECK_EQ(m, PanelMode::kTight);
  CHECK_FALSE(best_mode(p, 100, 3, m));
  // Narrow panel (gw 24 = the Tight minimum): never Full or Compact,
  // Tight from h >= 4 up.
  CHECK(best_mode(p, 32, 10, m));
  CHECK_EQ(m, PanelMode::kTight);
  CHECK(best_mode(p, 32, 4, m));
  CHECK_EQ(m, PanelMode::kTight);
  CHECK_FALSE(best_mode(p, 32, 3, m));
}

}  // TEST_SUITE
