#include "sources/temps.hpp"

#include "sources/sensor_tables.hpp"
#include "util/parse.hpp"

#include <filesystem>
#include <fstream>

namespace geth {

std::optional<std::size_t> choose_temp_index(const std::string& driver,
                                             const std::map<std::size_t, std::string>& labels) {
  const TempPreference* entry = nullptr;
  for (const auto& e : cpu_temp_preferences())
    if (e.driver == driver) {
      entry = &e;
      break;
    }
  if (!entry) return std::nullopt;  // unknown driver: let a later candidate win

  for (const auto& want : entry->preferred_labels)
    for (const auto& [idx, label] : labels)
      if (label == want) return idx;

  // acpitz has no labels worth matching: accept its first sensor.
  if (entry->accept_any && !labels.empty()) return labels.begin()->first;
  return std::nullopt;
}

std::optional<double> parse_millidegrees(std::string_view text) {
  text = parse::trim(text);
  if (text.empty()) return std::nullopt;
  std::size_t i = 0;
  if (text[0] == '-') {
    i = 1;
    // Negative = "no data" in hwmon convention; reject the whole value.
    (void)text;
    return std::nullopt;
  }
  unsigned long long v = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return std::nullopt;
    v = v * 10 + static_cast<unsigned long long>(text[i] - '0');
    if (v > 4000000ull) return std::nullopt;  // 4000 °C is not physical
  }
  return static_cast<double>(v) / 1000.0;
}

namespace {

// tempN_label contents for one hwmon device (index 1..N until missing).
std::map<std::size_t, std::string> read_labels(const std::string& dir) {
  std::map<std::size_t, std::string> labels;
  for (std::size_t i = 1; i <= 32; ++i) {
    std::ifstream f(dir + "/temp" + std::to_string(i) + "_label");
    std::string v;
    std::getline(f, v);
    if (v.empty()) break;
    labels[i] = v;
  }
  return labels;
}

std::optional<std::string> find_hwmon_dir(const std::string& driver) {
  const std::string root = "/sys/class/hwmon";
  if (!std::filesystem::is_directory(root)) return std::nullopt;
  auto it = std::filesystem::directory_iterator(root);
  for (; it != std::filesystem::directory_iterator(); ++it) {
    if (!it->is_directory()) continue;
    std::ifstream f(it->path().string() + "/name");
    std::string name;
    std::getline(f, name);
    if (name == driver) return it->path().string();
  }
  return std::nullopt;
}

}  // namespace

SourceStatus CpuTempSource::probe() {
  SourceStatus st;
  st.description = name();

  auto try_hwmon = [&](const TempPreference& pref) -> bool {
    const auto dir = find_hwmon_dir(pref.driver);
    if (!dir) return false;
    const auto labels = read_labels(*dir);
    const auto idx = choose_temp_index(pref.driver, labels);
    if (!idx) return false;
    int err = 0;
    const std::string vpath = *dir + "/temp" + std::to_string(*idx) + "_input";
    if (!SysfsFile::open(vpath, _value, err)) return false;
    const std::string label = labels.count(*idx) ? " (" + labels.at(*idx) + ")" : "";
    _what = std::string(pref.driver) + label;
    st.paths.push_back(_value.path());
    return true;
  };

  // The kernel's own package sensor, when exposed as a thermal zone instead
  // of a hwmon device.
  auto try_pkg_temp_zone = [&]() -> bool {
    const std::string tz_root = "/sys/class/thermal";
    if (!std::filesystem::is_directory(tz_root)) return false;
    auto it = std::filesystem::directory_iterator(tz_root);
    for (; it != std::filesystem::directory_iterator(); ++it) {
      const std::string p = it->path().string();
      if (!it->is_directory()) continue;
      std::ifstream f(p + "/type");
      std::string type;
      std::getline(f, type);
      if (type != "x86_pkg_temp") continue;
      int err = 0;
      if (SysfsFile::open(p + "/temp", _value, err)) {
        _what = "thermal_zone x86_pkg_temp";
        st.paths.push_back(_value.path());
        return true;
      }
    }
    return false;
  };

  // Walk the preference table (data, sensor_tables.hpp): first entry with a
  // readable sensor wins.
  std::string tried;
  bool found = false;
  for (const auto& pref : cpu_temp_preferences()) {
    if (!tried.empty()) tried += ", ";
    tried += pref.driver;
    found = (pref.kind == TempPreference::Kind::kThermalZone) ? try_pkg_temp_zone()
                                                              : try_hwmon(pref);
    if (found) break;
  }

  if (found) {
    st.detail = _what;
    if (_what.rfind("acpitz", 0) == 0) st.detail += " (ACPI fallback)";
  } else {
    st.detail = "no CPU package temperature sensor found (tried " + tried + ")";
    st.hint = "AMD: sudo modprobe k10temp — Intel: sudo modprobe coretemp";
  }
  st.available = found;
  return st;
}

void CpuTempSource::sample(Snapshot& out) {
  if (!_value.valid()) return;
  // parse_millidegrees() maps negative "no data" readings to nullopt, which
  // the UI renders as n/a instead of a bogus -127 °C.
  out.cpu.temp_c = parse_millidegrees(_value.read());
}

}  // namespace geth
