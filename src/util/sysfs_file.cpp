#include "util/sysfs_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <utility>

namespace geth {

SysfsFile::SysfsFile(SysfsFile&& other) noexcept
    : _fd(std::exchange(other._fd, -1)),
      _error(other._error),
      _path(std::move(other._path)),
      _buf(std::move(other._buf)) {}

SysfsFile& SysfsFile::operator=(SysfsFile&& other) noexcept {
  if (this != &other) {
    ::close(_fd);
    _fd = std::exchange(other._fd, -1);
    _error = other._error;
    _path = std::move(other._path);
    _buf = std::move(other._buf);
  }
  return *this;
}

SysfsFile::~SysfsFile() {
  if (_fd >= 0) ::close(_fd);
}

bool SysfsFile::open(const std::string& path, SysfsFile& out, int& error) {
  // O_CLOEXEC: never leak the fd into a child process. GETH spawns none, but
  // the flag is cheap insurance if that ever changes.
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = errno;
    return false;
  }
  out = SysfsFile(fd, path);
  error = 0;
  return true;
}

const std::string& SysfsFile::read() const {
  _buf.clear();
  // procfs files report size 0 and the sysfs values we read are short; 8 KiB
  // chunks are plenty. The loop ends on a short read, a read error, or the
  // 512 KiB ceiling (a 128-core /proc/stat is ~15 KiB, so this is generous).
  char chunk[8192];
  for (off_t off = 0; _buf.size() < 512 * 1024;) {
    const ssize_t n = ::pread(_fd, chunk, sizeof(chunk), off);
    if (n < 0) {
      // The file can vanish at runtime (GPU unplug, module unload): report ""
      // and let the parser turn that into "unavailable" instead of crashing.
      _error = errno;
      return _buf;
    }
    if (n == 0) break;
    _buf.append(chunk, static_cast<std::size_t>(n));
    off += static_cast<off_t>(n);
    if (static_cast<std::size_t>(n) < sizeof(chunk)) break;
  }
  return _buf;
}

}  // namespace geth
