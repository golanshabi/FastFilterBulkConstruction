#include "cxxopts.hpp"
#include "sqlite3.h"
#include "Tests/wrappers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Required by Tests/wrappers.hpp. This benchmark is intentionally single-threaded.
int THREAD_NUM = 1;

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kDefaultBuildRows = UINT64_C(1) << 25;
constexpr std::uint64_t kDefaultProbeRows = UINT64_C(1) << 20;
constexpr std::uint64_t kNonmemberDomain = UINT64_C(1) << 63;
constexpr double kLoadFactor = 0.91;
constexpr std::uint64_t kDefaultOverlapPercent = 10;
constexpr std::uint64_t kStreamingBatchSize = UINT64_C(1) << 22;

using PrefixFilter = Prefix_Filter<Impala512<>>;

// SplitMix64 is a permutation of the 64-bit key space. Applying it to disjoint
// integer domains gives A and B random-looking keys without accidental
// duplicates or accidental overlap.
std::uint64_t random_key(std::uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

std::uint64_t calculate_overlap_rows(std::uint64_t probe_rows,
                                     std::uint64_t overlap_percent) {
  if (overlap_percent > 100)
    throw std::runtime_error("overlap_percent must be between 0 and 100");
  return (probe_rows / 100) * overlap_percent +
         ((probe_rows % 100) * overlap_percent) / 100;
}

std::size_t nominal_capacity(std::size_t entries, double load_factor) {
  return static_cast<std::size_t>(
      std::ceil(static_cast<double>(entries) / load_factor));
}

template <typename Filter>
Filter construct_filter(std::size_t capacity) {
  if constexpr (std::is_same_v<Filter, PQF::PQF_8_53>) {
    return Filter(capacity, false);
  }
  return FilterAPI<Filter>::ConstructFromAddCount(capacity);
}

template <typename Filter>
Filter construct_streaming_filter(std::size_t capacity) {
  if constexpr (std::is_same_v<Filter, PQF::PQF_8_53>) {
    return Filter(capacity, false, false);
  }
  return FilterAPI<Filter>::ConstructFromAddCount(capacity, false);
}

enum class ConstructionMode { Incremental, Bulk, Streaming };

constexpr const char *construction_name(ConstructionMode mode) {
  switch (mode) {
    case ConstructionMode::Incremental: return "incremental";
    case ConstructionMode::Bulk: return "bulk";
    case ConstructionMode::Streaming: return "streaming";
  }
  return "unknown";
}

template <typename Filter>
class StreamBufferArena {
  using StreamBuffer = typename FilterAPI<Filter>::StreamBuffer;
  StreamBuffer *buffers_ = nullptr;
  std::size_t bytes_ = 0;

public:
  explicit StreamBufferArena(std::size_t num_buckets) {
    bytes_ = (num_buckets + 1) * sizeof(StreamBuffer);
    void *raw = nullptr;
    const int ok = posix_memalign(&raw, 64, bytes_);
    if (ok != 0)
      throw std::runtime_error("posix_memalign failed for stream buffers");
    buffers_ = static_cast<StreamBuffer *>(raw);
    std::memset(buffers_, 0, bytes_);
    buffers_ += 1;
  }

  ~StreamBufferArena() {
    if (buffers_) free(buffers_ - 1);
  }

  void clear() {
    if (buffers_) std::memset(buffers_ - 1, 0, bytes_);
  }

  StreamBuffer *get() const { return buffers_; }
};

template <typename Filter>
double realized_primary_load(std::size_t entries, Filter &filter) {
  if constexpr (std::is_same_v<Filter, PQF::PQF_8_53>) {
    return static_cast<double>(entries) /
           (static_cast<double>(filter.getNumFrontyardBuckets()) * 51.0);
  } else {
    return static_cast<double>(entries) /
           (static_cast<double>(filter.GetNumPd()) * min_pd::MAX_CAP0);
  }
}

template <typename Filter>
constexpr std::string_view filter_backend() {
  if constexpr (std::is_same_v<Filter, PQF::PQF_8_53>) return "pqf_8_53";
  return "impala512";
}

struct TrialTimings {
  double fill_seconds = 0.0;
  double query_without_fill_seconds = 0.0;
  double scan_seconds = 0.0;
  double total_seconds = 0.0;

  TrialTimings &operator+=(const TrialTimings &other) {
    fill_seconds += other.fill_seconds;
    query_without_fill_seconds += other.query_without_fill_seconds;
    scan_seconds += other.scan_seconds;
    total_seconds += other.total_seconds;
    return *this;
  }
};

class Database {
public:
  explicit Database(const std::string &path) {
    const int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
      const std::string message = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
      if (db_) sqlite3_close(db_);
      throw std::runtime_error(message);
    }
  }

  ~Database() { sqlite3_close(db_); }
  sqlite3 *get() const { return db_; }

  void exec(const std::string &sql) const {
    char *error = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
      const std::string message = error ? error : sqlite3_errmsg(db_);
      sqlite3_free(error);
      throw std::runtime_error(message);
    }
  }

