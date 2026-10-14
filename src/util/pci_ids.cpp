#include "util/pci_ids.hpp"

#include "util/parse.hpp"

#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace geth {

namespace {

bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

const std::vector<std::string>& default_pci_ids_paths() {
  static const std::vector<std::string> paths = {
      "/usr/share/hwdata/pci.ids",  // e.g. Arch (hwdata package)
      "/usr/share/misc/pci.ids",    // older distros
  };
  return paths;
}

PciIds::PciIds(std::string path) : _source(std::move(path)) {}

PciIds::PciIds() : PciIds(std::string()) {
  // Pick the first file that exists; keep whichever we picked (possibly none).
  for (const auto& candidate : default_pci_ids_paths()) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      _source = candidate;
      return;
    }
  }
}

void PciIds::load_subset(const std::vector<std::pair<unsigned, unsigned>>& wanted) {
  if (_subset_loaded || _source.empty()) return;
  _subset_loaded = true;

  std::unordered_set<unsigned long long> want;
  for (const auto& [v, d] : wanted)
    want.insert((static_cast<unsigned long long>(v) << 32) | d);

  // Stream the file line by line: it is never held in memory, and lines that
  // are not requested device entries are dropped without copying.
  std::ifstream f(_source);
  if (!f) return;
  _loaded = true;

  unsigned vendor = 0;
  std::string raw;
  while (std::getline(f, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    const std::string_view line = parse::trim(raw);
    if (line.empty() || line[0] == '#') continue;

    // Class ("C03 Display controller") and revision ("R01 ...") lines are
    // unindented and reset the vendor context. Note: only the exact C/R +
    // 2-hex-digit shape is a class/revision id — a *vendor* id may itself
    // start with C (e.g. "C0A9 ..."), so match precisely.
    auto toks0 = parse::tokens(line);
    const bool class_rev = toks0.size() >= 1 && toks0[0].size() == 3 &&
                           (toks0[0][0] == 'C' || toks0[0][0] == 'R') &&
                           is_hex_digit(toks0[0][1]) && is_hex_digit(toks0[0][2]);
    if (class_rev) {
      vendor = 0;
      continue;
    }

    // Indentation is the structure: no indent = vendor, one level = device,
    // two levels = subsystem (skipped). Files use tabs (1/2/4) or spaces
    // (0/4/8), so count leading tabs first, then spaces.
    std::size_t tabs = 0, spaces = 0;
    for (char ch : raw) {
      if (ch == '\t')
        ++tabs;
      else if (ch == ' ')
        ++spaces;
      else
        break;
    }
    const bool device_line =
        (tabs == 1 && spaces < 4) || (tabs == 0 && spaces >= 4 && spaces < 8);
    if (!device_line && tabs > 0 && tabs != 1) continue;  // subsystem level
    if (tabs == 0 && spaces >= 8) continue;               // subsystem level
    if (tabs == 0 && spaces > 0 && spaces < 4) continue;  // malformed

    auto toks = parse::tokens(line);
    if (toks.size() < 2) continue;
    const auto id = parse::to_hex32(toks[0]);
    if (!id) continue;

    if (tabs == 0 && spaces == 0) {
      vendor = *id;  // vendor line
    } else if (vendor != 0) {
      const unsigned long long key =
          (static_cast<unsigned long long>(vendor) << 32) | *id;
      if (!want.count(key)) continue;  // not needed: skip without copying
      // The name is the *rest* of the line ("Cezanne [Radeon Vega Series /
      // Radeon Vega Mobile Series]"), not just its first token.
      const std::size_t id_end = line.find(toks[0]) + toks[0].size();
      const std::string name = std::string(parse::trim(line.substr(id_end)));
      if (name.empty()) continue;
      _map[key] = std::move(name);
    }
  }
}

std::optional<std::string> PciIds::lookup(unsigned vendor, unsigned device) const {
  if (!_loaded) return std::nullopt;
  const auto it = _map.find((static_cast<unsigned long long>(vendor) << 32) | device);
  return it == _map.end() ? std::nullopt : std::optional<std::string>(it->second);
}

}  // namespace geth
