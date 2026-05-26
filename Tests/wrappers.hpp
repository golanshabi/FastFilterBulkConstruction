/* Taken from
 * https://github.com/FastFilter/fastfilter_cpp
 * */

//#pragma once
#ifndef FILTERS_WRAPPERS_HPP
#define FILTERS_WRAPPERS_HPP
// #define CONTAIN_ATTRIBUTES inline
#define CONTAIN_ATTRIBUTES __attribute__((always_inline))
// #define CONTAIN_ATTRIBUTES __attribute__((noinline))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define DEBUG_MACRO __FILE__ << ":" << __LINE__ << " " << __FUNCTION__
#define DEBUG(x) std::cout << x << std::endl

#define IS_POW_OF_2(x) ((x & (x - 1)) == 0)
// #define TIME_TEST
#define GEN_NEW_PD
#define HASH_AHEAD

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <omp.h>
#include <sched.h>
#include <thread>
#include <atomic>
#include "../Bloom_Filter/bloom.hpp"
#include "../Bloom_Filter/simd-block-fixed-fpp.h"
#include "../Bloom_Filter/Impala512.h"
#include "../Bloom_Filter/simd-block.h"
#include "../Prefix-Filter/min_pd256.hpp"
#include "../TC-Shortcut/TC-shortcut.hpp"
#include "../bcf/PartitionQuotientFilter.hpp"
#include "../cuckoofilter/src/cuckoofilter.h"
#include "../cuckoofilter/src/cuckoofilter_stable.h"
// #include "linux-perf-events.h"
#include "TimeTracker.hpp"
#include "sort_bucket_keys.hpp"

#include <map>
#include <unistd.h>
#include <signal.h>
#include <ctime>
#include <queue>
#include <sys/mman.h>  // For mmap(), munmap(), madvise(), msync()
#include <fcntl.h>     // For open(), O_RDWR, O_CREAT
#include <unistd.h>    // For close(), ftruncate()
#include <sys/stat.h>  // For fstat()

enum filter_id {
    Trivial_id,
    CF,
    SIMD,
    BBF,
    BBF_gen_id,
    SIMD_fixed,
    BBF_Flex,
    prefix_id,
    TC_shortcut_id,
    VQF_Wrapper_id,
    cf1ma_id,
    cf3ma_id,
    cf_stable_id,
    cf_flex_id,
    bloom_id,
    bf_ma_id,
    bloomSimple_id,
    bloomTrivial_id,
    bloomPower_id,
    bloomPowerDoubleHash_id,
    simple_bbf_id,
    pqf_id,
};

extern int THREAD_NUM;

class FileIterator {
public:
    void init(uint64_t size_to_iter, uint16_t iter_index, uint64_t buf_size, int fd, uint64_t* shared_buffer) {
        _fd = fd;
        _index = iter_index;
        _size_to_iter = size_to_iter;
        _buf_size = buf_size;
        _current_position = _size_to_iter * _index;
        _buffer = shared_buffer + (iter_index * (_buf_size / sizeof(uint64_t)));
        _cur = _buffer;
        _cur_end = _cur;
        _actual_end = _cur + (_size_to_iter >> 3);
        
        // Seek to the correct position in the file
        if (lseek(_fd, _current_position, SEEK_SET) == -1) {
            std::cerr << "Failed to seek file for iterator " << _index << std::endl;
            exit(1);
        }
        refillBuffer();
    }

    uint64_t next() {
        if (_cur == _cur_end) {
            if (!refillBuffer()) {
                return UINT64_MAX;
            }
        }
        return *_cur++;
    }

private:
    uint64_t _index;
    uint64_t _size_to_iter;
    uint64_t _buf_size;
    uint64_t _current_position;
    int _fd;
    uint64_t* _buffer;
    uint64_t* _cur;
    uint64_t* _cur_end;
    uint64_t* _actual_end;

    bool refillBuffer() {
        ssize_t bytes_read = read(_fd, _buffer, _buf_size);
        if (bytes_read <= 0) {
            return false;
        }
        _cur = _buffer;
        _cur_end = _cur + (bytes_read / sizeof(uint64_t));
        return true;
    }
};

struct NumWithInd {
    uint64_t val;
    uint16_t ind;

    bool operator>(const NumWithInd& other) const {
        return val > other.val;
    }
};

class SortedIterator {
private:
    std::vector<FileIterator> _iters;
    std::priority_queue<NumWithInd, std::vector<NumWithInd>, std::greater<NumWithInd>> _min_heap;
    int _fd;
    uint64_t _num_iters;
    std::vector<uint64_t> _shared_buffer;

public:
    SortedIterator(std::string filename, uint64_t file_size, uint64_t batch_size,
                   std::vector<NumWithInd>& preallocated_vec, uint64_t num_iters)
        : _num_iters(num_iters), _min_heap(std::greater<NumWithInd>(), std::move(preallocated_vec)) {
        if (file_size % _num_iters != 0) {
            exit(1);
        }

        while (!_min_heap.empty()) {
            _min_heap.pop();
        }

        _fd = open(filename.c_str(), O_RDONLY);
        if (_fd < 0) {
            exit(1);
        }

        struct stat st;
        if (fstat(_fd, &st) < 0) {
            close(_fd);
            exit(1);
        }

        auto start_time = std::chrono::high_resolution_clock::now();
        uint64_t size_to_iter = file_size / _num_iters;
        _shared_buffer.resize(_num_iters * (batch_size / sizeof(uint64_t)));
        _iters.resize(_num_iters);
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        std::cout << "Time spent allocating:" << std::setw(10) << elapsed.count() << " s" << std::endl;

        for (uint64_t i = 0; i < _num_iters; i++) {
            _iters[i].init(size_to_iter, i, batch_size, _fd, _shared_buffer.data());
        }

        for (uint64_t i = 0; i < _num_iters; i++) {
            uint64_t val = _iters[i].next();
            _min_heap.push(NumWithInd{val, static_cast<uint16_t>(i)});
        }
    }

    ~SortedIterator() {
        close(_fd);
    }

    bool is_done() { return _min_heap.empty(); }

    uint64_t next() {
        NumWithInd val = _min_heap.top();
        _min_heap.pop();
        next_iter(val);
        return val.val;
    }

    void next_iter(NumWithInd val) {
        uint64_t next_val = _iters[val.ind].next();
        if (next_val != UINT64_MAX) {
            _min_heap.push(NumWithInd{next_val, val.ind});
        }
    }
};

#include <cstdint>
#include <limits>

template <uint8_t MaxLen>
class FixedArray {
public:
    uint8_t size = 0;
    uint16_t data[MaxLen]{};
    FixedArray() {
        for (uint8_t i = 0; i < MaxLen; i++) {
            data[i] = std::numeric_limits<uint16_t>::max();
        }
    }

    inline uint16_t insert(uint16_t value) {
        if (size < MaxLen) {
            data[size] = value;
            size++;
            return std::numeric_limits<uint16_t>::max();
        }
        uint16_t maxVal = 0;
        uint8_t maxIndex = 0;
    
        for (uint8_t i = 0; i < MaxLen; i++) {
            uint16_t is_greater = -(data[i] > maxVal);  // -1 (0xFFFF) if true, else 0
            maxVal = (data[i] & is_greater) | (maxVal & ~is_greater);
            maxIndex = (i & is_greater) | (maxIndex & ~is_greater);
        }


        uint16_t replace = -(maxVal > value);  // -1 if maxVal > value, 0 otherwise
        data[maxIndex] = (data[maxIndex] & ~replace) | (value & replace);
        maxVal = (maxVal & replace) | (value & ~replace);

        return maxVal;
    }
};

template<typename Table>
struct FilterAPI {
};

inline uint64_t elapsed_ns(const std::chrono::high_resolution_clock::time_point &start_time,
                           const std::chrono::high_resolution_clock::time_point &end_time);
inline double compute_fill_time_per_entry_ns(uint64_t fill_time_ns, size_t inserted_entries);
inline void print_unsupported_filter_operation(const char *operation);
static void SortPackedVector(std::vector<u64> &v_add,
                             std::vector<uint64_t> &temp_arr_to_use,
                             size_t num_bits,
                             double &fill_time_per_entry_ns);
static void radixSortAVX2(std::vector<uint64_t> &arr, int numBits, int totalBits,
                          std::vector<uint64_t> &temp, int startingBit);

template<size_t FingerprintBits_, size_t Capacity_, size_t NumQuots_, size_t HeaderBytes_,
         size_t ReservedBits_ = 0, u64 NotOverflowedBit_ = 0, u64 LastQuotMask_ = 0>
struct SortedBucketLayout {
    static constexpr size_t RemainderBits = 8;
    static constexpr size_t FingerprintBits = FingerprintBits_;
    static constexpr size_t Capacity = Capacity_;
    static constexpr size_t NumQuots = NumQuots_;
    static constexpr size_t HeaderBytes = HeaderBytes_;
    static constexpr size_t ReservedBits = ReservedBits_;
    static constexpr u64 NotOverflowedBit = NotOverflowedBit_;
    static constexpr u64 LastQuotMask = LastQuotMask_;

    static constexpr bool TracksOverflow = NotOverflowedBit != 0;
    static constexpr u64 QuotMask = (UINT64_C(1) << (FingerprintBits - RemainderBits)) - 1;
    static constexpr size_t MaxHeaderBits = ReservedBits + NumQuots + Capacity;

    using HeaderWord = std::conditional_t<(MaxHeaderBits < 64), u64, __uint128_t>;

    static constexpr size_t bucket_of(u64 value) { return value >> FingerprintBits; }
    static constexpr u64 quot_of(u64 value) { return (value >> RemainderBits) & QuotMask; }
    static constexpr u8 rem_of(u64 value) { return (u8) value; }
};

struct SortedBucketBytes {
    u8 *header;
    u8 *body;
};

// The keys of one bucket are either packed with their bucket index (u64), or already grouped
// per bucket and packed as quotient and remainder only (uint16_t).
//
// The bucket does not have to be empty: the header is always written in full, count == 0 included,
// and a unary header can never point at a body byte past the last key, so whatever is left in the
// rest of the body is never read.
template<typename Layout, typename Key>
inline void WriteSortedBucket(const Key *values, size_t count, bool overflowed,
                              u8 *header_bytes, u8 *body) {
    using Word = typename Layout::HeaderWord;

    Word key_bits = 0;

    for (size_t i = 0; i < count; ++i) {
        const u64 value = values[i];
        key_bits |= ((Word) 1) << (Layout::quot_of(value) + i + Layout::ReservedBits);
        body[i] = Layout::rem_of(value);
    }

    // Every bit of the unary region that is not a key bit is a quotient separator, and
    // everything above the region stays zero.
    Word header = ((((Word) 1) << (count + Layout::NumQuots)) - 1) << Layout::ReservedBits;
    header &= ~key_bits;

    if constexpr (Layout::TracksOverflow) {
        // An overflowing bucket clears the flag and records its last occupied quotient,
        // which for sorted input is the quotient of the last key that fit.
        header |= overflowed ? (Layout::quot_of(values[count - 1]) & Layout::LastQuotMask)
                             : Layout::NotOverflowedBit;
    } else {
        (void) overflowed;
    }

    memcpy(header_bytes, &header, Layout::HeaderBytes);
}

// Writes as many of one bucket's sorted keys as it can hold and hands the rest over, which is
// all a bulk build has left to do once a bucket's keys are contiguous and sorted. Returns the
// number of keys that were written into the bucket.
template<typename Layout, typename Key, typename HandleOverflow>
inline size_t FillSortedBucket(size_t bucket_index, const Key *keys, size_t num_keys,
                               SortedBucketBytes bucket, HandleOverflow &&handle_overflow) {
    const bool overflowed = num_keys > Layout::Capacity;
    const size_t count = overflowed ? Layout::Capacity : num_keys;

    WriteSortedBucket<Layout>(keys, count, overflowed, bucket.header, bucket.body);

    if (overflowed) {
        handle_overflow(bucket_index, keys + count, num_keys - count);
    }

    return count;
}

// A streaming build hashes a key into [bucket index | 16 bit bucket key], which lets one radix
// sort over the bucket bits group the keys and lets a bucket's buffer hold just the 16 bit part.
inline constexpr size_t StreamKeyBits = 16;

static void radixSortAVX2Parallel(std::vector<uint64_t>& arr, int numBits, int totalBits,
                                  std::vector<uint64_t> &temp, int firstSortBit,
                                  int finalSortBit);

// Bits of the first radix pass, whose ranges the remaining passes then sort in parallel. A wider
// first pass cuts the keys into smaller ranges, which keeps the passes over a range in cache, and
static int firstPassBits() {
    if (THREAD_NUM > 1) {
        return 11;
    }
    return 8;
}

/**
 * @brief Hashes one chunk of keys, groups it by bucket, and appends each key to its bucket buffer.
 *
 * A buffer keeps the smallest keys of its bucket, so a key it evicts is one the bucket could never
 * have held and the filter's second level has to take it.
 *
 * @param keys              Keys to add, overwritten with their packed hashes.
 * @param buffers           One key buffer per bucket.
 * @param num_buckets       Number of buckets in the filter's first level.
 * @param temp              Scratch space for the radix sort, as large as keys.
 * @param pack_key          key -> [bucket index | 16 bit bucket key].
 * @param handle_eviction   (segment, bucket index, evicted key) for every key a buffer gave up.
 */
