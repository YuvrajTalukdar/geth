#include "sources/pci_discovery.hpp"

#include "util/parse.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>

namespace geth {

namespace {

unsigned hex_val(const std::string& s) {
  // sysfs prints e.g. "0x1002"
  const auto v = parse::to_hex32(s.size() > 2 && s.compare(0, 2, "0x") == 0
                                   ? std::string_view(s).substr(2)
                                   : std::string_view(s));
  return v ? *v : 0;
}

unsigned hex_opt(const std::optional<std::string>& s) { return s ? hex_val(*s) : 0; }

std::optional<std::string> read_str(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  std::string v;
  std::getline(f, v);
  if (v.empty()) return std::nullopt;
  return v;
}

std::optional<std::string> read_link_target(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path target = std::filesystem::read_symlink(path, ec);
  // (nullopt_t and string have no common type for ?: — return the empty
  // optional explicitly.)
  return ec ? std::optional<std::string>() : target.filename().string();
}

std::string canonical(const std::string& path) {
  std::error_code ec;
  const auto p = std::filesystem::weakly_canonical(path, ec);
  return ec ? path : p.string();
}

}  // namespace

std::vector<PciDevice> discover_pci_gpus(const std::string& root,
                                         const std::string& drm_root) {
  std::vector<PciDevice> out;
  std::error_code ec;

  // DRM card map first: canonical device path -> cardN number.
  std::map<std::string, int> card_of;
  auto drm_it = std::filesystem::directory_iterator(drm_root, ec);
  if (!ec) {
    for (; drm_it != std::filesystem::directory_iterator(); ++drm_it) {
      const std::string n = drm_it->path().filename().string();
      if (n.size() < 5 || n.compare(0, 4, "card") != 0) continue;
      const auto cardnum = parse::to_u64(n.substr(4));
      if (!cardnum) continue;
      card_of[canonical((drm_it->path() / "device").string())] =
          static_cast<int>(*cardnum);
    }
  }

  auto it = std::filesystem::directory_iterator(root + "/bus/pci/devices", ec);
  if (ec) return out;  // no PCI bus (container, non-PCI arch): nothing found

  for (; it != std::filesystem::directory_iterator(); ++it) {
    const auto ent = it->status(ec);  // follows symlinks; skips dangling ones
    if (ec || ent.type() != std::filesystem::file_type::directory) continue;
    const std::string dev_path = it->path().string();

    // class file: "0x03xxxx" = display controller (VGA/3D/other display).
    const auto cls = read_str(dev_path + "/class");
    if (!cls || cls->size() < 6 || cls->compare(0, 4, "0x03") != 0) continue;

    PciDevice d;
    d.bus_id = it->path().filename().string();
    d.dev_path = canonical(dev_path);
    d.vendor_id = hex_opt(read_str(dev_path + "/vendor"));
    d.device_id = hex_opt(read_str(dev_path + "/device"));
    d.subsystem_vendor_id = hex_opt(read_str(dev_path + "/subsystem_vendor"));
    d.subsystem_device_id = hex_opt(read_str(dev_path + "/subsystem_device"));
    d.driver = read_link_target(dev_path + "/driver").value_or("");
    if (const auto b = read_str(dev_path + "/boot_vga")) d.boot_vga = *b == "1";
    d.runtime_status = read_str(dev_path + "/power/runtime_status").value_or("");
    if (const auto c = card_of.find(d.dev_path); c != card_of.end())
      d.drm_card = c->second;
    out.push_back(std::move(d));
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const PciDevice& a, const PciDevice& b) {
                     return a.bus_id < b.bus_id;
                   });
  return out;
}

}  // namespace geth