private:
  sqlite3 *db_ = nullptr;
};

class Statement {
public:
  Statement(sqlite3 *db, const char *sql) : db_(db) {
    const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  sqlite3_stmt *get() const { return stmt_; }

  int step() const {
    const int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errmsg(db_));
    return rc;
  }

  void bind(std::uint64_t value) const {
    if (sqlite3_bind_int64(stmt_, 1, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db_));
  }

  void reset() const {
    if (sqlite3_reset(stmt_) != SQLITE_OK || sqlite3_clear_bindings(stmt_) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db_));
  }

private:
  sqlite3 *db_ = nullptr;
  sqlite3_stmt *stmt_ = nullptr;
};

void load_probes(Database &db, std::uint64_t build_rows,
                 std::uint64_t probe_rows,
                 std::uint64_t overlap_percent) {
  const std::uint64_t overlap_rows =
      calculate_overlap_rows(probe_rows, overlap_percent);
  if (overlap_rows > build_rows)
    throw std::runtime_error("Table A is too small for the requested overlap");

  db.exec("DROP TABLE IF EXISTS bloom_probe");
  db.exec("DROP TABLE IF EXISTS filter_benchmark_config");
  db.exec("CREATE TABLE bloom_probe(k INTEGER NOT NULL)");
  db.exec("CREATE TABLE filter_benchmark_config("
          "build_rows INTEGER, probe_rows INTEGER, overlap_percent INTEGER, "
          "overlap_rows INTEGER)");
  db.exec("BEGIN");

  std::vector<std::uint64_t> probes;
  probes.reserve(probe_rows);
  for (std::uint64_t i = 0; i < overlap_rows; ++i)
    probes.push_back(random_key(i));
  for (std::uint64_t i = 0; i < probe_rows - overlap_rows; ++i)
    probes.push_back(random_key(kNonmemberDomain + i));
  std::mt19937_64 generator(UINT64_C(0xd1b54a32d192ed03));
  std::shuffle(probes.begin(), probes.end(), generator);

  Statement insert_probe(db.get(), "INSERT INTO bloom_probe VALUES(?1)");
  for (const std::uint64_t key : probes) {
    insert_probe.bind(key);
    insert_probe.step();
    insert_probe.reset();
  }

  db.exec("INSERT INTO filter_benchmark_config VALUES(" +
          std::to_string(build_rows) + "," + std::to_string(probe_rows) + "," +
          std::to_string(overlap_percent) + "," +
          std::to_string(overlap_rows) + ")");
  db.exec("COMMIT");
  db.exec("ANALYZE");
  std::cout << "loaded,table_a_entries=" << build_rows
            << ",table_b_entries=" << probe_rows
            << ",overlap_percent=" << overlap_percent
            << ",overlap_entries=" << overlap_rows << '\n';
}

