#!/usr/bin/env bash
#
# run.sh -- every configuration of cujson_baseline on the six bulky records,
# on one pinned core.
#
#   cujson_baseline/run.sh [--build <dir>] [--data <dir>] [--cpu <n>] [--reps <n>]
#
# --data holds the bulky records (<name>_large_record.json), as fetched by
# datasets.sh into <dir>/bulky.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/../build"
DATA="$HOME/jsonbench/bulky"
CPU=2
REPS=10
while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD="${2:?}"; shift 2 ;;
    --data) DATA="${2:?}"; shift 2 ;;
    --cpu) CPU="${2:?}"; shift 2 ;;
    --reps) REPS="${2:?}"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
BIN="$BUILD/cujson_baseline/cujson_baseline"
[ -x "$BIN" ] || { echo "build first: $BIN not found" >&2; exit 1; }
echo "# host=$(hostname) cpu=$(LC_ALL=C lscpu | sed -n 's/^Model name: *//p') pinned=$CPU"
for d in twitter bestbuy google_map nspl walmart wiki; do
  f="$DATA/${d}_large_record.json"
  [ -s "$f" ] || { echo "missing $f" >&2; exit 1; }
  taskset -c "$CPU" "$BIN" --file "$f" --label "$d" --reps "$REPS"
done
