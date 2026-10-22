#include "ui/csv_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace geth::ui {

const char* log_unit_name(LogUnit u) {
  switch (u) {
    case LogUnit::kPercent: return "%";
    case LogUnit::kMHz: return "MHz";
    case LogUnit::kBytes: return "B";
    case LogUnit::kBytesPerSec: return "B/s";
    case LogUnit::kWatts: return "W";
    case LogUnit::kCelsius: return "°C";
    case LogUnit::kNone: return "";
  }
  return "";
}

namespace {

// A column while it is still being assembled: the explicit log name
// ("<device> <quantity>", set next to the series/row in build_default_page —
// the UI label is the fallback) and the unit, which the header suffix comes
// from.
struct Col {
  std::string metric;
  std::string name;      // the UI label (legend / row)
  std::string log_name;  // "<device> <quantity>"; empty = use name
  std::string device;    // the device's display name (leading part of log_name)
  bool from_legend = true;  // the owning series shows a legend line
  LogUnit unit = LogUnit::kNone;
  std::function<std::optional<double>(const Snapshot&)> value_fn;
};

// Civil days since 1970-01-01 (Howard Hinnant's days_from_civil formula).
long long days_from_civil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = static_cast<int>(y - era * 400);  // [0, 399]
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
  return era * 146097 + doe - 719468;
}

// A monotonic linear seconds value from a broken-down time (no DST term).
// Used ONLY to take the difference between the localtime and gmtime of one
// instant — that difference is exactly the UTC offset in seconds.
long long civil_seconds(const struct tm& t) {
  return days_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) * 86400 +
         t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

// Locale-independent by construction: the digits are composed from integer
// math, never %f (whose decimal point would follow LC_NUMERIC).
void fmt_value(std::string& out, double v, LogUnit u) {
  const int dec =
      (u == LogUnit::kPercent || u == LogUnit::kCelsius) ? 1
      : (u == LogUnit::kWatts) ? 2
                               : 0;  // kMHz / kBytes / kBytesPerSec: integer
  const long long mul = (dec == 0) ? 1 : (dec == 1) ? 10 : 100;
  long long scaled = std::llround(v * static_cast<double>(mul));
  const bool neg = scaled < 0;
  if (neg) scaled = -scaled;
  char buf[40];
  if (dec == 0)
    std::snprintf(buf, sizeof buf, "%s%lld", neg ? "-" : "", scaled);
  else
    std::snprintf(buf, sizeof buf, "%s%lld.%0*lld", neg ? "-" : "",
                  scaled / mul, dec, scaled % mul);
  out += buf;
}

// "2026-10-22T14:30:05.123+05:30" — local time. The wall instant is the
// file's wall anchor plus the sample's steady-clock offset from the file's
// steady anchor, so it is immune to wall-clock jumps (and counts suspend
// time the way the sample stream did).
void append_timestamp(std::string& out, const TimePoint& t,
                      const TimePoint& steady_start,
                      const std::chrono::system_clock::time_point& wall_start) {
  long long dt_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t - steady_start)
          .count();
  if (dt_ns < 0) dt_ns = 0;  // never negative (a pre-file sample)
  const auto wall = wall_start + std::chrono::nanoseconds(dt_ns);
  const std::time_t secs = std::chrono::system_clock::to_time_t(wall);
  struct tm lt{};
  struct tm gt{};
  localtime_r(&secs, &lt);
  gmtime_r(&secs, &gt);
  const long long off = civil_seconds(lt) - civil_seconds(gt);  // s east of UTC
  const int off_min = static_cast<int>(off / 60);
  const int off_h = off_min / 60;
  const int off_m = std::abs(off_min % 60);
  const int ms = static_cast<int>(
      (std::chrono::duration_cast<std::chrono::milliseconds>(
           wall.time_since_epoch())
           .count() +
       1000LL) %
      1000LL);
  // 96 bytes: the realistic max is 29 ("2026-10-22T14:30:05.123+05:30"), but
  // -Wformat-truncation at -O2 assumes each %d could be a full-range int and
  // sizes the buffer against that pessimistic bound, not the real one.
  char buf[96];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%+03d:%02d",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
                lt.tm_min, lt.tm_sec, ms, off_h, off_m);
  out += buf;
}

// "12.345" — seconds since THIS FILE started, 3 decimals.
void append_elapsed(std::string& out, const TimePoint& t,
                    const TimePoint& steady_start) {
  long long dt_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t - steady_start)
          .count();
  if (dt_ns < 0) dt_ns = 0;
  char buf[24];
  std::snprintf(buf, sizeof buf, "%lld.%03lld", dt_ns / 1000000000LL,
                (dt_ns / 1000000LL) % 1000LL);
  out += buf;
}

// RFC 4180: quote only when the field contains a comma, a quote or a line
// break; double the quotes inside.
void append_quoted(std::string& out, const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) {
    out += s;
    return;
  }
  out += '"';
  for (const char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
}

}  // namespace

