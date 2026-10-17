#include "ui/panel_spec.hpp"

#include <algorithm>

#include "util/format.hpp"
#include "util/names.hpp"
#include "util/parse.hpp"

namespace geth::ui {

namespace {

// Legend/info value formatters. Missing data renders "—" — never 0.
std::string v_pct(const std::optional<double>& v) {
  return v ? format::percent(*v) : "—";
}
std::string v_freq(const std::optional<double>& v) {
  return v ? format::freq(*v) : "—";
}
std::string v_bytes(const std::optional<double>& v) {
  return v ? format::bytes(*v) : "—";
}
std::string v_watts(const std::optional<double>& v) {
  return v ? format::watts(*v) : "—";
}
std::string v_rate(const std::optional<double>& v) {
  return v ? format::rate(*v) : "—";
}
std::string v_celsius(const std::optional<double>& v) {
  return v ? format::celsius(*v) : "—";
}

// dGPUs first, then iGPUs, each group in PCI bus order (Snapshot order).
std::vector<std::size_t> gpu_order(const Hardware& hw) {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < hw.gpus.size(); ++i)
    if (!hw.gpus[i].is_integrated) out.push_back(i);
  for (std::size_t i = 0; i < hw.gpus.size(); ++i)
    if (hw.gpus[i].is_integrated) out.push_back(i);
  return out;
}

// The palette slot (index into Theme::series[6]) for a device, by ROLE
// (Phase 6, Part C) so the same device keeps one color in every panel:
//   slot 1 (0) CPU/primary   slot 2 (1) first dGPU
//   slot 3 (2) iGPU          slot 4 (3) second dGPU
//   slot 5 (4) / slot 6 (5)  third / fourth dGPU (rare)
int device_color_index(const Hardware& hw, std::size_t gpu_i) {
  if (hw.gpus[gpu_i].is_integrated) return 2;  // slot 3
  int d = 0;  // this dGPU's ordinal among the discrete GPUs
  for (std::size_t i = 0; i < gpu_i; ++i)
    if (!hw.gpus[i].is_integrated) ++d;
  static const int kDgpuSlot[4] = {1, 3, 4, 5};  // slots 2, 4, 5, 6
  return kDgpuSlot[std::min(d, 3)];
}

// The smallest palette slot not already used by an earlier series of the same
// panel (Phase 6, Part B: no two series in a panel share a color; the second
// series — GTT next to VRAM — takes the next free slot).
int next_free_slot(const std::vector<int>& used) {
  for (int s = 0; s < 6; ++s)
    if (std::find(used.begin(), used.end(), s) == used.end()) return s;
  return used.empty() ? 0 : used.back();
}

// The Phase 6 Part B fill rule, applied to a whole panel by series count:
//   1 series  -> its one series is filled
//   2 series  -> only the PRIMARY (first) is filled
//   3+ series -> nothing is filled (Temperature, Power)
void apply_fill_rule(PanelSpec& p) {
  const int n = static_cast<int>(p.series.size());
  for (int i = 0; i < n; ++i)
    p.series[i].fill = (n == 1) ? true : (n == 2 ? i == 0 : false);
}

}  // namespace

// Exported (panel_spec.hpp): the single choke point for every GPU-derived
// title, legend label and the auto header title.
std::string gpu_short_name(const GpuInfo& g) {
  if (g.renamed) return std::string(parse::trim(g.name));
  return names::short_gpu(g.name, g.is_integrated, g.product_name);
}

namespace {
std::string gpu_title(const GpuInfo& g) { return gpu_short_name(g); }
}  // namespace

Hardware hardware_from(const Snapshot& s, const std::string& cpu_name) {
  Hardware h;
  h.cpu_name = cpu_name;
  if (s.mem.ram_total_bytes) h.ram_total_bytes = *s.mem.ram_total_bytes;
  for (const auto& g : s.gpus) {
    GpuInfo gi;
    gi.bus_id = g.pci_bus_id;
    gi.name = g.name;
    gi.product_name = g.product_name;
    gi.vendor = g.vendor;
    gi.is_integrated = g.is_integrated;
    if (g.vram_total_bytes) gi.vram_total_bytes = *g.vram_total_bytes;
    if (g.gtt_total_bytes) gi.gtt_total_bytes = *g.gtt_total_bytes;
    gi.reports_power = !g.is_integrated;
    h.gpus.push_back(std::move(gi));
  }
  if (s.cpu.package_power_source)
    h.cpu_power_label = *s.cpu.package_power_source == CpuPackagePowerSource::kApuPpt
                            ? "APU Package (PPT)"
                            : "CPU Package";
  return h;
}

