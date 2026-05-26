# Fast Bulk Construction of Single-Probe Filters

An implementation and empirical study of bulk-construction algorithms for the
Single-Probe Filters, approximate set-membership data structures designed to
answer each query with a single memory probe.
The repository contains both in-memory and out-of-memoty (streaming) bulk-build
variants, along with a sequence of incremental optimizations to the underlying
radix sort used during construction.

## Prerequisites

- **Compiler:** A C++17 compiler such as GNU `g++` or LLVM `clang++`.
- **Build system:** CMake 3.10 or newer.
- **Operating system:** Linux.
- **Hardware:** An x86-64 CPU with AVX-512 support (required).

## Repository Layout

Each algorithmic variant is maintained on a dedicated branch so that the
contribution of every optimization can be inspected and reproduced
independently. The `main` branch contains only this overview; all source code
lives on the branches below.

There are two executables exposed by the build system:

| Target            | Purpose                                                  |
| ----------------- | -------------------------------------------------------- |
| `measure_fpp`     | Benchmarks the **in-memory** bulk-construction algorithms. |
| `measure_drives`  | Benchmarks the **streaming** (out-of-core) algorithms.   |

## Branches

### Bulk-insertion algorithms (4 branches)

| Branch                       | Description                                              |
| ---------------------------- | -------------------------------------------------------- |
| `in_memory`                  | Single-threaded in-memory bulk construction.             |
| `in-memory-multi-threaded`   | Multi-threaded in-memory bulk construction.              |
| `streaming`                  | Single-threaded streaming (out-of-core) construction.    |
| `streaming_multi_threaded`   | Multi-threaded streaming construction.                   |

### Radix-sort optimization sequence (7 branches)

Each branch in this sequence builds on the previous one by introducing a
single, well-defined optimization. Comparing consecutive branches therefore
isolates the impact of that optimization.

1. `unoptimized_radix_1` — baseline radix sort, no optimizations.
2. `partial_sorting_2` — partial (bucket-level) sorting.
3. `prefetching_3` — software prefetching of upcoming buckets.
4. `compacting_4` — bucket compaction to reduce memory traffic.
5. `ooe_5` — out-of-order execution friendly restructuring.
6. `manual_loop_unrolling_6` — manual loop unrolling.
7. `radix_simd_7` — SIMD-vectorized radix sort.

In total there are **11 implementation branches** in addition to `main`.

To restrict the measurements to the bucket-sorting phase alone (rather than
sorting all keys), define the compile-time flag `TEST_ONLY_BUCKET`, e.g. by
passing `-DTEST_ONLY_BUCKET=ON` to CMake or `-DTEST_ONLY_BUCKET` to the
compiler directly.

On the `streaming` and `in_memory` branches, collect Linux `perf stat` counters
by defining the compile-time flag `ENABLE_PERF_STAT`, e.g. from the build
directory:

```bash
cmake -DCMAKE_CXX_FLAGS="-DENABLE_PERF_STAT" ..
```

Without this flag, no `perf` process is started. Results are appended to
`perf_stats.txt` by default; set `PERF_OUT` when running the benchmark to use a
different output file.

## Building

```bash
git clone -b main https://github.com/golanshabi/FastFilterBulkConstruction.git
cd FastFilterBulkConstruction
git checkout <branch>          # e.g. radix_simd_7
mkdir build && cd build
cmake ..
make <target>                  # measure_fpp or measure_drives
```

## Running

### SQLite filter-construction benchmark

The SQLite benchmark is available on the `in_memory` branch. Its build is
owned by this repository and uses the unmodified
`sqlite-past-present-future` submodule as a source dependency.

From the FastFilterBulkConstruction repository root, configure and build it
with:

```bash
git switch in_memory
git submodule update --init sqlite-past-present-future
cmake -S benchmarks/sqlite-filter -B build/sqlite-filter -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-11 -DCMAKE_CXX_COMPILER=g++-11
cmake --build build/sqlite-filter --target filter_build_sqlite3 -j
```

The first configure may need network access to download `cxxopts`. Do not use
the submodule's `build-filter-gcc11` directory for this target: that directory
was configured from the submodule's own CMake project, which intentionally
does not define `filter_build_sqlite3`.

