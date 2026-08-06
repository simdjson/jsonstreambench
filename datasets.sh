#!/usr/bin/env bash
#
# datasets.sh -- obtain the benchmark corpus.
#
#   ./datasets.sh --dir <dir> [--build <build dir>]
#                     [--corpus-from <host>:<path>]
#
# The Pison and cuJSON papers use six datasets, each published in two forms: one
# bulky JSON record, and a stream of the elements of its dominating array, one
# per line. The public Google Drive folder linked from the cuJSON README ships
# all six bulky records but only two of the six JSON-lines files, so we
# regenerate the JSON-lines form from the bulky records with `make_ndjson`.
#
# The regeneration is validated: on the two datasets published in both forms,
# the derived file has the same record count as the published one, and every
# query below reproduces the match count printed in the Pison paper (Table 1):
#
#   twitter 300,270   bestbuy 459,332   google_map 1,716,752
#   walmart 288,391   wiki     15,603
#
# NSPL is the exception. The paper's NSPL query addresses the file's metadata
# header, which does not exist in the data rows that form the JSON-lines
# version, so we define our own two-column extraction for it (see src/common.h).
#
# OpenAlex authors (a paper dataset, CC0, hosted on Zenodo) is part of the
# corpus by default; see the README for its description.
#
# The corpus is roughly 6 GB of JSON-lines plus 6 GB of bulky records, plus
# about 6.1 GB for OpenAlex authors. Nothing is committed to this repository.
set -euo pipefail

DIR=""
BUILD=""
CORPUS_FROM="${JSONBENCH_CORPUS_FROM:-}"
DRIVE_FOLDER="1PkDEy0zWOkVREfL7VuINI-m9wJe45P2Q"

log() { printf '\033[1;34m[datasets]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[datasets] ERR:\033[0m %s\n' "$*" >&2; exit 1; }

fetch_openalex() {
  local out="$DIR/ndjson/openalex_authors.ndjson"
  if [ -s "$out" ]; then
    log "openalex authors already present: $out"
    return 0
  fi
  local url="https://zenodo.org/records/21813521/files/openalex-authors.ndjson?download=1"
  local expected=6419215096
  command -v curl >/dev/null 2>&1 || die "curl required for OpenAlex"
  log "downloading OpenAlex authors from Zenodo (6.1 GB) ..."
  curl -fL --retry 3 -o "$out" "$url" >&2
  local actual
  actual="$(stat -c%s "$out")"
  if [ "$actual" != "$expected" ]; then
    rm -f "$out"
    die "OpenAlex download size mismatch (expected $expected, got $actual); the pinned object changed"
  fi
  log "openalex authors ready: $(ls -la "$out")"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dir)   DIR="${2:?}"; shift 2 ;;
    --build) BUILD="${2:?}"; shift 2 ;;
    --corpus-from) CORPUS_FROM="${2:?}"; shift 2 ;;
    -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done
[ -n "$DIR" ] || die "pass --dir <directory>"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -n "$BUILD" ] || BUILD="$REPO_DIR/build"
[ -x "$BUILD/make_ndjson" ] || die "make_ndjson not found in $BUILD (build the benchmark first)"

mkdir -p "$DIR/bulky" "$DIR/ndjson"

# --- 1. obtain the JSON-lines corpus ----------------------------------------
# Preferred: copy it from a machine that already has it. Google Drive enforces a
# per-file download quota on public links, and several machines fetching the
# same folder at once exhausts it ("Cannot retrieve the public link of the
# file"), which no amount of retrying fixes. Copying from a peer also avoids
# re-deriving the corpus on every machine.
#
#   ./datasets.sh --dir ~/jsonbench --corpus-from big4:~/jsonbench/ndjson
#
# JSONBENCH_CORPUS_RSH may carry ssh options (identity file, user, port).
if [ -n "$CORPUS_FROM" ]; then
  log "copying the JSON-lines corpus from $CORPUS_FROM ..."
  rsync -a --info=progress2 -e "${JSONBENCH_CORPUS_RSH:-ssh}" \
    "$CORPUS_FROM/" "$DIR/ndjson/" || die "corpus copy failed"
  log "corpus ready in $DIR/ndjson:"
  ls -la "$DIR/ndjson" >&2
  exit 0
fi

fetch_openalex

