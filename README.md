# jsonstreambench

A fair benchmark for parsing **streams of JSON documents**, comparing
[simdjson](https://github.com/simdjson/simdjson) — driven across many threads by
the slicing rule in `src/parallel_stream.h` — against
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
  --query <name>       twitter|bestbuy|google_map|nspl|walmart|wiki|openalex
                       (default: inferred from the filename)
  --threads a,b,c      thread counts to sweep (default: 1..hw, doubling)
  --reps <n>           repetitions per configuration, best wins (default 3)
  --slice-kb <n>       parallel slice size (default 64)
  --assign <mode>      slice assignment: static (default) or dynamic
  --batch-mb <n>       iterate_many batch size (default 16)
  --sections <list>    load,verify,single,scaling,e2e,format (default: all
                       but format)
  --single-record      treat the input as one bulky JSON document
  --verify             check that the engines agree, then exit
  --dump <n>           print the first n extracted values from each engine
  --engine-only <name> run only this engine in the scaling section
  --impl <name>        force a simdjson kernel (haswell, icelake, ...)
  --levels <n>         override Pison's level_num
```

The `format` section compares comma-delimited against newline-delimited
encoding of the same records, serially and on simdjson's two-thread pipeline.

Output is one `RESULT key=value ...` line per measured configuration. Each
carries `spread_pct`, the gap between the slowest and fastest repetition
relative to the fastest: only the best repetition is reported, and how
repeatable it was varies enormously between machines.

`--engine-only` narrows the scaling section to one engine, which is what makes
an aggregate profile interpretable — `perf stat -a` cannot attribute a counter
to an engine when several run in the same process. Pair it with
`--sections scaling` so nothing else runs either.


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

## The parallel driver

`src/parallel_stream.h` is this repository's own driver, not a simdjson API.
An earlier version of it lived in simdjson as an experimental header
([PR #2788](https://github.com/simdjson/simdjson/pull/2788)); it belongs in user
code instead. Nothing in it needs to be inside the library — it is a slicing
rule plus a thread pool over the public `iterate_many` interface — and keeping
it out means a caller can adapt the decomposition to their own pipeline rather
than accept ours. simdjson is therefore pinned to an ordinary master commit
rather than to a patched branch, and the benchmark measures a decomposition
anyone can write.

The rule: cut the input into fixed-size slices, snap both ends forward to the
next delimiter so slices abut and no document is split, and give each worker its
own parser and its own output vector. Nothing is shared on the hot path and no
lock is taken. Results come back as one vector per worker, so values keep their
order within a worker but not across the input.

This needs a delimiter that cannot occur inside a JSON value: a line feed for
NDJSON, or a record separator (0x1E) for RFC 7464. Comma-delimited input cannot
be sliced this way at all, because a top-level comma is only found by a serial
structural scan.

The corpus is strictly one document per line — the same structure Pison is
handed as its record table — so simdjson is told that too, with
`stream_format::newline_delimited`, which lets it skip the tail of a partially
read document instead of walking its structural characters.

That format and `simdjson::slice_at` are detected at configure time rather than
required, so the tree still builds against a simdjson that has neither; it then
falls back to slicing with `memchr`. That is not a corner case: simdjson cuts
releases from a `4.6.x` branch, and neither feature has shipped in one, so
*every* release takes the fallback. The pin is a master commit for exactly this
reason, and the numbers below are master's.

`src/dom_parallel.h` applies the same rule to yyjson, RapidJSON, Boost.JSON and
nlohmann. The slicing does not know what parses a document, and running the
conventional parsers under it is how that claim is checked rather than asserted.

Two knobs control it, and they interact:

* `--slice-kb` (default 64). Smaller slices balance better and hold less live
  data per worker.
* `--assign` (default `static`). Under `static`, each worker owns one contiguous
  run of slices and walks it forward. Under `dynamic`, workers claim the next
  free slice from a shared atomic counter, which scatters a worker's regions
  across the input — at 128 threads and 64 KB slices, consecutive regions sit
  8 MB apart.

The defaults changed when this driver landed: the slice size was 1024 KB and the
assignment was dynamic. **Numbers collected before that change are not
comparable to numbers collected after it.** What follows is the measurement that
motivated it, on 2× Xeon Gold 6548N (64 physical cores, 128 threads), best of
three runs of ten repetitions, GB/s on the `query` phase.

Slice size is the larger effect, and it is not a plateau: at 128 threads a
1024 KB slice is a third slower than a 64 KB one, because 128 workers × 1 MB of
live data no longer fits where it needs to. Assignment then decides how far down
the slice size can be pushed before a shared counter becomes the bottleneck.

| nspl, 128 threads | 8 KB | 16 KB | 32 KB | 64 KB | 256 KB | 1024 KB |
|---|---|---|---|---|---|---|
| `--assign static` | 58.3 | 57.9 | 61.8 | 55.6 | 57.8 | 36.4 |
| `--assign dynamic` | 27.2 | 43.1 | 46.9 | 62.8 | 56.2 | 35.9 |

Static assignment is what makes the choice of slice size stop mattering: it holds
55–62 GB/s across two orders of magnitude, while dynamic loses more than half its
throughput once a slice is small enough that workers contend on the counter.

At the 64 KB default the two policies are close on most of the corpus, and the
gap opens with thread count. At 128 threads:

| | TT | BB | GMD | NSPL | WM | WP |
|---|---|---|---|---|---|---|
| `static` | 84.2 | 86.4 | 73.6 | 56.1 | 92.6 | 84.4 |
| `dynamic` | 77.6 | 80.6 | 46.8 | 61.9 | 85.2 | 65.3 |

Google Maps is the case that decides it, at 1.57×; Wikipedia follows at 1.29×.
Both have highly variable document sizes, which is where a worker's regions
being scattered across the input costs the most. NSPL is the one dataset that prefers
dynamic, by 9%: its documents are small and uniform, so there is nothing for
locality to buy and the counter is never hot. Static is the default because its
worst case is that 9% while its best case is 1.57×, and because it removes the
small-slice cliff entirely.

Both defaults were tuned on the six Pison datasets, whose documents are small.
They do not transfer to a corpus of bulky records: when a document is much
larger than a slice, every slice that starts inside it scans forward to the
document's end before finding it has nothing to do, so the driver rescans the
body once per overlapping slice. On synthetic input with 1.36 MB documents --
the size of the largest OpenAlex author record -- 64 KB slices run at 2.4 GB/s
against 6.0 GB/s for 1024 KB, and 16 KB slices at 0.8 GB/s. Results stay
correct at every size; only throughput suffers. Raise `--slice-kb` above the
longest document for such a corpus.

## Corpus

`./datasets.sh` obtains the whole corpus:

```sh
./datasets.sh --dir ~/jsonbench
```

It downloads the six bulky records from the public collection the Pison and
cuJSON papers use, then derives the JSON-lines form of each with `make_ndjson`,
which minifies every element of the dataset's dominating array onto its own line
(`tweets`, `data`, `products`, `items`, `items`, `items`). Roughly 18 GB of disk
and a `pip install gdown`. The result is `~/jsonbench/ndjson/<dataset>.ndjson`.

OpenAlex authors joins the corpus the same way: the 232,330 author records
updated on 2026-03-30 in the OpenAlex snapshot (CC0, about 6.1 GB, largest
record about 1.37 MB), hosted on Zenodo and pinned by record and size, with
query `$.display_name, $.works_count`. With `--corpus-from` the peer's copy
is reused when present, exactly as for the six cuJSON datasets.

The collection publishes all six datasets as bulky records but only two of the
six JSON-lines files, which is why the rest are derived. On the two published in
both forms, the derived file has exactly the same record count as the published
one — and five of the six queries then reproduce the paper's match counts
exactly, which is the stronger check.

Two cautions, both learned the hard way:

* The bulky records are fetched **by file id**, not by pulling the whole
  published folder. That folder also holds the scalability corpus, the cuDF
  copies, and the `meta_json` set — about 51 GB, eight times what the benchmark
  needs — and downloading all of it is what trips Google Drive's per-file quota.
  The fetch order is rotated per host so co-provisioned machines do not request
  the same file at the same moment.
* Even so, provisioning several machines at once can exhaust the quota
  (`Cannot retrieve the public link of the file`); retrying does not help, it
  resets after roughly a day. Keep one machine as the corpus holder and copy
  from it instead:

```sh
./datasets.sh --dir ~/jsonbench --corpus-from holder:jsonbench/ndjson
```

A single file can also be converted directly:

```sh
./build/make_ndjson twitter_large_record.json twitter.ndjson tweets
```

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
