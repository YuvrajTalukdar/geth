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

// ---------------------------------------------------------------------------
// Compact variants for y-axis tick labels ONLY (the legend keeps the
// full-precision formatters above): no trailing zeros, a single space before
// the unit — "30 W", "15 W", "0 W", "5 MiB/s", "512 MiB", "32 GiB",
// "100 %", "45 °C".

namespace detail {
// "%.2f" with trailing zeros (and a dangling ".") stripped: 30.00 -> "30",
// 82.50 -> "82.5", 0.00 -> "0".
inline std::string trim_zeros(double v) {
  char b[64];
  std::snprintf(b, sizeof(b), "%.2f", v);
  std::string s(b);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}
}  // namespace detail

inline std::string compact_bytes(double v) {
  if (!std::isfinite(v) || v < 0) return "n/a";
  static const char* const kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double x = v;
  int u = 0;
  while (x >= 1024.0 && u < 5) {
    x /= 1024.0;
    ++u;
  }
  return detail::trim_zeros(x) + " " + kUnits[u];
}

inline std::string compact_rate(double v) {
  if (!std::isfinite(v) || v < 0) return "n/a";
  return compact_bytes(v) + "/s";
}

inline std::string compact_watts(double w) {
  if (!std::isfinite(w) || w < 0) return "n/a";
  if (w >= 1000.0) return detail::trim_zeros(w / 1000.0) + " kW";
  return detail::trim_zeros(w) + " W";
}

inline std::string compact_celsius(double c) {
  if (!std::isfinite(c) || c < -273.15) return "n/a";
  // " °C" — the split literal stops the hex escape from eating the 'C'.
  return detail::trim_zeros(c) + " \xC2\xB0" "C";
}

inline std::string compact_percent(double p) {
  if (!std::isfinite(p) || p < 0) return "n/a";
  char b[16];
  std::snprintf(b, sizeof(b), "%d %%", static_cast<int>(std::lround(p)));
  return b;
}

// ---------------------------------------------------------------------------
// Abbreviated variants for the y-axis of NARROW panels (inner width < 60):
// single-letter units, no spaces, no trailing zeros — "100%", "100°",
// "24G", "768K", "1.5T", "20W", "1.5kW", "768K/s", "1.5M/s". Each kind stays
// within the max width the layout engine budgets for its axis (see
// ui/layout.hpp: abbrev_axis_width).

namespace detail {
// "%.1f" with trailing ".0" (and zeros) stripped: 1.5 -> "1.5", 2.0 -> "2",
// 9.95 -> "9.9", 9.999 -> "10".
inline std::string one_dec(double v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.1f", v);
  std::string s(b);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}
}  // namespace detail

inline std::string abbrev_percent(double p) {
  if (!std::isfinite(p) || p < 0) return "n/a";
  char b[16];
  std::snprintf(b, sizeof(b), "%d%%", static_cast<int>(std::lround(p)));
  return b;  // "100%" — max 4
}

inline std::string abbrev_celsius(double c) {
  if (!std::isfinite(c) || c < -273.15) return "n/a";
  // "°" as a hex escape so the source encoding never matters.
  char b[24];  // long long needs <= 21 bytes (sign + 19 digits + NUL)
  std::snprintf(b, sizeof(b), "%lld\xC2\xB0",
               static_cast<long long>(std::llround(c)));
  return b;  // "100°" — max 4
}

inline std::string abbrev_bytes(double v) {
  if (!std::isfinite(v) || v < 0) return "n/a";
  static const char kUnits[] = "BKMGTP";
  double x = v;
  int u = 0;
  while (x >= 1024.0 && u < 5) {
    x /= 1024.0;
    ++u;
  }
  if (x >= 10.0) {
    long long r = std::llround(x);
    if (r >= 1024 && u < 5) { r /= 1024; ++u; }  // carry from rounding
    char b[24];  // long long needs <= 21 bytes (sign + 19 digits + NUL)
    std::snprintf(b, sizeof(b), "%lld", r);
    return std::string(b) + kUnits[u];  // "768K" — max 4
  }
  return detail::one_dec(x) + kUnits[u];  // "1.5T" — max 4
}

inline std::string abbrev_rate(double v) {
  if (!std::isfinite(v) || v < 0) return "n/a";
  return abbrev_bytes(v) + "/s";  // "1.5M/s" — max 6
}

inline std::string abbrev_watts(double w) {
  if (!std::isfinite(w) || w < 0) return "n/a";
  if (w >= 1000.0) return detail::one_dec(w / 1000.0) + "kW";  // "1.5kW"
  if (w >= 100.0) {
    char b[24];  // long long needs <= 21 bytes (sign + 19 digits + NUL)
    std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(std::llround(w)));
    return std::string(b) + "W";  // "500W"
  }
  return detail::one_dec(w) + "W";  // "20W" — max 5
}

}  // namespace geth::format
