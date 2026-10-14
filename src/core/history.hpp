// Fixed-capacity ring buffer of (timestamp, value) pairs.
//
// push() overwrites the oldest sample once the buffer is full, so memory is
// bounded (3600 samples by default ≈ one hour at 1 s) and nothing is
// allocated after construction. The UI phases of GETH only ever read from
// these buffers.
#pragma once

#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

namespace geth {

template <typename T>
class History {
 public:
  struct Entry {
    std::chrono::steady_clock::time_point time;
    T value;
  };

  explicit History(std::size_t capacity = 3600) : _buf(capacity), _cap(capacity) {}

  void push(std::chrono::steady_clock::time_point t, T v) {
    if (_cap == 0) return;
    const std::size_t idx = (_head + _size) % _cap;
    _buf[idx] = Entry{t, std::move(v)};
    if (_size < _cap) {
      ++_size;
    } else {
      _head = (_head + 1) % _cap;  // drop the oldest slot
    }
  }

  std::size_t size() const { return _size; }
  std::size_t capacity() const { return _cap; }
  bool empty() const { return _size == 0; }
  void clear() {
    _size = 0;
    _head = 0;
  }

  // i is 0-based from the oldest sample (only valid when i < size()).
  const Entry& at(std::size_t i) const { return _buf[(_head + i) % _cap]; }

 private:
  std::vector<Entry> _buf;
  std::size_t _head = 0;
  std::size_t _size = 0;
  std::size_t _cap = 0;
};

}  // namespace geth
