// AMD GPU data from amdgpu's sysfs interface (all files live in the PCI
// device directory — the same one PCI discovery hands us):
//   gpu_busy_percent                          usage %
//   mem_info_vram_used|total                  VRAM (bytes)
//   gpu_clock | mem_clock                     clocks (MHz, discrete)
//   pp_dpm_sclk | pp_dpm_mclk                 current clock (APUs)
//   gpu_metrics                               APU/dGPU detection
//   hwmon/hwmonX/{power1_input,power1_average,temp1_input,freq1_input}
//     amdgpu hwmon: power in microwatts (instantaneous, not a counter!),
//     temperature in millidegrees, freq1 is the shader clock in Hz.
//   power/runtime_status                      runtime-PM state
//
// There is no userspace API library on the AMD side, so this is pure sysfs.
// GPU names come from the pci.ids database (amdgpu exposes no product string).
//
// NOTE: reading gpu_metrics makes the driver query the SMU and can *wake a
// suspended GPU*, so it is read only while the device reports active, and
// only once (the result is cached). Reading runtime_status itself is safe:
// it reports a kernel counter without touching device registers.
//
// POWER: amdgpu's hwmon power1_input is PPT (power capping) — an
// instantaneous reading. On a DISCRETE card that is the card's power. On an
// APU (integrated GPU) it is the WHOLE-PACKAGE power: CPU + iGPU + SoC
// (proven: 44-51 W during a CPU-only compile with the iGPU at 0%). So for
// integrated cards power_w stays nullopt, the raw value is kept as ppt_w,
// and the Collector lifts it into CpuSample::package_power_w.
#pragma once

#include "sources/gpu_backend.hpp"  // GpuBackend, RuntimeState, RuntimeStatusReader
#include "util/pci_ids.hpp"
#include "util/sysfs_file.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace geth {

// ---------------------------------------------------------------------------
// Pure parsers for the AMD-specific file formats
// ---------------------------------------------------------------------------

// gpu_metrics header (struct metrics_table_header, kgd_pp_interface.h):
//   u16 structure_size; u8 format_revision; u8 content_revision;
// format_revision 1 = discrete GPU, 2 or 3 = APU.
std::optional<int> parse_gpu_metrics_format(std::string_view buf);

// pp_dpm_sclk / pp_dpm_mclk: one "N: <freq><unit>" level per line, the active
// level marked with '*'. Returns the active level in MHz.
std::optional<double> parse_pp_dpm_mhz(std::string_view text);

// ---------------------------------------------------------------------------
// Integrated-GPU decision (pure, so it is unit-testable)
// ---------------------------------------------------------------------------

enum class AmdIntegratedMethod {
  kGpuMetrics,        // format_revision from gpu_metrics (authoritative)
  kApuDeviceId,       // PCI device id on the known-APU list
  kSmallVram,         // VRAM total <= 2 GiB (APUs share system memory)
  kBootVgaTiebreak,   // last resort; see note below
  kDefaultDiscrete,   // nothing matched
};

struct AmdIntegratedDecision {
  bool integrated = false;
  AmdIntegratedMethod method = AmdIntegratedMethod::kDefaultDiscrete;
  int metrics_format = -1;      // set when method == kGpuMetrics
  unsigned device_id = 0;       // set when method == kApuDeviceId
  // "gpu_metrics (format 2)", "APU device id 0x1638", ...
  std::string describe() const;

  // Struct equality (no strings): lets the backend re-evaluate the decision
  // every tick and touch the describe() string only when it actually
  // changes — the per-tick path must not allocate.
  bool operator==(const AmdIntegratedDecision& o) const {
    return integrated == o.integrated && method == o.method &&
           metrics_format == o.metrics_format && device_id == o.device_id;
  }
};

