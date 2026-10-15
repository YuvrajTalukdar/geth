// Short display names for the UI header and panel titles.
//
// The full strings are what the hardware reports: the /proc/cpuinfo "model
// name" for CPUs, and the pci.ids lookup ("Cezanne [Radeon Vega Series /
// ...]") or NVML name for GPUs. These are far too long for a panel title, so
// the rules below reduce them to the part a human actually says.
#pragma once

#include <string>
#include <string_view>

#include "util/parse.hpp"

namespace geth::names {

namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string strip_prefix(std::string s, std::string_view p) {
  if (starts_with(s, p)) s.erase(0, p.size());
  return s;
}

std::string strip_suffix(std::string s, std::string_view p) {
  if (ends_with(s, p)) s.erase(s.size() - p.size());
  return s;
}

}  // namespace

// CPU model names, e.g.
//   "AMD Ryzen 5 5600H with Radeon Graphics" -> "Ryzen 5 5600H"
//   "Intel(R) Core(TM) i5-1340P"              -> "i5-1340P"
//   "AMD Ryzen 9 7945HX 16-Core Processor"    -> "Ryzen 9 7945HX"
//
// Rules: strip the "AMD " / "Intel(R) " / "Core(TM) " prefix, the
// " with Radeon Graphics" suffix (and the " with Radeon <X> Graphics"
// variant that newer APUs report, e.g. " with Radeon 880M Graphics"), and a
// trailing "NN-Core Processor".
inline std::string short_cpu(std::string s) {
  s = strip_prefix(s, "AMD ");
  s = strip_prefix(s, "Intel(R) ");
  s = strip_prefix(s, "Core(TM) ");
  if (const auto p = s.find(" with Radeon "); p != std::string::npos) {
    // Only drop the suffix when it really is a graphics-suffix: it must end
    // with " Graphics". Keeps a hypothetical "with Radeon <something else>"
    // intact instead of eating part of the model name.
    const auto e = s.find(" Graphics", p);
    if (e != std::string::npos && e + 9 == s.size()) s.erase(p);
  }
  // "NN-Core Processor" suffix: last token "Processor", previous one "<digits>-Core".
  if (ends_with(s, " Processor")) {
    const auto b = s.rfind(' ', s.size() - 10);  // space before "Processor"
    if (b != std::string::npos) {
      const auto c = s.rfind(' ', b - 1);
      // A std::string, not a string_view: s.substr() returns a temporary
      // that would dangle for the rest of the block.
      const std::string mid = c == std::string::npos ? s.substr(0, b)
                                                     : s.substr(c + 1, b - c - 1);
      const auto is_digits = [](std::string_view v) {
        return !v.empty() && v.find_first_not_of("0123456789") == std::string_view::npos;
      };
      const bool core = mid.size() > 5 && mid[mid.size() - 5] == '-' &&
                        mid.substr(mid.size() - 4) == "Core" &&
                        is_digits(mid.substr(0, mid.size() - 5));
      if (core) s = c == std::string::npos ? std::string() : s.substr(0, c);
    }
  }
  return std::string(parse::trim(s));
}

// GPU names. `name` is the pci.ids string ("Cezanne [Radeon Vega Series /
// ...]", "Navi 22 [Radeon RX 6700/6700 XT ...]") or the NVML name ("NVIDIA
// GeForce RTX 3050 Laptop GPU"). `product_name` is the amdgpu sysfs
// attribute when present (empty otherwise).
//
//   NVIDIA: strip "NVIDIA ", "GeForce " and the trailing " GPU":
//           "NVIDIA GeForce RTX 3050 Laptop GPU" -> "RTX 3050 Laptop"
//   AMD iGPU: the codename before "[", plus " iGPU":
//           "Cezanne [Radeon Vega Series / ...]" -> "Cezanne iGPU"
//   AMD dGPU: product_name if present, else the first "/"-separated
//           alternative inside the brackets:
//           "Navi 22 [Radeon RX 6700/6700 XT ...]" -> "Radeon RX 6700"
inline std::string short_gpu(std::string name, bool integrated,
                             std::string_view product_name) {
  name = std::string(parse::trim(name));
  if (name.empty()) return "";

  // NVIDIA: the "NVIDIA " prefix is not guaranteed (NVML and pci.ids differ —
  // e.g. "GeForce RTX 3050 Mobile" has no "NVIDIA "), so detect on either and
  // strip "NVIDIA " and/or "GeForce " plus a trailing " GPU".
  if (starts_with(name, "NVIDIA ") || starts_with(name, "GeForce ")) {
    name = strip_prefix(name, "NVIDIA ");
    name = strip_prefix(name, "GeForce ");
    name = strip_suffix(name, " GPU");
    return std::string(parse::trim(name));
  }

  // AMD (and everything else pci.ids hands us that is not NVIDIA).
  if (integrated) {
    std::string codename = name;
    if (const auto p = name.find('['); p != std::string::npos) codename = name.substr(0, p);
    codename = std::string(parse::trim(codename));
    return codename.empty() ? name : codename + " iGPU";
  }

  if (!product_name.empty()) {
    std::string p = std::string(parse::trim(product_name));
    return strip_prefix(p, "AMD ");
  }

  if (const auto lb = name.find('['); lb != std::string::npos) {
    const auto rb = name.rfind(']');
    if (rb > lb) {
      // A std::string (not a string_view): name.substr() returns a temporary
      // that would otherwise dangle.
      std::string inside = name.substr(lb + 1, rb - lb - 1);
      if (const auto slash = inside.find('/'); slash != std::string::npos)
        inside = inside.substr(0, slash);
      const std::string first = std::string(parse::trim(inside));
      if (!first.empty()) return first;
    }
  }
  return strip_prefix(name, "AMD ");
}

}  // namespace geth::names