void load(Database &db, std::uint64_t build_rows, std::uint64_t probe_rows,
          std::uint64_t overlap_percent) {
  const std::uint64_t overlap_rows =
      calculate_overlap_rows(probe_rows, overlap_percent);
  if (overlap_rows > build_rows)
    throw std::runtime_error("Table A is too small for the requested overlap");

  db.exec("PRAGMA journal_mode=DELETE");
  db.exec("PRAGMA synchronous=OFF");
  db.exec("DROP TABLE IF EXISTS bloom_build");
  db.exec("CREATE TABLE bloom_build(k INTEGER PRIMARY KEY, keep INTEGER NOT NULL)");
  db.exec("BEGIN");
  {
    Statement insert_build(db.get(), "INSERT INTO bloom_build VALUES(?1,1)");
    for (std::uint64_t i = 0; i < build_rows; ++i) {
      insert_build.bind(random_key(i));
      insert_build.step();
      insert_build.reset();
    }
  }
  db.exec("COMMIT");
  load_probes(db, build_rows, probe_rows, overlap_percent);
}

void reload_probes(Database &db, std::uint64_t build_rows,
                   std::uint64_t probe_rows,
                   std::uint64_t overlap_percent) {
  {
    Statement count(db.get(), "SELECT count(*) FROM bloom_build WHERE keep=1");
    if (count.step() != SQLITE_ROW ||
        static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0)) !=
            build_rows)
      throw std::runtime_error(
          "Table A cardinality differs from --build_rows; use --load to rebuild it");
  }
  db.exec("PRAGMA journal_mode=DELETE");
  db.exec("PRAGMA synchronous=OFF");
  load_probes(db, build_rows, probe_rows, overlap_percent);
}

struct BenchmarkConfig {
  std::uint64_t build_rows;
  std::uint64_t probe_rows;
  std::uint64_t overlap_percent;
  std::uint64_t overlap_rows;
};

BenchmarkConfig configured_rows(Database &db) {
  Statement config(db.get(),
                   "SELECT build_rows,probe_rows,overlap_percent,overlap_rows "
                   "FROM filter_benchmark_config");
  if (config.step() != SQLITE_ROW)
    throw std::runtime_error("database is not loaded for this benchmark");
  BenchmarkConfig result{
      static_cast<std::uint64_t>(sqlite3_column_int64(config.get(), 0)),
      static_cast<std::uint64_t>(sqlite3_column_int64(config.get(), 1)),
      static_cast<std::uint64_t>(sqlite3_column_int64(config.get(), 2)),
      static_cast<std::uint64_t>(sqlite3_column_int64(config.get(), 3))};
  if (result.overlap_rows !=
      calculate_overlap_rows(result.probe_rows, result.overlap_percent))
    throw std::runtime_error("invalid overlap metadata in benchmark database");
  return result;
}

