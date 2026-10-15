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

// A 6-panel "desktop" page: one dGPU, no iGPU. Built directly so the panel
// count is exactly six (CPU, one GPU, Memory, Temperature, Power, Network).
std::vector<PanelSpec> make_page_6() {
  std::vector<PanelSpec> page;

  // value_fn is nullptr in every fixture series: the layout engine never
  // calls it, and a null std::function keeps the designated initializers
  // warning-free under -Wmissing-field-initializers.
  auto s = [](const char* id, const char* label, int ci = 0, bool fill = false,
              bool in_legend = true) {
    SeriesSpec ss;
    ss.id = id;
    ss.label = label;
    ss.color_index = ci;
    ss.fill = fill;
    ss.in_legend = in_legend;
    return ss;
  };

  PanelSpec cpu;
  cpu.id = "cpu";
  cpu.title = "CPU";
  cpu.series = {s("cpu.usage", "CPU", 0, true, false)};
  cpu.info_rows = {{"Total Usage", nullptr, 0}, {"Average CPU Frequency", nullptr, -1}};
  cpu.y_scale.kind = ScaleSpec::Kind::kPercent;
  page.push_back(cpu);

  PanelSpec gpu;
  gpu.id = "gpu.0000:01:00.0.usage";
  gpu.title = "RTX 3050";
  gpu.gpu_bus = "0000:01:00.0";
  gpu.series = {s("gpu.0000:01:00.0.usage", "RTX 3050", 1, true, false)};
  gpu.info_rows = {{"RTX 3050 Usage", nullptr, 1}, {"Core Frequency", nullptr, -1}};
  gpu.y_scale.kind = ScaleSpec::Kind::kPercent;
  page.push_back(gpu);

  PanelSpec mem;
  mem.id = "mem";
  mem.title = "Memory";
  mem.series = {s("mem.ram_used", "RAM", 0, true), s("mem.swap_used", "Swap", 2)};
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
// minimum width.
int column_bound(int term_w, const std::vector<PanelSpec>& panels) {
  int need = 0;
  for (const auto& p : panels) need = std::max(need, min_panel_width(p));
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

  // Every rect is within the page: inside the terminal width, and within the
  // page height (which equals the content height unless scrolling).
  for (int i = 0; i < (int)lo.panels.size(); ++i) {
    const auto& r = lo.panels[i].rect;
    CHECK(r.x >= 0);
    CHECK(r.x + r.w <= W);
    CHECK(r.y >= 0);
    CHECK(r.y + r.h <= lo.page_height);
  }

  // Every panel meets the minimum size for its mode.
  for (int i = 0; i < (int)lo.panels.size(); ++i) {
    const PanelSpec& p = panels[i];
    const auto& pl = lo.panels[i];
    const auto& r = pl.rect;
    CHECK(r.w >= min_panel_width(p));
    if (pl.mode == PanelMode::kFull)
      CHECK(r.h >= full_height(p, r.w));
    else
      CHECK(r.h >= compact_height());
  }

  // The column count never exceeds the minimum-width bound.
  CHECK(lo.columns <= column_bound(W, panels));
  CHECK(lo.columns >= 1);

  // Scrolling is active exactly when the page is taller than the content area.
  CHECK(lo.scrolling == (lo.page_height > lo.content_height));
  CHECK_EQ(lo.content_height, H - 2);  // one header row + one footer row
}

}  // namespace

TEST_SUITE("layout") {

TEST_CASE("the two fixture pages have the expected panel counts") {
  const auto p10 = make_page_10();
  const auto p6 = make_page_6();
  CHECK_EQ((int)p10.size(), 10);
  CHECK_EQ((int)p6.size(), 6);
}

TEST_CASE("every size: both pages satisfy the layout invariants") {
  const std::vector<std::pair<int, int>> sizes = {
      {60, 20},  {80, 24},  {100, 30}, {120, 40}, {160, 48},
      {200, 60}, {250, 70}, {320, 90}, {80, 100}, {300, 30}};
  const auto p10 = make_page_10();
  const auto p6 = make_page_6();
  for (const auto& [W, H] : sizes) {
    const Layout l10 = compute_layout(W, H, p10, LayoutPrefs{});
    check_invariants(l10, p10, W, H);
    const Layout l6 = compute_layout(W, H, p6, LayoutPrefs{});
    check_invariants(l6, p6, W, H);
  }
}

TEST_CASE("large terminals: all Full and no scrolling (10-panel page)") {
  const auto p10 = make_page_10();
  for (const auto& [W, H] : {std::pair{200, 60}, std::pair{320, 90}, std::pair{250, 70}}) {
    const Layout lo = compute_layout(W, H, p10, LayoutPrefs{});
    CHECK_FALSE(lo.scrolling);
    bool all_full = true;
    for (const auto& pl : lo.panels)
      if (pl.mode != PanelMode::kFull) all_full = false;
    CHECK(all_full);
  }
}

TEST_CASE("very short terminal falls back to scrolling (10-panel page)") {
  const auto p10 = make_page_10();
  // 60x20: content height 18 rows cannot hold ten panels even at the minimum
  // Compact height, so the engine must scroll.
  const Layout lo = compute_layout(60, 20, p10, LayoutPrefs{});
  CHECK(lo.scrolling);
  CHECK(lo.page_height > lo.content_height);
  check_invariants(lo, p10, 60, 20);
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

TEST_CASE("panel minimum widths match the 32-column graph rule") {
  PanelSpec p;
  p.y_scale.kind = ScaleSpec::Kind::kPercent;
  // graph_w at the minimum width must be exactly 32.
  CHECK_EQ(graph_w(p, min_panel_width(p)), 32);
  p.y_scale.kind = ScaleSpec::Kind::kBytesPerSec;
  CHECK_EQ(graph_w(p, min_panel_width(p)), 32);
  CHECK_EQ(compact_height(), 6);  // border(2) + graph(3) + legend(1)
}

}  // TEST_SUITE
