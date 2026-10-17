#include "core/series_store.hpp"

#include <cmath>
#include <cstdio>

#include "util/trace.hpp"

namespace geth {

void SeriesStore::init(const std::vector<SeriesId>& ids) {
  _ids.clear();
  _series.clear();
  _index.clear();
  _ids.reserve(ids.size());
  _series.reserve(ids.size());
  _index.reserve(ids.size());
  for (const auto& id : ids) {
    if (id.empty() || _index.count(id)) continue;
    _index.emplace(id, static_cast<int>(_series.size()));
    _ids.push_back(id);
    _series.emplace_back();
  }
}

void SeriesStore::begin_tick(TimePoint now) {
  _times.push(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  now.time_since_epoch())
                  .count());
  // Lay a NaN placeholder into EVERY series: even a series that reports
  // nothing this tick keeps its ring aligned with the timestamp ring, so
  // missing data always draws as a gap, never as 0.
  for (auto& s : _series) s.values.push(std::nanf(""));
}

void SeriesStore::put(std::string_view id, std::optional<double> v) {
  if (!v) return;  // the placeholder NaN already covers this tick
  const auto it = _index.find(id);
  if (it == _index.end()) return;  // unknown id: drop, never grow
  _series[it->second].values.overwrite_last(static_cast<float>(*v));
  if (trace::matches(id)) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s = %.3f", std::string(id).c_str(), *v);
    trace::stage("store", id, buf);
  }
}

bool SeriesStore::has(std::string_view id) const {
  return _index.find(id) != _index.end();
}

const RingBuffer<float>* SeriesStore::values(std::string_view id) const {
  const auto it = _index.find(id);
  if (it == _index.end()) return nullptr;
  return &_series[it->second].values;
}

}  // namespace geth