template <typename Filter>
TrialTimings run_once(Database &db, std::string_view filter_name,
                      std::string label,
                      std::uint64_t expected_build_rows,
                      std::uint64_t expected_probe_rows,
                      std::uint64_t expected_overlap_rows,
                      std::uint64_t overlap_percent, double load_factor,
                      ConstructionMode mode, bool validate,
                      std::vector<std::uint64_t> &build_keys,
                      std::vector<std::uint64_t> &hashed_keys,
                      std::vector<std::uint64_t> &scratch,
                      typename FilterAPI<Filter>::StreamBuffer *stream_buffers) {
  using API = FilterAPI<Filter>;
  const bool bulk = mode == ConstructionMode::Bulk;
  const bool streaming = mode == ConstructionMode::Streaming;

  build_keys.resize(expected_build_rows);
  if (bulk) {
    hashed_keys.resize(expected_build_rows);
    scratch.resize(expected_build_rows);
  } else if (streaming) {
    scratch.resize(kStreamingBatchSize);
  }
  const auto query_start = Clock::now();

  std::size_t build_index = 0;
  const auto scan_start = Clock::now();
  {
    Statement scan(db.get(), "SELECT k FROM bloom_build WHERE keep=1");
    while (scan.step() == SQLITE_ROW) {
      if (build_index == build_keys.size())
        throw std::runtime_error("Table A has more rows than benchmark metadata");
      build_keys[build_index++] =
          static_cast<std::uint64_t>(sqlite3_column_int64(scan.get(), 0));
    }
  }
  const double table_a_scan_seconds =
      std::chrono::duration<double>(Clock::now() - scan_start).count();
  if (build_index != expected_build_rows)
    throw std::runtime_error("Table A cardinality differs from benchmark metadata");

  const std::size_t truncated_entries = bulk ? build_keys.size() % 4 : 0;
  if (truncated_entries != 0) {
    build_keys.resize(build_keys.size() - truncated_entries);
    hashed_keys.resize(build_keys.size());
    scratch.resize(build_keys.size());
  }

  double vector_shuffle_seconds = 0.0;
  if (mode == ConstructionMode::Incremental) {
    const auto shuffle_start = Clock::now();
    std::mt19937_64 generator(UINT64_C(0x9e3779b97f4a7c15));
    std::shuffle(build_keys.begin(), build_keys.end(), generator);
    vector_shuffle_seconds =
        std::chrono::duration<double>(Clock::now() - shuffle_start).count();
  }
  const std::size_t requested_capacity =
      nominal_capacity(build_keys.size(), load_factor);
  Filter filter = streaming
                        ? construct_streaming_filter<Filter>(requested_capacity)
                        : construct_filter<Filter>(requested_capacity);

  // Time spent only marshalling keys into the streaming batch buffer (the two
  // resizes and the copy). This is harness overhead, not part of the algorithm,
  // so it is excluded from both the build and the query measurements below.
  double streaming_prep_seconds = 0.0;
  const auto build_start = Clock::now();
  if (bulk) {
    API::HashVec(&build_keys, &hashed_keys, &filter);
    API::Sort(hashed_keys, &filter, scratch, SortType::RadixAVX2);
    API::Fill(&hashed_keys, &filter, true, true);
  } else if (streaming) {
    std::vector<std::uint64_t> batch_buf(kStreamingBatchSize);
    const std::size_t num_entries = build_keys.size();
    for (std::size_t offset = 0; offset < num_entries;) {
      const std::size_t chunk_size =
          std::min(kStreamingBatchSize, num_entries - offset);
      const auto prep_start = Clock::now();
      batch_buf.resize(chunk_size);
      std::copy(build_keys.begin() + offset,
                build_keys.begin() + offset + chunk_size, batch_buf.begin());
      // radixSortAVX2 swaps the batch buffer with the scratch buffer on every
      // pass, so the scratch has to be exactly the batch size (matching the
      // reference streaming implementation, where tmp.size() == buffer_vec.size()).
      scratch.resize(chunk_size);
      streaming_prep_seconds +=
          std::chrono::duration<double>(Clock::now() - prep_start).count();
      API::ImprovedHashVec(&batch_buf, stream_buffers, &filter, scratch,
                           num_entries);
      offset += chunk_size;
    }
    API::FillWithBuffer(stream_buffers, &filter);
  } else {
    for (const std::uint64_t key : build_keys)
      API::Add(key, &filter);
  }
  const double filter_build_seconds =
      std::chrono::duration<double>(Clock::now() - build_start).count() -
      streaming_prep_seconds;

  std::uint64_t probe_entries = 0;
  std::uint64_t filter_positives = 0;
  std::uint64_t matches = 0;
  Statement probes(db.get(), "SELECT k FROM bloom_probe");
  Statement lookup(db.get(), "SELECT 1 FROM bloom_build WHERE k=?1 AND keep=1");
  while (probes.step() == SQLITE_ROW) {
    const std::uint64_t key =
        static_cast<std::uint64_t>(sqlite3_column_int64(probes.get(), 0));
    ++probe_entries;
    if (!API::Contain(key, &filter)) continue;
    ++filter_positives;
    lookup.bind(key);
    if (lookup.step() == SQLITE_ROW) ++matches;
    lookup.reset();
  }
  if (probe_entries != expected_probe_rows)
    throw std::runtime_error("Table B cardinality differs from benchmark metadata");

  const double query_seconds =
      std::chrono::duration<double>(Clock::now() - query_start).count() -
      vector_shuffle_seconds - streaming_prep_seconds;

  const std::uint64_t expected_matches = expected_overlap_rows;
  if (matches != expected_matches)
    throw std::runtime_error(
        "Table A/Table B overlap differs from the benchmark configuration; reload the database");

  std::uint64_t positive_checks = 0;
  std::uint64_t positive_false_negatives = 0;
  double validation_seconds = 0.0;
  if (validate) {
    const auto validation_start = Clock::now();
    positive_checks = build_keys.size();
    for (const std::uint64_t key : build_keys)
      if (!API::Contain(key, &filter))
        ++positive_false_negatives;
    validation_seconds =
        std::chrono::duration<double>(Clock::now() - validation_start).count();
  }

  const double construction_seconds =
      table_a_scan_seconds + filter_build_seconds;
  const double build_ratio = filter_build_seconds / query_seconds;
  const double construction_ratio = construction_seconds / query_seconds;

  std::cout << std::fixed << std::setprecision(9) << label
            << ",filter=" << filter_name
            << ",filter_backend=" << filter_backend<Filter>()
            << ",construction=" << construction_name(mode)
            << ",query_seconds=" << query_seconds
            << ",filter_build_seconds=" << filter_build_seconds
            << ",table_a_scan_seconds=" << table_a_scan_seconds
            << ",vector_shuffle_seconds=" << vector_shuffle_seconds
            << ",filter_construction_seconds=" << construction_seconds
            << ",build_query_ratio=" << build_ratio
            << ",construction_query_ratio=" << construction_ratio
            << ",table_a_entries=" << build_index
            << ",table_b_entries=" << probe_entries
            << ",overlap_percent=" << overlap_percent
            << ",overlap_entries=" << expected_overlap_rows
            << ",filter_entries=" << build_keys.size()
            << ",truncated_entries=" << truncated_entries
            << ",filter_bytes=" << API::get_byte_size(&filter)
            << ",requested_capacity=" << requested_capacity
            << ",requested_load_factor=" << load_factor
            << ",realized_nominal_load_factor="
            << static_cast<double>(build_keys.size()) / requested_capacity
            << ",realized_primary_load_factor="
            << realized_primary_load(build_keys.size(), filter)
            << ",validation_enabled=" << (validate ? "true" : "false")
            << ",positive_checks=" << positive_checks
            << ",positive_false_negatives=" << positive_false_negatives
            << ",validation_seconds=" << validation_seconds
            << ",filter_positives=" << filter_positives
            << ",false_positives=" << (filter_positives - matches)
            << ",matches=" << matches
            << ",expected_matches=" << expected_matches << '\n';

  return TrialTimings{filter_build_seconds,
                      query_seconds - filter_build_seconds -
                          table_a_scan_seconds,
                      table_a_scan_seconds, query_seconds};
}