template<typename Buffer, typename PackKey, typename HandleEviction>
inline void HashIntoBucketBuffers(std::vector<u64> *keys, Buffer *buffers, size_t num_buckets,
                                  std::vector<uint64_t> &temp, PackKey &&pack_key,
                                  HandleEviction &&handle_eviction) {
    if (keys->empty()) {
        return;
    }

    #ifdef _OPENMP
    #pragma omp parallel for num_threads(THREAD_NUM) schedule(static)
    #endif
    for (size_t i = 0; i < keys->size(); ++i) {
        (*keys)[i] = pack_key((*keys)[i]);
    }

    const size_t bucket_bits = (size_t) std::ceil(std::log2(num_buckets));
    // const int radix = bucket_bits > 23 ? 9 : 8;
    // Only the bucket decides where a key is written, so the key bits are left unsorted here and
    // each bucket sorts its own keys once it is built.
    uint64_t first_pass = firstPassBits();
    radixSortAVX2Parallel(
            *keys, first_pass, StreamKeyBits + bucket_bits, temp, 9, StreamKeyBits);

    // Each range is moved to bucket boundaries. Consequently, a bucket is owned by exactly one
    // thread and appending to its buffer needs no synchronization.
    const size_t thread_count = std::min<size_t>(
            std::max(THREAD_NUM, 1), keys->size());
    const size_t step_size = keys->size() / thread_count;

    #ifdef _OPENMP
    #pragma omp parallel for num_threads(THREAD_NUM) schedule(static) proc_bind(spread)
    #endif
    for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        size_t start = thread_index * step_size;
        size_t end = thread_index + 1 == thread_count
                   ? keys->size()
                   : (thread_index + 1) * step_size;

        if (start != 0 && start < keys->size()) {
            // The previous range already took the whole bucket it ended inside, so this one skips
            // that bucket and only that one. Keying off the key at start instead would skip a
            // bucket nobody took whenever a range happens to begin on a bucket boundary.
            const size_t split_bucket = (*keys)[start - 1] >> StreamKeyBits;
            while (start < keys->size() &&
                   ((*keys)[start] >> StreamKeyBits) == split_bucket) {
                ++start;
            }
        }
        if (end < keys->size()) {
            const size_t split_bucket = (*keys)[end - 1] >> StreamKeyBits;
            while (end < keys->size() &&
                   ((*keys)[end] >> StreamKeyBits) == split_bucket) {
                ++end;
            }
        }

        for (size_t i = start; i < end; ++i) {
            const u64 packed = (*keys)[i];
            const size_t bucket_index = packed >> StreamKeyBits;
            const uint16_t evicted = buffers[bucket_index].insert((uint16_t) packed);
            if (evicted != std::numeric_limits<uint16_t>::max()) {
                handle_eviction(thread_index, bucket_index, evicted);
            }
        }
    }
}

template<typename Layout, typename LocateBucket, typename HandleOverflow>
inline size_t FillSortedBuckets(const u64 *packed, size_t num_values,
                                LocateBucket &&locate_bucket,
                                HandleOverflow &&handle_overflow) {
    size_t entries_in_buckets = 0;
    size_t begin = 0;

    while (begin < num_values) {
        const size_t bucket_index = Layout::bucket_of(packed[begin]);

        size_t end = begin + 1;
        while (end < num_values && Layout::bucket_of(packed[end]) == bucket_index) {
            ++end;
        }

        const SortedBucketBytes bucket = locate_bucket(bucket_index);
        _mm_prefetch(reinterpret_cast<const char *>(bucket.header), _MM_HINT_T0);

        // find start and end of a bucket and then bulk build it
        entries_in_buckets += FillSortedBucket<Layout>(bucket_index, packed + begin, end - begin,
                                                      bucket, handle_overflow);
        begin = end;
    }

    return entries_in_buckets;
}

template<std::size_t SizeRemainders, std::size_t BucketNumMiniBuckets,
         std::size_t FrontyardBucketCapacity, std::size_t BackyardBucketCapacity,
         std::size_t FrontyardToBackyardRatio, std::size_t FrontyardBucketSize,
         std::size_t BackyardBucketSize, bool FastSQuery, bool Threaded>
struct FilterAPI<PQF::PartitionQuotientFilter<
        SizeRemainders, BucketNumMiniBuckets, FrontyardBucketCapacity,
        BackyardBucketCapacity, FrontyardToBackyardRatio, FrontyardBucketSize,
        BackyardBucketSize, FastSQuery, Threaded>> {
    using Table = PQF::PartitionQuotientFilter<
            SizeRemainders, BucketNumMiniBuckets, FrontyardBucketCapacity,
            BackyardBucketCapacity, FrontyardToBackyardRatio, FrontyardBucketSize,
            BackyardBucketSize, FastSQuery, Threaded>;

    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        return Table(add_count, true, allocate);
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    CONTAIN_ATTRIBUTES static uint64_t Reduce(uint64_t key, const Table *table) {
        return static_cast<uint64_t>(
                (static_cast<__uint128_t>(key) * table->range) >> 64);
    }

    inline static constexpr unsigned MiniBucketBits =
            std::bit_width(BucketNumMiniBuckets - 1);
    inline static constexpr unsigned FingerprintBits =
            SizeRemainders + MiniBucketBits;
    inline static constexpr uint64_t RemainderMask =
            (uint64_t{1} << SizeRemainders) - 1;
    inline static constexpr uint64_t MiniBucketMask =
            (uint64_t{1} << MiniBucketBits) - 1;

    CONTAIN_ATTRIBUTES static uint64_t EncodeBucketFingerprint(uint64_t reduced_key) {
        const uint64_t quotient = reduced_key >> SizeRemainders;
        const uint64_t bucket = quotient / BucketNumMiniBuckets;
        const uint64_t mini_bucket = quotient % BucketNumMiniBuckets;
        const uint64_t remainder = reduced_key & RemainderMask;
        const uint64_t fingerprint = (mini_bucket << SizeRemainders) | remainder;
        return (bucket << FingerprintBits) | fingerprint;
    }

    static_assert(SizeRemainders == 8, "Bulk construction is written for the PQF_8_53 layout");
    using FrontyardLayout =
            SortedBucketLayout<FingerprintBits, FrontyardBucketCapacity, BucketNumMiniBuckets,
                               Table::FrontyardHeaderBytes>;

    static SortedBucketBytes LocateFrontyardBucket(Table *table, size_t bucket_index) {
        return SortedBucketBytes{table->getFrontyardHeaderByIndex(bucket_index),
                                 table->getFrontyardBodyByIndex(bucket_index)};
    }

    // A streaming build buffers the keys of a front-yard bucket while hashing, and gives the bucket
    // room beyond its capacity so that only keys the backyard really has to take are evicted early.
    // At the load the filter is built for, a bucket sees about FrontyardBucketCapacity keys, and the
    // slack covers the spread around that.
    static constexpr size_t StreamBufferCapacity = FrontyardBucketCapacity + 13;
    using StreamBuffer = FixedArray<StreamBufferCapacity>;
    static_assert(StreamBufferCapacity <= MaxSortedBucketKeys,
                  "SortBucketKeys has to cover a full buffer");
    static_assert(sizeof(StreamBuffer) >= 2 * FrontyardBucketSize,
                  "Buckets [k, 2k) are written over the buffers below k, which needs a bucket to be "
                  "at most half a buffer");

    static size_t GetNumBuckets(Table *table) {
        return table->getNumFrontyardBuckets();
    }

    static u64 PackOverflow(size_t bucket_index, u64 fingerprint) {
        return (((u64) bucket_index) << FingerprintBits) | fingerprint;
    }

    // One lock byte per backyard bucket. A mutex per bucket would be an array far too big to stay
    // cached, so taking it would cost a trip to memory on top of the bucket's own; a byte keeps the
    // whole array small enough that locking stays in cache.
    class BackyardLocks {
      public:
        explicit BackyardLocks(size_t num_buckets) : locks_(num_buckets) {}

        void LockPair(size_t first, size_t second) {
            if (first == second) {
                Lock(first);
                return;
            }
            // Taken in index order, so two insertions sharing a bucket cannot deadlock.
            if (first > second) std::swap(first, second);
            Lock(first);
            Lock(second);
        }

        void UnlockPair(size_t first, size_t second) {
            if (first == second) {
                Unlock(first);
                return;
            }
            Unlock(first);
            Unlock(second);
        }

        // Relocation planning takes one bucket at a time. Its final commit phase passes a sorted,
        // deduplicated set here, so every worker uses the same global acquisition order.
        void Lock(size_t index) {
            while (locks_[index].exchange(1, std::memory_order_acquire) != 0) {
                _mm_pause();
            }
        }

        void Unlock(size_t index) {
            locks_[index].store(0, std::memory_order_release);
        }

        void LockBuckets(const size_t *sorted_indices, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                Lock(sorted_indices[i]);
            }
        }

        void UnlockBuckets(const size_t *sorted_indices, size_t count) {
            while (count != 0) {
                Unlock(sorted_indices[--count]);
            }
        }

        void LockBucketRange(size_t count) {
            for (size_t i = 0; i < count; ++i) {
                Lock(i);
            }
        }

        void UnlockBucketRange(size_t count) {
            while (count != 0) {
                Unlock(--count);
            }
        }

      private:
        std::vector<std::atomic<uint8_t>> locks_;
    };

    // Puts one overflowing key in the backyard while other front-yard ranges do the same. The
    // common two-choice insertion locks only its two buckets; a full pair is rebalanced immediately
    // through an ordered, revalidated multi-bucket relocation in the table.
    static bool InsertOverflowConcurrently(Table *table, BackyardLocks &locks, u64 packed) {
        return table->bulkInsertOverflowConcurrently(packed, locks);
    }

    // One lock set covers the whole streaming build, including evictions produced while hashing
    // and overflows produced later while the front yard is written.
    struct StreamingBuildState {
        explicit StreamingBuildState(Table *table)
                : backyard_locks(table->getNumBackyardBuckets()) {}

        void ThrowIfInsertFailed() const {
            if (insert_failed.load()) {
                throw std::logic_error(
                        "Breadcrumb Filter backyard insertion failed during bulk construction");
            }
        }

        BackyardLocks backyard_locks;
        std::atomic<bool> insert_failed{false};
    };

    // Where a front-yard bucket sends the keys it cannot hold. Every overflow, including one that
    // needs a third backyard bucket, is inserted while the front yard is still being built.
    struct BackyardSink {
        Table *table;
        StreamingBuildState *build_state;

        inline void Add(size_t bucket_index, uint16_t fingerprint) const {
            const u64 packed = PackOverflow(bucket_index, fingerprint);
            if (!InsertOverflowConcurrently(table, build_state->backyard_locks, packed)) {
                build_state->insert_failed.store(true);
            }
        }
    };

    static void ImprovedHashVec(std::vector<u64> *v_orig, StreamBuffer *bucket_array, Table *table,
                                std::vector<u64> &tmp, size_t num_entries,
                                StreamingBuildState &build_state) {
        HashIntoBucketBuffers(
                v_orig, bucket_array, table->getNumFrontyardBuckets(), tmp,
                [table](u64 key) {
                    const u64 reduced_key = Reduce(key, table);
                    const u64 quotient = reduced_key >> SizeRemainders;
                    const u64 bucket_index = quotient / BucketNumMiniBuckets;
                    const u64 mini_bucket = quotient - bucket_index * BucketNumMiniBuckets;
                    return (bucket_index << StreamKeyBits) | (mini_bucket << SizeRemainders) |
                           (reduced_key & RemainderMask);
                },
                [table, &build_state](size_t, size_t bucket_index, uint16_t evicted) {
                    const u64 packed = PackOverflow(bucket_index, evicted);
                    if (!InsertOverflowConcurrently(
                                table, build_state.backyard_locks, packed)) {
                        build_state.insert_failed.store(true);
                    }
                });

        // HashIntoBucketBuffers has joined its OpenMP workers, so throwing is safe here.
        build_state.ThrowIfInsertFailed();
    }

    static void ProcessFrontyardBucket(size_t bucket_index, StreamBuffer *arr_buff, Table *table,
                                       const BackyardSink &sink) {
        const uint16_t *const keys = arr_buff[bucket_index].data;
        const size_t num_keys = arr_buff[bucket_index].size;

        // Grouping by mini bucket is all a bucket needs; a query never sees the order within one.
        uint16_t grouped[StreamBufferCapacity];
        GroupBucketKeysByQuotient<BucketNumMiniBuckets>(keys, num_keys, grouped);

        FillSortedBucket<FrontyardLayout>(
                bucket_index, grouped, num_keys, LocateFrontyardBucket(table, bucket_index),
                [&sink](size_t index, const uint16_t *overflowing, size_t count) {
                    for (size_t i = 0; i < count; ++i) {
                        sink.Add(index, overflowing[i]);
                    }
                });
    }

    /**
     * @brief Builds the whole front yard from the per-bucket key buffers of the hashing pass.
     *
     * The buffers already group the keys by bucket, so unlike the sorted bulk build there are no
     * runs to look for, only each bucket's own keys to group by mini bucket. The front yard is
     * aliased one buffer behind the buffers, which puts a bucket entirely before the keys of its
     * own buffer, so every buffer is consumed before it can be overwritten. Keys a bucket cannot
     * hold go to the backyard as that bucket is written.
     *
     * A query masks one mini bucket's slots and compares every remainder in them, and reaches the
     * backyard only when those slots run to the end of the bucket, so the bucket only needs its
     * keys grouped by mini bucket and the remainders inside one can stay in any order. The
     * incremental insert leaves them in arrival order for the same reason.
     */
    static void FillWithBuffer(StreamBuffer *arr_buff, Table *table,
                               StreamingBuildState &build_state) {
        table->setFrontyardArray(arr_buff - 1);
        const size_t num_buckets = table->getNumFrontyardBuckets();

        // A wave only has to hand out distinct front-yard buckets. A front-yard bucket is less than
        // half a stream buffer, so building [k, 2k) overwrites only buffers below k, all of which an
        // earlier wave already consumed.
        size_t wave_start = 0;
        size_t wave_end = 2;
        while (wave_start < num_buckets) {
            const size_t bucket_end = std::min(wave_end, num_buckets);
            const size_t buckets_in_wave = bucket_end - wave_start;
            const size_t num_segments = std::min<size_t>(std::max(THREAD_NUM, 1), buckets_in_wave);
            const size_t segment_size = (buckets_in_wave + num_segments - 1) / num_segments;

            #ifdef _OPENMP
            #pragma omp parallel for num_threads(THREAD_NUM) schedule(static)
            #endif
            for (size_t segment = 0; segment < num_segments; ++segment) {
                const size_t segment_begin = wave_start + segment * segment_size;
                const size_t segment_end = std::min(segment_begin + segment_size, bucket_end);
                const BackyardSink sink{table, &build_state};
                for (size_t bucket_index = segment_begin; bucket_index < segment_end;
                     ++bucket_index) {
                    ProcessFrontyardBucket(bucket_index, arr_buff, table, sink);
                }
            }

            // By the implicit OpenMP barrier every worker has released its bucket locks, so this
            // is the first safe place to report a worker's failure by throwing.
            build_state.ThrowIfInsertFailed();

            wave_start = wave_end;
            wave_end <<= 1;
        }
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] PartitionQuotientFilter\n";
        size_t inserted_entries = 0;
        std::chrono::high_resolution_clock::time_point start_time;

        if (!sorted) {
            start_time = std::chrono::high_resolution_clock::now();
            for (const auto key : *v_add) {
                const uint64_t reduced_key = Reduce(key, table);
                if (!table->insert(reduced_key)) {
                    throw std::logic_error(
                        "Breadcrumb Filter backyard insertion failed during construction");
                }
                ++inserted_entries;
            }
        } else if (!v_add->empty()) {
            // HashVec packs sorted BCF entries as:
            // [front-yard bucket | mini-bucket | remainder].
            // The current caller does not propagate is_hashed to Fill.
            (void)is_hashed;

            start_time = std::chrono::high_resolution_clock::now();
            size_t overflow_entries = 0;
            const size_t frontyard_entries = FillSortedBuckets<FrontyardLayout>(
                    v_add->data(), v_add->size(),
                    [table](size_t bucket_index) {
                        return LocateFrontyardBucket(table, bucket_index);
                    },
                    [table, &overflow_entries](size_t, const u64 *overflowing,
                                               size_t count) {
                        for (size_t i = 0; i < count; ++i) {
                            if (!table->bulkInsertOverflowPacked(overflowing[i])) {
                                throw std::logic_error(
                                        "Breadcrumb Filter backyard insertion failed "
                                        "during bulk construction");
                            }
                        }
                        overflow_entries += count;
                    });
            inserted_entries = frontyard_entries + overflow_entries;
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        #ifndef SORT_ONLY
            double time = compute_fill_time_per_entry_ns(
                elapsed_ns(start_time, end_time),
                inserted_entries);
            std::cout << "fill=" << time << std::endl;
            fill_time_per_entry_ns += time;
        #endif
        // std::cout << "Inserted entries: " << inserted_entries << std::endl;
    }

    static void HashVec(std::vector<u64> *v_orig, std::vector<u64> *v_hashed, Table *table) {
        v_hashed->resize(v_orig->size());
        const auto start_time = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < v_orig->size(); ++i) {
            const uint64_t reduced_key = Reduce(v_orig->at(i), table);
            v_hashed->at(i) = EncodeBucketFingerprint(reduced_key);
        }
        const auto end_time = std::chrono::high_resolution_clock::now();
