#include "sources/memory.hpp"

#include "util/parse.hpp"

namespace geth {

Meminfo parse_meminfo(std::string_view text) {
  Meminfo mi;
  parse::for_each_line(text, [&](std::string_view line) {
    auto value_of = [&](const char* key) -> std::optional<unsigned long long> {
      const std::string_view k(key);
      if (!line.starts_with(k)) return std::nullopt;
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) return std::nullopt;
      // The value is the first field after the colon; every line in
      // /proc/meminfo is of the form "Key: 12345 kB", and strict to_u64
      // would reject the " kB" unit.
      const std::string_view rest = parse::trim(line.substr(colon + 1));
      const auto sp = rest.find_first_of(" \t");
      return parse::to_u64(sp == std::string_view::npos ? rest : rest.substr(0, sp));
    };
    if (auto v = value_of("MemTotal:")) {
      mi.mem_total_kb = *v;
      mi.ok = true;
    } else if (auto v = value_of("MemAvailable:")) {
      mi.mem_available_kb = *v;
    } else if (auto v = value_of("SwapTotal:")) {
      mi.swap_total_kb = *v;
    } else if (auto v = value_of("SwapFree:")) {
      mi.swap_free_kb = *v;
    }
    return true;
  });
  return mi;
}

SourceStatus MemorySource::probe() {
  SourceStatus st;
  st.description = name();
  int err = 0;
  if (!SysfsFile::open("/proc/meminfo", _meminfo, err)) {
    st.detail = "cannot open /proc/meminfo (errno " + std::to_string(err) + ")";
    return st;
  }
  st.paths.push_back("/proc/meminfo");
  st.detail = "RAM used = MemTotal - MemAvailable (KDE's definition)";
  st.available = true;
  return st;
}

void MemorySource::sample(Snapshot& out) {
  const auto mi = parse_meminfo(_meminfo.read());
  if (!mi.ok) return;
  out.mem.ram_total_bytes = static_cast<double>(mi.mem_total_kb) * 1024.0;
  // Guard the subtraction: MemAvailable can transiently exceed MemTotal.
  const unsigned long long used_kb =
      mi.mem_total_kb >= mi.mem_available_kb ? mi.mem_total_kb - mi.mem_available_kb : 0;
  out.mem.ram_used_bytes = static_cast<double>(used_kb) * 1024.0;

  if (mi.swap_total_kb > 0) {
    out.mem.swap_total_bytes = static_cast<double>(mi.swap_total_kb) * 1024.0;
    const unsigned long long sused =
        mi.swap_total_kb >= mi.swap_free_kb ? mi.swap_total_kb - mi.swap_free_kb : 0;
    out.mem.swap_used_bytes = static_cast<double>(sused) * 1024.0;
  }
  // No swap device: the kernel reports SwapTotal: 0. KDE hides swap then, so
  // we leave the fields empty instead of showing a fake 0 GiB.
}

}  // namespace geth