template <typename Filter>
void run_trials(Database &db, std::string_view name, unsigned trials, bool warmup,
                std::uint64_t build_rows, std::uint64_t probe_rows,
                std::uint64_t overlap_rows, std::uint64_t overlap_percent,
                double load_factor, ConstructionMode mode, bool validate) {
  if (trials == 0)
    throw std::runtime_error("trials must be positive");

  const bool bulk = mode == ConstructionMode::Bulk;
  const bool streaming = mode == ConstructionMode::Streaming;

  std::vector<std::uint64_t> build_keys(build_rows);
  std::vector<std::uint64_t> hashed_keys(bulk ? build_rows : 0);
  std::vector<std::uint64_t> scratch(
      bulk ? build_rows : (streaming ? kStreamingBatchSize : 0));
  std::unique_ptr<StreamBufferArena<Filter>> arena;
  if (streaming) {
    const std::size_t probe_capacity =
        nominal_capacity(build_rows, load_factor);
    Filter probe = construct_streaming_filter<Filter>(probe_capacity);
    arena = std::make_unique<StreamBufferArena<Filter>>(
        FilterAPI<Filter>::GetNumBuckets(&probe));
  }

  const auto run = [&](const std::string &label) {
    if (streaming) arena->clear();
    return run_once<Filter>(db, name, label, build_rows, probe_rows,
                            overlap_rows, overlap_percent, load_factor, mode,
                            validate, build_keys, hashed_keys, scratch,
                            streaming ? arena->get() : nullptr);
  };

  if (warmup) run("warmup");
  TrialTimings totals;
  std::vector<double> query_samples;
  query_samples.reserve(trials);
  for (unsigned trial = 1; trial <= trials; ++trial) {
    const TrialTimings timing = run("trial=" + std::to_string(trial));
    totals += timing;
    query_samples.push_back(timing.total_seconds);
  }

  const double divisor = static_cast<double>(trials);
  const double query_mean = totals.total_seconds / divisor;
  double query_seconds_variance = 0.0;
  for (const double sample : query_samples) {
    const double deviation = sample - query_mean;
    query_seconds_variance += deviation * deviation;
  }
  query_seconds_variance /= divisor;

  std::cout << std::fixed << std::setprecision(9)
            << "average"
            << ",filter=" << name
            << ",filter_backend=" << filter_backend<Filter>()
            << ",construction=" << construction_name(mode)
            << ",measured_trials=" << trials
            << ",overlap_percent=" << overlap_percent
            << ",fill_seconds=" << totals.fill_seconds / divisor
            << ",query_without_fill_seconds="
            << totals.query_without_fill_seconds / divisor
            << ",scan_seconds=" << totals.scan_seconds / divisor
            << ",total_seconds=" << query_mean
            << ",query_seconds_variance=" << query_seconds_variance << '\n';
}