Run this small smoke test first:

```bash
BUILD_ROWS=100000 PROBE_ROWS=10000 TRIALS=1 WARMUP=false OVERLAPS="10" VALIDATE=true build/sqlite-filter/filter_build.sh
```

Then run the full overlap sweep with:

```bash
build/sqlite-filter/filter_build.sh
```

The defaults are 33,554,432 build rows, 1,048,576 probe rows, ten trials, and
overlap percentages `1 5 10 20 30`. Override them with `BUILD_ROWS`,
`PROBE_ROWS`, `TRIALS`, `WARMUP`, `OVERLAPS`, and `VALIDATE`. Set `CPU_CORE` to
pin the benchmark with `taskset`, or set `OUTPUT_DIR` and `DATABASE` to select
the result and database paths. The script prints both paths when it finishes.

For all executable options, run:

```bash
build/sqlite-filter/filter_build_sqlite3 --help
```

### Prefix Filter benchmarks

```bash
./measure_fpp <LOG_NUM_ENTRIES>
./measure_drives <LOG_NUM_ENTRIES> <STREAM_BATCH_SIZE>
```

The multi threaded variants also require a third `<NUM_THREAD>` argument.

## Results

Each measurement is repeated `NUM_SAMPLES` times (defined in
`Tests/TimeTracker.hpp`), and the reported numbers are the mean and variance
across those runs.

# Credits

- **Prefix Filter**:
["Prefix Filter: Practically and Theoretically Better Than Bloom"](https://www.vldb.org/pvldb/vol15/p1311-even.pdf). 
Tomer Even, Guy Even, Adam Morrison.

- **Breadcrumb Filter**:
["Breadcrumb Filters: Fast Fully Featured Filters"](https://dl.acm.org/doi/pdf/10.1145/3786629).
Andrew Krapivin, Aaditya Rangarajan, Alex Conway, Martín Farach-Colton, Rob Johnson, and Prashant Pandey.

- **Xor Filter**:
["Xor Filters: Faster and Smaller Than Bloom and Cuckoo
Filters."](https://arxiv.org/pdf/1912.08258.pdf)
Graf, Thomas Mueller, and Daniel Lemire. \
[Repository](https://github.com/FastFilter/fastfilter_cpp).\
We build upon Xor filter's benchmarks.
We also used Its BBF variant, its fast Bloom filter, and its [fast modulo alternative](https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/).
<!-- ---
We Integrated a perf event wrapper taken from [viktorleis/perfevent](https://github.com/viktorleis/perfevent). This requires running the code as root.

--- -->
- **Cuckoo Filter** \
["Cuckoo Filter: Practically Better Than Bloom." ](https://www.cs.cmu.edu/~dga/papers/cuckoo-conext2014.pdf) Fan B, Andersen DG, Kaminsky M, Mitzenmacher MD.
[Repository](https://github.com/efficient/cuckoofilter)
- **Blocked Bloom Filter**\
We used two variants taken from the Xor filter's repository.
<!-- One taken from Impala repository, one from Xor filter's repository, and we also implemented another one, using AVX512 instructions, which is build upon the Xor filter's variant. -->
<!-- --- -->
- **Vector Quotient Filter**\
["Vector Quotient Filters: Overcoming The Time/Space Trade-Off In Filter Design."](https://research.vmware.com/files/attachments/0/0/0/0/1/4/7/sigmod21.pdf). Pandey P, Conway A, Durie J, Bender MA, Farach-Colton M, Johnson R. Vector quotient filters: Overcoming the time/space trade-off in filter design.\
[Repository](https://github.com/splatlab/vqf).\
However, we used our own implementation, called *twoChoicer* (In file `TC-Shortcut/TC-shortcut.hpp`).

- **VQSort**\
["Vectorized and performance-portable Quicksort"](https://arxiv.org/pdf/2205.05982). Mark Blacher, Joachim Giesen, Peter Sanders,
Jan Wassenberg.\
[Repository](https://github.com/google/highway/blob/master/hwy/contrib/sort/vqsort.h)
