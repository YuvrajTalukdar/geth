// Global heap-allocation counter.
//
// The .cpp defines the process's operator new / operator delete as thin
// wrappers around malloc/free that bump an atomic counter. This is the
// standard "counting operator new" technique: geth_tests uses it to prove
// that the sampling path performs zero heap allocations per tick after
// warm-up, and the shipped geth binary uses it to report allocations per
// frame under --stats (rendering may allocate; the sampling path may not).
// The wrapper is a few atomic loads over malloc — negligible at GETH's tick
// rate, and the binary only pays it for the counter itself.
#pragma once

namespace geth {

// Total number of heap allocations since process start.
long long alloc_count();
// Reset the counter to zero.
void alloc_reset();
// When false, operator new stops counting (useful to exclude framework setup).
void set_alloc_counting(bool on);

}  // namespace geth
