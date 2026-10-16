// Shared helpers for the UI tests (test_ui.cpp, test_dashboard.cpp).
//
// has_forbidden_style is the machine-checked form of the "no background" rule:
// GETH never paints a background color or inverts text, so an SGR sequence may
// only carry foreground styles. It is a free function (global namespace) so
// both test TUs can call it unqualified.
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

// True if any SGR sequence in `s` paints a background or inverts text — the
// machine-checked form of the "no background" rule (48;2;…, 48;5;…, 4X, 10X,
// or reverse 7). An extended color (38;… / 48;…) consumes its mode + argument
// params so a *foreground* color component (e.g. the "47" in "38;2;20;89;47")
// is not mistaken for a standalone palette-16 background code. (SGR 4 alone is
// underline, not a background, and is allowed.)
inline bool has_forbidden_style(const std::string& s) {
  for (std::size_t i = 0; i + 2 < s.size(); ++i) {
    if (s[i] != 0x1b || s[i + 1] != '[') continue;
    std::size_t j = i + 2;
    std::size_t start = j;
    while (j < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == ';'))
      ++j;
    if (j >= s.size() || s[j] != 'm') {
      i = j;
      continue;
    }
    std::vector<int> pv;
    {
      std::string cur;
      for (std::size_t k = start; k < j; ++k) {
        if (s[k] == ';') {
          if (!cur.empty()) pv.push_back(std::atoi(cur.c_str()));
          cur.clear();
        } else {
          cur += s[k];
        }
      }
      if (!cur.empty()) pv.push_back(std::atoi(cur.c_str()));
    }
    std::size_t p = 0;
    while (p < pv.size()) {
      const int v = pv[p++];
      if (v == 38 || v == 48) {
        if (v == 48) return true;  // an extended background
        if (p < pv.size()) {
          const int mode = pv[p++];
          p += (mode == 2) ? 3 : 1;  // 2;r;g;b or 5;n
        }
      } else if (v == 7 || (v >= 40 && v <= 47) || (v >= 100 && v <= 107)) {
        return true;  // reverse, or a palette-16 background
      }
    }
    i = j;
  }
  return false;
}

// The number of SGR sequences that paint a background or invert (48;2, 48;5,
// 40-47, 100-107, or reverse 7). Transparent mode must render this as 0 for
// every theme; solid mode must render it > 0 (it paints the whole screen).
inline int count_background_codes(const std::string& s) {
  int n = 0;
  for (std::size_t i = 0; i + 2 < s.size(); ++i) {
    if (s[i] != 0x1b || s[i + 1] != '[') continue;
    std::size_t j = i + 2;
    std::size_t start = j;
    while (j < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == ';'))
      ++j;
    if (j >= s.size() || s[j] != 'm') {
      i = j;
      continue;
    }
    std::vector<int> pv;
    {
      std::string cur;
      for (std::size_t k = start; k < j; ++k) {
        if (s[k] == ';') {
          if (!cur.empty()) pv.push_back(std::atoi(cur.c_str()));
          cur.clear();
        } else {
          cur += s[k];
        }
      }
      if (!cur.empty()) pv.push_back(std::atoi(cur.c_str()));
    }
    std::size_t p = 0;
    while (p < pv.size()) {
      const int v = pv[p++];
      if (v == 38 || v == 48) {
        if (v == 48) {
          ++n;  // an extended background
          if (p < pv.size()) {
            const int mode = pv[p++];
            p += (mode == 2) ? 3 : 1;  // 2;r;g;b or 5;n
          }
        } else if (p < pv.size()) {
          const int mode = pv[p++];
          p += (mode == 2) ? 3 : 1;  // skip a foreground color
        }
      } else if (v == 7 || (v >= 40 && v <= 47) || (v >= 100 && v <= 107)) {
        ++n;  // reverse, or a palette-16 background
      }
    }
    i = j;
  }
  return n;
}
