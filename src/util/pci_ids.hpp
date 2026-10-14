// Look up human-readable PCI device names from the pci.ids database
// (usually /usr/share/hwdata/pci.ids, older distros use /usr/share/misc/pci.ids).
//
// Needed mainly for AMD GPUs: amdgpu exposes no product string in sysfs, so
// "Radeon 780M" has to come from the device id (0x1002:0x1638) via this table.
//
// The database is ~150k entries; keeping it in memory would cost several MB
// of RSS for a table geth reads a handful of times. So it is NOT kept: it is
// streamed once (single pass) at probe time, and only the (vendor, device)
// -> name pairs that were actually requested are retained.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace geth {

// System pci.ids locations, in preference order.
const std::vector<std::string>& default_pci_ids_paths();

class PciIds {
 public:
  // Picks the first existing system file (possibly none: lookups then fail).
  PciIds();
  // Explicit path (tests use a fixture file).
  explicit PciIds(std::string path);

  bool loaded() const { return _loaded; }
  const std::string& source() const { return _source; }
  // Number of (vendor, device) names retained — tiny by design.
  std::size_t size() const { return _map.size(); }

  // Streams the file once, keeping only the names for the pairs in `wanted`
  // (vendor << 32 | device). A second call is a no-op. Must run before
  // lookup(); lookups before it (or for unrequested pairs) return nullopt.
  void load_subset(const std::vector<std::pair<unsigned, unsigned>>& wanted);

  std::optional<std::string> lookup(unsigned vendor, unsigned device) const;

 private:
  bool _loaded = false;
  bool _subset_loaded = false;
  std::string _source;
  // Key: (vendor << 32) | device — only the requested entries, not the DB.
  std::unordered_map<unsigned long long, std::string> _map;
};

}  // namespace geth
