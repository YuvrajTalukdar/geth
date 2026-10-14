// Small text-parsing helpers shared by the /proc and /sys readers.
// Everything operates on std::string_view so tests can feed fixture text
// without touching the filesystem.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace geth::parse {

// Strips leading/trailing whitespace (spaces, tabs, newlines). sysfs values
// come as "0\n" and the strict numeric parsers below reject any non-digit,
// so \n must be stripped here or every one-line file read fails.
inline std::string_view trim(std::string_view s) {
  const char ws[] = " \t\n\r";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

// Splits a line into whitespace-separated tokens (views into `line`).
inline std::vector<std::string_view> tokens(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) break;
    std::size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

// Splits `text` on '\n'. Strips a trailing '\r' (defence against CRLF) and
// drops one final empty line (files end in '\n').
inline std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = text.find('\n', start);
    std::string_view line =
        nl == std::string_view::npos ? text.substr(start)
                                     : text.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

// Strict unsigned 64-bit parse: every character must be a digit. Callers pass
// pre-tokenized fields, so strictness is deliberate — a partial parse would
// silently turn "123abc" into 123.
inline std::optional<unsigned long long> to_u64(std::string_view s) {
  if (s.empty() || s.size() > 20) return std::nullopt;
  unsigned long long v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<unsigned long long>(c - '0');
  }
  return v;
}

// Hex parse (PCI ids: "1002", "25a2"). Rejects non-hex characters.
inline std::optional<unsigned> to_hex32(std::string_view s) {
  if (s.empty() || s.size() > 8) return std::nullopt;
  unsigned v = 0;
  for (char c : s) {
    unsigned d;
    if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
    else return std::nullopt;
    v = v * 16 + d;
  }
  return v;
}

// Base-10 double parse that must consume the whole trimmed string.
inline std::optional<double> to_double(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  try {
    std::size_t consumed = 0;
    const double v = std::stod(std::string(s), &consumed);
    if (consumed != s.size()) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace geth::parse
