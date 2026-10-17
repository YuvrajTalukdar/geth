// --trace SERIES_ID: per-tick stage trace of one series, to stdout.
//
// For each tick of the traced series, one line per stage, e.g. for
// `--trace cpu.power`:
//
//   [tick 0042] raw     gpu=0000:07:00.0 /sys/.../power1_input = 4532000 µW
//   [tick 0042] parse   gpu=0000:07:00.0 ppt_w=4.532 W
//   [tick 0042] lift    APU PPT <- gpu 0000:07:00.0 (4.532 W)
//   [tick 0042] store   cpu.power = 4.532
//   [tick 0042] panel   power: APU Package (PPT) = 4.5 W
//
// Single writer: the tick/render thread (the timer thread only posts
// events). The cost when inactive is one comparison per call; callers must
// gate detail-string construction on matches() so the hot path stays
// allocation-free.
#pragma once

#include <string>
#include <string_view>

namespace geth::trace {

// Enable tracing of one series (empty = off). Call once before the first
// tick.
void start(const std::string& series_id);
// True when any tracing is on.
bool active() noexcept;
// True when `id` is the traced series. Build stage details only under this.
bool matches(std::string_view id) noexcept;
// Advance the tick counter. Called once per Collector::sample().
void tick() noexcept;
// Emit one line for the traced series; a no-op unless matches(id).
void stage(const char* stage, std::string_view id, const std::string& detail) noexcept;

}  // namespace geth::trace
