# cujson_baseline

simdjson on the workloads of the cuJSON paper (Gargary, Loaliyan and Zhao,
*cuJSON: A Highly Parallel JSON Parser for GPUs*, ASPLOS '26), measured
fairly. Two programs:

* `github_archive` -- cuJSON's end-to-end case study (Section 8.7, Figure 18):
  the repository name of every PushEvent in a day and a half of GitHub Archive
  events, with simdjson serially and with the slicing driver of
  `src/parallel_stream.h` on any number of threads.
* `cujson_baseline` -- single-threaded simdjson on the six Pison datasets as
  single JSON documents (Figures 9 and 10), measured the way cuJSON measures
  its simdjson baseline and the way it should be measured.

Both are Linux only and are built with the rest of the benchmark:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target github_archive cujson_baseline -j
```

## Data

All files come from cuJSON's public dataset folder,
<https://drive.google.com/drive/folders/1PkDEy0zWOkVREfL7VuINI-m9wJe45P2Q>.
With [gdown](https://github.com/wkentaro/gdown) (`pip install gdown`):

```sh
mkdir -p ~/jsonbench/github_archive && cd ~/jsonbench/github_archive
gdown -O github_archive_small_records.json 1xoU7iXTZRwf-C3LhYxv1pi3h6SIlB3im  # JSON lines
gdown -O github_archive_large_record.json  1e8rFE4sJ7kZmFKH05qjuwxbVale_GzlU  # one array
```

The JSON-lines file has 1,365,438,324 bytes and 554,617 events, of which
289,117 are PushEvents: the counts the paper reports. One event is 2.7 MB.
The six bulky Pison records (`<name>_large_record.json`) are what
`datasets.sh` fetches into `~/jsonbench/bulky`.

## github_archive

```
github_archive --lines <github_archive_small_records.json>
               [--document <github_archive_large_record.json>]
               [--threads a,b,c] [--assign static|dynamic] [--reps n]
```

The query is `$[?(@.type=="PushEvent")].repo.name`; the names are unescaped
and hashed. Each configuration runs once untimed, then `--reps` times (default
10), and reports the mean and best time:

* `lines/parallel`: the slicing driver over the JSON lines, at each thread
  count (default 1, 2, 4, ... up to the hardware threads), with 256 KiB slices
  and static (default) or dynamic assignment;
* `lines/serial`: simdjson's `iterate_many`, one thread, 16 MB batches;
* `lines/threaded`: the same with `parser.threaded` set, so that stage 1 of
  the next batch runs on a second thread (simdjson's default when built with
  threads);
* `document/serial`: the events as one JSON array, one thread.

Every configuration must return the same number of names with the same hash,
or the program fails. The input is in memory before the clock starts, as in
the rest of this benchmark; cuJSON's 323.1 ms on an NVIDIA A100 also counts
copying the input to the GPU and the results back.

On a dual-socket Intel Xeon Gold 6548N (`results/big4_github.txt`), the
slicing driver answers the query in 18.8 ms at 32 threads (static
assignment). On one thread, `iterate_many` takes 342 ms and the slicing driver
334 ms; with its second thread, `iterate_many` takes 196 ms.

## cujson_baseline

cuJSON's harness for its simdjson baseline,
`paper_reproduced/related_works/simdjson/quickstart-original-iterate.cpp` in
[AutomataLab/cuJSON](https://github.com/AutomataLab/cuJSON), does this ten
times per dataset and reports the mean:

```cpp
ondemand::parser parser;                         // a new parser every run
padded_string json = padded_string::load(file);  // not timed
auto start = high_resolution_clock::now();
ondemand::document doc = parser.iterate(json);   // timed
auto end = high_resolution_clock::now();
```

Two things make that number unrepresentative:

* **It is stage 1 only.** On-demand `iterate()` validates UTF-8 and builds the
  structural index; it parses nothing and never checks the nesting. cuJSON
  validates UTF-8, tokenizes, and matches every bracket (its pairing index).
* **The parser is new each run**, so its buffers -- gigabytes for a 1 GB
  document -- are allocated inside the timed region, and the time is dominated
  by page faults and by the kernel zeroing fresh pages.

```
cujson_baseline --file <document.json> [options]
  --label <name>   dataset label for the output (default: file name)
  --api a,b        ondemand  stage 1 only, as cuJSON times it
                   dom       a full DOM parse: everything cuJSON's parse does
                             (validation, bracket matching) and more (every
                             string and number decoded)
                   query     end to end: stage 1 plus the paper's query on every
                             record of the dominating array, values raw
                   decode    as query, values decoded
  --parser a,b     fresh (new parser per run), reuse (one parser, warmed up)
  --alloc a,b      how the large buffers are obtained:
                     malloc  the C library, unchanged
                     4k      mmap advised MADV_NOHUGEPAGE
                     huge    2 MiB-aligned mmap advised MADV_HUGEPAGE
                     pool    as huge, but freed blocks are kept and reused,
                             as a long-running process's allocator would
  --reps <n>       timed runs per configuration (default 10)
```

Every list defaults to all of its values. The configuration
`--api ondemand --parser fresh --alloc malloc` reproduces cuJSON's
measurement. Each configuration runs once untimed, then `--reps` times; the
output reports the mean, median and best times, the throughput of the mean,
the minor page faults per run, and, for `query` and `decode`, the match count
and value hash (which equal those of the JSON-lines runs in `jsonbench`).

The allocation modes work by replacing the global `operator new` and
`operator delete` in this executable: simdjson obtains every parser buffer
with `new (std::nothrow) T[n]`.

```sh
cujson_baseline/run.sh --data ~/jsonbench/bulky --cpu 2
```

`run.sh` pins the process to one core and runs every configuration on the six
bulky records. On the Xeon Gold 6548N (`results/big4_parse.txt`,
`results/big4_query.txt`), cuJSON's measurement gives 1.9 to 3.9 GB/s, a
full DOM parse with warm memory 1.3 to 2.1 GB/s, and the end-to-end query 2.3
to 2.8 GB/s; with a fresh parser and the C library's allocator, page faults
cost 23 to 42 percent of that end-to-end time.

## Results

`results/` holds the raw output behind the numbers above, from a dual-socket
Intel Xeon Gold 6548N with transparent huge pages set to `always`. The
simdjson commit is the one the benchmark pins (5.0.6; its version header still
reads 5.0.5).
