// Global heap-allocation counter for the test binary.
//
// The .cpp defines the process's operator new / operator delete as thin
// wrappers around malloc/free that bump an atomic counter. This is the
// standard "counting operator new" technique used to prove that the sampling
// path performs zero heap allocations per tick after warm-up. It is compiled
// into geth_tests ONLY — the shipped geth binary has the normal allocator.
#pragma once

namespace geth {

// Total number of heap allocations since process start.
long long alloc_count();
// Reset the counter to zero.
void alloc_reset();
// When false, operator new stops counting (useful to exclude framework setup).
void set_alloc_counting(bool on);

}  // namespace geth
