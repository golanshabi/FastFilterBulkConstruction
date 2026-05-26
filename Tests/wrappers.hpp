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

// #define TIME_TEST
#define GEN_NEW_PD
#define HASH_AHEAD

#include <chrono>
#include <stdexcept>

#include "../Bloom_Filter/bloom.hpp"
#include "../Bloom_Filter/simd-block-fixed-fpp.h"
#include "../Bloom_Filter/Impala512.h"
#include "../Bloom_Filter/simd-block.h"
#include "../Prefix-Filter/min_pd256.hpp"
#include "../TC-Shortcut/TC-shortcut.hpp"
#include "../cuckoofilter/src/cuckoofilter.h"
#include "../cuckoofilter/src/cuckoofilter_stable.h"
// #include "linux-perf-events.h"
#include "hwy/contrib/sort/vqsort.h"
#include "hwy/contrib/sort/order.h"
#include "TimeTracker.hpp"

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
};

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

template<typename Table>
struct FilterAPI {
};

enum class SortType {
    RadixAVX2,
    VQSort,
    Radix,
    RadixBuiltSlowly
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

    static Table ConstructFromAddCount(size_t add_count) {
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

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
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

    static Table ConstructFromAddCount(size_t add_count) {
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
        for (int i = 0; i < v_add->size(); i++) {
            auto el = v_add->at(i);
            if (!Add_attempt(el, table)) {
                break;
            }
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        const size_t inserted_entries = table->get_cap() - entries_before;
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), inserted_entries);
        std::cout << "Inserted entries: " << inserted_entries << std::endl;
    }