#ifdef TIME_TEST
        const std::chrono::duration<double> elapsed = end_time - start_time;
        std::cout << std::setw(20) << "Reduction elapsed time:"
                  << std::setw(10) << elapsed.count() << " s" << std::endl;
#endif
#ifndef SORT_ONLY
        double time = compute_fill_time_per_entry_ns(
            elapsed_ns(start_time, end_time),
            v_orig->size());
        std::cout << "hash=" << time << std::endl;
        fill_time_per_entry_ns += time;
#endif
    }

    static void Sort(std::vector<u64> &v_add, Table *table,
                     std::vector<uint64_t> &temp_arr_to_use) {
        const size_t frontyard_bucket_count =
                (table->capacity + BucketNumMiniBuckets - 1) /
                BucketNumMiniBuckets;
        const size_t bucket_bits = std::bit_width(frontyard_bucket_count - 1);
        SortPackedVector(v_add, temp_arr_to_use,
                         FingerprintBits + bucket_bits,
                         fill_time_per_entry_ns);
    }

    static inline void AddHashed(uint64_t key, Table *table) {}

    static void Add(uint64_t key, Table *table) {
        if (!table->insert(Reduce(key, table))) {
            throw std::logic_error("The filter is too small to hold all of the elements");
        }
    }

    static bool Add_attempt(uint64_t key, Table *table) {
        return table->insert(Reduce(key, table));
    }

    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        return const_cast<Table *>(table)->query(Reduce(key, table));
    }

    static void Remove(uint64_t key, Table *table) {
        table->remove(Reduce(key, table));
    }

    static std::string get_name(const Table *) {
        return "PartitionQuotientFilter-" + std::to_string(SizeRemainders);
    }

    static auto get_functionality(const Table *) -> uint32_t {
        return 7;
    }

    static auto get_ID(const Table *) -> filter_id {
        return pqf_id;
    }

    static size_t get_byte_size(const Table *table) {
        return const_cast<Table *>(table)->sizeFilter();
    }

    static size_t get_cap(const Table *table) {
        return table->capacity;
    }
};

inline uint64_t elapsed_ns(const std::chrono::high_resolution_clock::time_point &start_time,
                           const std::chrono::high_resolution_clock::time_point &end_time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
}

inline double compute_fill_time_per_entry_ns(uint64_t fill_time_ns, size_t inserted_entries) {
    return inserted_entries == 0 ? 0 : fill_time_ns / static_cast<double>(inserted_entries);
}

inline void print_unsupported_filter_operation(const char *operation) {
    std::cerr << "[Error] " << operation << " is only supported for Prefix_Filter" << std::endl;
}

class TrivialFilter {
public:
    TrivialFilter(size_t max_items) {
    }

    __attribute__((always_inline)) inline static constexpr uint16_t fixed_reduce(uint16_t hash) {
        // http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
        return (uint16_t) (((uint32_t) hash * 6400) >> 16);
    }


    inline auto Find(const u64 &item) const -> bool {
        return true;
    }

    static inline void AddHashed(uint64_t key) {}

    void Add(const u64 &item) {}

    auto get_capacity() const -> size_t {
        return -1;
    }

    auto get_name() const -> std::string {
        return "Trivial-Filter ";
    }

    auto get_byte_size() const -> size_t {
        return 0;
    }

    auto get_cap() const -> size_t {
        return -1;
    }
};

template<>
struct FilterAPI<TrivialFilter> {
    using Table = TrivialFilter;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        return Table(add_count);
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] TrivialFilter\n";
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_add->size());
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        table->Add(key);
    }

    static void Add(uint64_t key, Table *table) {
        table->Add(key);
    }
    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        return table->Find(key);
    }

    static void Remove(uint64_t key, Table *table) {
        throw std::runtime_error("Unsupported");
    }

    static std::string get_name(const Table *table) {
        return table->get_name();
    }

    static auto get_functionality(const Table *table) -> uint32_t {
        return 0;
    }
    static auto get_ID(const Table *table) -> filter_id {
        return Trivial_id;
    }

    static size_t get_byte_size(const Table *table) {
        return table->get_byte_size();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }
};


template<typename ItemType, size_t bits_per_item, template<size_t> class TableType, typename HashFamily>
struct FilterAPI<cuckoofilter::CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>> {
    using Table = cuckoofilter::CuckooFilter<ItemType, bits_per_item, TableType, HashFamily>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        return Table(add_count);
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] CuckooFilter\n";
        const size_t entries_before = table->get_cap();
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        const size_t inserted_entries = table->get_cap() - entries_before;
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), inserted_entries);
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            std::cerr << "Cuckoo filter is too full. Insertion of the element (" << key << ") failed.\n";
            std::cout << get_info(table).str() << std::endl;

            throw std::logic_error("The filter is too small to hold all of the elements");
        }
    }

    static void Add(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            std::cerr << "Cuckoo filter is too full. Insertion of the element (" << key << ") failed.\n";
            std::cout << get_info(table).str() << std::endl;

            throw std::logic_error("The filter is too small to hold all of the elements");
        }
    }

    static bool Add_attempt(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            std::cout << get_info(table).str() << std::endl;
            return false;
            // throw std::logic_error("The filter is too small to hold all of the elements");
        }
        return true;
    }

    static void Remove(uint64_t key, Table *table) {
        table->Delete(key);
    }

    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        return (0 == table->Contain(key));
    }

    static std::string get_name(const Table *table) {
        auto ss = table->Info();
        std::string temp = "PackedHashtable";
        if (ss.find(temp) != std::string::npos) {
            return "CF-ss";
        }
        if (bits_per_item == 8) {
            // return "Cuckoo-8-mod-m1";
            return "Cuckoo-8";
        } else if (bits_per_item == 12) {
            return "Cuckoo-12";
            // return "Cuckoo-12-mod-m1";
        } else if (bits_per_item == 16)
            return "Cuckoo-16";
        else if (bits_per_item == 32) {
            return "Cuckoo-32";
        }
        return "Cuckoo-?";
        // return "Cuckoo-" + std::to_string(bits_per_item);
    }

    static auto get_info(const Table *table) -> std::stringstream {
        std::string state = table->Info();
        std::stringstream ss;
        ss << state;
        return ss;
        // std::cout << state << std::endl;
    }

    /**
     * Returns int indicating which function can the filter do.
     * 1 is for lookups.
     * 2 is for adds.
     * 4 is for deletions.
     */
    static auto get_functionality(const Table *table) -> uint32_t {
        return 7;
    }

    static auto get_ID(const Table *table) -> filter_id {
        return CF;
    }

    static size_t get_byte_size(const Table *table) {
        return table->SizeInBytes();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }
};
template<typename ItemType, size_t bits_per_item, template<size_t> class TableType, typename HashFamily>
struct FilterAPI<cuckoofilter::CuckooFilterStable<ItemType, bits_per_item, TableType, HashFamily>> {
    using Table = cuckoofilter::CuckooFilterStable<ItemType, bits_per_item, TableType, HashFamily>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        return Table(add_count);
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] CuckooFilterStable\n";
        const size_t entries_before = table->get_cap();
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        const size_t inserted_entries = table->get_cap() - entries_before;
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), inserted_entries);
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            std::cerr << "Stable Cuckoo filter is too full. Insertion of the element (" << key << ") failed.\n";
            std::cout << get_info(table).str() << std::endl;

            throw std::logic_error("The filter is too small to hold all of the elements");
        }
    }

    static void Add(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            std::cerr << "Stable Cuckoo filter is too full. Insertion of the element (" << key << ") failed.\n";
            std::cout << get_info(table).str() << std::endl;

            throw std::logic_error("The filter is too small to hold all of the elements");
        }
    }

    static void Remove(uint64_t key, Table *table) {
        table->Delete(key);
    }

    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        return (0 == table->Contain(key));
    }

    static std::string get_name(const Table *table) {
        auto ss = table->Info();
        std::string temp = "PackedHashtable";
        if (ss.find(temp) != std::string::npos) {
            return "CF-ss";
        }
        if (bits_per_item == 8) {
            // return "Cuckoo-8-mod-m1";
            return "CuckooStable-8";
        } else if (bits_per_item == 12) {
            return "CuckooStable-12";
        } else if (bits_per_item == 16)
            return "CuckooStable-16";
        else if (bits_per_item == 32) {
            return "CuckooStable-32";
        }
        return "Cuckoo-?";
    }

    static auto get_info(const Table *table) -> std::stringstream {
        std::string state = table->Info();
        std::stringstream ss;
        ss << state;
        return ss;
        // std::cout << state << std::endl;
    }

    /**
             * Returns int indicating which function can the filter do.
             * 1 is for lookups.
             * 2 is for adds.
             * 4 is for deletions.
             */
    static auto get_functionality(const Table *table) -> uint32_t {
        return 7;
    }

    static auto get_ID(const Table *table) -> filter_id {
        return cf_stable_id;
    }
    static size_t get_byte_size(const Table *table) {
        return table->SizeInBytes();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }

    static double get_eLoad(const Table *table) {
        return table->get_effective_load();
    }
};

