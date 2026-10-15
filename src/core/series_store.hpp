// Per-series graph history, keyed by a stable string SeriesId.
//
// This REPLACES the old ring-of-whole-Snapshots. There is one RingBuffer of
// plain floats per series plus ONE shared timestamp ring — no strings and no
// vectors inside any ring. The set of series is fixed at init() (derived from
// the hardware after the warm-up sample), so everything that allocates — the
// id->index map and the ring storage — happens exactly once, there. Every
// later tick just pushes floats (NaN for a missing sample) into the rings:
// ZERO heap allocations, proven by the counting-operator-new test.
#pragma once

#include "core/ring_buffer.hpp"
#include "core/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geth {

// A stable string key naming one plotted metric, e.g. "cpu.usage",
// "gpu.0000:01:00.0.power", "net.total.rx", "disk.nvme0n1.write".
using SeriesId = std::string;

// A transparent hasher: it can hash a std::string_view, so put()/has()/
// values() can look up with a string_view without constructing a temporary
// std::string (that would be a heap allocation on the hot path). It returns
// the same value as std::hash<std::string> for the same character sequence,
// which is what keeps it consistent with the std::string keys in the map.
struct SeriesIdHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

class SeriesStore {
 public:
  // 3600 samples: one hour at the default 1 s interval.
  static constexpr std::size_t kCapacity = 3600;

  // Allocate every ring for `ids`. Call exactly once, before begin_tick().
  // Duplicate ids are ignored. This is the only point at which the store may
  // allocate.
  void init(const std::vector<SeriesId>& ids);

  // Once per tick, before the puts: record this tick's timestamp and lay a
  // NaN placeholder into every series, so even a series that reports nothing
  // this tick stays aligned with the timestamp ring.
  void begin_tick(TimePoint now);

  // Store one sample for `id`: overwrite the placeholder that begin_tick()
  // laid for this tick. A std::nullopt keeps the placeholder (a gap).
  // Unknown ids (hardware that appeared after init) are dropped rather than
  // growing the map, which would allocate.
  void put(std::string_view id, std::optional<double> v);

  bool has(std::string_view id) const;

  // The ring of values for `id`, or nullptr if the id is unknown.
  const RingBuffer<float>* values(std::string_view id) const;

  // The single shared timestamp ring (steady-clock nanoseconds since epoch).
  const RingBuffer<std::int64_t>* times() const { return &_times; }

  std::size_t series_count() const { return _series.size(); }
  const std::vector<SeriesId>& ids() const { return _ids; }

 private:
  struct Series {
    RingBuffer<float> values{kCapacity};
  };

  std::vector<Series> _series;
  std::vector<SeriesId> _ids;
  // Transparent hash/equality (SeriesIdHash + equal_to<>): put()/has()/
  // values() look up with a std::string_view and must not construct a
  // temporary std::string — that would be a heap allocation on the hot
  // path, the one thing this store is designed to eliminate.
  std::unordered_map<SeriesId, int, SeriesIdHash, std::equal_to<>> _index;
  RingBuffer<std::int64_t> _times{kCapacity};
};

}  // namespace geth
