// CPU package temperature.
//
// The search order and per-driver label preferences are DATA in
// sensor_tables.hpp (cpu_temp_preferences()): hwmon drivers k10temp,
// zenpower, coretemp, then the x86_pkg_temp thermal zone, then acpitz.
// Values are in millidegrees; negative values mean "no data".
#pragma once

#include "core/source.hpp"
#include "util/sysfs_file.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace geth {

// Picks which tempN of a hwmon device to trust, given the driver name and
// the tempN_label contents (1-based index -> label). nullopt = no match.
std::optional<std::size_t> choose_temp_index(const std::string& driver,
                                             const std::map<std::size_t, std::string>& labels);

// Parses a hwmon millidegree reading. Negative values (e.g. -127000) are how
// hwmon signals "not reading", so they map to nullopt rather than -127 °C.
std::optional<double> parse_millidegrees(std::string_view text);

class CpuTempSource : public Source {
 public:
  const char* name() const override { return "CPU temperature"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  SysfsFile _value;  // whichever sensor probe() picked
  std::string _what;  // e.g. "k10temp Tctl" — shown in --probe
};

}  // namespace geth
