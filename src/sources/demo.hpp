// Synthetic data source for --demo: exercises every field of the data model
// (including both GPU vendors and every "unavailable" code path being
// exercised is left to real hardware) without touching any sysfs file.
//
// Values are smooth deterministic functions of time (no RNG), so --demo
// output is reproducible and looks like a real laptop under light load.
#pragma once

#include "core/source.hpp"

namespace geth {

class DemoSource : public Source {
 public:
  const char* name() const override { return "demo (synthetic data)"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  TimePoint _t0;
  bool _started = false;
};

// --demo=pattern: a DETERMINISTIC pattern (no smooth waves) for exact column/
// row assertions in the graph tests and for before/after frames. As a function
// of elapsed virtual seconds t since the first sample:
//   cpu.usage  square wave: 0% on [0,10), 100% on [10,20), period 20 s — with
//              a 5 s NaN gap at the end of every 45 s cycle ([40,45) mod 45)
//   gpu.usage  linear ramp 0->100% over 60 s (sawtooth, period 60 s)
//   cpu.temp   50 C constant
//   gpu.temp   50 C constant  (EQUAL to cpu.temp: two overlapping series for
//              the overlap-resolution test)
// The values are pure functions of t, so a frame rendered after N virtual
// seconds is reproducible and the exact dot row for any column is computable.
class PatternSource : public Source {
 public:
  const char* name() const override { return "pattern (synthetic)"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  TimePoint _t0;
  bool _started = false;
};

}  // namespace geth
