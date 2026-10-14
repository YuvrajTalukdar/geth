// PCI enumeration for GPU discovery — the single source of truth for
// "which GPUs exist on this machine".
//
// The backends (NVIDIA/AMD) do NOT enumerate /sys/class/drm themselves: the
// registry discovers every PCI display controller (class 0x03xxxx) once, and
// each backend CLAIMS the devices it can drive (GpuBackend::claims()). A
// device no backend claims (e.g. Intel i915/xe, nouveau, unbound) is still
// reported by --probe as "detected, no backend yet" — so geth never silently
// forgets a GPU it cannot read.
//
// Everything is testable over a fake sysfs tree: both roots are parameters.
#pragma once

#include <string>
#include <vector>

namespace geth {

struct PciDevice {
  std::string bus_id;  // "0000:01:00.0"
  std::string dev_path;  // canonical /sys/devices/... device directory
  unsigned vendor_id = 0;
  unsigned device_id = 0;
  unsigned subsystem_vendor_id = 0;
  unsigned subsystem_device_id = 0;
  std::string driver;  // bound driver name ("nvidia", "amdgpu", "i915" ...);
                       // empty when the device is unbound
  int drm_card = -1;  // /sys/class/drm/cardN owning this device; -1 = none
  bool boot_vga = false;  // owns the BIOS framebuffer
  std::string runtime_status;  // raw power/runtime_status ("" if absent);
                               // reading it is safe — a kernel counter
};

// Enumerates PCI devices with class 0x03xxxx (display controllers) under
// `root` (default /sys). Results are sorted by bus_id.
//
// DRM card matching: cardN/device is a symlink to the device directory, so
// both sides are canonicalized and compared — no assumption about where the
// PCI device directory physically lives.
std::vector<PciDevice> discover_pci_gpus(const std::string& root = "/sys",
                                         const std::string& drm_root = "/sys/class/drm");

}  // namespace geth
