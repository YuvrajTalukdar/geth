#!/usr/bin/env python3
"""Validate a GETH CSV log file with Python's csv module.

A development tool (NOT a runtime dependency of geth). It re-parses a file the
logger wrote, using the standard-library RFC 4180 reader, and reports:

  * the header (every column) and the column count
  * the number of data rows
  * that every row has exactly as many fields as the header
  * per-column: the count of EMPTY cells (a missing value is an empty field,
    never 0) and the first/last non-empty value seen
  * that column 0 is a well-formed local ISO 8601 timestamp with ms + offset
  * that column 1 (elapsed_s) is monotonic non-decreasing, 3 decimals

Usage:
    python3 tools/validate_log.py FILE [FILE ...]

Exit status: 0 if every file is well-formed, 1 otherwise.
"""

import csv
import re
import sys

# 2026-10-22T14:30:05.123+05:30  (local time, ms, signed UTC offset)
TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2}$")
# elapsed_s: a non-negative number with exactly 3 decimals (0.000, 1.250, ...)
ELAPSED_RE = re.compile(r"^\d+\.\d{3}$")


def validate(path):
    problems = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        rows = list(csv.reader(f))

    if not rows:
        return ["file is empty (not even a header)"], None, None, None, None

    header = rows[0]
    data = rows[1:]
    ncols = len(header)

    # --- structural: every row must have exactly ncols fields --------------
    bad_len = []
    for i, r in enumerate(data, start=2):  # line numbers (1-based, +1 header)
        if len(r) != ncols:
            bad_len.append((i, len(r)))
    if bad_len:
        problems.append(
            "%d row(s) with the wrong field count: %s"
            % (len(bad_len), bad_len[:5])
        )

    # --- timestamp column (index 0) ---------------------------------------
    bad_ts = [r[0] for r in data if r and not TS_RE.match(r[0])]
    if bad_ts:
        problems.append(
            "%d bad timestamp(s), e.g. %r" % (len(bad_ts), bad_ts[0])
        )

    # --- elapsed_s column (index 1): shape + monotonic ---------------------
    prev = -1.0
    bad_el = []
    for r in data:
        if len(r) < 2:
            continue
        v = r[1]
        if not ELAPSED_RE.match(v):
            bad_el.append(("shape", v))
            continue
        fv = float(v)
        if fv < prev - 1e-9:
            bad_el.append(("order", v))
        prev = fv
    if bad_el:
        problems.append("%d bad elapsed_s value(s), e.g. %s" % (len(bad_el), bad_el[0]))

    # --- per-column empty-cell counts + first/last non-empty ---------------
    per_col = []
    for c in range(ncols):
        empty = 0
        first = last = None
        for r in data:
            if c >= len(r):
                empty += 1
                continue
            v = r[c]
            if v == "":
                empty += 1
            else:
                if first is None:
                    first = v
                last = v
        per_col.append((header[c], empty, first, last))

    return problems, header, ncols, len(data), per_col


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    rc = 0
    for path in argv[1:]:
        print("=" * 78)
        print("FILE: %s" % path)
        problems, header, ncols, nrows, per_col = validate(path)
        if header is None:
            print("  EMPTY FILE")
            rc = 1
            continue
        print("  columns : %d" % ncols)
        print("  data rows: %d" % nrows)
        print("  header  :")
        for i, h in enumerate(header):
            print("    [%2d] %s" % (i, h))
        print("  per-column empty cells (out of %d rows):" % nrows)
        for name, empty, first, last in per_col:
            print("    %-6d  %s   (first=%r last=%r)" % (empty, name, first, last))
        if problems:
            rc = 1
            print("  PROBLEMS:")
            for p in problems:
                print("    - %s" % p)
        else:
            print("  OK: well-formed (all rows ncols, valid timestamps, monotonic elapsed)")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
