#include "sources/gpu_nvidia.hpp"

#include "util/parse.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <fstream>

namespace geth {

namespace {

constexpr int kNvmlSuccess = 0;
// NVML_CLOCK_DOMAIN_*: GRAPHICS=0, SM=1, MEMORY=2, VIDEO=3.
// Note: domain 1 is SM, NOT memory — reading 1 for "memory" is what made
// graphics and memory clocks report the same number.
constexpr int kClockGraphics = 0;
constexpr int kClockMemory = 2;
constexpr int kTempGpu = 0;  // NVML_TEMPERATURE_GPU

struct NvmlMemory {
  unsigned long long total;
  unsigned long long free_;
  unsigned long long used;
};

struct NvmlUtil {
  unsigned int gpu;
  unsigned int memory;
};

template <typename T>
T resolve(void* lib, const char* v2_name, const char* v1_name) {
  void* p = dlsym(lib, v2_name);
  // v1_name may be null (no legacy spelling); dlsym()'s name argument is
  // nonnull, so guard the second lookup.
  if (!p && v1_name) p = dlsym(lib, v1_name);
  return reinterpret_cast<T>(p);
}

}  // namespace

// ---------------------------------------------------------------------------
// pure matching
// ---------------------------------------------------------------------------

std::optional<std::string> normalize_pci_bus_id(std::string_view id) {
  id = parse::trim(id);
  // "DDDD:BB:DD.F" — 4:2:2 hex plus one hex function digit, domain included.
  // Positions: 0-3 domain, 4 ':', 5-6 bus, 7 ':', 8-9 device, 10 '.', 11 fn.
  if (id.size() != 12 || id[4] != ':' || id[7] != ':' || id[10] != '.')
    return std::nullopt;
  auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  const int start[4] = {0, 5, 8, 11};
  const int len[4] = {4, 2, 2, 1};
  std::string out;
  out.reserve(12);
  for (int g = 0; g < 4; ++g) {
    for (int i = 0; i < len[g]; ++i) {
      char c = id[start[g] + i];
      if (!is_hex(c)) return std::nullopt;
      if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
      out += c;
    }
    if (g == 2)
      out += '.';
    else if (g < 2)
      out += ':';
  }
  return out;
}

NvmlDeviceMatch match_nvml_devices(const std::vector<std::string>& sysfs_ids,
                                   const std::vector<std::string>& nvml_ids) {
  NvmlDeviceMatch m;

  std::map<std::string, int> nvml_by_id;
  for (std::size_t i = 0; i < nvml_ids.size(); ++i)
    if (const auto n = normalize_pci_bus_id(nvml_ids[i]))
      nvml_by_id[*n] = static_cast<int>(i);

  for (std::size_t i = 0; i < sysfs_ids.size(); ++i) {
    const auto n = normalize_pci_bus_id(sysfs_ids[i]);
    if (!n) {
      m.sysfs_unmatched.push_back(static_cast<int>(i));
      continue;
    }
    const auto it = nvml_by_id.find(*n);
    if (it == nvml_by_id.end())
      m.sysfs_unmatched.push_back(static_cast<int>(i));
    else
      m.matched.emplace_back(static_cast<int>(i), it->second);
  }

  for (std::size_t i = 0; i < nvml_ids.size(); ++i) {
    bool matched = false;
    for (const auto& [s, n] : m.matched)
      if (n == static_cast<int>(i)) {
        matched = true;
        break;
      }
    if (!matched) m.nvml_unmatched.push_back(nvml_ids[i]);
  }
  return m;
}

// ---------------------------------------------------------------------------
// session lifecycle
// ---------------------------------------------------------------------------

GpuNvidiaBackend::~GpuNvidiaBackend() { close_session(); }

bool GpuNvidiaBackend::open_session(Enum& e) {
  _session_error.clear();
  if (!_has_fake_api) {
    if (!_lib) {
      _lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
      if (!_lib) _lib = dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!_lib) {
      _session_error = "libnvidia-ml.so.1 not found";
      return false;
    }
    _api.init = resolve<int (*)()>(_lib, "nvmlInit_v2", "nvmlInit");
    _api.shutdown = resolve<void (*)()>(_lib, "nvmlShutdown", nullptr);
    _api.get_count =
        resolve<int (*)(unsigned*)>(_lib, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
    _api.get_handle =
        resolve<int (*)(unsigned, void**)>(_lib, "nvmlDeviceGetHandleByIndex_v2",
                                           "nvmlDeviceGetHandleByIndex");
    _api.get_name = resolve<int (*)(void*, char*, unsigned)>(_lib, "nvmlDeviceGetName",
                                                             nullptr);
    _api.get_pci =
        resolve<int (*)(void*, void*)>(_lib, "nvmlDeviceGetPciInfo_v2", "nvmlDeviceGetPciInfo");
    _api.get_util = resolve<int (*)(void*, unsigned*, unsigned*)>(
        _lib, "nvmlDeviceGetUtilizationRates", nullptr);
    _api.get_mem =
        resolve<int (*)(void*, unsigned long long*, unsigned long long*,
                        unsigned long long*)>(_lib, "nvmlDeviceGetMemoryInfo", nullptr);
    _api.get_clock =
        resolve<int (*)(void*, int, unsigned*)>(_lib, "nvmlDeviceGetClockInfo", nullptr);
    _api.get_power =
        resolve<int (*)(void*, int*)>(_lib, "nvmlDeviceGetPowerUsage", nullptr);
    _api.get_temp =
        resolve<int (*)(void*, int, unsigned*)>(_lib, "nvmlDeviceGetTemperature", nullptr);
    _api.get_driver_version =
        resolve<int (*)(char*, unsigned)>(_lib, "nvmlSystemGetDriverVersion", nullptr);
    if (!_api.init || !_api.get_count || !_api.get_handle) {
      _session_error = "NVML entry points not found in library";
      close_session();
      return false;
    }
  }

  if (_api.init() != kNvmlSuccess) {
    _session_error = "nvmlInit failed (driver not ready?)";
    close_session();
    return false;
  }
  _nvml_up = true;

  unsigned count = 0;
  if (_api.get_count(&count) != kNvmlSuccess) {
    _session_error = "nvmlDeviceGetCount failed";
    close_session();
    return false;
  }

  e.ids.resize(count);
  e.handles.assign(count, nullptr);
  e.names.resize(count);
  for (unsigned i = 0; i < count; ++i) {
    void* h = nullptr;
    if (_api.get_handle(i, &h) != kNvmlSuccess) continue;
    e.handles[i] = h;
    if (_api.get_pci) {
      // Opaque buffer, sized larger than the largest documented
      // nvmlPciInfo_t (132 bytes as of NVML 555); only the leading
      // busId[32] is read.
      unsigned char pci[256] = {0};
      if (_api.get_pci(h, pci) == kNvmlSuccess)
        e.ids[i] = reinterpret_cast<const char*>(pci);
    }
    if (_api.get_name) {
      char nb[96] = {0};
      if (_api.get_name(h, nb, sizeof nb) == kNvmlSuccess) e.names[i] = nb;
    }
  }
  return true;
}

void GpuNvidiaBackend::close_session() {
  if (_nvml_up && _api.shutdown) _api.shutdown();
  if (_lib) dlclose(_lib);
  _lib = nullptr;
  _nvml_up = false;
  _api = _has_fake_api ? _fake_api : NvmlApi{};
  for (auto& c : _cards) {
    c.handle = nullptr;
    c.nvml_index = -1;
  }
}

bool GpuNvidiaBackend::ensure_session() {
  if (_nvml_up) return true;
  Enum e;
  if (!open_session(e)) return false;

  // Strict matching: normalized PCI bus id identity only. A device that does
  // not match in either direction simply gets no handle — and is reported
  // as unmatched by probe_info().
  std::vector<std::string> sysfs_ids;
  sysfs_ids.reserve(_cards.size());
  for (const auto& c : _cards) sysfs_ids.push_back(c.dev.bus_id);
  const auto match = match_nvml_devices(sysfs_ids, e.ids);

  for (std::size_t si = 0; si < _cards.size(); ++si) {
    _cards[si].handle = nullptr;
    _cards[si].nvml_index = -1;
    for (const auto& [s, n] : match.matched) {
      if (s != static_cast<int>(si)) continue;
      _cards[si].handle = e.handles[n];
      _cards[si].nvml_index = n;
      // NVML's name is authoritative; keep it over the pci.ids fallback.
      if (!e.names[n].empty()) _cards[si].name = e.names[n];
      break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// GpuBackend interface
// ---------------------------------------------------------------------------

bool GpuNvidiaBackend::claims(const PciDevice& dev) const {
  return dev.vendor_id == kVendorId && dev.driver == "nvidia";
}

void GpuNvidiaBackend::attach(const PciDevice& dev) {
  Card c;
  c.dev = dev;
  if (_ids)
    if (auto n = _ids->lookup(dev.vendor_id, dev.device_id)) c.name = *n;
  if (c.name.empty()) c.name = "NVIDIA GPU [" + dev.bus_id + "]";
  int err = 0;
  SysfsFile::open(dev.dev_path + "/power/runtime_status", c.runtime_status, err);
  _cards.push_back(std::move(c));
}

GpuNvidiaBackend::Card* GpuNvidiaBackend::card(const PciDevice& dev) {
  for (auto& c : _cards)
    if (c.dev.bus_id == dev.bus_id) return &c;
  return nullptr;
}

const GpuNvidiaBackend::Card* GpuNvidiaBackend::card(const PciDevice& dev) const {
  for (const auto& c : _cards)
    if (c.dev.bus_id == dev.bus_id) return &c;
  return nullptr;
}

std::string GpuNvidiaBackend::runtime_state(const PciDevice& dev) const {
  if (_read_runtime) return _read_runtime(dev);
  if (const Card* c = card(dev))
    if (c->runtime_status.valid()) return c->runtime_status.read();
  return "";
}

void GpuNvidiaBackend::begin_tick() {
  _ensured_this_tick = false;
  if (_cards.empty()) return;

  bool all_suspended = true;
  for (const auto& c : _cards)
    if (parse_runtime_status(runtime_state(c.dev)) != RuntimeState::kSleeping)
      all_suspended = false;

  if (all_suspended) {
    ++_suspend_streak;
    // A live NVML session may keep the GPU awake: once every card has been
    // suspended for kShutdownStreak consecutive ticks, close the session.
    // It is re-opened lazily on the next active tick.
    if (_suspend_streak >= kShutdownStreak && _nvml_up) close_session();
  } else {
    _suspend_streak = 0;
  }
}

void GpuNvidiaBackend::sample(const PciDevice& dev, GpuSample& out) {
  Card* c = card(dev);
  if (!c) return;  // not ours (defensive)

  // Identity: written on the first fill (empty pci_bus_id). NVML's name can
  // only replace the pci.ids fallback once the session matches, so refresh
  // it while it differs — a one-time change, not per-tick churn.
  if (out.pci_bus_id.empty()) {
    out.vendor = GpuVendor::kNvidia;
    out.name = c->name;
    out.pci_bus_id = dev.bus_id;
    out.is_integrated = false;  // NVIDIA has no consumer iGPU
    out.integrated_method = "always discrete (no NVIDIA iGPUs)";
  } else if (out.name != c->name) {
    out.name = c->name;
  }

  const auto rs = parse_runtime_status(runtime_state(dev));
  switch (rs) {
    case RuntimeState::kSleeping: out.state = GpuState::kSleeping; break;
    case RuntimeState::kError: out.state = GpuState::kError; break;
    default: out.state = GpuState::kActive; break;
  }

  const bool awake =
      rs == RuntimeState::kActive || rs == RuntimeState::kUnsupported;
  // "resuming" counts as active for the streak, but the driver is not ready
  // to serve queries yet: no session work, no reads this tick.
  if (awake && !_nvml_up && !_ensured_this_tick) {
    ensure_session();
    _ensured_this_tick = true;
  }

  if (awake && _nvml_up && (c = card(dev)) && c->handle) {
    NvmlUtil u{};
    if (_api.get_util && _api.get_util(c->handle, &u.gpu, &u.memory) == kNvmlSuccess)
      out.usage_pct = static_cast<double>(u.gpu);

    NvmlMemory m{};
    if (_api.get_mem &&
        _api.get_mem(c->handle, &m.total, &m.free_, &m.used) == kNvmlSuccess) {
      out.vram_used_bytes = static_cast<double>(m.used);
      out.vram_total_bytes = static_cast<double>(m.total);
    }

    unsigned clk = 0;
    if (_api.get_clock &&
        _api.get_clock(c->handle, kClockGraphics, &clk) == kNvmlSuccess)
      out.core_clock_mhz = static_cast<double>(clk);
    if (_api.get_clock &&
        _api.get_clock(c->handle, kClockMemory, &clk) == kNvmlSuccess)
      out.mem_clock_mhz = static_cast<double>(clk);

    int mw = 0;
    if (_api.get_power && _api.get_power(c->handle, &mw) == kNvmlSuccess)
      out.power_w = static_cast<double>(mw) / 1000.0;  // NVML reports milliwatts

    unsigned t = 0;
    if (_api.get_temp && _api.get_temp(c->handle, kTempGpu, &t) == kNvmlSuccess)
      out.temp_c = static_cast<double>(t);
  }
}

SourceStatus GpuNvidiaBackend::probe_info() {
  SourceStatus st;
  st.description = name();

  if (_cards.empty()) {
    st.detail = "no NVIDIA devices found";
    return st;
  }

  st.available = true;
  st.detail = std::to_string(_cards.size()) + " NVIDIA device(s)";

  Enum e;
  if (open_session(e)) {
    // Brief enumeration session: it exists only so --probe can match NVML
    // handles to sysfs devices by bus id and report mismatches in BOTH
    // directions. It is closed immediately; the sample-time session is
    // opened lazily later. Enumeration reads driver metadata, not GPU
    // registers.
    std::vector<std::string> sysfs_ids;
    for (const auto& c : _cards) sysfs_ids.push_back(c.dev.bus_id);
    const auto match = match_nvml_devices(sysfs_ids, e.ids);

    for (std::size_t si = 0; si < _cards.size(); ++si) {
      const auto& c = _cards[si];
      std::string line = "  " + c.dev.bus_id + "  " + c.name;
      for (const auto& [s, n] : match.matched) {
        if (s != static_cast<int>(si)) continue;
        if (!e.names[n].empty()) line += "  (NVML: " + e.names[n] + ")";
        line += "  [matched by PCI bus id]";
        break;
      }
      if (line.find("matched by PCI bus id") == std::string::npos)
        line += "  UNMATCHED: no NVML handle for this device";
      st.detail += "\n" + line;
    }
    for (const auto& id : match.nvml_unmatched)
      st.detail += "\n  UNMATCHED: NVML device " + id + " has no PCI device in /sys";
    close_session();
    st.detail +=
        "\n  NVML session opened lazily on first active sample, dropped after " +
        std::to_string(kShutdownStreak) + " suspended ticks";
  } else {
    close_session();  // releases the partially-opened library handle
    st.detail += "\n  " + _session_error;
    st.hint = "install the NVIDIA driver to read GPU data";
  }
  return st;
}

std::string GpuNvidiaBackend::driver_version() const {
  // nvmlSystemGetDriverVersion only if the NVML session is already live;
  // otherwise the plain file read, which never wakes the GPU.
  if (_nvml_up && _api.get_driver_version) {
    char v[80] = {0};
    if (_api.get_driver_version(v, sizeof v) == kNvmlSuccess) return v;
  }
  std::ifstream f("/sys/module/nvidia/version");
  std::string v;
  std::getline(f, v);
  return v.empty() ? "" : v;
}

void GpuNvidiaBackend::set_fake_api(NvmlApi api) {
  close_session();
  _api = api;
  _fake_api = api;
  _has_fake_api = true;
}

}  // namespace geth
