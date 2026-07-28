# jsonstreambench

A fair benchmark for parsing **streams of JSON documents**, comparing
[simdjson](https://github.com/simdjson/simdjson) — including the experimental
parallel stream parser of
[PR #2788](https://github.com/simdjson/simdjson/pull/2788) — against
[Pison](https://github.com/AutomataLab/Pison), on Pison's own corpus and
queries.

Nothing is vendored: simdjson, Pison, and the performance-counter library are
fetched and pinned by commit at configure time with
[CPM](https://github.com/cpm-cmake/CPM.cmake).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/jsonbench --dataset twitter.ndjson
```

Requires a C++17 compiler and threads. Hardware performance counters need
`kernel.perf_event_paranoid <= 1` or root. Export `CPM_SOURCE_CACHE` to share a
single dependency checkout across build trees.

```
jsonbench --dataset <file.ndjson> [options]
  --query <name>       twitter|bestbuy|google_map|nspl|walmart|wiki
                       (default: inferred from the filename)
  --threads a,b,c      thread counts to sweep (default: 1..hw, doubling)
  --reps <n>           repetitions per configuration, best wins (default 3)
  --slice-kb <n>       parse_many_parallel slice size (default 1024)
  --sections <list>    load,verify,single,scaling,e2e (default: all)
  --single-record      treat the input as one bulky JSON document
  --verify             check that the engines agree, then exit
  --dump <n>           print the first n extracted values from each engine
```

Output is one `RESULT key=value ...` line per measured configuration.

## How the comparison is kept fair

Both engines answer the **same** query and must produce the same answer. The
benchmark checks the match count *and* an order-independent hash of the
extracted values before reporting any timing, and refuses to claim agreement
otherwise. Five of the six queries reproduce the match counts published in the
Pison paper exactly:

| | TT | BB | GMD | WM | WP |
|---|---|---|---|---|---|
| matches | 300,270 | 459,332 | 1,716,752 | 288,391 | 15,603 |

Every phase is timed separately, so nothing hides:

| phase | what it measures |
|---|---|
| `load-published` | Pison's own `RecordLoader::loadRecords` |
| `load-reference` | the same required layout, written carefully, serial |
| `load-fast` | the same required layout, in parallel — the steelman |
| `boundary-only` | record boundaries with no copy: a lower bound, and what simdjson's dispatcher does |
| `index` | structural index only |
| `query` | index + JSONPath evaluation |
| `decode` | + unescape strings and convert numbers |
| `e2e` | everything a caller actually pays, buffer in to values out |

Pison's record-table pass is **included** in its end-to-end numbers. It is
mandatory: its index construction derives its scan bound as `length/32` while
sizing bitmaps at `length/64`, so a record not padded to a 64-byte multiple
writes past every bitmap. Reporting only its published loader would overstate
the gap, so `load-fast` gives Pison a parallel, carefully written implementation
of the pass its algorithm requires.

Pison is also given the smallest *valid* `level_num` per dataset. That is not
the query's depth: Pison writes at the record's true nesting level regardless of
how many levels it allocated, so `level_num` must cover the document. The minima
(2, 3, 8, 1, 2, 5) were determined empirically.

Hardware counters are collected for single-threaded configurations only.
`perf_event_open` without inheritance sees just the calling thread, and both
engines spawn workers internally, so a multi-threaded reading would be wrong
rather than noisy. Parallel runs report wall-clock throughput plus CPU seconds
per gigabyte from `getrusage`, which does aggregate all threads.

## Corpus

The Pison/cuJSON collection publishes six datasets as bulky records but only two
of the six JSON-lines files. `tools/make_ndjson.cpp` regenerates the rest by
minifying every element of each dataset's dominating array onto its own line
(`tweets`, `data`, `products`, `items`, `items`, `items`):

```sh
./build/make_ndjson twitter_large_record.json twitter.ndjson tweets
```

On the two datasets published in both forms, the derived file has exactly the
same record count as the published one.

Note that the published collection is served from a rate-limited public link:
fetching it from several machines at once exhausts the per-file quota. Copy the
derived corpus between machines instead.

## Defects found in the upstream artifacts

Reported because they affect anyone using these as a baseline.

**Pison**
- `SerialBitmapIterator::getValue` returns a `malloc`'d buffer on most paths but
  a string literal `""` on two, so the `free(value)` idiom used throughout
  Pison's own examples is undefined behaviour (`munmap_chunk: invalid pointer`).
- `ParallelBitmapConstructor` aborts with heap corruption at exactly
  `thread_num == 2` on our inputs; 1 and ≥ 4 are fine.
- `RecordLoader::loadRecords` accumulates record offsets in an `int`, which
  overflows past 2 GB.
- Records must be 64-byte aligned *and* 64-byte padded, or index construction
  writes out of bounds. Neither requirement is documented.

**cuJSON's `related_works/pison` harnesses**
- `google.cpp` runs the Best Buy query, not the Google Maps one.
- `nspl.cpp` runs the Google Maps query with `"text"` misspelled `"tex"`, so it
  matches nothing.
- The JSON-lines harnesses hard-code `thread_num = 1`, so the Pison figures
  reported against them are single-threaded.

We implement all queries from the specifications in Pison's paper rather than
from these harnesses, which is why the match counts reproduce.


## License

Apache 2.0. See `LICENSE`.