# Fallback: fetch the bulky records from the published Drive folder and derive
# the JSON-lines form locally.
#
# We fetch the six files we need *by id* rather than pulling the whole folder.
# The folder also holds the scalability corpus, the cuDF copies, and the
# meta_json set -- about 51 GB in total, eight times what the benchmark uses.
# Downloading all of it is what exhausts Drive's per-file quota when several
# machines are provisioned at once.
BULKY_IDS="
twitter:1HFa2yN-CRmlQiH1McQtIX_kmpyCdtf3q
bestbuy:1Ji1H_c3qWJEkO0r6FY-bzfyXXIhrODOd
google_map:1mNatwVGsg2VxULRf5Yncn4XAq9pDhWC7
nspl:1k-PbUxuRksDiZZxrg6u5ojH_uQdRJoLq
walmart:1-3ybfnG26jZy1tb-7b2T6piGBh6oN53k
wiki:188Ap0_JdARZ9zLc10SKfESF-GE2Q6uhJ
"

fetch_bulky() { # fetch_bulky <name> <id>
  local name="$1" id="$2" out="$DIR/bulky/${1}_large_record.json"
  [ -s "$out" ] && { log "$name: already present"; return 0; }
  local attempt
  for attempt in 1 2 3 4 5; do
    if gdown --no-cookies --continue -O "$out" "$id" 2>&1 | tail -1 >&2; then
      [ -s "$out" ] && return 0
    fi
    # Drive returns a quota error rather than data; back off and retry, since
    # the limit is per file per window and often clears within minutes.
    rm -f "$out"
    log "$name: attempt $attempt failed; backing off $((attempt * 30))s"
    sleep $((attempt * 30))
  done
  return 1
}

if [ -z "$(ls -A "$DIR/bulky" 2>/dev/null)" ] || \
   [ "$(ls "$DIR/bulky"/*_large_record.json 2>/dev/null | wc -l)" -lt 6 ]; then
  command -v gdown >/dev/null 2>&1 || die "gdown required: pip install gdown"
  log "downloading the six bulky records from Google Drive (about 6 GB) ..."
  log "note: Drive rate-limits public links; prefer --corpus-from when several"
  log "      machines are being provisioned at once."
  # Rotate the order by a hash of the hostname: when several machines are
  # provisioned together they otherwise request the same file at the same
  # moment, which is what concentrates load on one file's quota.
  _list=($BULKY_IDS)
  _n=${#_list[@]}
  _off=$(( $(hostname | cksum | cut -d' ' -f1) % _n ))
  failed=0
  for _i in $(seq 0 $((_n - 1))); do
    spec="${_list[$(( (_i + _off) % _n ))]}"
    fetch_bulky "${spec%%:*}" "${spec##*:}" || { log "WARNING: ${spec%%:*} failed"; failed=1; }
  done
  [ "$failed" -eq 0 ] || die "some downloads failed (Drive quota?); use --corpus-from <host>:<path>"
fi

find_bulky() { find "$DIR/bulky" -name "$1_large_record.json" -print -quit; }

# --- 2. derive the JSON-lines form -----------------------------------------
# dataset:dominating-array-key
for spec in twitter:tweets nspl:data bestbuy:products \
            google_map:items walmart:items wiki:items; do
  name="${spec%%:*}"; key="${spec##*:}"
  out="$DIR/ndjson/$name.ndjson"
  [ -s "$out" ] && { log "$name.ndjson already present"; continue; }
  src="$(find_bulky "$name")"
  [ -n "$src" ] || { log "WARNING: no bulky record for $name; skipping"; continue; }
  log "deriving $name.ndjson from $(basename "$src") (key=$key)"
  "$BUILD/make_ndjson" "$src" "$out" "$key"
done

# --- 3. validate against the published JSON-lines files, when present -------
for name in twitter nspl; do
  pub="$(find "$DIR/bulky" -name "${name}_small_records.json" -print -quit || true)"
  [ -n "$pub" ] || continue
  a=$(wc -l < "$pub"); b=$(wc -l < "$DIR/ndjson/$name.ndjson")
  if [ "$a" = "$b" ]; then
    log "validated $name: derived and published forms both have $a records"
  else
    log "WARNING: $name record count differs (published $a, derived $b)"
  fi
done

log "corpus ready in $DIR/ndjson:"
ls -la "$DIR/ndjson" >&2
