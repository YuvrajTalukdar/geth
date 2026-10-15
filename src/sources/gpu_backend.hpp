// GPU backend abstraction.
//
// A backend knows how to read metrics for one family of GPUs (NVIDIA via
// NVML, AMD via amdgpu's sysfs). The registry (gpu_registry.{hpp,cpp}) is
// the ONLY file that lists backends; each discovered PCI device is handed to
// the FIRST backend that claims it, and devices no backend claims are still
// reported by --probe as "detected, no backend yet".
//
// Adding a backend = one new file pair + one line in gpu_registry.cpp.
//
// Lifecycle:
//   probe:  discover devices -> claims -> attach(dev) per claim -> probe_info()
//   sample: per claimed device, sample(dev, g) appends one GpuSample
//
// The runtime-status reader is injectable (see RuntimeStatusReader) so the
// suspend lifecycle can be unit-tested without a real GPU. When no reader is
// injected, backends use the power/runtime_status SysfsFile they opened at
// attach (open-once, pread per tick — the read is a kernel counter and never
// wakes the device).
#pragma once

#include "core/snapshot.hpp"
#include "core/source.hpp"
#include "sources/pci_discovery.hpp"
#include "util/sysfs_file.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace geth {

// ---------------------------------------------------------------------------
// runtime PM state (shared by all GPU backends)
// ---------------------------------------------------------------------------

// device/power/runtime_status values. The kernel writes exactly one of:
//   active | suspended | suspending | resuming | error | unsupported
enum class RuntimeState { kActive, kResuming, kSleeping, kError, kUnsupported };

// Pure over string_view for testing. "unsupported" means runtime PM is
// disabled for this device, i.e. it is always awake: that is *active*, not
// sleeping. Unknown text is conservatively treated as active too — a device
// we cannot read is not one we may declare asleep.
RuntimeState parse_runtime_status(std::string_view text);

// Reads a device's power/runtime_status, returning the raw text ("" if
// absent). Injectable for tests (the sleep-path test scripts it).
using RuntimeStatusReader = std::function<std::string(const PciDevice&)>;

class GpuBackend {
 public:
  virtual ~GpuBackend() = default;

  // Family name ("NVIDIA", "AMD").
  virtual const char* name() const = 0;

  // Does this backend drive the device? (vendor id + bound driver)
  virtual bool claims(const PciDevice& dev) const = 0;

  // Attach one claimed device: open its sysfs files once, remember it.
  // Called once at probe, before probe_info().
  virtual void attach(const PciDevice& dev) = 0;

  // One sample for one attached device.
  virtual void sample(const PciDevice& dev, GpuSample& out) = 0;

  // Per-backend contribution to the --probe report (per-device evidence,
  // match/mismatch reports, capability notes). Called at probe, after all
  // attach() calls.
  virtual SourceStatus probe_info() = 0;

  // Called before probe(): the mode in which the whole run is happening.
  // Backends whose probe_info() can wake the driver (the NVIDIA backend's
  // brief NVML enumeration session) run that work ONLY in probe mode —
  // UI/dump startup must never open an NVML session, even to enumerate.
  // The default implementation is a no-op.
  virtual void set_probe_mode(bool) {}

  // Once per tick, before any of the backend's sample() calls: per-tick
  // housekeeping (the NVIDIA backend uses it for its NVML suspend streak).
  virtual void begin_tick() {}

  // Driver version for the --probe header ("" if the family has no
  // readable version here).
  virtual std::string driver_version() const { return ""; }

  // Test seam: swap in a scripted runtime-status reader.
  void set_runtime_reader(RuntimeStatusReader r) { _read_runtime = std::move(r); }

 protected:
  // Injected reader; null = use the SysfsFile opened at attach.
  RuntimeStatusReader _read_runtime;
};

}  // namespace geth
