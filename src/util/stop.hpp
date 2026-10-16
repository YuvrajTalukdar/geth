// The process-wide stop flag: set by SIGINT/SIGTERM/SIGHUP (the handler
// installed in main.cpp) so the non-UI modes (--dump/--json) exit promptly,
// and checked by main.cpp after the (possibly long) probe phase.
#pragma once

#include <atomic>

namespace geth::util {

inline std::atomic<bool> g_stop{false};

}  // namespace geth::util