    static void HashVec(std::vector<u64> *, std::vector<u64> *, Table *) {
        print_unsupported_filter_operation("HashVec");
    }

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
        print_unsupported_filter_operation("Sort");
    }

    static inline void AddHashed(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            std::cout << get_info(table).str() << std::endl;

            throw std::logic_error("The filter is too small to hold all of the elements");
        }
    }

    static void Add(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            std::cout << get_info(table).str() << std::endl;

            throw std::logic_error("The filter is too small to hold all of the elements");
        }
    }

    static bool Add_attempt(uint64_t key, Table *table) {
        if (table->Add(key) != cuckoofilter::Ok) {
            // std::cout << get_info(table).str() << std::endl;
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

    static Table ConstructFromAddCount(size_t add_count) {
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

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
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

    static Table ConstructFromAddCount(size_t add_count) {
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

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
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

    static Table ConstructFromAddCount(size_t add_count) {
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

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
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

    static Table ConstructFromAddCount(size_t add_count) {
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

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
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

    static Table ConstructFromAddCount(size_t add_count) {
        fill_time_per_entry_ns = 0;
        constexpr float load = .935;
        return Table(add_count, load);
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

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
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
    Table GenSpare;

    static double constexpr overflowing_items_ratio = 0.0586;//  = expected_items95

public:
    __m256i *pd_array;
    Prefix_Filter(size_t max_items, const float loads[2])
        : filter_max_capacity(max_items),
          number_of_pd(std::ceil(1.0 * max_items / (min_pd::MAX_CAP0 * loads[0]))),
          GenSpare(FilterAPI<Table>::ConstructFromAddCount(get_l2_slots<Table>(max_items, overflowing_items_ratio, loads))),
          Hasher(), H0() {

        int ok = posix_memalign((void **) &pd_array, 32, 32 * number_of_pd);
        if (ok != 0) {
            std::cout << "Space allocation failed!" << std::endl;
            assert(false);
            exit(-3);
        }

        constexpr uint64_t pd256_plus_init_header = (((INT64_C(1) << min_pd::QUOTS) - 1) << 6) | 32;
        std::fill(pd_array, pd_array + number_of_pd, __m256i{pd256_plus_init_header, 0, 0, 0});

        // std::cout << "num_pd=" << number_of_pd << std::endl;

        // size_t l1 = sizeof(__m256i) * number_of_pd;
        // size_t l2 = FilterAPI<Table>::get_byte_size(&GenSpare);
        // double ratio = 1.0 * l2 / l1;
        // std::cout << get_name() << ".\t";
        // std::cout << "l1=" << l1 << std::endl;
        // std::cout << "l2=" << l2 << std::endl;
        // std::cout << "l1 + l2 = " << l1 + l2 << std::endl;
        // std::cout << "B per entry = " << ((double)(l1 + l2)) / max_items << std::endl;
    }

    void set_cap(int cap_type, size_t to_set) {
        cap[cap_type] = to_set;
    }

    bool is_empty() { return cap[0] == 0; }

    ~Prefix_Filter() {
        free(pd_array);
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

        _mm_prefetch(reinterpret_cast<const char*>(&arr[i + 4 + 256]), _MM_HINT_T0);
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

static void doCountLastIterScalar(std::vector<uint64_t>& arr, std::vector<uint32_t>& count, const uint64_t mask, int shift)
{
    int countZero = 0;

    if (arr.empty()) return;

    const int unrollFactor = 4;
    uint64_t current = arr[0];
    size_t i = 0;
    
    // Unrolled loop
    for (; i + unrollFactor <= arr.size() - 1; i += unrollFactor) {
        // Process 4 elements with OOE
        uint64_t next0 = arr[i + 1];
        uint64_t next1 = arr[i + 2];
        uint64_t next2 = arr[i + 3];
        uint64_t next3 = arr[i + 4];

        uint64_t bucket0 = (current >> shift) & mask;
        uint64_t bucket1 = (next0 >> shift) & mask;
        uint64_t bucket2 = (next1 >> shift) & mask;
        uint64_t bucket3 = (next2 >> shift) & mask;

        _mm_prefetch(reinterpret_cast<const char*>(&arr[i + 256]), _MM_HINT_T0);

        count[bucket0]++;
        count[bucket1]++;
        count[bucket2]++;
        count[bucket3]++;

        current = next3;
    }

    // Handle remaining elements
    for (; i < arr.size() - 1; ++i) {
        uint64_t shifted = current >> shift;
        uint64_t bucketIndex = shifted & mask;
        _mm_prefetch(reinterpret_cast<const char*>(&arr[i + 256]), _MM_HINT_T0);
        current = arr[i + 1];
        count[bucketIndex]++;
    }
    
    // Handle last element
    uint64_t shifted = current >> shift;
    uint64_t bucketIndex = shifted & mask;
    count[bucketIndex]++;
}


static void radixSortInPlace(std::vector<uint64_t>& arr, int numBits, int totalBits) {
    if (arr.empty() || totalBits <= 0 || numBits <= 0) return;

    struct Range {
        int left;
        int right;
        int shift;
    };

    // Manual stack to replace recursion
    std::vector<Range> stack;
    stack.push_back({0, static_cast<int>(arr.size()) - 1, totalBits - numBits});

    const uint64_t mask = (1ULL << numBits) - 1;
    const int numBuckets = 1 << numBits;

    // Pre-allocate buffers to reuse across iterations to minimize allocations
    std::vector<int> count(numBuckets);
    std::vector<int> bucketStarts(numBuckets);
    std::vector<int> currentOffsets(numBuckets);

    while (!stack.empty()) {
        Range top = stack.back();
        stack.pop_back();

        if (top.left >= top.right || top.shift < 0) continue;

        // 1. Reset and Count frequencies for the current bit-group
        std::fill(count.begin(), count.end(), 0);
        for (int i = top.left; i <= top.right; ++i) {
            count[(arr[i] >> top.shift) & mask]++;
        }

        // 2. Calculate boundaries for each bucket
        bucketStarts[0] = top.left;
        for (int i = 1; i < numBuckets; ++i) {
            bucketStarts[i] = bucketStarts[i - 1] + count[i - 1];
        }
        currentOffsets = bucketStarts;

        // 3. In-place partitioning (American Flag Sort logic)
        for (int b = 0; b < numBuckets; ++b) {
            int bucketEnd = (b == numBuckets - 1) ? top.right + 1 : bucketStarts[b + 1];
            
            while (currentOffsets[b] < bucketEnd) {
                uint64_t val = arr[currentOffsets[b]];
                int targetBucket = (val >> top.shift) & mask;

                if (targetBucket == b) {
                    currentOffsets[b]++;
                } else {
                    // Swap the element to its target bucket and increment its pointer
                    std::swap(arr[currentOffsets[b]], arr[currentOffsets[targetBucket]]);
                    currentOffsets[targetBucket]++;
                }
            }
        }

        // 4. Push sub-ranges onto the stack for the next bit-group (shift - numBits)
        if (top.shift - numBits >= 0) {
            for (int i = 0; i < numBuckets; ++i) {
                int bStart = bucketStarts[i];
                int bEnd = (i == numBuckets - 1) ? top.right : bucketStarts[i + 1] - 1;
                
                // Only push ranges that have more than one element
                if (bEnd > bStart) {
                    stack.push_back({bStart, bEnd, top.shift - numBits});
                }
            }
        }
    }
}

static void radixSortAVX2(std::vector<uint64_t>& arr, int numBits, int totalBits, std::vector<uint64_t> &temp, uint64_t starting_bit) {
    int alignment = ((uint64_t)arr.data()) % 4096;
    int tmp_alignment = ((uint64_t)temp.data()) % 4096;
    // std::cout << "arr alignment: " << alignment << std::endl;
    // std::cout << "temp alignment: " << tmp_alignment << std::endl;
    if (arr.empty() || numBits <= 0 || numBits > 64 || totalBits <= 0 || totalBits > 64) return;

    const int bucketCount = 1 << numBits; // Number of buckets (2^numBits)
    const uint64_t mask = bucketCount - 1; // Mask to extract numBits
    const int simdWidth = 4; // 4 x 64-bit integers per AVX2 register

    alignas(32) uint64_t nums[4];
    alignas(32) uint64_t buckets[4];
    alignas(32) uint64_t indices[4];
    std::vector<uint32_t> count(bucketCount, 0);
    for (int shift = starting_bit; shift < totalBits; shift += numBits) {
        bool is_last_iter = shift + numBits >= totalBits;
        // Count occurrences of each bucket using AVX2
        // auto iter_start_time = std::chrono::high_resolution_clock::now();
        std::fill(count.begin(), count.end(), 0);

        // auto start_time = std::chrono::high_resolution_clock::now();
        doCountLastIter(arr, count, mask, shift);
        // doCountLastIterScalar(arr, count, mask, shift);

        // auto end_time = std::chrono::high_resolution_clock::now();
        // std::chrono::duration<double> elapsed = end_time - start_time;
        // std::cout << std::setw(20) << "First Part Elapsed Time:" << std::setw(10) << elapsed.count() << " s" << std::endl;
        // for (int i = 0; i < count.size(); i++) {
        //     std::cout << "index=" << i << " value=" << count[i] << std::endl;
        // }
        //     kill(cpid, 9);
        // }
        // for (int i = 0; i < count.size(); i++) {
        //     std::cout << "index=" << i << " count=" << count[i] << std::endl;
        // }
        // Optimized prefix sum calculation by unrolling
        {
            // auto start_time = std::chrono::high_resolution_clock::now();
            for (int i = 1; i < bucketCount; i += 4) {
                count[i] += count[i - 1];
                count[i + 1] += count[i];
                count[i + 2] += count[i + 1];
                count[i + 3] += count[i + 2];
            }
            // auto end_time = std::chrono::high_resolution_clock::now();
            // std::chrono::duration<double> elapsed = end_time - start_time;
            // std::cout << std::setw(20) << "Second Part Elapsed Time:" << std::setw(10) << elapsed.count() << " s" << std::endl;
        }

        {
            // auto start_time = std::chrono::high_resolution_clock::now();
            uint64_t num[4];
            for (int j = 0; j < 4; j++) {
                num[j] = arr[arr.size() - 1 - j];
            }
            uint64_t bucket[4];
            uint64_t index[4];
            // #pragma unroll 10
            for (int i = arr.size() - 1; i >= 4; i -= 4) {
                // #pragma unroll 10
                for (int j = 0; j < 4; ++j) {
                    // num[j] = arr[i - j];
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
            
                for (int j = 0; j < 4; ++j) {
                    temp[index[j]] = num[j];
                    num[j] = arr[i - 4 - j];
                }
            }

            // #pragma unroll 10
            for (int j = 0; j < 4; ++j) {
                // num[j] = arr[i - j];
                bucket[j] = (num[j] >> shift) & mask;
                index[j] = --count[bucket[j]];
            }

            for (int j = 0; j < 4; ++j) {
                temp[index[j]] = num[j];
            }

            // auto end_time = std::chrono::high_resolution_clock::now();
            // std::chrono::duration<double> elapsed = end_time - start_time;
            // std::cout << std::setw(20) << "Third Part Elapsed Time:" << std::setw(10) << elapsed.count() << " s" << std::endl;
        }

        // Copy back the sorted elements from temp to arr
        arr.swap(temp);

        // auto iter_end_time = std::chrono::high_resolution_clock::now();
        // std::chrono::duration<double> iter_elapsed = iter_end_time - iter_start_time;
        // std::cout << std::setw(20) << "Total Iteration Time:" << std::setw(10) << iter_elapsed.count() << " s" << std::endl;
    }
}

static void radixSortBuiltSlowly(std::vector<uint64_t>& arr, int numBits, int totalBits, std::vector<uint64_t> &temp, int starting_bit) {
    if (arr.empty() || numBits <= 0 || numBits > 64 || totalBits <= 0 || totalBits > 64) return;

    const int bucketCount = 1 << numBits; // Number of buckets (2^numBits)
    const uint64_t mask = bucketCount - 1; // Mask to extract numBits

    std::vector<uint64_t> count(bucketCount, 0); // Use vector for bucket counts

    for (int shift = starting_bit; shift < totalBits; shift += numBits) {
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

static void radixSort(std::vector<uint64_t>& arr, int numBits, int totalBits, std::vector<uint64_t> &temp) {
    if (arr.empty() || numBits <= 0 || numBits > 64 || totalBits <= 0 || totalBits > 64) return;

    const int bucketCount = 1 << numBits; // Number of buckets (2^numBits)
    const uint64_t mask = bucketCount - 1; // Mask to extract numBits

    std::vector<int> count(bucketCount, 0); // Use vector for bucket counts

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

#define SORT_ONLY
// #define TEST_ONLY_BUCKET
#if defined(TEST_ONLY_BUCKET) && !defined(SORT_ONLY)
    #error "TEST_ONLY_BUCKET requires SORT_ONLY to be defined"
#endif

template<typename filterTable>
struct FilterAPI<Prefix_Filter<filterTable>> {
    using Table = Prefix_Filter<filterTable>;
    static inline double fill_time_per_entry_ns = 0;

    static Table ConstructFromAddCount(size_t add_count) {
        fill_time_per_entry_ns = 0;
        constexpr float loads[2] = {.95, .95};
        // std::cout << "Lower workload" << std::endl;
        // std::cout << "Workload 1!" << std::endl;
        return Table(add_count, loads);
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
    static void Fill(const std::vector<u64> *v_add, Table *table, bool sorted, bool is_hashed) {
        std::cout << "[Fill] Prefix_Filter\n";
        const size_t entries_before = table->get_cap();
        auto start_time = std::chrono::high_resolution_clock::now();
        if (!sorted) {
                for (auto el : *v_add) {
                    Add(el, table);
                }
        } else {
            hashing::TwoIndependentMultiplyShift H0 = *table->GetH0();
            static constexpr uint64_t pd256_plus_init_header = (((INT64_C(1) << min_pd::QUOTS) - 1) << 6) | 32;
            uint32_t num_pd = (uint32_t) table->GetNumPd();
            #ifdef HASH_AHEAD
                u64 start = v_add->at(0);
            #else
                u64 start = H0(v_add->at(0));
            #endif
            u64 pd_index = start >> 16;

            __m256i cur_pd = __m256i{pd256_plus_init_header, 0, 0, 0};
            __m256i *cur_pd_ptr = &cur_pd;

            uint64_t cur_pd_64 = 0;
            uint64_t my_header = pd256_plus_init_header;
            size_t prev_body_index = 0;
            size_t i = 0;
            size_t sec = 0;
            // uint8_t first_rem;
            for (auto el : *v_add) {
                #ifndef HASH_AHEAD
                    el = H0(el);
                #endif
                u64 next_pd_index = el >> 16;
                if (unlikely(next_pd_index != pd_index)) {
                    memcpy(cur_pd_ptr, &my_header, 7);
                    if (cur_pd_64 != 0) {
                        ((uint64_t*)cur_pd_ptr)[(prev_body_index + 7) >> 3] = cur_pd_64;
                        cur_pd_64 = 0;
                    }
                    my_header = pd256_plus_init_header;
                    table->WritePd(pd_index, cur_pd_ptr, sizeof(__m256i));
                    cur_pd = __m256i{pd256_plus_init_header, 0, 0, 0};
                    pd_index = next_pd_index;
                }
                const uint16_t qr = el & 65535;
                const int64_t quot = qr >> 8;
                const uint8_t rem = qr;
                bool is_pd_full = min_pd::is_pd_full(my_header);
                if (unlikely(is_pd_full)) {
                    sec++;
                    const uint64_t spare_pre = reinterpret_cast<const u64 *>(cur_pd_ptr)[0];
                    
                    if (my_header & 32) {
                        uint64_t last_quot_sec = min_pd::sort_last_quot_with_header(cur_pd_ptr, (my_header >> 6ul) & ((1ULL << 50ul) - 1));
                        my_header ^= (last_quot_sec | 32);
                    }
                    table->incSpare_add(pd_index, quot, rem);
                } else {
                    _mm_prefetch(reinterpret_cast<const char*>(table->pd_array + pd_index), _MM_HINT_T0);
                    i++;
                    size_t end_sec = min_pd::select64(my_header >> 6, quot);
                    const size_t h_index_sec = end_sec + 6;
                    const u64 mask_sec = _bzhi_u64(-1, h_index_sec);
                    const u64 lo_sec = my_header & mask_sec;
                    const u64 hi_sec = ((my_header & ~mask_sec) << 1u);// & h_mask;
                    const u64 h7_sec = lo_sec | hi_sec;
                    my_header = h7_sec;
    
                    
                    prev_body_index = end_sec - quot;
                    if (unlikely(prev_body_index == 0)) {
                        // first_rem = rem;
                        // ((uint8_t*) &my_header)[7] = rem;
            
                        min_pd::body_add_case0_avx(prev_body_index, rem, cur_pd);
                        continue;
                    }
                    ((uint8_t*) &cur_pd_64)[(prev_body_index - 1) % 8] = rem;
                    if (unlikely((prev_body_index & 7) == 0)) {
                        if (prev_body_index != 1) {
                            ((uint64_t*)cur_pd_ptr)[prev_body_index >> 3] = cur_pd_64;
                        }
                        cur_pd_64 = 0;
                    }
                }
            }
            #ifdef GEN_NEW_PD
                memcpy(cur_pd_ptr, &my_header, 7);
                if (cur_pd_64 != 0) {
                    ((uint64_t*)cur_pd_ptr)[(prev_body_index + 7) >> 3] = cur_pd_64;
                }
                table->WritePd(pd_index, cur_pd_ptr, sizeof(__m256i));
            #endif
            table->set_cap(0, i);
            table->set_cap(1, sec);
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        const uint64_t fill_time_ns = elapsed_ns(start_time, end_time);
        const size_t inserted_entries = table->get_cap() - entries_before;
        #ifndef SORT_ONLY
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(fill_time_ns, inserted_entries);
        #endif
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
        #ifndef SORT_ONLY
        fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_orig->size());
        #endif
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

    static void Sort(std::vector<u64> &v_add, Table *table, std::vector<uint64_t> &temp_arr_to_use, SortType sort_type) {
        // int pid = getpid();
        // int cpid = fork();
        // if(cpid == 0) {
        //     // child process .  Run your perf stat
        //     char buf[1024];
        //     // sprintf(buf, "/opt/intel/oneapi/vtune/latest/bin64/vtune -collect system-overview -target-pid %d", pid);
        //     sprintf(buf, "perf stat -p %d -e instructions -e LLC-load-misses", pid);
        //     execl("/bin/sh", "sh", "-c", buf, NULL);
        // } else {
        //     setpgid(cpid, 0);
        //     sleep(1);
            auto start_time = std::chrono::high_resolution_clock::now();
            hashing::TwoIndependentMultiplyShift H0 = *table->GetH0();
            auto comp = [&H0](u64 a, u64 b) { return (H0(a))  <= (H0(b)); };
            #ifdef HASH_AHEAD
                // cppsort::ska_sort(v_add);
                // std::cout << "n_bits=" << 16 + std::ceil(std::log2(table->GetNumPd())) << " vector_size=" << v_add.size() << std::endl;
                int radix = v_add.size() > 1 << 28 ? 9 : 8;
                size_t num_bits = 16 + std::ceil(std::log2(table->GetNumPd()));
                size_t starting_bit = 8;
                #ifdef TEST_ONLY_BUCKET
                    starting_bit = 16; // skip quot too, ordering by bucket index alone
                #endif
                // std::cout << "radix=" << radix << " bits=" << 16 + std::ceil(std::log2(table->GetNumPd())) << std::endl;
                switch (sort_type) {
                    case SortType::RadixAVX2:
                        radixSortAVX2(v_add, radix, num_bits, temp_arr_to_use, starting_bit);
                        break;
                    case SortType::VQSort:
                        hwy::VQSort(v_add.data(), v_add.size(), hwy::SortAscending());
                        break;
                    case SortType::Radix:
                        radixSort(v_add, radix, num_bits, temp_arr_to_use);
                        break;
                    case SortType::RadixBuiltSlowly:
                        radixSortBuiltSlowly(v_add, radix, num_bits, temp_arr_to_use, starting_bit);
                        break;
                }
                // radixSortInPlace(v_add, radix, 16 + std::ceil(std::log2(table->GetNumPd())));
            #else
                cppsort::quick_sort(v_add, comp);
            #endif
            std::clock_t cpu_end = std::clock();
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end_time - start_time;
            #ifdef TIME_TEST
                std::cout << std::setw(20) << "Sorting Elapsed time:" << std::setw(10) << elapsed.count() << " s" << std::endl;
            #endif
            fill_time_per_entry_ns += compute_fill_time_per_entry_ns(elapsed_ns(start_time, end_time), v_add.size());
        //     kill(cpid, 9);
        // }
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

    static Table ConstructFromAddCount(size_t add_count) {
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

    static void Sort(std::vector<u64> &, Table *, std::vector<uint64_t> &, SortType) {
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
