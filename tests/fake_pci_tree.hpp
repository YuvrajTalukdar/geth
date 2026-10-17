// A minimal /sys stand-in for tests: <root>/bus/pci/devices/<bus_id>/* and
// <root>/class/drm/cardN/device symlinks. remove_all'd on destruction.
//
// Shared by test_parsers.cpp (pci discovery, the APU-PPT lift over a fake
// amdgpu hwmon) and test_ui.cpp (panel-visibility render regression).
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace geth::test {

class FakePciTree {
 public:
  std::filesystem::path root;

  FakePciTree() {
    static int next = 0;
    root = std::filesystem::temp_directory_path() /
           ("geth_fakepci_" + std::to_string(::getpid()) + "_" + std::to_string(++next));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bus/pci/devices");
    std::filesystem::create_directories(root / "class/drm");
  }
  ~FakePciTree() { std::filesystem::remove_all(root); }

  void add_device(const std::string& bus_id, const std::string& vendor,
                  const std::string& device, const std::string& driver,
                  bool boot_vga = false, const std::string& runtime = "active",
                  int drm_card = -1) {
    const auto dev = root / "bus/pci/devices" / bus_id;
    std::filesystem::create_directories(dev / "power");
    auto write = [&](const std::filesystem::path& p, const std::string& v) {
      std::ofstream f(p);
      f << v;
    };
    write(dev / "class", "0x030000\n");
    write(dev / "vendor", "0x" + vendor + "\n");
    write(dev / "device", "0x" + device + "\n");
    write(dev / "subsystem_vendor", "0x" + vendor + "\n");
    write(dev / "subsystem_device", "0x0001\n");
    write(dev / "boot_vga", boot_vga ? "1\n" : "0\n");
    write(dev / "power/runtime_status", runtime + "\n");
    if (!driver.empty()) std::filesystem::create_symlink(driver, dev / "driver");
    if (drm_card >= 0) {
      const auto card = root / "class/drm" / ("card" + std::to_string(drm_card));
      std::filesystem::create_directories(card);
      std::filesystem::create_symlink(dev.string(), card / "device");
    }
  }

  void write_dev_file(const std::string& bus_id, const std::string& rel,
                      const std::string& v) {
    const auto p = root / "bus/pci/devices" / bus_id / rel;
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << v;
  }
};

}  // namespace geth::test