template<>
struct FilterAPI<SimdBlockFilter<>> {
    using Table = SimdBlockFilter<>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        Table ans(ceil(log2(add_count * 8.0 / CHAR_BIT)));
        return ans;
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        table->Add(key);
    }

    static void Add(uint64_t key, Table *table) {
        table->Add(key);
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] SimdBlockFilter\n";
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_add->size());
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        return table->Find(key);
    }

    static void Remove(uint64_t key, Table *table) {
        throw std::runtime_error("Unsupported");
    }

    static std::string get_name(const Table *table) {
        return "SimdBlockFilter";
    }

    static auto get_info(const Table *table) -> std::stringstream {
        assert(false);
        std::stringstream ss;
        return ss;
    }

    /**
     * Returns int indicating which function can the filter do.
     * 1 is for lookups.
     * 2 is for adds.
     * 4 is for deletions.
     */
    static auto get_functionality(const Table *table) -> uint32_t {
        return 3;
    }
    static auto get_ID(const Table *table) -> filter_id {
        return BBF;
    }

    static size_t get_byte_size(const Table *table) {
        return table->SizeInBytes();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }
};


template<>
struct FilterAPI<SimdBlockFilterFixed<>> {
    using Table = SimdBlockFilterFixed<>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        Table ans(ceil(add_count * 8.0 / CHAR_BIT));
        return ans;
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        table->Add(key);
    }

    static void Add(uint64_t key, Table *table) {
        table->Add(key);
    }

    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        return table->Find(key);
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] SimdBlockFilterFixed\n";
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_add->size());
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    static void Remove(uint64_t key, Table *table) {
        throw std::runtime_error("Unsupported");
    }

    static std::string get_name(const Table *table) {
        return "BBF-Fixed";
    }

    static auto get_info(const Table *table) -> std::stringstream {
        assert(false);
        std::stringstream ss;
        return ss;
    }

    /**
     * Returns int indicating which function can the filter do.
     * 1 is for lookups.
     * 2 is for adds.
     * 4 is for deletions.
     */
    static auto get_functionality(const Table *table) -> uint32_t {
        return 3;
    }
    static auto get_ID(const Table *table) -> filter_id {
        return SIMD_fixed;
    }
    static size_t get_byte_size(const Table *table) {
        return table->SizeInBytes();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }
};

template<>
struct FilterAPI<Impala512<>> {
    using Table = Impala512<>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        return Table(add_count);
        // return ans;
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        table->Add(key);
    }

    static void Add(uint64_t key, Table *table) {
        table->Add(key);
    }

    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        return table->Find(key);
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] Impala512\n";
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_add->size());
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    static void Remove(uint64_t key, Table *table) {
        throw std::runtime_error("Unsupported");
    }

    static std::string get_name(const Table *table) {
        return "Impala512";
    }

    static auto get_info(const Table *table) -> std::stringstream {
        assert(false);
        std::stringstream ss;
        return ss;
    }

    /**
     * Returns int indicating which function can the filter do.
     * 1 is for lookups.
     * 2 is for adds.
     * 4 is for deletions.
     */
    static auto get_functionality(const Table *table) -> uint32_t {
        return 3;
    }
    static auto get_ID(const Table *table) -> filter_id {
        return SIMD_fixed;
    }
    static size_t get_byte_size(const Table *table) {
        return table->SizeInBytes();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }
};


template<>
struct FilterAPI<TC_shortcut> {
    using Table = TC_shortcut;
    static inline double fill_time_per_entry_ns = 0;
    //    using Table = dict512<TableType, spareItemType, itemType>;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        constexpr float load = .935;
        return Table(add_count, load, allocate);
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        if (!table->insert(key)) {
            std::cout << table->info() << std::endl;
            //            std::cout << "max_load: \t" << 0.945 << std::endl;
            throw std::logic_error(table->get_name() + " is too small to hold all of the elements");
        }
    }

    static void Add(uint64_t key, Table *table) {
        if (!table->insert(key)) {
            std::cout << table->info() << std::endl;
            //            std::cout << "max_load: \t" << 0.945 << std::endl;
            throw std::logic_error(table->get_name() + " is too small to hold all of the elements");
        }
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] TC_shortcut\n";
        const size_t entries_before = table->get_cap();
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        const size_t inserted_entries = table->get_cap() - entries_before;
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), inserted_entries);
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    static bool Add_attempt(uint64_t key, Table *table) {
        if (!table->insert(key)) {
            std::cout << "load when failed: \t" << table->get_effective_load() << std::endl;
            std::cout << table->info() << std::endl;
            return false;
        }
        return true;
    }

    static void Remove(uint64_t key, Table *table) {
        // throw std::runtime_error("Unsupported");
        table->remove(key);
    }

    CONTAIN_ATTRIBUTES static bool Contain(uint64_t key, const Table *table) {
        // std::cout << "here!!!" << std::endl;
        return table->lookup(key);
        // return table->lookup_consecutive_only_body(key);
        // return table->lookup_consecutive(key);
    }

    static std::string get_name(const Table *table) {
        return table->get_name();
    }

    static auto get_info(const Table *table) -> std::stringstream {
        std::stringstream ss;
        ss << "";
        return ss;
        // return table->get_extended_info();
    }

    static auto get_functionality(const Table *table) -> uint32_t {
        return 7;
    }

    static auto get_ID(const Table *table) -> filter_id {
        return TC_shortcut_id;
    }

    static size_t get_byte_size(const Table *table) {
        return table->get_byte_size();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }

    static double get_eLoad(const Table *table) {
        return table->get_effective_load();
    }
};


template<typename Table>
inline size_t get_l2_slots(size_t l1_items, const double overflowing_items_ratio, const float loads[2]) {
    const double expected_items_reaching_next_level = l1_items * overflowing_items_ratio;
    size_t slots_in_l2 = (expected_items_reaching_next_level / loads[1]);
    return slots_in_l2;
}

template<>
inline size_t get_l2_slots<cuckoofilter::CuckooFilterStable<u64, 12>>(size_t l1_items, const double overflowing_items_ratio, const float loads[2]) {
    // const double expected_items_reaching_next_level = l1_items * 0.0752;
    // const double spare_workload = 0.0752 / 0.0586;
    // size_t slots_in_l2 = std::ceil(expected_items_reaching_next_level);
    // return slots_in_l2;
    constexpr auto expected_items95 = 0.0586;
    constexpr auto expected_items100 = 0.07952;
    constexpr auto expected_items105 = 0.1031;
    constexpr auto spare_workload = 0.94;
    constexpr auto safety = 1.08;
    constexpr auto factor95 = safety * expected_items95 / spare_workload;
    constexpr auto factor100 = safety * expected_items100 / spare_workload;
    constexpr auto factor105 = safety * expected_items105 / spare_workload;
    // const double expected_items_reaching_next_level = l1_items * (0.06 / 0.9);
    const double expected_items_reaching_next_level = l1_items * factor95;
    return expected_items_reaching_next_level;
}

template<>
inline size_t get_l2_slots<TC_shortcut>(size_t l1_items, const double overflowing_items_ratio, const float loads[2]) {
    constexpr auto expected_items95 = 0.0586;
    constexpr auto expected_items100 = 0.07952;
    constexpr auto expected_items105 = 0.1031;
    constexpr auto spare_workload = 0.935;
    constexpr auto safety = 1.08;
    constexpr auto factor95 = safety * expected_items95 / spare_workload;
    constexpr auto factor100 = safety * expected_items100 / spare_workload;
    constexpr auto factor105 = safety * expected_items105 / spare_workload;
    // const double expected_items_reaching_next_level = l1_items * (0.06 / 0.9);
    const double expected_items_reaching_next_level = l1_items * factor95;
    size_t slots_in_l2 = std::ceil(expected_items_reaching_next_level);
    return slots_in_l2;
}


template<>
inline size_t get_l2_slots<SimdBlockFilter<>>(size_t l1_items, const double overflowing_items_ratio, const float loads[2]) {
    const double expected_items_reaching_next_level = l1_items * overflowing_items_ratio;
    size_t slots_in_l2 = (expected_items_reaching_next_level / loads[1]);
    return slots_in_l2 * 4;
}

template<>
inline size_t get_l2_slots<SimdBlockFilterFixed<>>(size_t l1_items, const double overflowing_items_ratio, const float loads[2]) {
    const double expected_items_reaching_next_level = l1_items * overflowing_items_ratio;
    size_t slots_in_l2 = (expected_items_reaching_next_level / loads[1]);
    return slots_in_l2 * 2;
}

template<>
inline size_t get_l2_slots<Impala512<>>(size_t l1_items, const double overflowing_items_ratio, const float loads[2]) {
    constexpr auto expected_items95 = 0.0586;
    constexpr auto expected_items100 = 0.07952;
    constexpr auto expected_items105 = 0.1031;
    constexpr auto spare_workload = 1;
    constexpr auto safety = 1.08;
    constexpr auto factor95 = safety * expected_items95 / spare_workload;
    constexpr auto factor100 = safety * expected_items100 / spare_workload;
    constexpr auto factor105 = safety * expected_items105 / spare_workload;
    // const double expected_items_reaching_next_level = l1_items * (0.06 / 0.9);
    const double expected_items_reaching_next_level = l1_items * factor95;
    size_t slots_in_l2 = std::ceil(expected_items_reaching_next_level);
    return slots_in_l2;
}


template<typename Table>
class Prefix_Filter {
    const size_t filter_max_capacity;
    const size_t number_of_pd;
    size_t cap[2] = {0};

    hashing::TwoIndependentMultiplyShift Hasher, H0;
    __m256i *pd_array;
    Table GenSpare;
    bool free_pd_array;

    static double constexpr overflowing_items_ratio = 0.0586;//  = expected_items95

public:
    Prefix_Filter(size_t max_items, const float loads[2], bool allocate = true)
        : filter_max_capacity(max_items),
          number_of_pd(std::ceil(1.0 * max_items / (min_pd::MAX_CAP0 * loads[0]))),
          GenSpare(FilterAPI<Table>::ConstructFromAddCount(get_l2_slots<Table>(max_items, overflowing_items_ratio, loads))),
          Hasher(), H0() {
        free_pd_array = allocate;
        if (allocate) {
            int ok = posix_memalign((void **) &pd_array, 32, 32 * number_of_pd);
            if (ok != 0) {
                std::cout << "Space allocation failed!" << std::endl;
                assert(false);
                exit(-3);
            }
    
            constexpr uint64_t pd256_plus_init_header = (((INT64_C(1) << min_pd::QUOTS) - 1) << 6) | 32;
            std::fill(pd_array, pd_array + number_of_pd, __m256i{pd256_plus_init_header, 0, 0, 0});
        }
        size_t l1 = sizeof(__m256i) * number_of_pd;
        size_t l2 = FilterAPI<Table>::get_byte_size(&GenSpare);
        // double ratio = 1.0 * l2 / l1;
        // std::cout << get_name() << ".\t";
        // std::cout << "spare-size / First level:\t\t " << ratio << std::endl;
    }

    void set_cap(int cap_type, size_t to_set) {
        cap[cap_type] = to_set;
    }

    bool is_empty() { return cap[0] == 0; }

    ~Prefix_Filter() {
        if (free_pd_array) {
            free(pd_array);
        }
    }

    __attribute__((always_inline)) inline static constexpr uint32_t reduce32(uint32_t hash, uint32_t n) {
        // http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
        return (uint32_t) (((uint64_t) hash * n) >> 32);
    }


    __attribute__((always_inline)) inline static constexpr uint16_t fixed_reduce(uint16_t hash) {
        // http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
        return (uint16_t) (((uint32_t) hash * 6400) >> 16);
    }

    hashing::TwoIndependentMultiplyShift* GetH0() { return &H0; }

    inline auto Find(const u64 &item) const -> bool {
        const u64 s = H0(item);
        uint32_t out1 = s >> 32u, out2 = s;
        const uint32_t pd_index = reduce32(out1, (uint32_t) number_of_pd);
        const uint16_t qr = fixed_reduce(out2);
        const int64_t quot = qr >> 8;
        const uint8_t rem = qr;
        // return min_pd::pd_find_25(quot, rem, &pd_array[pd_index]);
        // return (!min_pd::cmp_qr1(qr, &pd_array[pd_index])) ? min_pd::pd_find_25(quot, rem, &pd_array[pd_index])
        return (!min_pd::cmp_qr1(qr, &pd_array[pd_index])) ? min_pd::find_core(quot, rem, &pd_array[pd_index])
                                                           : incSpare_lookup(pd_index, qr);
    }

    inline auto incSpare_lookup(size_t pd_index, u16 qr) const -> bool {
        const u64 data = (pd_index << 13u) | qr;
        //        u64 hashed_res = Hasher(data);
        return FilterAPI<Table>::Contain(data, &GenSpare);
    }

    inline void incSpare_add(size_t pd_index, const min_pd::add_res &a_info) {
        incSpare_add(pd_index, a_info.quot, a_info.rem);
    }

