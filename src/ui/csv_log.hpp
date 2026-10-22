// v1.5.0 — the CSV data logger (toggled with `l`).
//
// It records ONLY what the user has chosen to view: for every visible panel,
// in display order, each non-hidden series, then each info row that carries a
// numeric value — de-duplicated by metric (a value shown in two visible
// panels, or by a series AND its info row, gets one column).
//
// File format (RFC 4180):
//   timestamp,elapsed_s,<one column per logged value>
//   * timestamp  — local time, ISO 8601 with milliseconds and UTC offset
//                  (2026-10-22T14:30:05.123+05:30)
//   * elapsed_s  — seconds since THIS FILE started, 3 decimals
//   * values     — locale-independent (always '.' as the decimal point):
//                  % and °C 1 decimal, MHz / B / B/s integer, W 2 decimals
//   * missing (nullopt, NaN, a sleeping GPU, a rate's first tick, the
//                  suspend gap) is an EMPTY field — never 0
//   * fields with commas / quotes / newlines are quoted, quotes doubled
//
// Rows are written unbuffered — one write() per row — so a crash loses at
// most the row in flight, and the SIGHUP path (the terminal-restore handler
// calls _exit) needs no cleanup: the kernel closes the fd when the process
// dies, and the file is already complete.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/snapshot.hpp"
#include "ui/panel_spec.hpp"

namespace geth::ui {

// The unit string for a LogUnit: "%", "MHz", "B", "B/s", "W", "°C", "" (kNone).
const char* log_unit_name(LogUnit u);

// One output column: the de-dup key, the final unique header and the sampler.
struct LogColumn {
  std::string metric;  // the de-dup key (series id / info-row metric)
  std::string header;  // "<display name> [<unit>]", unique across the set
  LogUnit unit = LogUnit::kNone;
  std::function<std::optional<double>(const Snapshot&)> value_fn;
};

// The logged column set for `page`: visible panels in page order, each
// non-hidden series with a numeric_fn, then each numeric info row, de-dup'd
// by metric. When a series and an info row share a metric, the row's label
// becomes the column's display name when the series has no legend (the row
// is then the surface that shows the value). Header names are made unique:
// on a collision the panel title is prefixed, then a numeric suffix as a
// last resort (two GPUs renamed to the same name).
std::vector<LogColumn> log_columns(const std::vector<PanelSpec>& page);

// The order-sensitive fingerprint of that column set (metric + header of
// every column). The logger re-computes it ONLY when the Dashboard reports
// a page mutation; a change means the current file is closed and a new one
// (base-2.csv, base-3.csv, ...) is started.
std::string column_signature(const std::vector<PanelSpec>& page);

// One logging session: a directory, a "geth-YYYYMMDD-HHMMSS" base (the local
// time at start) and a file sequence base.csv, base-2.csv, .... The first
// file of a NEW session (start after stop) gets a fresh base name; rotations
// within a session keep the base and bump the suffix.
class CsvLogger {
 public:
  // The deleted copy ctor is a user-declared ctor, so the default ctor must
  // be declared explicitly.
  CsvLogger() = default;
  ~CsvLogger();  // closes the open file

  CsvLogger(const CsvLogger&) = delete;
  CsvLogger& operator=(const CsvLogger&) = delete;

  // Opens a new session in `dir` ("" = $XDG_DATA_HOME/geth/logs, falling back
  // to ~/.local/share/geth/logs), creating the directory if needed, and
  // writes the header. Never overwrites an existing file. Returns the file
  // path, or "" with the reason in `err` (logging stays off then).
  //
  // Test-safety guard (extends the config's GETH_TEST guard): with
  // GETH_TEST=1, a directory under the REAL ~/.local/share is refused —
  // point XDG_DATA_HOME at a temp directory instead.
  std::string start(const std::vector<PanelSpec>& page, const std::string& dir,
                    std::string& err);

  // Flushes and closes the current file (a no-op when not active).
  void stop();

  bool active() const { return fd_ >= 0; }
  unsigned long long row_count() const { return rows_; }
  const std::string& path() const { return path_; }
  // "● REC hh:mm:ss " (elapsed since this file started) — "" when inactive.
  // Recomputed per frame; the header draws it in the theme's warn color.
  std::string rec_label() const;

  // One sample tick (the collector-side tick, not a render): rotates when
  // `columns_may_have_changed` and the column set actually changed (new file
  // + header), then appends one row for `snap`. Returns false — with the
  // reason in `err` — when a write/open failure stops logging cleanly.
  bool tick(const std::vector<PanelSpec>& page, const Snapshot& snap,
            bool columns_may_have_changed, std::string& err);

 private:
  // Closes the current file (if any) and opens base[-seq].csv, writing the
  // header. Bumps the sequence past any file that already exists (the
  // O_EXCL open is the no-overwrite guarantee).
  bool open_next(const std::vector<PanelSpec>& page, std::string& err);
  bool write_all(const char* data, std::size_t len, std::string& err);

  int fd_ = -1;
  std::string base_;    // dir + "/geth-YYYYMMDD-HHMMSS"
  std::string path_;    // the current file
  int seq_ = 1;         // 1 = base.csv, 2 = base-2.csv, ...
  std::vector<LogColumn> cols_;
  std::string sig_;     // the column signature the open file was built for
  unsigned long long rows_ = 0;
  TimePoint steady_start_{};                   // the steady anchor of THIS file
  std::chrono::system_clock::time_point wall_start_{};  // its wall anchor
  std::string row_buf_;  // reused row buffer: no per-row allocation after warm-up
};

}  // namespace geth::ui
