// RAM and swap usage from /proc/meminfo.
//
// RAM used is MemTotal - MemAvailable, which matches KDE System Monitor (and
// "used" as the rest of the desktop world understands it), not the older
// MemTotal - MemFree which ignores reclaimable cache.
#pragma once

#include "core/source.hpp"
#include "util/sysfs_file.hpp"

#include <string_view>

namespace geth {

struct Meminfo {
  bool ok = false;  // MemTotal parsed
  unsigned long long mem_total_kb = 0;
  unsigned long long mem_available_kb = 0;
  unsigned long long swap_total_kb = 0;
  unsigned long long swap_free_kb = 0;
};

// Pure over string_view for testing.
Meminfo parse_meminfo(std::string_view text);

class MemorySource : public Source {
 public:
  const char* name() const override { return "Memory"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  SysfsFile _meminfo;  // /proc/meminfo
};

}  // namespace geth
