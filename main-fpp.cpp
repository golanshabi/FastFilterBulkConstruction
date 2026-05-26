#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>  // For std::strtoul
#include <cmath>    // For std::pow
#include "Tests/smart_tests.hpp"

static constexpr double FILL_RATIO = 0.91;
static constexpr double BCF_FILL_RATIO = 0.91;

template<typename Filter>
void run_FPR_test(const std::string &name, std::vector<u64> *v_add, const std::vector<u64> *v_find, const std::string &path, bool is_hashable = false, bool create_file = false, bool sorted = false) {
    for (int i = 0; i < NUM_SAMPLES; i++) {
        testSmart::FPR_test<Filter>(v_add, v_find, path, is_hashable, create_file, sorted);
    }
    std::cout << "Filter=" << name << " Average time=" << get_average_time() << " Variance=" << get_variance() << std::endl;
    reset_tracker();
}

void write_fpp_to_file();

int main() {
    write_fpp_to_file();
    return 0;
}

void write_fpp_to_file() {
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

    constexpr size_t fp_capacity = ((1ULL << 30)) / testSmart::db_speeder;
    constexpr size_t fp_lookups = fp_capacity;

    std::vector<u64> fp_v_add, fp_v_find;
    testSmart::fill_vec_smart(&fp_v_add, fp_capacity);

    size_t bcf_capacity = fp_max_capacity * BCF_FILL_RATIO;
    bcf_capacity += (4 - (bcf_capacity % 4)); // round up to nearest multiple of 4

    std::vector<u64> bcf_v_add, bcf_v_find;
    std::cout << "bcf_capacity: " << bcf_capacity << std::endl;
    for (size_t i = 0; i < bcf_capacity; i++) {
        bcf_v_add.push_back(fp_v_add[i]);
        bcf_v_find.push_back(fp_v_add[i]);
    }

    // testSmart::fill_vec_smart(&fp_v_find, fp_lookups);

    std::string path = "../scripts/fpp_table.csv";

    std::fstream file(path, std::fstream::in | std::fstream::out | std::fstream::app);
    file << "n =, " << fp_capacity << ", Lookups =, " << fp_lookups << std::endl;
    std::string header = "Filter, Size in bytes, Ratio of yes-queries bits per item (average), optimal bits per item (w.r.t. yes-queries), difference of BPI to optimal BPI, ratio of BPI to optimal BPI";
    // file << "name, byte size, FPR, BPI, opt-BPI, bpi-additive-difference, bpi-ratio" << std::endl;
    file << header << std::endl;
    file.close();
}
