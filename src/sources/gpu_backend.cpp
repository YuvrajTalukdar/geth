#include "sources/gpu_backend.hpp"

#include "util/parse.hpp"

namespace geth {

RuntimeState parse_runtime_status(std::string_view text) {
  const auto t = parse::trim(text);
  if (t == "active") return RuntimeState::kActive;
  if (t == "resuming") return RuntimeState::kResuming;
  if (t == "suspended" || t == "suspending") return RuntimeState::kSleeping;
  if (t == "error") return RuntimeState::kError;
  if (t == "unsupported") return RuntimeState::kUnsupported;  // PM off: always awake
  // Unknown/empty (file vanished, read failed): assume the device is awake
  // rather than declaring it asleep on the strength of a failed read.
  return RuntimeState::kActive;
}

}  // namespace geth
