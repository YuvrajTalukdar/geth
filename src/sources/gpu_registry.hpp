// The GPU registry: the ONLY file that lists GPU backends.
//
// discover_pci_gpus() finds every PCI display controller (class 0x03xxxx) —
// the single source of truth for which GPUs exist. Each device goes to the
// FIRST backend that claims it. Devices no backend claims (Intel i915/xe,
// nouveau, unbound) still appear in --probe as "detected, no backend yet"
// with vendor name, driver and bus id — geth never silently forgets a GPU it
// cannot read.
//
// Adding a backend: implement GpuBackend in a new file pair, then add ONE
// line to the backend list in the constructor below. Nothing else changes.
#pragma once

#include "core/source.hpp"
#include "sources/gpu_backend.hpp"
#include "sources/gpu_nvidia.hpp"  // nvidia_backend() test seam
#include "sources/pci_discovery.hpp"
#include "util/pci_ids.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace geth {

class GpuRegistrySource : public Source {
 public:
  GpuRegistrySource();

  const char* name() const override { return "GPU"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

  // For the --probe system header.
  std::string pci_ids_source() const;
  std::string nvidia_driver_version() const;

  // Test seams (the live machine uses real /sys and the system pci.ids).
  void set_sysfs_roots(std::string root, std::string drm_root) {
    _root = std::move(root);
    _drm_root = std::move(drm_root);
  }
  void set_pci_ids_path(std::string path) { _pci_ids_path = std::move(path); }

  // Forwarded to every backend: true only for --probe, so the NVIDIA backend
  // runs its brief NVML enumeration session there and never at UI/dump start.
  void set_probe_mode(bool on) {
    for (auto& b : _backends) b->set_probe_mode(on);
  }

  // For the A5 test: reach the NVIDIA backend to install a fake NVML table
  // with call counters (returns nullptr if there is no NVIDIA backend).
  GpuNvidiaBackend* nvidia_backend() {
    for (auto& b : _backends)
      if (const auto* n = dynamic_cast<GpuNvidiaBackend*>(b.get())) return const_cast<GpuNvidiaBackend*>(n);
    return nullptr;
  }

  // For tests: what was discovered, and what no backend claimed.
  const std::vector<PciDevice>& devices() const { return _devices; }
  const std::vector<const PciDevice*>& unclaimed() const { return _unclaimed; }

 private:
  struct Claim {
    std::size_t device_index;
    GpuBackend* backend;
  };

  std::string _root = "/sys";
  std::string _drm_root = "/sys/class/drm";
  std::string _pci_ids_path;  // empty = first existing system file
  PciIds _ids;  // owned here; backends reference it (subset-loaded at probe)
  std::vector<std::unique_ptr<GpuBackend>> _backends;
  std::vector<PciDevice> _devices;
  std::vector<Claim> _claims;
  std::vector<const PciDevice*> _unclaimed;
  // The reused Snapshot's gpus vector is filled in place every tick, so the
  // registry fixes the slot->claim order (PCI bus order) once:
  std::vector<std::size_t> _order;
  bool _gpus_sized = false;
};

}  // namespace geth
