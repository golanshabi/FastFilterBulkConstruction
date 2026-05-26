#ifndef SORT_BUCKET_KEYS_HPP
#define SORT_BUCKET_KEYS_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>

/**
 * @brief The largest bucket a bulk build sorts in one go.
 *
 * It has to cover the biggest key buffer any filter here streams into: the Prefix Filter buffers
 * 32 keys per PD, the Breadcrumb Filter's front-yard bucket holds 51 keys and buffers a few more
 * for its backyard.
 */
inline constexpr std::size_t MaxSortedBucketKeys = 64;

/**
 * @brief Sorts one bucket's keys in place, ascending, with a sorting network for its exact size.
 *
 * This lives in its own translation unit because the networks for every size up to
 * MaxSortedBucketKeys are slow to compile, and nothing about them depends on the filter.
 */
void SortBucketKeys(std::uint16_t *keys, std::size_t num_keys);

/**
 * @brief Groups one bucket's keys by quotient, ascending, with a counting sort.
 *
 * A bucket only has to be grouped, not sorted: the unary header records how many keys each
 * quotient has, and a query masks one quotient's slots and compares every remainder in them,
 * so the order within a quotient is never observed. Counting sort therefore costs 2n + Q on
 * an L1 resident array instead of the n log^2 n comparators a sorting network needs, which is
 * what makes a wide bucket expensive.
 *
 * Grouping alone leaves an arbitrary subset of the quotient the capacity splits in the bucket.
 * A filter that reads the largest key the bucket kept has to call SortQuotientAtSplit to fix
 * that one quotient up.
 *
 * @param keys      The bucket's keys, [quotient | 8 bit remainder].
 * @param num_keys  How many of them, at most MaxSortedBucketKeys.
 * @param grouped   Where to write them, num_keys of them, may not alias keys.
 */
template<std::size_t NumQuots>
inline void GroupBucketKeysByQuotient(const std::uint16_t *keys, std::size_t num_keys,
                                      std::uint16_t *grouped) {
    static constexpr std::size_t RemainderBits = 8;
    assert(num_keys <= MaxSortedBucketKeys);

    // A bucket never holds more keys than a sorting network covers, so a byte per quotient is
    // enough to count and then to address them.
    static_assert(MaxSortedBucketKeys <= 255);
    std::uint8_t nextSlotOfQuot[NumQuots] = {};

    for (std::size_t i = 0; i < num_keys; ++i) {
        assert((keys[i] >> RemainderBits) < NumQuots);
        ++nextSlotOfQuot[keys[i] >> RemainderBits];
    }

    std::uint8_t slot = 0;
    for (std::size_t quot = 0; quot < NumQuots; ++quot) {
        const std::uint8_t count = nextSlotOfQuot[quot];
        nextSlotOfQuot[quot] = slot;
        slot += count;
    }

    for (std::size_t i = 0; i < num_keys; ++i) {
        grouped[nextSlotOfQuot[keys[i] >> RemainderBits]++] = keys[i];
    }
}

/**
 * @brief Sorts the one quotient a bucket's capacity splits, so the split is exact.
 *
 * Grouping puts every quotient below the split fully in the bucket and every quotient above it
 * fully in the second level, so only the quotient the split lands in can be handed the wrong
 * remainders. Sorting that quotient makes the bucket hold the smallest keys of the whole bucket
 * and puts the largest of them in the last slot, which is what a filter needs when it stores
 * that key as the threshold a query compares against to skip the bucket.
 *
 * It is a few keys of work: a quotient holds about num_keys/NumQuots of them.
 *
 * @param grouped   The bucket's keys, already grouped by quotient.
 * @param num_keys  How many of them.
 * @param capacity  How many the bucket keeps, so nothing to do unless num_keys exceeds it.
 */
inline void SortQuotientAtSplit(std::uint16_t *grouped, std::size_t num_keys,
                                std::size_t capacity) {
    static constexpr std::size_t RemainderBits = 8;
    if (num_keys <= capacity || capacity == 0) {
        return;
    }

    const std::uint16_t quot = grouped[capacity - 1] >> RemainderBits;
    std::size_t begin = capacity - 1;
    while (begin > 0 && (grouped[begin - 1] >> RemainderBits) == quot) {
        --begin;
    }
    std::size_t end = capacity;
    while (end < num_keys && (grouped[end] >> RemainderBits) == quot) {
        ++end;
    }

    for (std::size_t i = begin + 1; i < end; ++i) {
        const std::uint16_t key = grouped[i];
        std::size_t j = i;
        while (j > begin && grouped[j - 1] > key) {
            grouped[j] = grouped[j - 1];
            --j;
        }
        grouped[j] = key;
    }
}

#endif