    inline void incSpare_add(size_t pd_index, u8 quot, u8 rem) {
        u16 qr = (((u16) quot) << 8u) | rem;
        const u64 data = (pd_index << 13u) | qr;
        //        u64 hashed_res = Hasher(data);
        return FilterAPI<Table>::Add(data, &GenSpare);
    }

    static inline void AddToPd(__m256i *pd, int64_t quot, u64 header, const uint8_t &rem) {
        assert(!min_pd::is_pd_full(pd));
        size_t end = min_pd::select64(header >> 6, quot);
        assert(min_pd::check::val_header(pd));
        const size_t h_index = end + 6;
        const u64 mask = _bzhi_u64(-1, h_index);
        const u64 lo = header & mask;
        const u64 hi = ((header & ~mask) << 1u);// & h_mask;
        assert(!(lo & hi));
        const u64 h7 = lo | hi;
        memcpy(pd, &h7, 7);

        assert(min_pd::check::val_header(pd));

        const size_t body_index = end - quot;
        min_pd::body_add_case0_avx(body_index, rem, pd);
        // auto mp = (u8 *) pd + 7 + body_index;
        // const size_t b2m = (32 - 7) - (body_index + 1);
        // memmove(mp + 1, mp, b2m);
        // mp[0] = rem;
        assert(min_pd::find_core(quot, rem, pd));
    }

    static inline void AddToPd(__m256i &pd, int64_t quot, u64 header, const uint8_t &rem) {
        // assert(!min_pd::is_pd_full(pd));
        size_t end = min_pd::select64(header >> 6, quot);
        // assert(min_pd::check::val_header(pd));
        const size_t h_index = end + 6;
        const u64 mask = _bzhi_u64(-1, h_index);
        const u64 lo = header & mask;
        const u64 hi = ((header & ~mask) << 1u);// & h_mask;
        assert(!(lo & hi));
        const u64 h7 = lo | hi;
        memcpy(&pd, &h7, 7);

        // assert(min_pd::check::val_header(pd));

        const size_t body_index = end - quot;
        min_pd::body_add_case0_avx(body_index, rem, pd);
        // auto mp = (u8 *) pd + 7 + body_index;
        // const size_t b2m = (32 - 7) - (body_index + 1);
        // memmove(mp + 1, mp, b2m);
        // mp[0] = rem;
        // assert(min_pd::find_core(quot, rem, pd));
    }

    inline void WritePd(const uint32_t &pd_index, __m256i *pd, const size_t &size) {
        // std::cout << "write specific=" << pd_array + pd_index << std::endl;
        _mm256_store_si256(pd_array + pd_index, *pd);
    }

    inline __m256i* get_pd_by_index(const uint32_t &pd_index) { return pd_array + pd_index; }

    inline void AddToSpare(__m256i *pd, int64_t quot, const uint8_t &rem, const uint32_t &pd_index) {
        auto add_res = min_pd::new_pd_swap_short(quot, rem, pd);
        assert(min_pd::check::val_last_quot_is_sorted(pd));
        cap[1]++;
        incSpare_add(pd_index, add_res);
    }

    inline void Add(const u64 &item) {
        const u64 s = H0(item);
        constexpr u64 full_mask = (1ULL << 55);
        uint32_t out1 = s >> 32u, out2 = s;

        const uint32_t pd_index = reduce32(out1, (uint32_t) number_of_pd);

        auto pd = pd_array + pd_index;
        const uint64_t header = reinterpret_cast<const u64 *>(pd)[0];
        const bool not_full = !(header & full_mask);

        const uint16_t qr = fixed_reduce(out2);
        const int64_t quot = qr >> 8;
        const uint8_t rem = qr;

        if (not_full) {
            cap[0]++;
            AddToPd(pd, quot, header, rem);
            return;
        } else {
            AddToSpare(pd, quot, rem, pd_index);
        }
    }

    inline void AddHashed(const u64 &s) {
        constexpr u64 full_mask = (1ULL << 55);
        uint32_t out1 = s >> 32u, out2 = s;

        const uint32_t pd_index = reduce32(out1, (uint32_t) number_of_pd);

        auto pd = pd_array + pd_index;
        const uint64_t header = reinterpret_cast<const u64 *>(pd)[0];
        const bool not_full = !(header & full_mask);

        const uint16_t qr = fixed_reduce(out2);
        const int64_t quot = qr >> 8;
        const uint8_t rem = qr;

        if (not_full) {
            cap[0]++;
            AddToPd(pd, quot, header, rem);
            return;
        } else {
            AddToSpare(pd, quot, rem, pd_index);
        }
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////// Validation functions.////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////////////

    auto get_capacity() const -> size_t {
        size_t res = 0;
        for (size_t i = 0; i < number_of_pd; ++i) {
            res += min_pd::get_capacity(&pd_array[i]);
        }
        assert(res == cap[0]);
        return res;
    }

    auto get_name() const -> std::string {
        std::string s0 = "Prefix-Filter ";
        std::string s1 = FilterAPI<Table>::get_name(&GenSpare);
        return s0 + "[ " + s1 + " ]";
    }

    auto count_overflowing_PDs() -> size_t {
        size_t count_overflowing_PD = 0;
        for (int i = 0; i < number_of_pd; ++i) {
            bool add_cond = min_pd::pd_full(&pd_array[i]);
            count_overflowing_PD += add_cond;
            bool is_full = min_pd::pd_full(&pd_array[i]);
            //            bool is_full2 = pd_vec[i]->is_full();
            //            assert(is_full == is_full2);
            bool final = (!add_cond or is_full);
            // assert(final);
        }
        return count_overflowing_PD;
    }

    void set_pd_array(void *arr) {
        pd_array = (__m256i *) arr;
    }

    auto count_empty_PDs() -> size_t {
        size_t count_empty_PD = 0;
        for (int i = 0; i < number_of_pd; ++i) {
            bool add_cond = (min_pd::get_capacity(&pd_array[i]) <= 0);
            count_empty_PD += add_cond;
        }
        return count_empty_PD;
    }

    auto get_byte_size() const -> size_t {
        size_t l1 = sizeof(__m256i) * number_of_pd;
        //        size_t l2 = FilterAPI<Table>::get_byte_size(GenSpare);
        size_t l2 = FilterAPI<Table>::get_byte_size(&GenSpare);
        auto res = l1 + l2;
        return res;
    }

    size_t GetNumPd() { return number_of_pd; }

    auto get_cap() const -> size_t {
        return cap[0] + cap[1];
    }
};

static void doCount(std::vector<uint64_t>& arr, std::vector<int>& count, const uint64_t mask, int shift)
{
    int countZero = 0;
    const int simdWidth = 4;
    for (size_t i = 0; i < arr.size(); i += simdWidth) {
        __m256i data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[i]));
        __m256i shifted = _mm256_srli_epi64(data, shift);
        __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

        alignas(32) uint64_t extracted[simdWidth];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);

        for (int lane = 0; lane < simdWidth; ++lane) {
            if (!extracted[lane]) {
                countZero++;
            } else {
                count[extracted[lane]]++;
            }
        }
    }
    count[0] = countZero;
}

static void doCountLastIterBuffer(uint16_t* arr, size_t arr_size, std::vector<uint64_t>& count, const uint64_t mask, int shift) {
    int countZero = 0;
    const int simdWidth = 4;
    __m256i data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[0]));
    size_t i = 0;
    for (; i < arr_size - 4; i += simdWidth) {
        __m256i shifted = _mm256_srli_epi64(data, shift);
        __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

        alignas(32) uint64_t extracted[simdWidth];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);

        data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[i + 4]));
        for (int lane = 0; lane < simdWidth; ++lane) {
            count[extracted[lane]]++;
        }
    }
    for (int j = 0; j < arr_size - i; i++) {
        int bucket = (arr[i] >> shift) & mask;
        count[bucket]++;
    }
    // __m256i shifted = _mm256_srli_epi64(data, shift);
    // __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

    // alignas(32) uint64_t extracted[simdWidth];
    // _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);
    // for (int lane = 0; lane < simdWidth; ++lane) {
    //     count[extracted[lane]]++;
    // }
}

static void doCountLastIter(std::vector<uint64_t>& arr, std::vector<uint32_t>& count, const uint64_t mask, int shift)
{
    int countZero = 0;
    const int simdWidth = 4;
    __m256i data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[0]));

    for (size_t i = 0; i < arr.size() - 4; i += simdWidth) {
        __m256i shifted = _mm256_srli_epi64(data, shift);
        __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

        alignas(32) uint64_t extracted[simdWidth];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);

        data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[i + 4]));
        for (int lane = 0; lane < simdWidth; ++lane) {
            count[extracted[lane]]++;
        }
    }
    __m256i shifted = _mm256_srli_epi64(data, shift);
    __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

    alignas(32) uint64_t extracted[simdWidth];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);
    for (int lane = 0; lane < simdWidth; ++lane) {
        count[extracted[lane]]++;
    }
}

static void radixSortAVX2Buffer(uint16_t* arr, size_t arr_size, int numBits, int totalBits, uint16_t* temp, std::vector<uint64_t>& count, int startingBit = 0) {
    if (!arr || arr_size == 0 || numBits <= 0 || numBits > 64 || totalBits <= 0 || totalBits > 64) return;

    const int bucketCount = 1 << numBits;
    const uint64_t mask = bucketCount - 1;
    const int simdWidth = 4;

    for (int shift = startingBit; shift < totalBits; shift += numBits) {
        std::fill(count.begin(), count.end(), 0);
        doCountLastIterBuffer(arr, arr_size, count, mask, shift);

        // std::cout << "sum" << std::endl;
        for (int i = 1; i < bucketCount; i += 4) {
            count[i] += count[i - 1];
            count[i + 1] += count[i];
            count[i + 2] += count[i + 1];
            count[i + 3] += count[i + 2];
        }

        uint64_t num[4];
        for (int j = 0; j < 4; j++) {
            num[j] = arr[arr_size - 1 - j];
        }
        uint64_t bucket[4];
        uint64_t index[4];
        // std::cout << "fill" << std::endl;
        size_t i = 0;
        for (i = arr_size - 1; i >= 4; i -= 4) {
            for (int j = 0; j < 4; ++j) {
                uint64_t cur = num[j];
                cur >>= shift;
                cur &= mask;
                bucket[j] = cur;
                index[j] = --count[cur];
            }

            _mm_prefetch(reinterpret_cast<const char*>(&temp[index[1] - 4]), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(&temp[index[2] - 4]), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(&temp[index[3] - 4]), _MM_HINT_T0);

            for (int j = 0; j < 4; ++j) {
                temp[index[j]] = num[j];
                num[j] = arr[i - 4 - j];
            }
        }

        for (int j = 0; j < arr_size % 4; ++j) {
            bucket[j] = (num[j] >> shift) & mask;
            index[j] = --count[bucket[j]];
        }

        for (int j = 0; j < arr_size % 4; ++j) {
            temp[index[j]] = num[j];
        }
        std::swap(arr, temp);
    }
        // std::cout << "done" << std::endl;
}

// Lightweight array view for zero-copy slicing
struct ArrayView {
    uint64_t* data_;
    size_t size_;
    
    ArrayView(std::vector<uint64_t>& vec) : data_(vec.data()), size_(vec.size()) {}
    ArrayView(std::vector<uint64_t>& vec, size_t start, size_t end) 
        : data_(vec.data() + start), size_(end - start) {}
    
    uint64_t& operator[](size_t i) { return data_[i]; }
    const uint64_t& operator[](size_t i) const { return data_[i]; }
    size_t size() const { return size_; }
    uint64_t* data() { return data_; }
    const uint64_t* data() const { return data_; }
    bool empty() const { return size_ == 0; }
    
    // Iterator support for compatibility
    uint64_t* begin() { return data_; }
    uint64_t* end() { return data_ + size_; }
    const uint64_t* begin() const { return data_; }
    const uint64_t* end() const { return data_ + size_; }
};

// Optimized counting assuming array size is divisible by 4
static void doCountLastIter_view_optimized(ArrayView& arr, std::vector<uint32_t>& count, const uint64_t mask, int shift)
{
    int countZero = 0;
    const int simdWidth = 4;
    __m256i data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[0]));

    for (size_t i = 0; i < arr.size() - 4; i += simdWidth) {
        __m256i shifted = _mm256_srli_epi64(data, shift);
        __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

        alignas(32) uint64_t extracted[simdWidth];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);

        data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[i + 4]));
        for (int lane = 0; lane < simdWidth; ++lane) {
            count[extracted[lane]]++;
        }
    }
    __m256i shifted = _mm256_srli_epi64(data, shift);
    __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

    alignas(32) uint64_t extracted[simdWidth];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);
    for (int lane = 0; lane < simdWidth; ++lane) {
        count[extracted[lane]]++;
    }
}

// Safe counting for any array size
static void doCountLastIter_view_safe(ArrayView& arr, std::vector<uint32_t>& count, const uint64_t mask, int shift)
{
    const int simdWidth = 4;
    __m256i data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[0]));

    size_t i = 0;
    for (; i + simdWidth < arr.size(); i += simdWidth) {
        __m256i shifted = _mm256_srli_epi64(data, shift);
        __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

        alignas(32) uint64_t extracted[simdWidth];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);
        
        _mm_prefetch(reinterpret_cast<const char*>(&arr[i + 256]), _MM_HINT_T0);

        data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[i + simdWidth]));
        
        for (int lane = 0; lane < simdWidth; ++lane) {
            count[extracted[lane]]++;
        }
    }

    for (; i < arr.size(); ++i) {
        uint64_t bucket = (arr[i] >> shift) & mask;
        count[bucket]++;
    }
}

