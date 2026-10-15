// NVIDIA GPU data via NVML (libnvidia-ml.so.1), loaded with dlopen().
//
// GETH deliberately never links against NVIDIA libraries: the driver library
// is only *looked up* at runtime, so machines without an NVIDIA GPU (or
// driver) simply get an unavailable backend instead of a link failure, and
// no NVIDIA package is a build/runtime dependency.
//
// NVML is opened *lazily*: probe() runs a brief enumeration session
// (init -> device list -> shutdown) purely so --probe can report unmatched
// devices in both directions; the sample-time session is opened on the first
// tick where a card is not suspended. If every card stays suspended for 10
// consecutive ticks, the session is closed again (nvmlShutdown + dlclose) —
// a live session may keep the GPU awake, and an idle geth must not be the
// reason.
//
// Matching is strict: NVML handles are matched to sysfs devices ONLY by
// normalized PCI bus id. A sysfs device with no NVML handle, or an NVML
// handle with no sysfs device, is reported in --probe as unmatched — never
// guessed by position.
//
// ABI notes: the structs passed to NVML are declared by hand. nvmlMemory_t
// (total/free/used u64) and nvmlUtilization_t (gpu/mem u32) are stable ABI;
// nvmlPciInfo_t is not, so it is written into an oversized opaque buffer and
// only its leading busId[32] is read.
#pragma once

#include "sources/gpu_backend.hpp"
#include "util/pci_ids.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace geth {

// ---------------------------------------------------------------------------
// Pure PCI bus id matching (unit-testable, no NVML involved)
// ---------------------------------------------------------------------------

// Normalizes a PCI bus id to uppercase. Accepts only the exact
// "DDDD:BB:DD.F" form (4:2:2 hex + 1 hex function digit); anything else —
// including a missing domain or a lowercase/malformed id — is nullopt.
std::optional<std::string> normalize_pci_bus_id(std::string_view id);

// Pure: match sysfs devices (by bus id) to NVML devices (by bus id).
// Identity-based only — there is no positional fallback: the order NVML
// reports devices in must NOT be trusted.
struct NvmlDeviceMatch {
  std::vector<std::pair<int, int>> matched;  // (sysfs index, nvml index)
  std::vector<int> sysfs_unmatched;          // sysfs devices with no NVML handle
  std::vector<std::string> nvml_unmatched;   // NVML bus ids with no sysfs device
};
NvmlDeviceMatch match_nvml_devices(const std::vector<std::string>& sysfs_ids,
                                   const std::vector<std::string>& nvml_ids);

// ---------------------------------------------------------------------------
// NVML function table (opaque ABI, resolved with dlsym)
// ---------------------------------------------------------------------------

struct NvmlApi {
  int (*init)() = nullptr;
  void (*shutdown)() = nullptr;
  int (*get_count)(unsigned*) = nullptr;
  int (*get_handle)(unsigned, void**) = nullptr;
  int (*get_name)(void*, char*, unsigned) = nullptr;
  int (*get_pci)(void*, void*) = nullptr;
  int (*get_util)(void*, unsigned*, unsigned*) = nullptr;
  int (*get_mem)(void*, unsigned long long*, unsigned long long*,
                 unsigned long long*) = nullptr;
  int (*get_clock)(void*, int, unsigned*) = nullptr;
  int (*get_power)(void*, int*) = nullptr;
  int (*get_temp)(void*, int, unsigned*) = nullptr;
  int (*get_driver_version)(char*, unsigned) = nullptr;
};

// ---------------------------------------------------------------------------

class GpuNvidiaBackend : public GpuBackend {
 public:
  static constexpr unsigned kVendorId = 0x10de;
  static constexpr int kShutdownStreak = 10;  // all-suspended ticks before close

  // `ids` must outlive the backend (the registry owns its PciIds and loads
  // the needed (vendor, device) subset onto it at probe).
  explicit GpuNvidiaBackend(const PciIds& ids) : _ids(&ids) {}
  ~GpuNvidiaBackend() override;
  GpuNvidiaBackend(const GpuNvidiaBackend&) = delete;
  GpuNvidiaBackend& operator=(const GpuNvidiaBackend&) = delete;

  const char* name() const override { return "NVIDIA"; }
  bool claims(const PciDevice& dev) const override;
  void attach(const PciDevice& dev) override;
  void sample(const PciDevice& dev, GpuSample& out) override;
  SourceStatus probe_info() override;
  // The brief NVML enumeration session runs ONLY in probe mode.
  void set_probe_mode(bool on) override { _probe_mode = on; }
  void begin_tick() override;
  std::string driver_version() const override;

  // Test seam: install a fake function table (with call counters); while a
  // fake is set, dlopen() is never attempted.
  void set_fake_api(NvmlApi api);

 private:
  struct Card {
    PciDevice dev;
    std::string name;  // pci.ids fallback; NVML's GetName wins once matched
    int nvml_index = -1;
    void* handle = nullptr;  // valid only while _nvml_up
    SysfsFile runtime_status;  // power/runtime_status (open-once, pread)
  };

  struct Enum {
    std::vector<std::string> ids;      // NVML-reported bus ids
    std::vector<void*> handles;
    std::vector<std::string> names;
  };

  bool open_session(Enum& e);   // dlopen + dlsym + nvmlInit + enumerate
  void close_session();        // nvmlShutdown + dlclose
  bool ensure_session();       // open (if needed) + match handles by bus id
  std::string runtime_state(const PciDevice& dev) const;
  Card* card(const PciDevice& dev);
  const Card* card(const PciDevice& dev) const;

  std::vector<Card> _cards;
  const PciIds* _ids;
  bool _probe_mode = false;  // true only in --probe: enumerate via NVML
  void* _lib = nullptr;
  NvmlApi _api;
  NvmlApi _fake_api;
  bool _has_fake_api = false;
  bool _nvml_up = false;
  bool _ensured_this_tick = false;
  int _suspend_streak = 0;
  std::string _session_error;  // why the last session open failed
};

}  // namespace geth
