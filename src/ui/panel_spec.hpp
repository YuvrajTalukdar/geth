// The data-driven panel model (Phase 2). The UI never renders a "CPU panel"
// or a "VRAM panel" — it renders a vector<PanelSpec> from
// build_default_page(), and the generic GraphPanel draws whatever spec it is
// given. Phase 4 (panel add/remove/config) and Phase 5 (themes) build on
// these types; nothing in the renderer is panel-specific.
#pragma once

#include "core/snapshot.hpp"
#include "core/series_store.hpp"

#include <functional>
#include <string>
#include <vector>

namespace geth::ui {

// The only kind for now; an enum so Phase 4 can add text-only panels
// without changing the PanelSpec shape.
enum class PanelKind { kGraph };

struct SeriesSpec {
  SeriesId id;          // must exist in the SeriesStore
  std::string label;    // legend label
  int color_index = 0;  // index into Theme::series[6]
  bool fill = false;    // filled area (Memory/VRAM first series)
  // False for the CPU / GPU-usage series: their value is already shown by a
  // color-barred info row (the KDE look the mock specifies), so a separate
  // legend line would duplicate it.
  bool in_legend = true;
  // Current-value text for the legend ("—"/"sleeping" is substituted by the
  // renderer when the value is missing / the GPU is asleep).
  std::function<std::string(const Snapshot&)> value_fn;
};

struct InfoRow {
  std::string label;
  std::function<std::string(const Snapshot&)> value_fn;
  // -1: no bar (a plain aligned row); else a bar in Theme::series[color_index].
  int color_index = -1;
};

struct ScaleSpec {
  enum class Kind {
    kPercent,       // fixed 0-100
    kTemperature,   // 0-100, growing to 110/120
    kAbsolute,      // 0..absolute_max (RAM/VRAM total)
    kWatts,         // auto-scale, nice 1/2/2.5/5×10ⁿ, floor 30 W
    kBytesPerSec,   // auto-scale, nice 1/2/5×1024ⁿ, floor per source
  } kind = Kind::kPercent;
  double absolute_max = 0;  // kAbsolute only
  // kWatts/kBytesPerSec: the minimum ceiling (30 W / 64 KiB/s / 1 MiB/s).
  double floor_ = 0;
};

struct PanelSpec {
  // Stable id for Phase 4's config: cpu, mem, gpu.<bus>.usage, gpu.<bus>.vram,
  // temps, power, net, disk.
  std::string id;
  std::string title;
  // The GPU this panel belongs to ("" for non-GPU panels): drives the
  // " · sleeping" title and the "sleeping" legend values.
  std::string gpu_bus;
  PanelKind kind = PanelKind::kGraph;
  std::vector<SeriesSpec> series;
  std::vector<InfoRow> info_rows;
  ScaleSpec y_scale;
};

// ---------------------------------------------------------------------------
// Hardware: discovered once at startup from the first (warm-up) sample.
// build_default_page() turns it into the panel list.
// ---------------------------------------------------------------------------

struct GpuInfo {
  std::string bus_id;
  std::string name;          // full name (pci.ids lookup or NVML)
  std::string product_name;  // amdgpu sysfs attribute; empty if not exposed
  GpuVendor vendor = GpuVendor::kAmd;
  bool is_integrated = false;
  double vram_total_bytes = 0;
  // Discrete GPUs report card power; integrated ones do not (their PPT is
  // whole-package power and is the CPU package line instead).
  bool reports_power = false;
};

struct Hardware {
  std::string cpu_name;  // full cpuinfo "model name" (or the demo's fake one)
  std::vector<GpuInfo> gpus;  // Snapshot order (PCI bus order)
  double ram_total_bytes = 0;
  // Power-panel label for the CPU package line: "CPU Package" (RAPL /
  // amd_energy / zenpower), "APU Package (PPT)" (the APU lift), or empty when
  // no CPU power source exists (no CPU line in the Power panel).
  std::string cpu_power_label;
};

Hardware hardware_from(const Snapshot& s, const std::string& cpu_name);

// The default page: CPU, one usage panel per GPU (dGPUs first, then iGPUs),
// Memory, one VRAM panel per GPU (same order), Temperature, Power, Network,
// Disk. Panel order flows left to right into two equal-width columns.
std::vector<PanelSpec> build_default_page(const Hardware& hw);

}  // namespace geth::ui