// Contiguous slice of `n` keys for `thread_id`. Groups of 4 stay together so the SIMD count
// and the scatter walk the exact same keys; the leftover tail, if any, goes to the last thread.
static void ParallelKeySlice(size_t n, int thread_id, int nthreads, size_t *begin, size_t *end) {
    const size_t aligned = n & ~size_t{3};
    const size_t groups = aligned / 4;
    const size_t base = groups / static_cast<size_t>(nthreads);
    const size_t rem = groups % static_cast<size_t>(nthreads);
    const size_t start_group =
            static_cast<size_t>(thread_id) * base + std::min(static_cast<size_t>(thread_id), rem);
    const size_t n_groups = base + (static_cast<size_t>(thread_id) < rem ? 1 : 0);
    *begin = start_group * 4;
    *end = *begin + n_groups * 4;
    if (thread_id == nthreads - 1) {
        *end = n;
    }
}

static void doCountParallel_view(ArrayView& arr, std::vector<uint32_t>& count, std::vector<std::vector<uint32_t>>& per_thread_count, const uint64_t mask, int shift)
{
    const int nthreads = THREAD_NUM;
    const int simdWidth = 4;
    #pragma omp parallel num_threads(nthreads)
    {
        const int thread_id = omp_get_thread_num();
        size_t begin = 0;
        size_t end = 0;
        ParallelKeySlice(arr.size(), thread_id, nthreads, &begin, &end);
        auto &my_count = per_thread_count[thread_id];

        size_t i = begin;
        for (; i + simdWidth <= end; i += simdWidth) {
            __m256i data = _mm256_loadu_si256(reinterpret_cast<__m256i*>(&arr[i]));
            __m256i shifted = _mm256_srli_epi64(data, shift);
            __m256i bucketIndices = _mm256_and_si256(shifted, _mm256_set1_epi64x(mask));

            alignas(32) uint64_t extracted[simdWidth];
            _mm_prefetch(reinterpret_cast<const char*>(&arr[i + 256]), _MM_HINT_T0);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(extracted), bucketIndices);

            for (int lane = 0; lane < simdWidth; ++lane) {
                my_count[extracted[lane]]++;
            }
        }
        for (; i < end; ++i) {
            my_count[(arr[i] >> shift) & mask]++;
        }
    }
    // Exclusive prefix in bucket-major, thread-minor order. Each cell turns from "how many keys
    // of this bucket does this thread hold" into "where in the output does this thread start
    // writing them", which is what lets every thread scatter its own slice with no
    // synchronization: the ranges it writes belong to it alone. `count` keeps the bucket totals.
    const int nbuckets = static_cast<int>(per_thread_count[0].size());
    uint32_t running = 0;
    for (int bucket = 0; bucket < nbuckets; ++bucket) {
        const uint32_t bucket_start = running;
        for (int thread = 0; thread < nthreads; ++thread) {
            const uint32_t keys = per_thread_count[thread][bucket];
            per_thread_count[thread][bucket] = running;
            running += keys;
        }
        count[bucket] = running - bucket_start;
    }
}

// Optimized version assuming array size is divisible by 4
static void radixDistribute_optimized(ArrayView& arr, ArrayView& temp, std::vector<uint32_t>& count, const uint64_t mask, int shift) {
    // auto start_time = std::chrono::high_resolution_clock::now();
    uint64_t num[4];
    for (int j = 0; j < 4; j++) {
        num[j] = arr[arr.size() - 1 - j];
    }
    uint64_t bucket[4];
    uint64_t index[4];
    
    #pragma unroll 10
    for (int i = arr.size() - 1; i >= 4; i -= 4) {
        #pragma unroll 10
        for (int j = 0; j < 4; ++j) {
            uint64_t cur = num[j];
            cur >>= shift;
            cur &= mask;
            bucket[j] = cur;
            index[j] = --count[cur];
        }
        
        _mm_prefetch(reinterpret_cast<const char*>(&temp[index[1] - 4]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&temp[index[2] - 4]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&temp[index[3] - 4]), _MM_HINT_T0);

        _mm_prefetch(reinterpret_cast<const char*>(&arr[i - 256]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&arr[i - 257]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&arr[i - 258]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&arr[i - 259]), _MM_HINT_T0);

        #pragma unroll 10
        for (int j = 0; j < 4; ++j) {
            temp[index[j]] = num[j];
            num[j] = arr[i - 4 - j];
        }
    }

    #pragma unroll 10
    for (int j = 0; j < 4; ++j) {
        bucket[j] = (num[j] >> shift) & mask;
        index[j] = --count[bucket[j]];
    }

    #pragma unroll 10
    for (int j = 0; j < 4; ++j) {
        temp[index[j]] = num[j];
    }

    // auto end_time = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> elapsed = end_time - start_time;
    // std::cout << "elapsed: " << elapsed.count() << std::endl;
}

// Each thread scatters only its own slice of the input, through the cursors the prefix over the
// histograms left in `cursors[thread][bucket]`. The array is therefore read once in total rather
// than once per thread, and since no two threads were given the same output index the stores need
// no synchronization. A thread's keys for one bucket land consecutively, so the store buffer
// already coalesces them and an explicit write-combine buffer only adds a second random-access
// stream: it measured slower than storing straight through.
static void radixDistribute_by_range(ArrayView& arr, ArrayView& temp,
                                     std::vector<std::vector<uint32_t>>& cursors,
                                     const uint64_t mask, int shift) {
    const int nthreads = THREAD_NUM;
    #pragma omp parallel num_threads(nthreads)
    {
        const int thread_id = omp_get_thread_num();
        uint32_t *cursor = cursors[thread_id].data();
        size_t begin = 0;
        size_t end = 0;
        ParallelKeySlice(arr.size(), thread_id, nthreads, &begin, &end);

        for (size_t i = begin; i < end; ++i) {
            _mm_prefetch(reinterpret_cast<const char *>(&arr[i + 64]), _MM_HINT_T0);
            const uint64_t key = arr[i];
            temp[cursor[(key >> shift) & mask]++] = key;
        }
    }
}

// Safe version that handles any array size
static void radixDistribute_safe(ArrayView& arr, ArrayView& temp, std::vector<uint32_t>& count, const uint64_t mask, int shift) {
    // auto start_time = std::chrono::high_resolution_clock::now();
    
    // Process all elements from end to beginning (like the original algorithm)
    for (int i = arr.size() - 1; i >= 0; i--) {
        uint64_t cur = arr[i];
        uint64_t bucket = (cur >> shift) & mask;
        uint64_t index = --count[bucket];
        _mm_prefetch(reinterpret_cast<const char*>(&temp[index - 4]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&arr[i - 256]), _MM_HINT_T0);
        temp[index] = cur;
    }

    // auto end_time = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> elapsed = end_time - start_time;
}

static void internalRadixAVX_view(ArrayView& arr, ArrayView& temp, 
                                  std::vector<std::vector<uint32_t>>& count_vec, std::vector<uint32_t>& count,
                                  const uint64_t mask, int shift, int bucketCount, bool optimized = false,
                                  std::vector<uint32_t> *bucketCounts = nullptr) {
    // Count occurrences of each bucket using AVX2
    // #define ASSUME_SIZE_DIVISIBLE_BY_4 1
    // auto iter_start_time = std::chrono::high_resolution_clock::now();
    std::fill(count.begin(), count.begin() + bucketCount, 0);
    for (size_t i = 0; i < count_vec.size(); i++) {
        std::fill(count_vec[i].begin(), count_vec[i].begin() + bucketCount, 0);
    }

    // auto start_time = std::chrono::high_resolution_clock::now();
    // if (THREAD_NUM > 1) {
        // doCountParallel_view(arr, count, count_vec, mask, shift);
    // } else {
    if (optimized) {
        // doCountLastIter_view_optimized(arr, count, mask, shift);
        doCountParallel_view(arr, count, count_vec, mask, shift);
    } else {
        doCountLastIter_view_safe(arr, count, mask, shift);
    }
        // doCountLastIter_view(arr, count, mask, shift);
    // }

    if (bucketCounts != nullptr) {
        memcpy(bucketCounts->data() + 1, count.data(), bucketCount * sizeof(uint32_t));
    }

    // auto end_time = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> elapsed = end_time - start_time;

    // Optimized prefix sum calculation by unrolling
    {
        // auto start_time = std::chrono::high_resolution_clock::now();
        for (int i = 1; i < bucketCount; i++) {
            count[i] += count[i - 1];
        }
        // auto end_time = std::chrono::high_resolution_clock::now();
        // std::chrono::duration<double> elapsed = end_time - start_time;
    }


    if (optimized) {
        // The parallel scatter takes its cursors from the per-thread histograms, which only the
        // parallel count builds, so a single-threaded build stays on the serial scatter.
        if (THREAD_NUM > 1) {
            radixDistribute_by_range(arr, temp, count_vec, mask, shift);
        } else {
            radixDistribute_optimized(arr, temp, count, mask, shift);
        }
    } else {
        radixDistribute_safe(arr, temp, count, mask, shift);
    }

    // Data is now in temp, next iteration will swap source/dest

    // auto iter_end_time = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> iter_elapsed = iter_end_time - iter_start_time;
}

static bool radixSortAVX2_internal_view(ArrayView& arr, ArrayView& temp, int numBits, int totalBits, int startBit, bool optimized = false,
                                        std::vector<uint32_t> *bucketCounts = nullptr) {
    if (arr.empty() || numBits <= 0 || numBits > 64 || totalBits <= 0 || totalBits > 64) return false;

    const int bucketCount = 1 << numBits; // Number of buckets (2^numBits)
    const uint64_t mask = bucketCount - 1; // Mask to extract numBits
    const int simdWidth = 4; // 4 x 64-bit integers per AVX2 register

    alignas(32) uint64_t nums[4];
    alignas(32) uint64_t buckets[4];
    alignas(32) uint64_t indices[4];
    // Only the pass over the whole array counts in parallel and so needs a histogram per thread.
    // A pass over a single first-pass range counts on the one thread that owns the range, and
    // there are as many of those passes as there are ranges, so giving them a histogram each
    // would allocate and zero THREAD_NUM of them per range for nothing.
    std::vector<std::vector<uint32_t>> count_vec;
    if (optimized) {
        count_vec.assign(THREAD_NUM, std::vector<uint32_t>(bucketCount, 0));
    }
    std::vector<uint32_t> count(bucketCount, 0);
    
    ArrayView* source = &arr;
    ArrayView* dest = &temp;
    
    for (int shift = startBit; shift < totalBits; shift += numBits) {
        bool is_last_iter = shift + numBits >= totalBits;
        int actual_shift = shift;
        
        // Optimization: In the last iteration, align to use exactly the final numBits
        // This takes advantage of the array being mostly sorted from previous iterations
        if (is_last_iter && totalBits > shift) {
            actual_shift = totalBits - numBits;
        }
        
        internalRadixAVX_view(*source, *dest, count_vec, count, mask, actual_shift, bucketCount, optimized, bucketCounts);
        
        // Swap source and destination for next iteration
        std::swap(source, dest);
        
        // If we adjusted the shift for the last iteration, we're done
        if (is_last_iter && actual_shift != shift) {
            break;
        }
    }
    
    // Return whether final result is in temp (caller will handle swapping)
    return (source == &temp);
}

static bool radixSortAVX2(std::vector<uint64_t>& arr, int numBits, int totalBits, std::vector<uint64_t> &temp, int startBit, size_t start_idx = 0, size_t end_idx = SIZE_MAX, bool optimized = false, std::vector<uint32_t> *bucketCounts = nullptr) {
    if (arr.empty() || numBits <= 0 || numBits > 64 || totalBits <= 0 || totalBits > 64) return false;
    
    // Set default bounds if not specified
    if (end_idx == SIZE_MAX) end_idx = arr.size();
    if (start_idx >= end_idx) return false;
    
    // Create zero-copy views
    ArrayView arr_view(arr, start_idx, end_idx);
    ArrayView temp_view(temp, start_idx, end_idx);
    
    bool result_in_temp = radixSortAVX2_internal_view(arr_view, temp_view, numBits, totalBits, startBit, optimized, bucketCounts);
    
    // If result is in temp, swap the entire vectors once (much faster than copying)
    // if (result_in_temp) {
    //     arr.swap(temp);
    // }
    return result_in_temp;
}

