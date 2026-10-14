// Common interface for data sources, plus the human-readable probe result.
#pragma once

#include "core/snapshot.hpp"

#include <string>
#include <vector>

namespace geth {

// Result of a Source::probe() call: availability plus the information a
// human needs to fix anything that is unavailable (--probe output).
struct SourceStatus {
  bool available = false;
  std::string description;  // what the source is, e.g. "CPU power"
  std::string detail;       // human-readable reason (or which backend was found)
  std::string hint;         // how to fix it, when unavailable
  std::vector<std::string> paths;  // files/libraries the source reads
};

// A single data source. probe() is called once at startup and opens all file
// handles; sample() is then called once per tick and must not allocate
// heavily or block. The whole program is single-threaded by design.
class Source {
 public:
  virtual ~Source() = default;
  virtual const char* name() const = 0;
  virtual SourceStatus probe() = 0;
  virtual void sample(Snapshot& out) = 0;
};

}  // namespace geth