TrialTimings run_once_without_filter(
    Database &db, std::string label, std::uint64_t expected_build_rows,
    std::uint64_t expected_probe_rows, std::uint64_t expected_overlap_rows,
    std::uint64_t overlap_percent, bool validate) {
  const auto query_start = Clock::now();

  std::uint64_t probe_entries = 0;
  std::uint64_t matches = 0;
  Statement probes(db.get(), "SELECT k FROM bloom_probe");
  Statement lookup(db.get(), "SELECT 1 FROM bloom_build WHERE k=?1 AND keep=1");
  while (probes.step() == SQLITE_ROW) {
    const std::uint64_t key =
        static_cast<std::uint64_t>(sqlite3_column_int64(probes.get(), 0));
    ++probe_entries;
    lookup.bind(key);
    if (lookup.step() == SQLITE_ROW) ++matches;
    lookup.reset();
  }
  if (probe_entries != expected_probe_rows)
    throw std::runtime_error("Table B cardinality differs from benchmark metadata");

  const double query_seconds =
      std::chrono::duration<double>(Clock::now() - query_start).count();
  if (matches != expected_overlap_rows)
    throw std::runtime_error(
        "Table A/Table B overlap differs from the benchmark configuration; reload the database");

  const std::uint64_t positive_checks = validate ? expected_build_rows : 0;
  const double filter_build_seconds = 0.0;

  std::cout << std::fixed << std::setprecision(9) << label
            << ",filter=none"
            << ",filter_backend=none"
            << ",construction=none"
            << ",query_seconds=" << query_seconds
            << ",filter_build_seconds=" << filter_build_seconds
            << ",table_a_scan_seconds=0.000000000"
            << ",vector_shuffle_seconds=0.000000000"
            << ",filter_construction_seconds=0.000000000"
            << ",build_query_ratio=0.000000000"
            << ",construction_query_ratio=0.000000000"
            << ",table_a_entries=" << expected_build_rows
            << ",table_b_entries=" << probe_entries
            << ",overlap_percent=" << overlap_percent
            << ",overlap_entries=" << expected_overlap_rows
            << ",filter_entries=0"
            << ",truncated_entries=0"
            << ",filter_bytes=0"
            << ",requested_capacity=0"
            << ",requested_load_factor=0.000000000"
            << ",realized_nominal_load_factor=0.000000000"
            << ",realized_primary_load_factor=0.000000000"
            << ",validation_enabled=" << (validate ? "true" : "false")
            << ",positive_checks=" << positive_checks
            << ",positive_false_negatives=0"
            << ",validation_seconds=0.000000000"
            << ",filter_positives=" << probe_entries
            << ",false_positives=" << (probe_entries - matches)
            << ",matches=" << matches
            << ",expected_matches=" << expected_overlap_rows << '\n';

  return TrialTimings{filter_build_seconds, query_seconds, 0.0, query_seconds};
}

