#include "bcf/PartitionQuotientFilter.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

namespace {

using Filter = PQF::PQF_8_53;

constexpr size_t kFilterCapacity = 65536;
constexpr size_t kKeyCount = 59637;
constexpr size_t kFrontyardBucketCapacity = 51;
constexpr size_t kTrials = 10;

uint64_t reduce(uint64_t key, const Filter& filter) {
    return static_cast<uint64_t>(
        (static_cast<__uint128_t>(key) * filter.range) >> 64);
}

}  // namespace

int main() {
    std::mt19937_64 generator(std::random_device{}());
    Filter filter(kFilterCapacity);

    struct Counts {
        size_t first = 0;
        size_t second = 0;
    };
    std::vector<Counts> counts(filter.getNumBackyardBuckets());

    size_t overflow_entries = 0;
    for (size_t trial = 0; trial < kTrials; ++trial) {
        std::vector<uint64_t> keys(kKeyCount);
        std::generate(keys.begin(), keys.end(), std::ref(generator));
        std::sort(keys.begin(), keys.end(),
                  [&filter](uint64_t lhs, uint64_t rhs) {
                      return reduce(lhs, filter) < reduce(rhs, filter);
                  });

        size_t begin = 0;
        while (begin < keys.size()) {
            const uint64_t reduced_key = reduce(keys[begin], filter);
            const size_t frontyard_bucket = (reduced_key >> 8) / 53;
            size_t end = begin + 1;
            while (end < keys.size() &&
                   ((reduce(keys[end], filter) >> 8) / 53) == frontyard_bucket) {
                ++end;
            }

            const size_t bucket_entries = end - begin;
            if (bucket_entries > kFrontyardBucketCapacity) {
                const size_t overflow_count =
                        bucket_entries - kFrontyardBucketCapacity;
                const auto [first, second] =
                        filter.getBackyardBucketIndices(reduced_key);
                counts[first].first += overflow_count;
                counts[second].second += overflow_count;
                overflow_entries += overflow_count;
            }
            begin = end;
        }
    }

    std::cout << "backyard_bucket,total,first,second\n";
    for (size_t bucket = 0; bucket < counts.size(); ++bucket) {
        const size_t total = counts[bucket].first + counts[bucket].second;
        std::cout << bucket << ',' << total << ',' << counts[bucket].first
                  << ',' << counts[bucket].second << '\n';
    }
    std::cout << "overflow_entries," << overflow_entries << '\n';
}
