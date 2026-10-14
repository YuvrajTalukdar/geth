// RAII wrapper around a sysfs/procfs file descriptor.
//
// Design rule: a file is opened exactly once (during probe()) and the fd is
// kept; every tick re-reads it with pread() into a reused buffer. That avoids
// a path lookup + open() per read, and makes read errors easy to attribute to
// a specific file.
#pragma once

#include <string>

namespace geth {

class SysfsFile {
 public:
  SysfsFile() = default;
  SysfsFile(SysfsFile&& other) noexcept;
  SysfsFile& operator=(SysfsFile&& other) noexcept;
  SysfsFile(const SysfsFile&) = delete;  // an fd is not copyable
  SysfsFile& operator=(const SysfsFile&) = delete;
  ~SysfsFile();

  // Opens `path` read-only. On failure, `error` receives errno so callers can
  // tell EACCES ("permission") apart from ENOENT ("not present").
  static bool open(const std::string& path, SysfsFile& out, int& error);

  bool valid() const { return _fd >= 0; }
  int error() const { return _error; }
  const std::string& path() const { return _path; }

  // Reads the whole (virtual) file. The returned reference aliases an
  // internal buffer that is reused by the next read(), so do not keep it
  // alive across another read() of this same file.
  const std::string& read() const;

 private:
  explicit SysfsFile(int fd, std::string path) : _fd(fd), _path(std::move(path)) {}

  int _fd = -1;
  mutable int _error = 0;  // cache of the last read() failure; mutable like _buf
  std::string _path;
  mutable std::string _buf;  // reused across reads: no allocation after warm-up
};

}  // namespace geth