void run_trials_without_filter(Database &db, unsigned trials, bool warmup,
                               const BenchmarkConfig &config, bool validate) {
  if (trials == 0)
    throw std::runtime_error("trials must be positive");

  const auto run = [&](const std::string &label) {
    return run_once_without_filter(
        db, label, config.build_rows, config.probe_rows, config.overlap_rows,
        config.overlap_percent, validate);
  };

  if (warmup) run("warmup");
  TrialTimings totals;
  std::vector<double> query_samples;
  query_samples.reserve(trials);
  for (unsigned trial = 1; trial <= trials; ++trial) {
    const TrialTimings timing = run("trial=" + std::to_string(trial));
    totals += timing;
    query_samples.push_back(timing.total_seconds);
  }

  const double divisor = static_cast<double>(trials);
  const double query_mean = totals.total_seconds / divisor;
  double query_seconds_variance = 0.0;
  for (const double sample : query_samples) {
    const double deviation = sample - query_mean;
    query_seconds_variance += deviation * deviation;
  }
  query_seconds_variance /= divisor;

  std::cout << std::fixed << std::setprecision(9)
            << "average"
            << ",filter=none"
            << ",filter_backend=none"
            << ",construction=none"
            << ",measured_trials=" << trials
            << ",overlap_percent=" << config.overlap_percent
            << ",fill_seconds=" << totals.fill_seconds / divisor
            << ",query_without_fill_seconds="
            << totals.query_without_fill_seconds / divisor
            << ",scan_seconds=" << totals.scan_seconds / divisor
            << ",total_seconds=" << query_mean
            << ",query_seconds_variance=" << query_seconds_variance << '\n';
}