std::vector<LogColumn> log_columns(const std::vector<PanelSpec>& page) {
  std::vector<Col> cols;
  std::map<std::string, std::size_t> by_metric;
  for (const auto& p : page) {
    if (!p.visible) continue;  // hidden panels are never logged
    // First the non-hidden series...
    for (const auto& s : p.series) {
      if (s.hidden || !s.numeric_fn) continue;
      const std::string metric = s.metric.empty() ? s.id : s.metric;
      if (by_metric.count(metric)) continue;  // de-duplicated
      Col c;
      c.metric = metric;
      c.name = s.label;
      c.log_name = s.log_name.empty() ? s.label : s.log_name;
      c.device = s.device;
      c.from_legend = s.in_legend;
      c.unit = s.unit;
      c.value_fn = s.numeric_fn;
      by_metric[metric] = cols.size();
      cols.push_back(std::move(c));
    }
    // ...then the numeric info rows. A row sharing a series' metric folds
    // into that column; when the series has no legend, the row IS the
    // surface that shows the value, so its label becomes the display name.
    for (const auto& r : p.info_rows) {
      if (!r.numeric_fn || r.metric.empty()) continue;
      const auto it = by_metric.find(r.metric);
      if (it == by_metric.end()) {
        Col c;
        c.metric = r.metric;
        c.name = r.label;
        c.log_name = r.log_name.empty() ? r.label : r.log_name;
        c.device = r.device;
        c.unit = r.unit;
        c.value_fn = r.numeric_fn;
        by_metric[r.metric] = cols.size();
        cols.push_back(std::move(c));
      } else if (!cols[it->second].from_legend) {
        cols[it->second].name = r.label;
        cols[it->second].log_name =
            r.log_name.empty() ? r.label : r.log_name;
      }
    }
  }

  // Header names: "<log name> [<unit>]". The log name is explicit (the
  // device's display name + quantity, set next to the series/row), so which
  // other panels are visible never changes a column's name — there is no
  // panel-title prefixing. A residual tie (two GPUs renamed to the same
  // display name) gets " #2", " #3", ... on the later occurrence's name; the
  // unit suffix stays last.
  const int n = static_cast<int>(cols.size());
  std::vector<std::string> header(n);
  for (int i = 0; i < n; ++i)
    header[i] = cols[i].log_name + " [" + log_unit_name(cols[i].unit) + "]";
  for (int j = 1; j < n; ++j) {
    for (int i = 0; i < j; ++i) {
      if (header[j] != header[i]) continue;
      int k = 2;
      for (;;) {
        // Put the " #k" on the DEVICE (two same-named GPUs), not after the
        // quantity: "Foo #2 Usage [%]", not "Foo Usage #2 [%]".
        const std::string d = cols[j].device;
        const std::string ln = cols[j].log_name;
        std::string new_name;
        if (!d.empty() && ln.size() > d.size() &&
            ln.compare(0, d.size(), d) == 0 && ln[d.size()] == ' ')
          new_name = d + " #" + std::to_string(k) + ln.substr(d.size());
        else
          new_name = ln + " #" + std::to_string(k);
        const std::string cand =
            new_name + " [" + log_unit_name(cols[j].unit) + "]";
        bool taken = false;
        for (int m = 0; m < n; ++m)
          if (m != j && header[m] == cand) {
            taken = true;
            break;
          }
        if (!taken) {
          header[j] = cand;
          break;
        }
        ++k;
      }
      break;
    }
  }

  std::vector<LogColumn> out;
  out.reserve(cols.size());
  for (int i = 0; i < n; ++i) {
    LogColumn c;
    c.metric = std::move(cols[i].metric);
    c.header = std::move(header[i]);
    c.unit = cols[i].unit;
    c.value_fn = std::move(cols[i].value_fn);
    out.push_back(std::move(c));
  }
  return out;
}

std::string column_signature(const std::vector<PanelSpec>& page) {
  std::string s;
  for (const auto& c : log_columns(page)) {
    s += c.metric;
    s += '\x1f';  // metric / header separator (never in a name)
    s += c.header;
    s += '\x1e';  // record separator
  }
  return s;
}

// ---------------------------------------------------------------------------
// CsvLogger
// ---------------------------------------------------------------------------

CsvLogger::~CsvLogger() { stop(); }

void CsvLogger::stop() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool CsvLogger::write_all(const char* data, std::size_t len, std::string& err) {
  std::size_t done = 0;
  while (done < len) {
    const ssize_t r = ::write(fd_, data + done, len - done);
    if (r < 0) {
      if (errno == EINTR) continue;
      err = "write failed: " + std::string(std::strerror(errno));
      return false;
    }
    done += static_cast<std::size_t>(r);
  }
  return true;
}

