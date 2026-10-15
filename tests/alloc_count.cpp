#include "alloc_count.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

std::atomic<long long> g_allocs{0};
std::atomic<bool> g_counting{true};

void note() {
  if (g_counting.load(std::memory_order_relaxed))
    g_allocs.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// The global allocation hooks for this (test) process. Every `new`/`new[]`
// goes through here, so the counter sees every heap allocation in the binary.
//
// Every pair below (an operator new / operator delete) MUST route through the
// same std::malloc/std::free pair — ASan aborts on an alloc-dealloc mismatch.
void* operator new(std::size_t size) {
  note();
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t size) {
  note();
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

// nothrow overloads: std::stable_sort (inside finalize_gpus) asks for its
// temporary buffer via new(size, nothrow). Without overrides here, that
// allocation would come from libstdc++/libasan's own allocator while the
// matching deallocation still went through our sized operator delete
// (std::free) — a mismatch ASan reports.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  note();
  return std::malloc(size ? size : 1);  // nothrow: nullptr on failure
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  note();
  return std::malloc(size ? size : 1);
}

// Sized deallocation (C++14): the compiler emits these when it knows the size.
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
// The nothrow siblings, matched to the nothrow operators above.
void operator delete(void* ptr, std::size_t, const std::nothrow_t&) noexcept {
  std::free(ptr);
}
void operator delete[](void* ptr, std::size_t, const std::nothrow_t&) noexcept {
  std::free(ptr);
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }

namespace geth {

long long alloc_count() { return g_allocs.load(std::memory_order_relaxed); }

void alloc_reset() { g_allocs.store(0, std::memory_order_relaxed); }

void set_alloc_counting(bool on) { g_counting.store(on, std::memory_order_relaxed); }

}  // namespace geth
