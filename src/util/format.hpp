// Human-readable formatting for --dump output (and, later, the UI).
// Values are doubles from the Snapshot data model.
#pragma once

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace geth::format {

// Binary (1024) units — the convention KDE System Monitor uses for RAM/VRAM.
inline std::string bytes(double v) {
  if (!std::isfinite(v) || v < 0) return "n/a";
  static const char* const kUnits[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
  double x = v;
  int u = -1;  // -1 == plain bytes
  while (x >= 1024.0 && u < 4) {
    x /= 1024.0;
    ++u;
  }
  char buf[64];
  if (u < 0) std::snprintf(buf, sizeof(buf), "%.0f B", x);
  else std::snprintf(buf, sizeof(buf), "%.2f %s", x, kUnits[u]);
  return buf;
}

inline std::string rate(double v) {
  if (!std::isfinite(v) || v < 0) return "n/a";
  return bytes(v) + "/s";
}

// MHz input; switches to GHz above 1000 MHz so laptop clocks stay short.
inline std::string freq(double mhz) {
  if (!std::isfinite(mhz) || mhz < 0) return "n/a";
  char buf[64];
  if (mhz >= 1000.0) std::snprintf(buf, sizeof(buf), "%.2f GHz", mhz / 1000.0);
  else std::snprintf(buf, sizeof(buf), "%.0f MHz", mhz);
  return buf;
}

inline std::string watts(double w) {
  if (!std::isfinite(w) || w < 0) return "n/a";
  char buf[64];
  if (w >= 1000.0) std::snprintf(buf, sizeof(buf), "%.2f kW", w / 1000.0);
  else std::snprintf(buf, sizeof(buf), "%.1f W", w);
  return buf;
}

inline std::string celsius(double c) {
  if (!std::isfinite(c) || c < -273.15) return "n/a";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f °C", c);
  return buf;
}

inline std::string percent(double p) {
  if (!std::isfinite(p) || p < 0) return "n/a";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f %%", p);
  return buf;
}

// Prints an optional metric through `f`, or "n/a" when unavailable.
template <typename F>
inline std::string or_na(const std::optional<double>& v, F&& f) {
  return v ? f(*v) : "n/a";
}

}  // namespace geth::format
