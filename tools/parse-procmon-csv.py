#!/usr/bin/env python3
"""Filter a Procmon CSV for file opens on a given extension and report counts per process.

Procmon's CLI has no /Filter switch (only /LoadConfig with a binary .pmc), so the
V1 recipe captures unfiltered for a few seconds and filters here.

Usage:
    python tools/parse-procmon-csv.py <csv> [--ext .apk] [--top 10]
"""
from __future__ import annotations

import argparse
import csv
import sys

OPEN_OPS = {"CreateFile", "OpenFile"}          # the V1 criterion: real opens
MAP_OPS = {"CreateFileMapping"}                 # section creation, reported separately


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path")
    ap.add_argument("--ext", default=".apk", help="extension to look for (case-insensitive)")
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()

    ext = args.ext.lower()
    total = 0
    by_op: dict[str, int] = {}
    hits: list[tuple[str, str, str, str]] = []   # process, pid, operation, path
    mapped: list[tuple[str, str, str, str]] = []

    with open(args.csv_path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        reader = csv.reader(fh)
        header = next(reader, None)
        if not header:
            print("empty csv", file=sys.stderr)
            return 2
        # Procmon writes columns: Time of Day, Process Name, PID, Operation, Path, Result, Detail
        try:
            i_proc = header.index("Process Name")
            i_pid = header.index("PID")
            i_op = header.index("Operation")
            i_path = header.index("Path")
        except ValueError:
            print(f"unexpected header: {header}", file=sys.stderr)
            return 2

        for row in reader:
            total += 1
            if len(row) <= i_path:
                continue
            op = row[i_op]
            by_op[op] = by_op.get(op, 0) + 1
            path = row[i_path].strip('"')
            if not path.lower().endswith(ext):
                continue
            if op in MAP_OPS:
                mapped.append((row[i_proc], row[i_pid], op, path))
                continue
            if op not in OPEN_OPS:
                continue
            hits.append((row[i_proc], row[i_pid], op, path))

    print(f"total events: {total}")
    print(f"events touching *{ext} (CreateFile/OpenFile): {len(hits)}")
    print(f"events touching *{ext} (CreateFileMapping)   : {len(mapped)}")
    per_proc: dict[str, int] = {}
    for proc, _pid, _op, _path in hits:
        per_proc[proc] = per_proc.get(proc, 0) + 1
    for proc, count in sorted(per_proc.items(), key=lambda kv: -kv[1])[: args.top]:
        print(f"  {proc}: {count}")
    for proc, pid, op, path in hits[: args.top]:
        print(f"    e.g. {proc}({pid}) {op} {path}")
    explorer_opens = [h for h in hits if h[0].lower() == "explorer.exe"]
    print(f"explorer.exe opens of *{ext}: {len(explorer_opens)}  (this is the V1 criterion)")
    if hits:
        print("NOTE: other processes opened the extension - check whether that is our handler's host")
    if explorer_opens:
        print("V1 RESULT: FAIL - explorer.exe opened the files")
        return 1
    print("V1 RESULT: PASS - explorer.exe made 0 CreateFile/OpenFile calls on *%s" % ext)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