static void radixSortAVX2Parallel(std::vector<uint64_t>& arr, int numBits, int totalBits, std::vector<uint64_t> &temp, int firstSortBit, int finalSortBit) {
    // Step 1: Sort by the highest firstSortBit bits to create independent buckets
    // We want to sort bits (totalBits - firstSortBit) through (totalBits - 1)
    // So startBit = totalBits - firstSortBit, and we want to sort firstSortBit bits
    int startBit = totalBits - numBits;
    int endBit = totalBits;  // Sort up to totalBits (exclusive)
    const int numBuckets = 1 << numBits;
    std::vector<uint32_t> bucketStarts(numBuckets + 1, 0);
    // std::cout << "start_bit=" << startBit << " end_bit=" << endBit << " numBits=" << numBits << std::endl;
    // auto firstSortStart = std::chrono::high_resolution_clock::now();
    bool result_in_temp = radixSortAVX2(arr, numBits, endBit, temp, startBit, 0, arr.size(), true, &bucketStarts);
    if (result_in_temp) {
        arr.swap(temp);
        if (firstSortBit == totalBits) {
            return;
        }
    }
    // auto firstSortEnd = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> firstSortElapsed = firstSortEnd - firstSortStart;
    // std::cout << "First sort elapsed time: " << firstSortElapsed.count() << " seconds" << std::endl;
     

    // Step 2: Find bucket boundaries
    // const int numBuckets = 1 << firstSortBit;
    const uint64_t bucketMask = numBuckets - 1;
    const int bucketShift = totalBits - numBits;
    // auto bucketStartsStart = std::chrono::high_resolution_clock::now();
    
    // // Find where each bucket starts and ends
    // std::vector<std::vector<size_t>> per_thread_bucket_counts(THREAD_NUM, std::vector<size_t>(numBuckets + 1, 0));
    
    // #pragma omp parallel num_threads(THREAD_NUM)
    // {
    //     int thread_id = omp_get_thread_num();
    //     #pragma omp for schedule(static)
    //     for (size_t i = 0; i < arr.size(); ++i) {
    //         uint64_t bucket = (arr[i] >> bucketShift) & bucketMask;
    //         per_thread_bucket_counts[thread_id][bucket + 1]++;
    //     }
    // }
    
    // // Combine per-thread counts
    // for (int bucket = 0; bucket <= numBuckets; ++bucket) {
    //     bucketStarts[bucket] = 0;
    //     for (int thread = 0; thread < THREAD_NUM; ++thread) {
    //         bucketStarts[bucket] += per_thread_bucket_counts[thread][bucket];
    //     }
    // }
    //

    // uint64_t currentBucket = (arr[0] >> bucketShift) & bucketMask;
    // size_t bucketEntryCount = 0;

    // for (size_t i = 0; i < arr.size(); ++i) {
    //     uint64_t bucket = (arr[i] >> bucketShift) & bucketMask;
    //     if (bucket != currentBucket) {
    //         bucketStarts[currentBucket + 1] = bucketEntryCount;
    //         currentBucket = bucket;
    //         bucketEntryCount = 0;
    //     }
    //     ++bucketEntryCount;
    // }
    // bucketStarts[currentBucket + 1] = bucketEntryCount;
    
    // Convert counts to cumulative starts
    for (int i = 1; i <= numBuckets; ++i) {
        bucketStarts[i] += bucketStarts[i - 1];
    }
    // auto bucketStartsEnd = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> bucketStartsElapsed = bucketStartsEnd - bucketStartsStart;
    // std::cout << "Bucket starts elapsed time: " << bucketStartsElapsed.count() << " seconds" << std::endl;
    
    // Step 3: Sort each bucket in parallel
    std::atomic<bool> parallel_result_in_temp{false};
    std::atomic<bool> result_set{false};
    // auto parallelSortStart = std::chrono::high_resolution_clock::now();
    #pragma omp parallel for num_threads(THREAD_NUM) schedule(dynamic)
    for (int bucket = 0; bucket < numBuckets; ++bucket) {
        size_t start = bucketStarts[bucket];
        size_t end = bucketStarts[bucket + 1];
        
        if (end > start) {  // Only sort non-empty buckets
            // Sort the remaining bits (0 to totalBits - firstSortBit)
            bool bucket_result_in_temp = radixSortAVX2(arr, 9, totalBits - numBits, temp, finalSortBit, start, end);
            
            // All threads should have the same result (since they sort the same number of bits)
            // We only need to set this once from any non-empty bucket
            bool expected = false;
            if (result_set.compare_exchange_weak(expected, true, std::memory_order_relaxed)) {
                parallel_result_in_temp.store(bucket_result_in_temp, std::memory_order_relaxed);
            }
        }
    }
    
    // If parallel phase results are in temp, swap back to arr
    if (parallel_result_in_temp.load(std::memory_order_relaxed)) {
        arr.swap(temp);
    }
    
    // auto parallelSortEnd = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> parallelSortElapsed = parallelSortEnd - parallelSortStart;
    // std::cout << "Parallel sort elapsed time: " << parallelSortElapsed.count() << " seconds" << std::endl;
}

static void radixSort(std::vector<uint64_t>& arr, int numBits, int totalBits) {
    if (arr.empty() || numBits <= 0 || numBits > 64 || totalBits <= 0 || totalBits > 64) return;

    const int bucketCount = 1 << numBits; // Number of buckets (2^numBits)
    const uint64_t mask = bucketCount - 1; // Mask to extract numBits

    std::vector<int> count(bucketCount, 0); // Use vector for bucket counts
    std::vector<uint64_t> temp(arr.size()); // Temporary array for stability

    for (int shift = 0; shift < totalBits; shift += numBits) {
        // Reset count array
        
        std::fill(count.begin(), count.end(), 0);

        // Count occurrences of each bucket
        for (uint64_t num : arr) {
            int bucket = (num >> shift) & mask;
            count[bucket]++;
        }

        // Compute prefix sums for bucket indices
        for (int i = 1; i < bucketCount; ++i) {
            count[i] += count[i - 1];
        }

        // Place elements into temporary array in sorted order
        for (int i = arr.size() - 1; i >= 0; --i) {
            int bucket = (arr[i] >> shift) & mask;
            temp[--count[bucket]] = arr[i];
        }

        // Swap arr and temp
        arr.swap(temp);
    }
}


static void SortPackedVector(std::vector<u64> &v_add,
                             std::vector<uint64_t> &temp_arr_to_use,
                             size_t num_bits,
                             double &fill_time_per_entry_ns) {
    std::cout << "num-bits=" << num_bits << std::endl;
    const auto start_time = std::chrono::high_resolution_clock::now();
    const int radix = v_add.size() > (1ULL << 28) ? 9 : 8;
    if (radixSortAVX2(v_add, radix, num_bits, temp_arr_to_use, 0, 0, v_add.size())) {
        v_add.swap(temp_arr_to_use);
    }
    const auto end_time = std::chrono::high_resolution_clock::now();
#ifdef TIME_TEST
    const std::chrono::duration<double> elapsed = end_time - start_time;
    std::cout << std::setw(20) << "Sorting Elapsed time:"
              << std::setw(10) << elapsed.count() << " s" << std::endl;
#endif
    double time = compute_fill_time_per_entry_ns(
        elapsed_ns(start_time, end_time),
        v_add.size());
    std::cout << "sort=" << time << std::endl;
    fill_time_per_entry_ns += time;
}

// #define SORT_ONLY
// #define TEST_ONLY_BUCKET
#if defined(TEST_ONLY_BUCKET) && !defined(SORT_ONLY)
    #error "TEST_ONLY_BUCKET requires SORT_ONLY to be defined"
#endif

template<typename filterTable>
struct FilterAPI<Prefix_Filter<filterTable>> {
    using Table = Prefix_Filter<filterTable>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        constexpr float loads[2] = {.95, .95};
        // std::cout << "Lower workload" << std::endl;
        // std::cout << "Workload 1!" << std::endl;
        return Table(add_count, loads, allocate);
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static inline void AddHashed(u64 key, Table *table) {
        table->AddHashed(key);
    }

    static inline void Add(u64 key, Table *table) {
        table->Add(key);
    }

    using PdLayout = SortedBucketLayout<16, min_pd::MAX_CAP0, min_pd::QUOTS, 7, 6, 32, 31>;

    // A PD buffers a few keys beyond its capacity, which are the ones the spare will take.
    using StreamBuffer = FixedArray<min_pd::MAX_WITH_SPARE>;
    static_assert(min_pd::MAX_WITH_SPARE <= MaxSortedBucketKeys,
                  "SortBucketKeys has to cover a full buffer");

    // Prefix Filter does not need extra synchronization state, but the shared streaming driver
    // gives every filter one build-lifetime state object.
    struct StreamingBuildState {
        explicit StreamingBuildState(Table *) {}
    };

    static size_t GetNumBuckets(Table *table) {
        return table->GetNumPd();
    }

    template<typename Key>
    static void AddOverflowToSpare(Table *table, size_t pd_index, const Key *overflowing,
                                   size_t count) {
        for (size_t i = 0; i < count; ++i) {
            table->incSpare_add(pd_index,
                                (u8) PdLayout::quot_of(overflowing[i]),
                                PdLayout::rem_of(overflowing[i]));
        }
    }

    // Groups one PD's buffer by quotient and writes the PD from it.
    static void ProcessPdBucket(uint64_t pd_index, StreamBuffer *arr_buff,
                                Table *table) {
        const uint16_t *const keys = arr_buff[pd_index].data;
        const size_t num_keys = arr_buff[pd_index].size;

        // A lookup skips the PD when the query is above the largest key it kept, so the PD has to
        // hold the smallest keys of its bucket with the largest in its last slot. Grouping settles
        // that for every quotient but the one the capacity splits, which SortQuotientAtSplit fixes.
        uint16_t grouped[min_pd::MAX_WITH_SPARE];
        GroupBucketKeysByQuotient<min_pd::QUOTS>(keys, num_keys, grouped);
        SortQuotientAtSplit(grouped, num_keys, PdLayout::Capacity);

        // Clearing the PD keeps the body bytes past the last key zero. It cannot clobber the
        // keys that are about to be read, since a PD only overlaps buffers that were already
        // consumed, never its own; the grouped copy above is independent of it regardless.
        __m256i empty_pd = _mm256_setzero_si256();
        table->WritePd(static_cast<uint32_t>(pd_index), &empty_pd, sizeof(empty_pd));

        u8 *const pd = reinterpret_cast<u8 *>(
                table->get_pd_by_index(static_cast<uint32_t>(pd_index)));
        FillSortedBucket<PdLayout>(
                pd_index, grouped, num_keys,
                SortedBucketBytes{pd, pd + PdLayout::HeaderBytes},
                [table](size_t index, const uint16_t *overflowing, size_t count) {
                    AddOverflowToSpare(table, index, overflowing, count);
                });
    }

    /**
     * @brief Builds every PD from the per-PD key buffers that the hashing pass produced.
     *
     * The buffers already group the keys by PD, so unlike FillSortedBulk there are no runs to
     * look for, only each PD's own keys to group by quotient. The PD array is aliased one buffer
     * behind the buffers, which puts a PD entirely before the keys of its own bucket, so every
     * buffer is consumed before it can be overwritten.
     *
     * A PD is under half a buffer wide, so the PDs of [k, 2k) only reach into buffers below k.
     * Building the PDs in those doubling waves keeps the aliasing safe once a wave is spread over
     * several threads: whatever a wave overwrites was already consumed by an earlier wave.
     */
    static void FillWithBuffer(StreamBuffer *arr_buff, Table *table,
                               StreamingBuildState &build_state) {
        (void) build_state;
        table->set_pd_array(arr_buff - 1);
        const uint64_t num_pd = table->GetNumPd();

        uint64_t wave_start = 0;
        uint64_t wave_end = 2;
        while (wave_start < num_pd) {
            const uint64_t pd_end = std::min(wave_end, num_pd);
            #ifdef _OPENMP
            #pragma omp parallel for num_threads(THREAD_NUM) schedule(static)
            #endif
            for (uint64_t pd_index = wave_start; pd_index < pd_end; ++pd_index) {
                ProcessPdBucket(pd_index, arr_buff, table);
            }
            wave_start = wave_end;
            wave_end <<= 1;
        }
    }