// Specified decision order:
//  1. gpu_metrics format_revision: 1 = dGPU, 2/3 = APU.
//  2. PCI device id on a small known-APU list (Cezanne, Renoir, Rembrandt,
//     Phoenix, Hawk Point, Strix).
//  3. VRAM total <= 2 GiB: APUs report only their slice of shared system
//     memory; dedicated cards report their full VRAM. (A 1 GiB dGPU would be
//     misclassified — accepted for a heuristic of this layer.)
//  4. boot_vga + multi-GPU: the LAST resort, because it is unreliable — on
//     MUX laptops in dGPU-only mode the *discrete* card owns the BIOS
//     framebuffer, and desktops always boot on their dGPU. It only helps on
//     iGPU+dGPU laptops where the iGPU owns the BIOS framebuffer.
AmdIntegratedDecision decide_amd_integrated(
    std::optional<int> metrics_format_revision, std::optional<unsigned> pci_device_id,
    std::optional<double> vram_total_bytes, bool boot_vga, bool multi_gpu);

// The known-APU iGPU PCI device ids (vendor 0x1002), from pci.ids.
const std::vector<unsigned>& kApuDeviceIds();

// Assigns stable gpu0/gpu1/... ids in PCI bus order. Call once after all GPU
// sources have sampled. is_integrated is decided by the sources themselves
// (decide_amd_integrated); this only sorts and names.
void finalize_gpus(std::vector<GpuSample>& gpus);

// ---------------------------------------------------------------------------

class GpuAmdBackend : public GpuBackend {
 public:
  static constexpr unsigned kVendorId = 0x1002;

  // `ids` must outlive the backend (the registry owns its PciIds and loads
  // the needed (vendor, device) subset onto it at probe).
  explicit GpuAmdBackend(const PciIds& ids) : _ids(&ids) {}

  const char* name() const override { return "AMD"; }
  bool claims(const PciDevice& dev) const override;
  void attach(const PciDevice& dev) override;
  void sample(const PciDevice& dev, GpuSample& out) override;
  SourceStatus probe_info() override;

 private:
  struct Card {
    PciDevice dev;
    std::string name;
    // Read once at attach (driver memory only — safe while suspended); used
    // by the VRAM-based integrated fallback on every tick.
    std::optional<double> vram_total_bytes;
    // Cache of the one-time gpu_metrics read (reading can wake the GPU, so
    // it happens at most once, and only while the device is active).
    std::optional<int> metrics_format;
    // Evidence: files whose open failed with a non-ENOENT error.
    std::vector<std::string> open_errors;
    SysfsFile runtime_status;
    SysfsFile busy;      // gpu_busy_percent (0-100)
    SysfsFile vram_used;  // mem_info_vram_used (bytes)
    SysfsFile vram_total;  // mem_info_vram_total (bytes)
    SysfsFile gpu_clock;  // gpu_clock (MHz) — absent on some ASICs/when off
    SysfsFile mem_clock;  // mem_clock (MHz)
    SysfsFile pp_dpm_sclk;  // current sclk level (APUs)
    SysfsFile pp_dpm_mclk;  // current mclk level (APUs)
    SysfsFile gpu_metrics;  // APU/dGPU format_revision (read once, when active)
    SysfsFile gtt_used;     // mem_info_gtt_used (bytes) — absent on many APUs
    SysfsFile gtt_total;    // mem_info_gtt_total (bytes)
    SysfsFile hwmon_power;   // power1_input (microwatts, PPT)
    SysfsFile hwmon_power_avg;  // power1_average (microwatts)
    SysfsFile hwmon_temp;   // temp1_input (millidegrees)
    SysfsFile hwmon_freq;   // freq1_input (Hz) — core-clock fallback
    // Integrated decision, cached so the per-tick path re-evaluates (cheap,
    // struct compare) but only touches the describe() string on a change.
    AmdIntegratedDecision decision;
    std::string method_str;
  };

  Card* card(const PciDevice& dev);
  const Card* card(const PciDevice& dev) const;
  std::string runtime_state(const PciDevice& dev) const;
  AmdIntegratedDecision decide(const Card& c, std::optional<double> live_vram) const;

  std::vector<Card> _cards;
  const PciIds* _ids;
};

}  // namespace geth