std::vector<PanelSpec> build_default_page(const Hardware& hw) {
  std::vector<PanelSpec> page;
  const auto order = gpu_order(hw);

  // 1. CPU -----------------------------------------------------------------
  {
    PanelSpec p;
    p.id = "cpu";
    p.title = "CPU";
    // The value line is the "Total Usage" info row below (with color bar),
    // so the series itself gets no separate legend line.
    p.series = {{.id = SeriesId("cpu.usage"), .label = "CPU", .color_index = 0,
                 .fill = true, .in_legend = false,
                 .value_fn = [](const Snapshot& s) { return v_pct(s.cpu.usage_pct); }}};
    p.info_rows = {
        {"Total Usage",
         [](const Snapshot& s) { return v_pct(s.cpu.usage_pct); }, 0},
        {"Average CPU Frequency",
         [](const Snapshot& s) { return v_freq(s.cpu.freq_avg_mhz); }, -1},
        {"Maximum CPU Frequency",
         [](const Snapshot& s) { return v_freq(s.cpu.freq_max_mhz); }, -1},
    };
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  // 2. One usage panel per GPU ---------------------------------------------
  for (std::size_t k = 0; k < order.size(); ++k) {
    const auto& g = hw.gpus[order[k]];
    const int ci = device_color_index(hw, order[k]);
    const std::string title = gpu_title(g);
    const std::string bus = g.bus_id;
    PanelSpec p;
    p.id = "gpu." + bus + ".usage";
    p.title = title;
    p.gpu_bus = bus;
    p.series = {{.id = SeriesId("gpu." + bus + ".usage"), .label = title, .color_index = ci,
                 .fill = true, .in_legend = false,
                 .value_fn = [bus](const Snapshot& s) {
                   for (const auto& x : s.gpus)
                     if (x.pci_bus_id == bus) return v_pct(x.usage_pct);
                   return std::string("—");
                 }}};
    p.info_rows = {
        {title + " Usage",
         [bus](const Snapshot& s) {
           for (const auto& x : s.gpus)
             if (x.pci_bus_id == bus) return v_pct(x.usage_pct);
           return std::string("—");
         },
         ci},
        {"Core Frequency",
         [bus](const Snapshot& s) {
           for (const auto& x : s.gpus)
             if (x.pci_bus_id == bus) return v_freq(x.core_clock_mhz);
           return std::string("—");
         },
         -1},
    };
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  // 3. Memory ----------------------------------------------------------------
  {
    PanelSpec p;
    p.id = "mem";
    p.title = "Memory";
    p.series = {
        {.id = SeriesId("mem.ram_used"), .label = "RAM", .color_index = 0,
         .fill = true,
         .value_fn = [](const Snapshot& s) { return v_bytes(s.mem.ram_used_bytes); }},
        {.id = SeriesId("mem.swap_used"), .label = "Swap", .color_index = 2,
         .value_fn = [](const Snapshot& s) { return v_bytes(s.mem.swap_used_bytes); }},
    };
    p.y_scale.kind = ScaleSpec::Kind::kAbsolute;
    p.y_scale.absolute_max = hw.ram_total_bytes;
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  // 4. One VRAM panel per GPU (same order) -----------------------------------
  for (std::size_t k = 0; k < order.size(); ++k) {
    const auto& g = hw.gpus[order[k]];
    const int ci = device_color_index(hw, order[k]);
    const std::string title = gpu_title(g);
    const std::string bus = g.bus_id;
    PanelSpec p;
    p.id = "gpu." + bus + ".vram";
    // Distinguished from the usage panel, which carries the bare GPU short
    // name: "RTX 3050 Laptop · Memory".
    p.title = title + " \xC2\xB7 Memory";
    p.gpu_bus = bus;
    if (g.is_integrated) {
      // Integrated: GTT is the primary series (the aperture the OS actually
      // uses) in the device's color; the fixed BIOS VRAM carve-out is the
      // SECOND series, which takes the next theme slot not used in this panel
      // (Phase 6: no two series of a panel share a color).
      const int vram_ci = next_free_slot({ci});
      p.series = {
          {.id = SeriesId("gpu." + bus + ".gtt"), .label = "GTT", .color_index = ci,
           .value_fn = [bus](const Snapshot& s) {
             for (const auto& x : s.gpus)
               if (x.pci_bus_id == bus) return v_bytes(x.gtt_used_bytes);
             return std::string("—");
           }},
          {.id = SeriesId("gpu." + bus + ".vram"), .label = "VRAM", .color_index = vram_ci,
           .value_fn = [bus](const Snapshot& s) {
             for (const auto& x : s.gpus)
               if (x.pci_bus_id == bus) return v_bytes(x.vram_used_bytes);
             return std::string("—");
           }},
      };
      // The scale is the {1, 1.5} x 2^k ceiling (nice_bytes_max, in
      // frame.cpp) of the LARGER of the two totals, so both lines fit.
      p.y_scale.kind = ScaleSpec::Kind::kAbsolute;
      p.y_scale.absolute_max =
          std::max(g.vram_total_bytes, g.gtt_total_bytes);
    } else {
      p.series = {{.id = SeriesId("gpu." + bus + ".vram"), .label = title,
                   .color_index = ci,
                   .fill = true,
                   .value_fn = [bus](const Snapshot& s) {
                     for (const auto& x : s.gpus)
                       if (x.pci_bus_id == bus) return v_bytes(x.vram_used_bytes);
                     return std::string("—");
                   }}};
      p.y_scale.kind = ScaleSpec::Kind::kAbsolute;
      p.y_scale.absolute_max = g.vram_total_bytes;
    }
    p.info_rows = {
        {"Memory Clock",
         [bus](const Snapshot& s) {
           for (const auto& x : s.gpus)
             if (x.pci_bus_id == bus) return v_freq(x.mem_clock_mhz);
           return std::string("—");
         },
         -1},
    };
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  // 5. Temperature -----------------------------------------------------------
  {
    PanelSpec p;
    p.id = "temps";
    p.title = "Temperature";
    p.series = {{.id = SeriesId("cpu.temp"), .label = "CPU",
                 .value_fn = [](const Snapshot& s) { return v_celsius(s.cpu.temp_c); }}};
    for (std::size_t k = 0; k < order.size(); ++k) {
      const auto& g = hw.gpus[order[k]];
      const std::string bus = g.bus_id;
      p.series.push_back({.id = SeriesId("gpu." + bus + ".temp"),
                          .label = gpu_title(g),
                          .color_index = device_color_index(hw, order[k]),
                          .value_fn = [bus](const Snapshot& s) {
                            for (const auto& x : s.gpus)
                              if (x.pci_bus_id == bus) return v_celsius(x.temp_c);
                            return std::string("—");
                          }});
    }
    p.y_scale.kind = ScaleSpec::Kind::kTemperature;
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  // 6. Power -----------------------------------------------------------------
  {
    PanelSpec p;
    p.id = "power";
    p.title = "Power";
    if (!hw.cpu_power_label.empty())
      p.series.push_back({.id = SeriesId("cpu.power"), .label = hw.cpu_power_label,
                          .value_fn = [](const Snapshot& s) {
                            return v_watts(s.cpu.package_power_w);
                          }});
    for (std::size_t k = 0; k < order.size(); ++k) {
      const auto& g = hw.gpus[order[k]];
      if (!g.reports_power) continue;  // never a separate iGPU power line
      const std::string bus = g.bus_id;
      p.series.push_back({.id = SeriesId("gpu." + bus + ".power"),
                          .label = gpu_title(g),
                          .color_index = device_color_index(hw, order[k]),
                          .value_fn = [bus](const Snapshot& s) {
                            for (const auto& x : s.gpus)
                              if (x.pci_bus_id == bus) return v_watts(x.power_w);
                            return std::string("—");
                          }});
    }
    p.y_scale.kind = ScaleSpec::Kind::kWatts;
    p.y_scale.floor_ = 20.0;  // minimum ceiling: 20 W (nice_watts' floor)
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  // 7. Network ----------------------------------------------------------------
  {
    PanelSpec p;
    p.id = "net";
    p.title = "Network";
    p.series = {
        {.id = SeriesId("net.total.rx"), .label = "Download",
         .value_fn = [](const Snapshot& s) { return v_rate(s.net.total_rx_bytes_per_s); }},
        {.id = SeriesId("net.total.tx"), .label = "Upload", .color_index = 2,
         .value_fn = [](const Snapshot& s) { return v_rate(s.net.total_tx_bytes_per_s); }},
    };
    p.y_scale.kind = ScaleSpec::Kind::kBytesPerSec;
    p.y_scale.floor_ = 64.0 * 1024.0;  // minimum ceiling: 64 KiB/s
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  // 8. Disk -------------------------------------------------------------------
  {
    PanelSpec p;
    p.id = "disk";
    p.title = "Disk";
    p.series = {
        {.id = SeriesId("disk.total.r"), .label = "Read",
         .value_fn = [](const Snapshot& s) { return v_rate(s.disk.total_read_bytes_per_s); }},
        {.id = SeriesId("disk.total.w"), .label = "Write", .color_index = 2,
         .value_fn = [](const Snapshot& s) { return v_rate(s.disk.total_write_bytes_per_s); }},
    };
    p.y_scale.kind = ScaleSpec::Kind::kBytesPerSec;
    p.y_scale.floor_ = 1024.0 * 1024.0;  // minimum ceiling: 1 MiB/s
    apply_fill_rule(p);
    page.push_back(std::move(p));
  }

  return page;
}

}  // namespace geth::ui
