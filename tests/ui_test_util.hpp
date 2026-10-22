// Shared helpers for the UI tests (test_ui.cpp, test_dashboard.cpp).
//
// has_forbidden_style is the machine-checked form of the "no background" rule:
// GETH never paints a background color or inverts text, so an SGR sequence may
// only carry foreground styles. It is a free function (global namespace) so
// both test TUs can call it unqualified.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// ---------------------------------------------------------------------------
// v1.5.0 CSV-logging helpers (test_csv_log.cpp, test_dashboard.cpp).
// ---------------------------------------------------------------------------

// Saves one environment variable and restores it on scope exit (the logging
// tests point XDG_DATA_HOME at a temp directory and set GETH_TEST=1). An
// EMPTY value unsets the variable instead of setting it to "".
struct ScopedEnv {
  const char* name;
  std::string old_value;
  bool had;
  ScopedEnv(const char* n, const std::string& v) : name(n) {
    const char* cur = std::getenv(n);
    had = (cur != nullptr);
    if (had) old_value = cur;
    // setenv/unsetenv are POSIX (global namespace, not std::).
    if (v.empty())
      ::unsetenv(n);
    else
      ::setenv(n, v.c_str(), 1);
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;
  ~ScopedEnv() {
    if (had)
      ::setenv(name, old_value.c_str(), 1);
    else
      ::unsetenv(name);
  }
};

// A fresh temp directory for one test (any leftover from a previous run is
// removed first — remove_all, since tests may leave trees behind).
inline std::filesystem::path fresh_tmp_dir(const std::string& name) {
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() / "geth_ui_tests" / name;
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  std::filesystem::create_directories(p, ec);
  return p;
}

inline std::string read_file_text(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The file's lines, split on '\n'; a trailing newline does not yield an
// empty last line.
inline std::vector<std::string> lines_of_file(const std::string& p) {
  const std::string t = read_file_text(p);
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < t.size()) {
    const std::size_t e = t.find('\n', start);
    if (e == std::string::npos) {
      out.push_back(t.substr(start));
      break;
    }
    out.push_back(t.substr(start, e - start));
    start = e + 1;
  }
  return out;
}

// The .csv files in `dir`, sorted by name (rotation order: base.csv,
// base-2.csv, ...).
inline std::vector<std::string> csv_files_in(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.path().extension() == ".csv") out.push_back(e.path().string());
  std::sort(out.begin(), out.end());
  return out;
}

// A minimal RFC 4180 field splitter — the test side of the round-trip: it
// must parse back exactly the fields the logger quoted on its way out.
inline std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_q = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (in_q) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          cur += '"';
          ++i;
        } else {
          in_q = false;
        }
      } else {
        cur += c;
      }
    } else if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (c == '"' && cur.empty()) {
      in_q = true;
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}
