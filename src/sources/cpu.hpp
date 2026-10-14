// CPU usage (from /proc/stat) + current frequency (cpufreq sysfs, with a
// /proc/cpuinfo fallback). Temperature and power live in temps.cpp and
// cpu_power.cpp.
#pragma once

#include "core/source.hpp"
#include "util/sysfs_file.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace geth {

// The 8 time fields of a /proc/stat cpu line, in file order (jiffies).
struct CpuTimes {
  unsigned long long user = 0;
  unsigned long long nice = 0;
  unsigned long long system = 0;
  unsigned long long idle = 0;
  unsigned long long iowait = 0;
  unsigned long long irq = 0;
  unsigned long long softirq = 0;
  unsigned long long steal = 0;

  // Total is the sum of the FIRST 8 fields only: guest and guest_nice are
  // already included in user/nice, so adding them would double-count.
  unsigned long long total() const {
    return user + nice + system + idle + iowait + irq + softirq + steal;
  }
  // Idle time is idle + iowait, matching how tools like top count it.
  unsigned long long idle_all() const { return idle + iowait; }
};

// Parses the aggregate "cpu" line and the per-core "cpuN" lines of /proc/stat,
// in file order (index 0 = aggregate). Pure over string_view for testing.
std::vector<CpuTimes> parse_proc_stat(std::string_view text);

// Usage % between two consecutive samples of one CPU.
std::optional<double> usage_pct_between(const CpuTimes& prev, const CpuTimes& now);

// "cpu MHz" values of /proc/cpuinfo, one per line (frequency fallback).
std::vector<double> parse_cpuinfo_mhz(std::string_view text);

// The first "model name" value of /proc/cpuinfo (hardware label, e.g.
// "AMD Ryzen 5 5600H with Radeon Graphics"). The probe prints it so the
// machine identification comes from the hardware, not from any spec.
std::optional<std::string> parse_cpuinfo_model_name(std::string_view text);

class CpuSource : public Source {
 public:
  const char* name() const override { return "CPU usage + frequency"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  SysfsFile _stat;                 // /proc/stat
  SysfsFile _cpuinfo;              // /proc/cpuinfo (fallback only)
  std::vector<SysfsFile> _cores;   // scaling_cur_freq (kHz), one per online core
  std::vector<CpuTimes> _prev;     // last tick's counters, aligned with lines
};

}  // namespace geth
