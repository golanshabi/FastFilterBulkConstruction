#ifndef BUCKETS_HPP
#define BUCKETS_HPP

#include <cstddef>
#include <cstdint>
#include "MiniFilter.hpp"
#include "RemainderStore.hpp"

namespace PQF {
    //Maybe have a bit set to if the bucket is not overflowed? Cause right now the bucket may send you to the backyard even if there is nothing in the backyard, but the bucket is just full. Not that big a deal, but this slight optimization might be worth a bit?
    //Like maybe have one extra key in the minifilter and then basically account for that or smth? Not sure.
    template<std::size_t SizeRemainders, std::size_t NumKeys, std::size_t NumMiniBuckets, template<std::size_t> typename TypeOfQRContainerTemplate, std::size_t Size, bool FastSQuery, bool Threaded>
    struct alignas(Size) Bucket {
        using TypeOfMiniFilter = MiniFilter<NumKeys, NumMiniBuckets, Threaded>;
        TypeOfMiniFilter miniFilter;
        using TypeOfRemainderStore = RemainderStore<SizeRemainders, NumKeys, TypeOfMiniFilter::Size>;
        TypeOfRemainderStore remainderStore;
        using TypeOfQRContainer = TypeOfQRContainerTemplate<NumMiniBuckets>;
        
        //Returns an overflowed remainder if there was one to be sent to the backyard.
        inline TypeOfQRContainer insert(TypeOfQRContainer qr) {
            std::size_t loc = miniFilter.queryMiniBucketBeginning(qr.miniBucketIndex);
            if(__builtin_expect(loc == NumKeys, 0)) return qr; //Find a way to remove this if statement!!! That would shave off an entire second!
            qr.miniBucketIndex = miniFilter.insert(qr.miniBucketIndex, loc);
            std::uint64_t overflowRemainder = remainderStore.insert(qr.remainder, loc);
            qr.remainder = overflowRemainder;
            return qr;
        }

        //Return 1 if found it, 2 if need to go to backyard, 0 if didn't find and don't need to go to backyard
        inline std::uint64_t query(TypeOfQRContainer qr) {
            if constexpr (!FastSQuery) {
                std::pair<std::uint64_t, std::uint64_t> boundsMask = miniFilter.queryMiniBucketBoundsMask(qr.miniBucketIndex);
                std::uint64_t inFilter = remainderStore.queryVectorizedMask(qr.remainder, boundsMask.second - boundsMask.first);
                if(inFilter != 0)
                    return 1;
                else if (boundsMask.second == (1ull << NumKeys)) {
                    return 2;
                }
                else {
                    return 0;
                }
            }
            else {
                if (!full() || !miniFilter.miniBucketOutofFilterBounds(qr.miniBucketIndex)) {
                    std::uint64_t inFilter = remainderStore.queryVectorizedMask(qr.remainder, -1ull);
                    if(inFilter == 0) {
                        return 0;
                    }
                    else if (NumKeys + NumMiniBuckets <= 64 && (inFilter & (inFilter-1)) == 0) {
                        return miniFilter.checkMiniBucketKeyPair(qr.miniBucketIndex, inFilter);
                    }
                    else {
                        std::pair<std::uint64_t, std::uint64_t> boundsMask = miniFilter.queryMiniBucketBoundsMask(qr.miniBucketIndex);
                        inFilter &= boundsMask.second - boundsMask.first;
                        if(inFilter != 0)
                            return 1;
                        // else if (boundsMask.second == (1ull << NumKeys)) {
                        //     return 2;
                        // }
                        else {
                            return 0;
                        }
                    }
                }
                else {
                    std::pair<std::uint64_t, std::uint64_t> boundsMask = miniFilter.queryMiniBucketBoundsMask(qr.miniBucketIndex);
                    std::uint64_t inFilter = remainderStore.queryVectorizedMask(qr.remainder, boundsMask.second - boundsMask.first);
                    if(inFilter != 0)
                        return 1;
                    else {
                        return 2;
                    }
                }
            }
        }

        inline bool querySimple(TypeOfQRContainer qr) {
            if constexpr (!FastSQuery) {
                std::pair<std::uint64_t, std::uint64_t> boundsMask = miniFilter.queryMiniBucketBoundsMask(qr.miniBucketIndex);
                std::uint64_t inFilter = remainderStore.queryVectorizedMask(qr.remainder, boundsMask.second - boundsMask.first);
                return inFilter != 0;
            }
            else {
                std::uint64_t inFilter = remainderStore.queryVectorizedMask(qr.remainder, -1ull);
                if(inFilter == 0) {
                    return 0;
                }
                else if (NumKeys + NumMiniBuckets <= 64 && (inFilter & (inFilter-1)) == 0) {
                    return miniFilter.checkMiniBucketKeyPair(qr.miniBucketIndex, inFilter);
                }
                else {
                    std::pair<std::uint64_t, std::uint64_t> boundsMask = miniFilter.queryMiniBucketBoundsMask(qr.miniBucketIndex);
                    inFilter &= boundsMask.second - boundsMask.first;
                    return inFilter != 0;
                }
            }
        }

        //Returns true if deleted, false if need to go to backyard (we assume that key exists, so we don't expect to not find it somewhere)
        inline bool remove(TypeOfQRContainer qr) {
            std::pair<std::uint64_t, std::uint64_t> boundsMask = miniFilter.queryMiniBucketBoundsMask(qr.miniBucketIndex);
            std::uint64_t inFilter = remainderStore.queryVectorizedMask(qr.remainder, boundsMask.second - boundsMask.first);
            if(inFilter == 0) {
                return false;
            }
            else {
                std::uint64_t locFirstMatch = __builtin_ctzll(inFilter);
                remainderStore.remove(locFirstMatch);
                miniFilter.remove(qr.miniBucketIndex, locFirstMatch);
                return true;
            }
        }

        inline std::uint64_t remainderStoreRemoveReturn(std::uint64_t keyIndex, std::uint64_t miniBucketIndex) {
            miniFilter.remove(miniBucketIndex, keyIndex);
            return remainderStore.removeReturn(keyIndex);
        }


        inline std::size_t queryWhichMiniBucket(std::size_t keyIndex) {
            return miniFilter.queryWhichMiniBucket(keyIndex);
        }

        inline bool full() {
            return miniFilter.full();
        }

        inline std::size_t countKeys() {
            return miniFilter.countKeys();
        }

        inline void lock() {
            miniFilter.lock();
        }

        inline void unlock() {
            miniFilter.unlock();
        }


        inline void deconstruct(std::vector<std::pair<uint64_t, uint64_t>>& v) const {
            for(size_t i=0; i < NumKeys; i++) {
                std::pair<uint64_t, uint64_t> x = {miniFilter.queryWhichMiniBucket(i), remainderStore.get(i)};
                if(x.first == NumMiniBuckets) continue;
                v.push_back(x);
            }
        }
    };
}

#endif
