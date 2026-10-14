#include "sources/gpu_registry.hpp"

#include "sources/gpu_amd.hpp"
#include "sources/gpu_nvidia.hpp"
#include "util/parse.hpp"

namespace geth {

GpuRegistrySource::GpuRegistrySource() {
  // ---- THE BACKEND LIST: the only place backends are named ---------------
  // Each backend references the registry's PciIds; probe() loads the
  // needed (vendor, device) subset onto it before any attach().
  //
  // To add an Intel (or other) backend: implement GpuBackend in a new
  // file pair, then insert ONE line here.
  _backends.push_back(std::make_unique<GpuNvidiaBackend>(_ids));
  _backends.push_back(std::make_unique<GpuAmdBackend>(_ids));
}

std::string GpuRegistrySource::pci_ids_source() const {
  return _ids.loaded() ? _ids.source() : std::string("(none)");
}

std::string GpuRegistrySource::nvidia_driver_version() const {
  for (const auto& b : _backends)
    if (const auto s = b->driver_version(); !s.empty()) return s;
  return "";
}

SourceStatus GpuRegistrySource::probe() {
  SourceStatus st;
  st.description = name();

  // 1) Discover: the single source of truth for which GPUs exist.
  _devices = discover_pci_gpus(_root, _drm_root);
  _claims.clear();
  _unclaimed.clear();
  for (std::size_t i = 0; i < _devices.size(); ++i) {
    GpuBackend* claim = nullptr;
    for (auto& b : _backends)
      if (b->claims(_devices[i])) {
        claim = b.get();
        break;
      }
    if (claim)
      _claims.push_back({i, claim});
    else
      _unclaimed.push_back(&_devices[i]);
  }

  // 2) pci.ids: stream the database once, keep only the ids this machine
  // actually has (never the whole ~150k-entry table).
  std::vector<std::pair<unsigned, unsigned>> wanted;
  wanted.reserve(_devices.size());
  for (const auto& d : _devices) wanted.emplace_back(d.vendor_id, d.device_id);
  _ids = _pci_ids_path.empty() ? PciIds() : PciIds(_pci_ids_path);
  _ids.load_subset(wanted);

  // 3) Attach claimed devices (opens each sysfs file once).
  for (const auto& cl : _claims) cl.backend->attach(_devices[cl.device_index]);

  if (_devices.empty()) {
    st.detail = "no PCI display controllers found";
    st.hint = "no GPU on this machine — expected if it has none";
    return st;  // unavailable
  }

  st.available = true;
  st.detail = std::to_string(_devices.size()) + " GPU(s) detected, " +
              std::to_string(_claims.size()) + " claimed by a backend";

  // 4) Per-backend evidence (file states, NVML match report, ...).
  for (auto& b : _backends) {
    bool any = false;
    for (const auto& cl : _claims)
      if (cl.backend == b.get()) {
        any = true;
        break;
      }
    if (!any) continue;
    const auto bi = b->probe_info();
    const auto lines = parse::split_lines(bi.detail);
    bool first = true;
    for (const auto& l : lines) {
      if (first) {
        st.detail += "\n  " + std::string(b->name()) + ": " + std::string(l);
        first = false;
      } else {
        st.detail += std::string("\n    ") + std::string(l);
      }
    }
    if (!bi.available && !bi.hint.empty())
      st.detail += "\n    fix: " + bi.hint;
  }

  // 5) Unclaimed devices: reported, never forgotten.
  if (!_unclaimed.empty()) {
    st.detail += "\n  unclaimed: " + std::to_string(_unclaimed.size()) +
                 " device(s) — detected, no backend yet";
    for (const auto* d : _unclaimed) {
      std::string n = "unknown";
      if (auto nm = _ids.lookup(d->vendor_id, d->device_id)) n = *nm;
      st.detail += "\n    " + d->bus_id + "  " + n +
                   (d->driver.empty() ? "  driver: (unbound)" : "  driver: " + d->driver);
    }
  }
  return st;
}

void GpuRegistrySource::sample(Snapshot& out) {
  // Per-tick housekeeping first (the NVIDIA backend's suspend streak).
  for (auto& b : _backends) b->begin_tick();

  for (const auto& cl : _claims) {
    GpuSample g;
    cl.backend->sample(_devices[cl.device_index], g);
    out.gpus.push_back(std::move(g));
  }
}

}  // namespace geth