    static void FillSortedBulk(const std::vector<u64> *v_add, Table *table) {
        const size_t entries_in_pds = FillSortedBuckets<PdLayout>(
                v_add->data(), v_add->size(),
                [table](size_t pd_index) {
                    u8 *const pd = reinterpret_cast<u8 *>(
                            table->get_pd_by_index(static_cast<uint32_t>(pd_index)));
                    return SortedBucketBytes{pd, pd + PdLayout::HeaderBytes};
                },
                [table](size_t pd_index, const u64 *overflowing, size_t count) {
                    AddOverflowToSpare(table, pd_index, overflowing, count);
                });

        table->set_cap(0, entries_in_pds);
        table->set_cap(1, v_add->size() - entries_in_pds);
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] Prefix_Filter\n";
        const size_t entries_before = table->get_cap();
        auto start_time = std::chrono::high_resolution_clock::now();
        if (!sorted) {
            for (auto el : *v_add) {
                Add(el, table);
            }
        } else {
            FillSortedBulk(v_add, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        const uint64_t fill_time_ns = elapsed_ns(start_time, end_time);
        const size_t inserted_entries = table->get_cap() - entries_before;
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(fill_time_ns, inserted_entries);
        #ifdef TIME_TEST
            std::chrono::duration<double> elapsed = end_time - start_time;
            const std::string separator = "--------------------------------------------------------";

            std::cout << separator << std::endl;
            std::cout << "===== START OF PERFORMANCE SUMMARY =====" << std::endl;

            // Display elapsed time
            std::cout << std::setw(20) << "Elapsed time:" << std::setw(10) << elapsed.count() << " s" << std::endl;

            // Display sorted status
            std::cout << std::setw(20) << "Sorted:" << std::setw(10) << (sorted ? "Yes" : "No") << std::endl;

            // Display size of v_add
            std::cout << std::setw(20) << "v_add.size():" << std::setw(10) << v_add->size() << std::endl;

            // Display table's capacity
            std::cout << std::setw(20) << "Num in PD:" << std::setw(10) << table->get_capacity() << std::endl;

            // Display table's cap
            std::cout << std::setw(20) << "Capacity:" << std::setw(10) << table->get_cap() << std::endl;

            // Display empty PDs count
            std::cout << std::setw(20) << "Num empty PDs:" << std::setw(10) << table->count_empty_PDs() << std::endl;

            // Display total number of PDs
            std::cout << std::setw(20) << "Total num PDs:" << std::setw(10) << table->GetNumPd() << std::endl;

            std::cout << "===== END OF PERFORMANCE SUMMARY =====" << std::endl;
            std::cout << separator << std::endl;
        #endif
    }

    static void HashBufInVec(u64 *v_orig, Table *table, std::vector<u64> &v_fill, uint64_t size) {
        // auto start_time = std::chrono::high_resolution_clock::now();
        hashing::TwoIndependentMultiplyShift H0 = *table->GetH0();
        for (int i = 0; i < size; i++) {
            u64 hashed_val = H0(v_orig[i]);
            u32 reduced_hash = table->reduce32(hashed_val >> 32, table->GetNumPd());
            uint16_t qr = table->fixed_reduce(hashed_val);
            u64 saved_val = ((uint64_t) reduced_hash) << 16 | qr;
            v_fill.at(i) = saved_val;
        }
        // auto end_time = std::chrono::high_resolution_clock::now();
        // std::chrono::duration<double> elapsed = end_time - start_time;
        // std::cout << std::setw(20) << "Hashing Elapsed time:" << std::setw(10) << elapsed.count() << " s" << std::endl;    
    }

    static void ImprovedHashVec(std::vector<u64> *v_orig, StreamBuffer *bucket_array, Table *table,
                                std::vector<u64> &tmp, size_t num_entries,
                                StreamingBuildState &build_state) {
        (void) build_state;
        const size_t num_pd = table->GetNumPd();
        HashIntoBucketBuffers(
                v_orig, bucket_array, num_pd, tmp,
                [table, num_pd, H0 = *table->GetH0()](u64 key) {
                    const u64 hashed_key = H0(key);
                    const u64 pd_index = table->reduce32(hashed_key >> 32, num_pd);
                    return (pd_index << StreamKeyBits) | table->fixed_reduce(hashed_key);
                },
                [table](size_t, size_t pd_index, uint16_t evicted) {
                    table->incSpare_add(pd_index, evicted >> 8, (u8) evicted);
                });
    }

    static void HashVec(std::vector<u64> *v_orig, std::vector<u64> *v_hashed, Table *table) {
        v_hashed->resize(v_orig->size());
        std::fill(v_hashed->begin(), v_hashed->end(), 0);
        auto start_time = std::chrono::high_resolution_clock::now();
        hashing::TwoIndependentMultiplyShift H0 = *table->GetH0();
        for (int i = 0; i < v_orig->size(); i++) {
            u64 hashed_val = H0(v_orig->at(i));
            u32 reduced_hash = table->reduce32(hashed_val >> 32, table->GetNumPd());
            uint16_t qr = table->fixed_reduce(hashed_val);
            u64 saved_val = ((uint64_t) reduced_hash) << 16 | qr;
            v_hashed->at(i) = saved_val;
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        #ifdef TIME_TEST
            std::cout << std::setw(20) << "Hashing Elapsed time:" << std::setw(10) << elapsed.count() << " s" << std::endl;    
        #endif
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_orig->size());
    }

    static void Merge(Table *table, std::vector<u64> &v_add) {
        static constexpr uint64_t pd256_plus_init_header = (((INT64_C(1) << min_pd::QUOTS) - 1) << 6) | 32;
        constexpr unsigned kBytes2copy = 7;

        // This function can be used to merge a sorted vector into the table efficiently.
        uint64_t cur_pd_header;
        uint8_t cur_pd_sections[min_pd::MAX_CAP0];
        uint8_t new_pd_sections[min_pd::MAX_CAP0];
        __m256i *cur_pd_ptr;
        u64 start = v_add.at(0);
        u64 pd_index = start >> 16;
        uint16_t cur_pd_max; // This is made of the first 8 bits (quot) and last 8 bits (rem).
        uint8_t index_in_pd = 0;
        bool is_pd_full = false;
        bool is_pd_overflowing = false;

        for (auto el : v_add) {
            // u64 next_pd_index = el >> 16;
            // if (pd_index != next_pd_index) {
            //     // write pd header and change it to the next header!
            //     // Next change the sections!
            //     // write pd!
            //     // Move pd_index to its correct spot!
            //     // Save the max!!!!!
            //     // Update is_pd_full and is_pd_overflowing if needed
            // }

            // const uint16_t qr = el & 65535;
            // const int64_t quot = qr >> 8;
            // const uint8_t rem = qr;
            // if (is_pd_full) {
            //     if (qr >= cur_pd_max) {
            //         // we know it will go straight to the spare so its ok.
            //         cur_cur_header |= 32; // mark the header as overflow
            //         table->incSpare_add(pd_index, quot, rem);
            //     } else {
            //         // In this case we know for sure that it wont go into the spares.
            //         // But we do know that max will go into the spares.
            //         // We need to:
            //         //     1. update the spares with the value we know will go away.
            //         //     2. update the PD.
            //         //     3. update the max value.
            //         uint8_t last_quot = cur_pd_max >> 8;
            //         table->incSpare_add(pd_index, last_quot, (uint8_t) cur_pd_max);
            //         if (!(cur_pd_header & 32)) { // PD isn't marked as overflowed
            //             cur_pd_header ^= (last_quot | 32);
            //         }
            //         // In both cases its easy to pick the last rem (and maybe quot) to move it to cur_pd_max
            //         if (last_quot == quot) {
            //             // In this case we need to fix only the last quot, orig code:
            //             size_t quot_capacity = min_pd::get_spec_quot_cap_from_header(quot, cur_pd_header);
            //             const uint64_t begin_fingerprint = min_pd::MAX_CAP0 - quot_capacity;
            //             const uint64_t end_fingerprint = min_pd::MAX_CAP0;
            //             uint64_t i = begin_fingerprint;
            //             if (i == 0) { // this case is annoying because the first value is at the end of the header.
            //                 i++;
            //                 if (rem <=(uint8_t) cur_pd_header) {
            //                     // in this case we need to move from here. So we may as well do it 
            //                 }
            //             }
            //             //     pd_add_50_only_rem(rem, quot_capacity, pd);

            //         } else {
            //             // In this case we need to move all entries
            //             //     const u8 old_rem = get_last_byte(pd);
            //             //     add_full_pd(did_ovf, last_quot, quot, rem, pd); THIS IS AN OVERKILL!
            //             //     return {(u8) last_quot, old_rem, false};
            //         }
            //     }
            // }

            
        }
    }

    static void FillAndMerge(std::vector<u64> &v_add, Table *table) {
        if (table->is_empty()) {
            Fill(&v_add, table, true);
        } else {
            Merge(table, v_add);
        }
    }

    static void Sort(std::vector<u64> &v_add, Table *table, std::vector<uint64_t> &temp_arr_to_use) {
        size_t num_bits = 16 + std::ceil(std::log2(table->GetNumPd()));
        SortPackedVector(v_add, temp_arr_to_use, num_bits,
                         fill_time_per_entry_ns);
    }

    static void Remove(u64 key, Table *table) {
        throw std::runtime_error("Unsupported");
    }

    CONTAIN_ATTRIBUTES static bool Contain(u64 key, const Table *table) {
        return table->Find(key);
    }

    static std::string get_name(const Table *table) {
        return table->get_name();
    }

    static auto get_functionality(const Table *table) -> uint32_t {
        return 3;
    }

    static auto get_ID(const Table *table) -> filter_id {
        return prefix_id;
    }

    static size_t get_byte_size(const Table *table) {
        return table->get_byte_size();
    }

    static size_t get_cap(const Table *table) {
        return table->get_cap();
    }
};


template<typename ItemType,
         size_t bits_per_item,
         bool branchless,
         typename HashFamily>
struct FilterAPI<bloomfilter::BloomFilter<ItemType, bits_per_item, branchless, HashFamily>> {
    using Table = bloomfilter::BloomFilter<ItemType, bits_per_item, branchless, HashFamily>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count, bool allocate = true) {
        fill_time_per_entry_ns = 0;
        return Table(add_count);
    }

    static double get_fill_time_per_entry_ns() {
        return fill_time_per_entry_ns;
    }

    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] BloomFilter\n";
        auto start_time = std::chrono::high_resolution_clock::now();
        for (auto el : *v_add) {
            Add(el, table);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_add->size());
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &) {
        print_unsupported_filter_operation("Sort");
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        table->Add(key);
    }

    static void Add(uint64_t key, Table *table) {
        table->Add(key);
    }

    static void Remove(uint64_t key, Table *table) {
        throw std::runtime_error("Unsupported");
    }

    inline static bool Contain(uint64_t key, const Table *table) {
        return table->Contain(key) == bloomfilter::Ok;
    }

    static std::string get_name(const Table *table) {
        return table->get_name();
    }

    static auto get_info(const Table *table) -> std::stringstream {
        assert(0);
        std::stringstream ss;
        return ss;
    }

    static auto get_functionality(const Table *table) -> uint32_t {
        return 3;
    }
    static auto get_ID(const Table *table) -> filter_id {
        return bloom_id;
    }

    static size_t get_byte_size(const Table *table) {
        return table->SizeInBytes();
    }

    static size_t get_cap(const Table *table) {
        return -1;
        // return table->get_cap();
    }
};

/* 
// The statistics gathered for each table type:
struct Statistics {
    size_t add_count;
    double nanos_per_add;
    double nanos_per_remove;
    // key: percent of queries that were expected to be positive
    map<int, double> nanos_per_finds;
    double false_positive_probabilty;
    double bits_per_item;
};


// Output for the first row of the table of results. type_width is the maximum number of
// characters of the description of any table type, and find_percent_count is the number
// of different lookup statistics gathered for each table. This function assumes the
// lookup expected positive probabiilties are evenly distributed, with the first being 0%
// and the last 100%.
inline string StatisticsTableHeader(int type_width, const std::vector<double> &found_probabilities) {
    ostringstream os;

    os << string(type_width, ' ');
    os << setw(8) << right << "";
    os << setw(8) << right << "";
    for (size_t i = 0; i < found_probabilities.size(); ++i) {
        os << setw(8) << "find";
    }
    os << setw(8) << "1*add+";
    os << setw(8) << "" << setw(11) << "" << setw(11)
       << "optimal" << setw(8) << "wasted" << setw(8) << "million" << endl;

    os << string(type_width, ' ');
    os << setw(8) << right << "add";
    os << setw(8) << right << "remove";
    for (double prob : found_probabilities) {
        os << setw(8 - 1) << static_cast<int>(prob * 100.0) << '%';
    }
    os << setw(8) << "3*find";
    os << setw(9) << "ε%" << setw(11) << "bits/item" << setw(11)
       << "bits/item" << setw(8) << "space%" << setw(8) << "keys";
    return os.str();
}

// Overloading the usual operator<< as used in "std::cout << foo", but for Statistics
template<class CharT, class Traits>
basic_ostream<CharT, Traits> &operator<<(
        basic_ostream<CharT, Traits> &os, const Statistics &stats) {
    os << fixed << setprecision(2) << setw(8) << right
       << stats.nanos_per_add;
    double add_and_find = 0;
    os << fixed << setprecision(2) << setw(8) << right
       << stats.nanos_per_remove;
    for (const auto &fps : stats.nanos_per_finds) {
        os << setw(8) << fps.second;
        add_and_find += fps.second;
    }
    add_and_find = add_and_find * 3 / stats.nanos_per_finds.size();
    add_and_find += stats.nanos_per_add;
    os << setw(8) << add_and_find;

    // we get some nonsensical result for very small fpps
    if (stats.false_positive_probabilty > 0.0000001) {
        const auto minbits = log2(1 / stats.false_positive_probabilty);
        os << setw(8) << setprecision(4) << stats.false_positive_probabilty * 100
           << setw(11) << setprecision(2) << stats.bits_per_item << setw(11) << minbits
           << setw(8) << setprecision(1) << 100 * (stats.bits_per_item / minbits - 1)
           << " " << setw(7) << setprecision(3) << (stats.add_count / 1000000.);
    } else {
        os << setw(8) << setprecision(4) << stats.false_positive_probabilty * 100
           << setw(11) << setprecision(2) << stats.bits_per_item << setw(11) << 64
           << setw(8) << setprecision(1) << 0
           << " " << setw(7) << setprecision(3) << (stats.add_count / 1000000.);
    }
    return os;
}

struct samples {
    double found_probability;
    std::vector<uint64_t> to_lookup_mixed;
    size_t true_match;
    size_t actual_sample_size;
};

typedef struct samples samples_t;
 */
#endif
