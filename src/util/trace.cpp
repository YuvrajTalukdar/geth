#include "util/trace.hpp"

#include <cstdio>

namespace geth::trace {
namespace {
std::string g_series;  // empty = off
int g_tick = 0;
}  // namespace

void start(const std::string& series_id) { g_series = series_id; }
bool active() noexcept { return !g_series.empty(); }
bool matches(std::string_view id) noexcept {
  return !g_series.empty() && g_series == id;
}
void tick() noexcept {
  if (active()) ++g_tick;
}
void stage(const char* stage, std::string_view id, const std::string& detail) noexcept {
  if (!matches(id)) return;
  char head[80];
  std::snprintf(head, sizeof(head), "[tick %04d] %-7s ", g_tick, stage);
  std::fputs(head, stdout);
  std::fputs(detail.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace geth::trace
