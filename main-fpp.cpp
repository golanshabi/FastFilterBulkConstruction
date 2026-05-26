#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>  // For std::strtoul
#include <cmath>    // For std::pow
#include "Tests/smart_tests.hpp"

// Definition of global thread count variable
int THREAD_NUM = 10;
static constexpr double FILL_RATIO = 0.91;
static constexpr double BCF_FILL_RATIO = 0.91;

template<typename Filter>
void run_FPR_test(const std::string &name, std::vector<u64> *v_add, const std::vector<u64> *v_find, const std::string &path, size_t filter_max_capacity, bool is_hashable = false, bool create_file = false, bool sorted = false) {
    for (int i = 0; i < NUM_SAMPLES; i++) {
        testSmart::FPR_test<Filter>(v_add, v_find, path, is_hashable, create_file, sorted, filter_max_capacity);
    }
    std::cout << "Filter=" << name << " Average time=" << get_average_time() << " Variance=" << get_variance() << std::endl;
    reset_tracker();
}

void write_fpp_to_file(size_t fp_capacity);

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <exponent> [thread_count]" << std::endl;
        return 1;
    }

    unsigned int exponent = std::strtoul(argv[1], nullptr, 10);
    size_t fp_capacity = 1ULL << (exponent);

    // Set thread count if provided as second argument
    if (argc >= 3) {
        THREAD_NUM = std::strtoul(argv[2], nullptr, 10);
        if (THREAD_NUM <= 0) {
            std::cerr << "Error: Thread count must be positive" << std::endl;
            return 1;
        }
    }


    std::cout << "Using fp_capacity = 2^(" << exponent << ") = " << fp_capacity << std::endl;
    std::cout << "Using THREAD_NUM = " << THREAD_NUM << std::endl;
    write_fpp_to_file(fp_capacity);

    return 0;
}

void write_fpp_to_file(size_t fp_capacity) {
    using CF8 = cuckoofilter::CuckooFilter<uint64_t, 8>;
    using CF12 = cuckoofilter::CuckooFilter<uint64_t, 12>;
    using CF16 = cuckoofilter::CuckooFilter<uint64_t, 16>;
    using CF32 = cuckoofilter::CuckooFilter<uint64_t, 32>;
    using CF8_N2 = cuckoofilter::CuckooFilterStable<uint64_t, 8>;
    using CF12_Flex = cuckoofilter::CuckooFilterStable<uint64_t, 12>;
    using CF16_N2 = cuckoofilter::CuckooFilterStable<uint64_t, 16>;
    using CF32_N2 = cuckoofilter::CuckooFilterStable<uint64_t, 32>;

    using inc4 = Prefix_Filter<SimdBlockFilterFixed<>>;
    using inc6 = Prefix_Filter<CF12_Flex>;
    using inc8 = Prefix_Filter<TC_shortcut>;
    using inc9 = Prefix_Filter<Impala512<>>;

    using L_BF8 = bloomfilter::BloomFilter<uint64_t, 8, 0>;
    using L_BF12 = bloomfilter::BloomFilter<uint64_t, 12, 0>;
    using L_BF16 = bloomfilter::BloomFilter<uint64_t, 16, 0>;

    size_t fp_lookups = fp_capacity;

    // FILTER_ONLY selects which bulk filter to build: "bcf", "pf", or unset for both. It
    // keeps a radix sweep from paying for the filter it is not measuring, in time and RAM.
    const char *filter_only = std::getenv("FILTER_ONLY");
    const bool run_bcf = filter_only == nullptr || std::string(filter_only) == "bcf";
    const bool run_pf = filter_only == nullptr || std::string(filter_only) == "pf";

    // Both filters are built for fp_capacity keys, but each one is filled up to the load it is
    // meant to be used at.
    uint64_t read_size = static_cast<uint64_t>(fp_capacity * FILL_RATIO);
    read_size = ((read_size + 1023) / 1024) * 1024;

    std::vector<u64> fp_v_add, fp_v_find;
    testSmart::fill_vec_smart(&fp_v_add, read_size);

    size_t bcf_capacity = fp_capacity * BCF_FILL_RATIO;
    bcf_capacity = ((bcf_capacity + 3) / 4) * 4; // round up to nearest multiple of 4

    std::vector<u64> bcf_v_add, bcf_v_find;
    if (run_bcf) {
        std::cout << "read_size: " << read_size << " bcf_capacity: " << bcf_capacity << std::endl;
        for (size_t i = 0; i < bcf_capacity; i++) {
            bcf_v_add.push_back(fp_v_add[i]);
            bcf_v_find.push_back(fp_v_add[i]);
        }
    }

    // testSmart::fill_vec_smart(&fp_v_find, fp_lookups);

    std::string path = "../scripts/fpp_table.csv";

    std::fstream file(path, std::fstream::in | std::fstream::out | std::fstream::app);
    file << "n =, " << fp_capacity << ", Lookups =, " << fp_lookups << std::endl;
    std::string header = "Filter, Size in bytes, Ratio of yes-queries bits per item (average), optimal bits per item (w.r.t. yes-queries), difference of BPI to optimal BPI, ratio of BPI to optimal BPI";
    file << header << std::endl;
    file.close();
    
    // std::cout << "Testing RAM taken for size " << fp_capacity << std::endl;
    // testSmart::FPR_test<inc9>(&fp_v_add, &fp_v_find, path, false, false, false);
    if (run_pf)
        run_FPR_test<inc9>("PFMultiThreadedInMem", &fp_v_add, &fp_v_find, path, fp_capacity, true, false, true);
    if (run_bcf) {
        run_FPR_test<PQF::PQF_8_53>("BCFMultiThreadedInMem", &bcf_v_add, &bcf_v_find, path,
                                    fp_capacity, true, false, true);
        // Incremental build: Threaded=true (PQF_8_52_T), raw keys, no bulk sort/hash path.
        // run_FPR_test<PQF::PQF_8_52_T>("BCFMultiThreadedIncInMem", &bcf_v_add, &bcf_v_find, path,
                                    //   fp_capacity, false, false, false);
    }
}
