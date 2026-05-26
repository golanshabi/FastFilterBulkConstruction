#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>  // For std::strtoul
#include "Tests/smart_tests.hpp"

static constexpr double FILL_RATIO = 0.91;

template<typename Filter>
void run_FPR_test(const std::string &name, std::vector<u64> *v_add, const std::vector<u64> *v_find, const std::string &path, size_t filter_max_capacity, bool is_hashable = false, bool create_file = false, bool sorted = false, SortType sort_type = SortType::RadixAVX2) {
    for (int i = 0; i < NUM_SAMPLES; i++) {
        testSmart::FPR_test<Filter>(v_add, v_find, path, is_hashable, create_file, sorted, sort_type, filter_max_capacity);
    }
    std::cout << "Filter=" << name << " Average time=" << get_average_time() << " Variance=" << get_variance() << std::endl;
    reset_tracker();
}

void write_fpp_to_file(size_t fp_capacity);

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <exponent>" << std::endl;
        return 1;
    }

    unsigned int exponent = std::strtoul(argv[1], nullptr, 10);
    size_t fp_capacity = 1ULL << (exponent);

    std::cout << "Using fp_capacity = 2^(" << exponent << ") = " << fp_capacity << std::endl;
    write_fpp_to_file(fp_capacity);

    return 0;
}

void write_fpp_to_file(size_t fp_max_capacity) {
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

    size_t fp_capacity = fp_max_capacity * FILL_RATIO;
    fp_capacity += (4 - (fp_capacity % 4)); // round up to nearest multiple of 4

    std::vector<u64> fp_v_add, fp_v_find;
    std::cout << "fp_capacity: " << fp_capacity << std::endl;
    testSmart::fill_vec_smart(&fp_v_add, fp_capacity);
    // testSmart::fill_vec_smart(&fp_v_find, fp_lookups);

    std::string path = "../scripts/fpp_table.csv";
    
    #ifndef SORT_ONLY
    run_FPR_test<CF12>("CF12", &fp_v_add, &fp_v_find, path, fp_max_capacity);
    run_FPR_test<CF8>("CF8", &fp_v_add, &fp_v_find, path, fp_max_capacity);
    run_FPR_test<TC_shortcut>("TC_shortcut", &fp_v_add, &fp_v_find, path, fp_max_capacity);
    run_FPR_test<inc9>("PrefixFilter", &fp_v_add, &fp_v_find, path, fp_max_capacity);
    run_FPR_test<inc9>("PrefixFilterBatch", &fp_v_add, &fp_v_find, path, fp_max_capacity, true, false, true);
    #endif
    #ifdef SORT_ONLY
    // run_FPR_test<inc9>("RadixAVX2", &fp_v_add, &fp_v_find, path, fp_max_capacity, true, false, true, SortType::RadixAVX2);
    // run_FPR_test<inc9>("VQSort", &fp_v_add, &fp_v_find, path, fp_max_capacity, true, false, true, SortType::VQSort);
    // run_FPR_test<inc9>("Radix", &fp_v_add, &fp_v_find, path, fp_max_capacity, true, false, true, SortType::Radix);
    run_FPR_test<inc9>("RadixBuiltSlowlyPartialSorting", &fp_v_add, &fp_v_find, path, fp_max_capacity, true, false, true, SortType::RadixBuiltSlowly);
    #endif

}
