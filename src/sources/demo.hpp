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

}  // namespace geth