// Runs one unfiltered baseline followed by incremental, sorted bulk, and
// streaming bulk for both filters in one process.
void run_all(Database &db, unsigned trials, bool warmup,
             const BenchmarkConfig &config, double load_factor, bool validate) {
  run_trials_without_filter(db, trials, warmup, config, validate);
  run_trials<PQF::PQF_8_53>(db, "bcf", trials, warmup, config.build_rows,
                            config.probe_rows, config.overlap_rows,
                            config.overlap_percent, load_factor,
                            ConstructionMode::Incremental, validate);
  run_trials<PQF::PQF_8_53>(db, "bcf", trials, warmup, config.build_rows,
                            config.probe_rows, config.overlap_rows,
                            config.overlap_percent, load_factor,
                            ConstructionMode::Bulk, validate);
  run_trials<PQF::PQF_8_53>(db, "bcf", trials, warmup, config.build_rows,
                            config.probe_rows, config.overlap_rows,
                            config.overlap_percent, load_factor,
                            ConstructionMode::Streaming, validate);
  run_trials<PrefixFilter>(db, "pf", trials, warmup, config.build_rows,
                           config.probe_rows, config.overlap_rows,
                           config.overlap_percent, load_factor,
                           ConstructionMode::Incremental, validate);
  run_trials<PrefixFilter>(db, "pf", trials, warmup, config.build_rows,
                           config.probe_rows, config.overlap_rows,
                           config.overlap_percent, load_factor,
                           ConstructionMode::Bulk, validate);
  run_trials<PrefixFilter>(db, "pf", trials, warmup, config.build_rows,
                           config.probe_rows, config.overlap_rows,
                           config.overlap_percent, load_factor,
                           ConstructionMode::Streaming, validate);
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (sqlite3_initialize() != SQLITE_OK)
      throw std::runtime_error("sqlite3_initialize failed");

    cxxopts::Options options(
        "filter_build_sqlite3",
        "SQLite semijoin benchmark running an unfiltered baseline, then BCF and "
        "Prefix Filter with incremental, sorted bulk, and streaming bulk "
        "construction in one process");
    options.add_options()
        ("h,help", "Print help")
        ("load", "Create and populate the benchmark database")
        ("reload_probes", "Replace Table B while retaining the existing Table A")
        ("run", "Run the unfiltered baseline followed by all filter configurations")
        ("no_filter", "Run only the unfiltered baseline")
        ("database", "Database path",
         cxxopts::value<std::string>()->default_value("filter_build.sqlite"))
        ("build_rows", "Rows in Table A",
         cxxopts::value<std::uint64_t>()->default_value(std::to_string(kDefaultBuildRows)))
        ("probe_rows", "Rows in Table B",
         cxxopts::value<std::uint64_t>()->default_value(std::to_string(kDefaultProbeRows)))
        ("overlap_percent", "Percentage of Table B keys present in Table A",
         cxxopts::value<std::uint64_t>()->default_value(
             std::to_string(kDefaultOverlapPercent)))
        ("trials", "Measured executions",
         cxxopts::value<unsigned>()->default_value("3"))
        ("warmup", "Run one labeled warmup before measured trials",
         cxxopts::value<bool>()->default_value("true"))
        ("validate", "Validate every inserted build key after each execution");
    const auto result = options.parse(argc, argv);
    if (result.count("help") ||
        (!result.count("load") && !result.count("reload_probes") &&
         !result.count("run"))) {
      std::cout << options.help() << '\n';
      return 0;
    }
    if (result.count("load") && result.count("reload_probes"))
      throw std::runtime_error("load and reload_probes are mutually exclusive");

    Database db(result["database"].as<std::string>());
    db.exec("PRAGMA cache_size=-2000000");
    db.exec("PRAGMA temp_store=MEMORY");
    if (result.count("load")) {
      const auto build_rows = result["build_rows"].as<std::uint64_t>();
      const auto probe_rows = result["probe_rows"].as<std::uint64_t>();
      if (build_rows == 0 || probe_rows == 0)
        throw std::runtime_error("row counts must be positive");
      const auto overlap_percent =
          result["overlap_percent"].as<std::uint64_t>();
      load(db, build_rows, probe_rows, overlap_percent);
    }
    if (result.count("reload_probes")) {
      const auto build_rows = result["build_rows"].as<std::uint64_t>();
      const auto probe_rows = result["probe_rows"].as<std::uint64_t>();
      if (build_rows == 0 || probe_rows == 0)
        throw std::runtime_error("row counts must be positive");
      const auto overlap_percent =
          result["overlap_percent"].as<std::uint64_t>();
      reload_probes(db, build_rows, probe_rows, overlap_percent);
    }
    if (result.count("run")) {
      const auto config = configured_rows(db);
      const auto trials = result["trials"].as<unsigned>();
      const auto warmup = result["warmup"].as<bool>();
      const bool validate = result.count("validate") != 0;
      if (result.count("no_filter"))
        run_trials_without_filter(db, trials, warmup, config, validate);
      else
        run_all(db, trials, warmup, config, kLoadFactor, validate);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
