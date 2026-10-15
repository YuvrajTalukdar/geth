// Fixed-capacity ring of plain values.
//
// push() overwrites the oldest slot once full, so memory is bounded and
// NOTHING is allocated after construction — the sampling path pushes into
// these every tick with zero heap traffic. The UI phases only read.
#pragma once

#include <cstddef>
#include <vector>

namespace geth {

template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(std::size_t capacity = 0)
      : _buf(capacity), _cap(capacity) {}

  void push(T v) {
    if (_cap == 0) return;
    _buf[(_head + _size) % _cap] = v;
    if (_size < _cap) {
      ++_size;
    } else {
      _head = (_head + 1) % _cap;  // drop the oldest slot
    }
  }

  std::size_t size() const { return _size; }
  std::size_t capacity() const { return _cap; }
  bool full() const { return _size == _cap; }
  bool empty() const { return _size == 0; }
  void clear() {
    _size = 0;
    _head = 0;
  }

  // i is 0-based from the oldest sample (only valid when i < size()).
  T at(std::size_t i) const { return _buf[(_head + i) % _cap]; }
  T last() const { return _buf[(_head + _size - 1) % _cap]; }

  // Replace the value most recently pushed (no-op on an empty ring).
  // SeriesStore uses it: begin_tick() lays a NaN placeholder into every
  // series, put() then overwrites the current slot with the real value.
  void overwrite_last(T v) {
    if (_size == 0) return;
    _buf[(_head + _size - 1) % _cap] = v;
  }

 private:
  std::vector<T> _buf;
  std::size_t _head = 0;
  std::size_t _size = 0;
  std::size_t _cap = 0;
};

}  // namespace geth