bool CsvLogger::open_next(const std::vector<PanelSpec>& page, std::string& err) {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  for (int guard = 0; guard < 10000; ++guard) {
    const std::string p = (seq_ == 1) ? base_ + ".csv"
                                      : base_ + "-" + std::to_string(seq_) + ".csv";
    // O_EXCL is the no-overwrite guarantee: an existing file is never
    // truncated (another geth may have taken this name in the same second).
    fd_ = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd_ >= 0) {
      path_ = p;
      cols_ = log_columns(page);
      sig_ = column_signature(page);
      rows_ = 0;
      steady_start_ = Clock::now();
      wall_start_ = std::chrono::system_clock::now();
      // The header is the only part of the file allowed to allocate.
      std::string h;
      h += "timestamp,elapsed_s";
      for (const auto& c : cols_) {
        h += ',';
        append_quoted(h, c.header);
      }
      h += '\n';
      if (!write_all(h.data(), h.size(), err)) {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
      row_buf_.reserve(8192);
      return true;
    }
    if (errno != EEXIST) {
      err = "cannot open the log file: " + std::string(std::strerror(errno));
      return false;
    }
    ++seq_;
  }
  err = "cannot open the log file: too many existing files with this name";
  return false;
}

std::string CsvLogger::start(const std::vector<PanelSpec>& page,
                             const std::string& dir_override, std::string& err) {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  std::string dir = dir_override;
  if (dir.empty()) {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
      dir = std::string(xdg) + "/geth/logs";
    else if (const char* home = std::getenv("HOME"); home && *home)
      dir = std::string(home) + "/.local/share/geth/logs";
    else {
      err = "no XDG_DATA_HOME or HOME — cannot resolve the log directory";
      return "";
    }
  }
  // Test-safety guard (v1.5.0; the logging extension of the config guard):
  // with GETH_TEST=1 refuse to write under the user's REAL ~/.local/share.
  // A temporary XDG_DATA_HOME (mktemp -d) is always fine.
  if (const char* test = std::getenv("GETH_TEST"); test && *test) {
    if (const char* home = std::getenv("HOME"); home && *home) {
      std::error_code ec;
      const std::filesystem::path real = std::filesystem::weakly_canonical(
          std::string(home) + "/.local/share", ec);
      const std::filesystem::path here = std::filesystem::weakly_canonical(dir, ec);
      const std::string real_str = real.string() + "/";
      if (!ec && !real_str.empty() &&
          here.string().compare(0, real_str.size(), real_str) == 0) {
        err = "GETH_TEST=1: refusing to write logs under your real "
              "~/.local/share (set XDG_DATA_HOME to a temp dir)";
        return "";
      }
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    err = "cannot create the log directory " + dir + ": " + ec.message();
    return "";
  }
  const std::time_t secs =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  struct tm lt{};
  localtime_r(&secs, &lt);
  char buf[40];
  std::snprintf(buf, sizeof buf, "geth-%04d%02d%02d-%02d%02d%02d",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
                lt.tm_min, lt.tm_sec);
  base_ = dir + "/" + buf;
  seq_ = 1;
  if (!open_next(page, err)) return "";
  return path_;
}

bool CsvLogger::tick(const std::vector<PanelSpec>& page, const Snapshot& snap,
                     bool columns_may_have_changed, std::string& err) {
  if (fd_ < 0) return false;
  // Re-fingerprint the column set ONLY when the Dashboard reported a page
  // mutation; a change closes this file and starts the next in the sequence
  // (same timestamp base).
  if (columns_may_have_changed && column_signature(page) != sig_) {
    if (!open_next(page, err)) return false;
  }
  row_buf_.clear();
  append_timestamp(row_buf_, snap.time, steady_start_, wall_start_);
  row_buf_ += ',';
  append_elapsed(row_buf_, snap.time, steady_start_);
  for (const auto& c : cols_) {
    row_buf_ += ',';
    // Missing = empty field: nullopt, NaN (the suspend gap blanks rates to
    // NaN), a sleeping GPU, a rate's first tick.
    const std::optional<double> v = c.value_fn ? c.value_fn(snap) : std::nullopt;
    if (v && std::isfinite(*v)) fmt_value(row_buf_, *v, c.unit);
  }
  row_buf_ += '\n';
  if (!write_all(row_buf_.data(), row_buf_.size(), err)) {
    stop();  // a write failure (e.g. a full disk) stops logging cleanly
    return false;
  }
  ++rows_;
  return true;
}

std::string CsvLogger::rec_label() const {
  if (fd_ < 0) return "";
  long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now() - wall_start_)
                     .count();
  if (ns < 0) ns = 0;
  const long long total_s = ns / 1000000000LL;
  char buf[40];
  std::snprintf(buf, sizeof buf, "\xE2\x97\x8F REC %02lld:%02lld:%02lld ",
                total_s / 3600, (total_s / 60) % 60, total_s % 60);
  return buf;
}

}  // namespace geth::ui
