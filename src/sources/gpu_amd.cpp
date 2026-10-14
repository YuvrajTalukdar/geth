#include "sources/gpu_amd.hpp"

#include "sources/temps.hpp"  // parse_millidegrees()
#include "util/parse.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace geth {

// ---------------------------------------------------------------------------
// file-format parsers
// ---------------------------------------------------------------------------

std::optional<int> parse_gpu_metrics_format(std::string_view buf) {
  // struct metrics_table_header (drivers/gpu/drm/amd/include/
  // kgd_pp_interface.h): u16 structure_size, u8 format_revision, u8
  // content_revision — little-endian on x86. format_revision: 1 = discrete
  // GPU, 2 or 3 = APU.
  if (buf.size() < 4) return std::nullopt;
  const unsigned size = static_cast<unsigned>(static_cast<unsigned char>(buf[0])) |
                        (static_cast<unsigned>(static_cast<unsigned char>(buf[1])) << 8);
  if (size < 4) return std::nullopt;  // not a metrics buffer
  return static_cast<int>(static_cast<unsigned char>(buf[2]));
}

std::optional<double> parse_pp_dpm_mhz(std::string_view text) {
  // pp_dpm_sclk looks like:
  //   0: 200Mhz
  //   1: 400Mhz *
  //   2: 1800Mhz
  // the '*' marks the currently active level.
  for (const auto line : parse::split_lines(text)) {
    if (line.find('*') == std::string_view::npos) continue;
    for (const auto& tok : parse::tokens(line)) {
      if (tok.empty() || tok == "*" ||
          !std::isdigit(static_cast<unsigned char>(tok.front())))
        continue;
      std::size_t i = 0;
      while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) ++i;
      const auto num = parse::to_u64(tok.substr(0, i));
      if (!num) continue;
      const std::string_view unit = tok.substr(i);
      double mhz = static_cast<double>(*num);
      if (unit == "Mhz") {
        // already MHz
      } else if (unit == "Khz") {
        mhz /= 1000.0;
      } else if (unit == "Ghz") {
        mhz *= 1000.0;
      } else {
        continue;  // unknown unit: do not guess
      }
      return mhz;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// integrated-GPU decision
// ---------------------------------------------------------------------------

const std::vector<unsigned>& kApuDeviceIds() {
  // Known-APU iGPU device ids (vendor 0x1002), from pci.ids: Cezanne, Renoir,
  // Rembrandt, Phoenix, Hawk Point, Strix (plus Lucienne / Strix Halo, the
  // same APU families under other codenames).
  static const std::vector<unsigned> ids = {
      0x150e,  // Strix [Radeon 880M/890M]
      0x1586,  // Strix Halo
      0x15bf,  // Phoenix1
      0x15c8,  // Phoenix2
      0x1636,  // Renoir
      0x1638,  // Cezanne
      0x164c,  // Lucienne
      0x164d,  // Rembrandt
      0x164f,  // Phoenix
      0x1681,  // Rembrandt [Radeon 680M]
      0x1900,  // HawkPoint1
      0x1901,  // HawkPoint2
  };
  return ids;
}

std::string AmdIntegratedDecision::describe() const {
  switch (method) {
    case AmdIntegratedMethod::kGpuMetrics:
      return "gpu_metrics format " + std::to_string(metrics_format) +
             (metrics_format == 1 ? " (dGPU)" : " (APU)");
    case AmdIntegratedMethod::kApuDeviceId: {
      static const char* hexd = "0123456789abcdef";
      std::string s = "APU device id 0x";
      for (int shift = 12; shift >= 0; shift -= 4)
        s += hexd[(device_id >> shift) & 0xf];
      return s;
    }
    case AmdIntegratedMethod::kSmallVram:
      return "VRAM <= 2 GiB";
    case AmdIntegratedMethod::kBootVgaTiebreak:
      return "boot_vga tiebreaker (multi-GPU; least reliable method)";
    case AmdIntegratedMethod::kDefaultDiscrete:
      return "discrete (no APU evidence)";
  }
  return "unknown";
}

AmdIntegratedDecision decide_amd_integrated(
    std::optional<int> metrics_format, std::optional<unsigned> pci_device_id,
    std::optional<double> vram_total_bytes, bool boot_vga, bool multi_gpu) {
  AmdIntegratedDecision d;
  if (metrics_format && *metrics_format >= 1 && *metrics_format <= 3) {
    d.metrics_format = *metrics_format;
    d.integrated = *metrics_format != 1;  // 2/3 = APU, 1 = dGPU
    d.method = AmdIntegratedMethod::kGpuMetrics;
    return d;
  }
  if (pci_device_id) {
    const auto& ids = kApuDeviceIds();
    if (std::find(ids.begin(), ids.end(), *pci_device_id) != ids.end()) {
      d.device_id = *pci_device_id;
      d.integrated = true;
      d.method = AmdIntegratedMethod::kApuDeviceId;
      return d;
    }
  }
  if (vram_total_bytes && *vram_total_bytes <= 2.0 * 1024.0 * 1024.0 * 1024.0) {
    d.integrated = true;
    d.method = AmdIntegratedMethod::kSmallVram;
    return d;
  }
  if (boot_vga && multi_gpu) {
    d.integrated = true;
    d.method = AmdIntegratedMethod::kBootVgaTiebreak;
    return d;
  }
  return d;  // kDefaultDiscrete
}

void finalize_gpus(std::vector<GpuSample>& gpus) {
  if (gpus.empty()) return;
  // Stable ids in PCI bus order: "DDDD:BB:DD.F" is fixed-width, so a plain
  // lexicographic sort is the bus order. is_integrated is decided by the
  // sources (decide_amd_integrated / "always discrete" for NVIDIA).
  std::stable_sort(gpus.begin(), gpus.end(),
                   [](const GpuSample& a, const GpuSample& b) {
                     return a.pci_bus_id < b.pci_bus_id;
                   });
  for (std::size_t i = 0; i < gpus.size(); ++i) gpus[i].id = "gpu" + std::to_string(i);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace {

std::optional<std::string> read_str(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  std::string v;
  std::getline(f, v);
  if (v.empty()) return std::nullopt;
  return v;
}

// The amdgpu hwmon may sit at device/hwmon as a symlink (most kernels) or as
// a real directory (some kernels). directory_iterator follows both.
std::optional<std::string> find_amdgpu_hwmon(const std::string& dev_dir) {
  std::error_code ec;
  auto it = std::filesystem::directory_iterator(dev_dir + "/hwmon", ec);
  if (ec) return std::nullopt;
  for (; it != std::filesystem::directory_iterator(); ++it) {
    if (!it->is_directory()) continue;
    if (const auto n = read_str(it->path().string() + "/name"); n && *n == "amdgpu")
      return it->path().string();
  }
  return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// GpuBackend interface
// ---------------------------------------------------------------------------

bool GpuAmdBackend::claims(const PciDevice& dev) const {
  return dev.vendor_id == kVendorId && dev.driver == "amdgpu";
}

void GpuAmdBackend::attach(const PciDevice& dev) {
  Card c;
  c.dev = dev;

  // Name via pci.ids: amdgpu exposes no product string in sysfs.
  if (_ids)
    if (auto n = _ids->lookup(dev.vendor_id, dev.device_id)) c.name = *n;
  if (c.name.empty()) c.name = "AMD GPU [" + dev.bus_id + "]";

  const std::string d = dev.dev_path;

  // --- open every metric file once; absence (ENOENT) is a valid state for
  // the optional ones, anything else is recorded as evidence ----------------
  struct FileLine {
    const char* rel;
    SysfsFile* out;
  };
  const FileLine dev_files[] = {
      {"gpu_busy_percent", &c.busy},
      {"mem_info_vram_used", &c.vram_used},
      {"mem_info_vram_total", &c.vram_total},
      {"gpu_clock", &c.gpu_clock},
      {"mem_clock", &c.mem_clock},
      {"pp_dpm_sclk", &c.pp_dpm_sclk},
      {"pp_dpm_mclk", &c.pp_dpm_mclk},
      {"gpu_metrics", &c.gpu_metrics},
      {"power/runtime_status", &c.runtime_status},
  };
  for (const auto& fl : dev_files) {
    const std::string p = d + "/" + fl.rel;
    int err = 0;
    if (!SysfsFile::open(p, *fl.out, err) && err != ENOENT)
      c.open_errors.push_back(p + ": open failed (errno " + std::to_string(err) + ")");
  }

  // amdgpu hwmon: power/temperature/frequency fallbacks.
  if (const auto hwmon = find_amdgpu_hwmon(d)) {
    const FileLine hw_files[] = {
        {"power1_input", &c.hwmon_power},
        {"power1_average", &c.hwmon_power_avg},
        {"temp1_input", &c.hwmon_temp},
        {"freq1_input", &c.hwmon_freq},
    };
    for (const auto& fl : hw_files) {
      const std::string p = *hwmon + "/" + fl.rel;
      int err = 0;
      if (!SysfsFile::open(p, *fl.out, err) && err != ENOENT)
        c.open_errors.push_back(p + ": open failed (errno " + std::to_string(err) + ")");
    }
  }

  // Reading mem_info_vram_total touches only driver memory (no registers),
  // so it is safe even while the device is suspended — needed for the
  // VRAM-based integrated fallback.
  if (c.vram_total.valid()) {
    if (const auto v = parse::to_u64(parse::trim(c.vram_total.read())))
      c.vram_total_bytes = static_cast<double>(*v);
  }

  // gpu_metrics may wake a suspended GPU (SMU read): only read it now when
  // the device is active, and never again (cached in c.metrics_format).
  // (card(dev) cannot be used here: `dev` is not attached yet.)
  if (c.gpu_metrics.valid()) {
    std::string rs_text;
    if (_read_runtime)
      rs_text = _read_runtime(dev);
    else if (c.runtime_status.valid())
      rs_text = c.runtime_status.read();
    const auto rs = parse_runtime_status(rs_text);
    if (rs == RuntimeState::kActive || rs == RuntimeState::kUnsupported) {
      if (const auto fmt = parse_gpu_metrics_format(c.gpu_metrics.read()))
        c.metrics_format = fmt;
    }
  }

  _cards.push_back(std::move(c));
}

GpuAmdBackend::Card* GpuAmdBackend::card(const PciDevice& dev) {
  for (auto& c : _cards)
    if (c.dev.bus_id == dev.bus_id) return &c;
  return nullptr;
}

const GpuAmdBackend::Card* GpuAmdBackend::card(const PciDevice& dev) const {
  for (const auto& c : _cards)
    if (c.dev.bus_id == dev.bus_id) return &c;
  return nullptr;
}

std::string GpuAmdBackend::runtime_state(const PciDevice& dev) const {
  if (_read_runtime) return _read_runtime(dev);
  if (const Card* c = card(dev))
    if (c->runtime_status.valid()) return c->runtime_status.read();
  return "";
}

AmdIntegratedDecision GpuAmdBackend::decide(const Card& c,
                                            std::optional<double> live_vram) const {
  // "multi-GPU" for the boot_vga tiebreaker = more than one amdgpu device.
  return decide_amd_integrated(c.metrics_format,
                               c.dev.device_id ? std::optional<unsigned>(c.dev.device_id)
                                               : std::nullopt,
                               c.vram_total_bytes ? c.vram_total_bytes : live_vram,
                               c.dev.boot_vga, _cards.size() >= 2);
}

void GpuAmdBackend::sample(const PciDevice& dev, GpuSample& out) {
  Card* c = card(dev);
  if (!c) return;  // not ours (defensive)

  GpuSample g;
  g.vendor = GpuVendor::kAmd;
  g.name = c->name;
  g.pci_bus_id = dev.bus_id;
  g.boot_vga = c->dev.boot_vga;

  const auto rs = parse_runtime_status(runtime_state(dev));
  // "unsupported" = runtime PM disabled = always awake -> active.
  switch (rs) {
    case RuntimeState::kSleeping: g.state = GpuState::kSleeping; break;
    case RuntimeState::kError: g.state = GpuState::kError; break;
    default: g.state = GpuState::kActive; break;
  }

  // Queries only while awake; "resuming" counts as active but the driver
  // cannot serve reads yet, so skip them this tick (fields stay n/a).
  const bool query = rs == RuntimeState::kActive || rs == RuntimeState::kUnsupported;

  if (query) {
    if (c->busy.valid()) {
      if (const auto b = parse::to_u64(parse::trim(c->busy.read()))) {
        g.usage_pct = static_cast<double>(*b);
      } else {
        g.state = GpuState::kError;  // counter unreadable: something is off
      }
    }
    if (c->vram_used.valid()) {
      if (const auto v = parse::to_u64(parse::trim(c->vram_used.read())))
        g.vram_used_bytes = static_cast<double>(*v);
    }
    if (c->vram_total.valid()) {
      if (const auto v = parse::to_u64(parse::trim(c->vram_total.read())))
        g.vram_total_bytes = static_cast<double>(*v);
    }

    // Core clock: prefer the device-level gpu_clock (MHz). APUs (iGPUs)
    // lack that file; pp_dpm_sclk carries the current level; the hwmon
    // sclk (Hz) is the last fallback.
    double core = 0.0;
    bool have_core = false;
    if (c->gpu_clock.valid()) {
      if (const auto v = parse::to_u64(parse::trim(c->gpu_clock.read()))) {
        core = static_cast<double>(*v);
        have_core = true;
      }
    }
    if (!have_core && c->pp_dpm_sclk.valid()) {
      if (const auto v = parse_pp_dpm_mhz(c->pp_dpm_sclk.read())) {
        core = *v;
        have_core = true;
      }
    }
    if (!have_core && c->hwmon_freq.valid()) {
      if (const auto v = parse::to_u64(parse::trim(c->hwmon_freq.read()))) {
        core = static_cast<double>(*v) / 1e6;
        have_core = true;
      }
    }
    if (have_core) g.core_clock_mhz = core;

    // Memory clock: mem_clock (MHz) -> pp_dpm_mclk.
    if (c->mem_clock.valid()) {
      if (const auto v = parse::to_u64(parse::trim(c->mem_clock.read())))
        g.mem_clock_mhz = static_cast<double>(*v);
    } else if (c->pp_dpm_mclk.valid()) {
      if (const auto v = parse_pp_dpm_mhz(c->pp_dpm_mclk.read()))
        g.mem_clock_mhz = *v;
    }

    // Power: amdgpu hwmon power1_* values are instantaneous microwatts (PPT),
    // *not* cumulative counters — no rate calculation, no wraparound.
    std::optional<double> ppt;
    if (c->hwmon_power.valid()) {
      if (const auto v = parse::to_u64(parse::trim(c->hwmon_power.read())))
        ppt = static_cast<double>(*v) / 1e6;
    }
    if (!ppt && c->hwmon_power_avg.valid()) {
      if (const auto v = parse::to_u64(parse::trim(c->hwmon_power_avg.read())))
        ppt = static_cast<double>(*v) / 1e6;
    }
    if (ppt) g.ppt_w = *ppt;  // always kept, raw, for --probe/debugging

    if (c->hwmon_temp.valid()) {
      g.temp_c = parse_millidegrees(c->hwmon_temp.read());
    }

    // One-time gpu_metrics read: only while active (reading can wake the
    // GPU), cached afterwards.
    if (!c->metrics_format && c->gpu_metrics.valid()) {
      if (const auto fmt = parse_gpu_metrics_format(c->gpu_metrics.read()))
        c->metrics_format = fmt;
    }
  }

  const auto dec = decide(*c, g.vram_total_bytes);
  g.is_integrated = dec.integrated;
  g.integrated_method = dec.describe();

  // APU split: the PPT reading of an integrated GPU is WHOLE-PACKAGE power
  // (CPU + iGPU + SoC), so it must not be reported as this card's power.
  // It is lifted into CpuSample::package_power_w by the Collector; here
  // power_w stays nullopt and the raw value remains in ppt_w.
  if (g.is_integrated) {
    g.power_w = std::nullopt;
  } else if (g.ppt_w) {
    g.power_w = g.ppt_w;  // discrete card: PPT is the card's power
  }

  out = std::move(g);
}

SourceStatus GpuAmdBackend::probe_info() {
  SourceStatus st;
  st.description = name();

  if (_cards.empty()) {
    st.detail = "no AMD devices found";
    return st;
  }

  st.available = true;
  st.detail = std::to_string(_cards.size()) + " AMD device(s)";
  if (_ids && _ids->loaded()) st.detail += "; names from " + _ids->source();

  // Per-card evidence: every file tried and its state, plus how the
  // integrated flag was decided and what the PPT reading means.
  for (const auto& c : _cards) {
    st.detail += "\n  " + c.dev.bus_id + "  " + c.name + (c.dev.boot_vga ? "  [boot_vga]" : "");
    for (const auto& e : c.open_errors) st.detail += "\n    " + e;
    const auto line = [&](const char* label, const SysfsFile& f) {
      if (!f.valid()) st.detail += std::string("\n    ") + label + ": absent";
      // open failures already reported above; present files are unremarkable.
    };
    line("gpu_clock", c.gpu_clock);
    line("mem_clock", c.mem_clock);
    line("pp_dpm_sclk", c.pp_dpm_sclk);
    line("pp_dpm_mclk", c.pp_dpm_mclk);
    line("hwmon/power1_input", c.hwmon_power);
    line("hwmon/power1_average", c.hwmon_power_avg);
    line("hwmon/temp1_input", c.hwmon_temp);
    line("hwmon/freq1_input", c.hwmon_freq);
    line("gpu_metrics", c.gpu_metrics);
    line("power/runtime_status", c.runtime_status);

    const auto dec = decide(c, std::nullopt);
    st.detail += "\n    integrated: " + dec.describe();
    if (!c.metrics_format) {
      st.detail +=
          "  (gpu_metrics pending: device not active at probe; read on first "
          "active tick)";
    }
    if (dec.integrated) {
      st.detail +=
          "\n    power: PPT (whole-package, includes iGPU) — reported as CPU "
          "package power";
    } else {
      st.detail += "\n    power: PPT (card power)";
    }
  }
  return st;
}

}  // namespace geth
