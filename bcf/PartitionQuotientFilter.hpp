#ifndef DYNAMIC_PREFIX_FILTER_HPP
#define DYNAMIC_PREFIX_FILTER_HPP

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include <set>
#include <random>
#include "Bucket.hpp"
#include "QRContainers.hpp"
#include "RemainderStore.hpp"

namespace PQF {

    template<typename T, size_t alignment>
    class AlignedVector { //Just here to make locking easier, lol
        private:
            std::size_t s;
            std::size_t alignedSize;
            T* vec;
            bool owned; //False while pointing at memory somebody else allocated, which a bulk build does

            constexpr size_t getAlignedSize(size_t num) {
                return ((num*sizeof(T)+alignment - 1) / alignment)*alignment;
            }

        public:
            AlignedVector(std::size_t s=0, bool allocate=true): s{s}, alignedSize{getAlignedSize(s)}, vec{allocate ? static_cast<T*>(std::aligned_alloc(alignment, alignedSize)) : NULL}, owned{allocate} {
                if(!allocate) return;
                for(size_t i{0}; i < s; i++) {
                    vec[i] = T();
                }
            }
            ~AlignedVector() {
                if(owned && vec != NULL)
                    free(vec);
            }

            AlignedVector(const AlignedVector& a): s{a.s}, alignedSize{getAlignedSize(s)}, vec{static_cast<T*>(std::aligned_alloc(alignment, alignedSize))}, owned{true} {
                memcpy(vec, a.vec, alignedSize);
            }

            AlignedVector& operator=(const AlignedVector& a) {
                if(owned && vec!=NULL)
                    free(vec);
                s = a.s;
                alignedSize = a.alignedSize;
                vec = static_cast<T*>(std::aligned_alloc(alignment, alignedSize));
                owned = true;
                memcpy(vec, a.vec, alignedSize);
                return *this;
            }

            AlignedVector(AlignedVector&& a): s{a.s}, alignedSize{a.alignedSize}, vec{a.vec}, owned{a.owned} {
                a.vec = NULL;
                a.s = 0;
                a.alignedSize = 0;
                a.owned = false;
            }

            AlignedVector& operator=(AlignedVector&& a) {
                vec = a.vec;
                s = a.s;
                alignedSize = a.alignedSize;
                owned = a.owned;
                a.s = 0;
                a.alignedSize = 0;
                a.vec = NULL;
                a.owned = false;
                return *this;
            }

            //Points the vector at memory it does not own and will not free, so that a bulk build
            //can turn the buffer that already holds the keys into the buckets themselves.
            void setExternalStorage(T* external) {
                if(owned && vec != NULL)
                    free(vec);
                vec = external;
                owned = false;
            }

            T& operator[](size_t i) {
                return vec[i];
            }

            const T& operator[](size_t i) const {
                return vec[i];
            }

            size_t size() const {
                return s;
            }
    };

    template<std::size_t SizeRemainders, std::size_t BucketNumMiniBuckets, std::size_t FrontyardBucketCapacity = 51, std::size_t BackyardBucketCapacity = 35, std::size_t FrontyardToBackyardRatio = 8, std::size_t FrontyardBucketSize = 64, std::size_t BackyardBucketSize = 64, bool FastSQuery = false, bool Threaded = false>
    class PartitionQuotientFilter {
        static_assert(FrontyardBucketSize == 32 || FrontyardBucketSize == 64);
        static_assert(BackyardBucketSize == 32 || BackyardBucketSize == 64);

        private:
            using FrontyardQRContainerType = FrontyardQRContainer<BucketNumMiniBuckets>;
            using FrontyardBucketType = Bucket<SizeRemainders, FrontyardBucketCapacity, BucketNumMiniBuckets, FrontyardQRContainer, FrontyardBucketSize, FastSQuery, Threaded>;
            static_assert(sizeof(FrontyardBucketType) == FrontyardBucketSize);
            using BackyardQRContainerType = BackyardQRContainer<BucketNumMiniBuckets, SizeRemainders, FrontyardToBackyardRatio>;
            template<size_t NumMiniBuckets>
            using WrappedBackyardQRContainerType = BackyardQRContainer<NumMiniBuckets, SizeRemainders, FrontyardToBackyardRatio>;
            using BackyardBucketType = Bucket<SizeRemainders + 4, BackyardBucketCapacity, BucketNumMiniBuckets, WrappedBackyardQRContainerType, BackyardBucketSize, FastSQuery, Threaded>;
            static_assert(sizeof(BackyardBucketType) == BackyardBucketSize);

            inline static constexpr double NormalizingFactor = (double)FrontyardBucketCapacity / (double) BucketNumMiniBuckets * (double)(1+FrontyardToBackyardRatio*FrontyardBucketSize/BackyardBucketSize)/(FrontyardToBackyardRatio*FrontyardBucketSize/BackyardBucketSize);

            // static constexpr uint64_t HashMask = (1ull << SizeRemainders) - 1;
            const uint64_t RealRemainderSize, HashMask;

            static_assert(64 % FrontyardBucketSize == 0 && 64 % BackyardBucketSize == 0);

            static constexpr std::size_t frontyardLockCachelineMask = ~(64ull / FrontyardBucketSize - 1); //So that if multiple buckets in same cacheline, we always pick the same one to lock to not get corruption.
            inline void lockFrontyard(std::size_t i) {
                if constexpr (Threaded) {
                    frontyard[i & frontyardLockCachelineMask].lock();
                }
            }
            inline void unlockFrontyard(std::size_t i) {
                if constexpr (Threaded) {
                    frontyard[i & frontyardLockCachelineMask].unlock();
                }
            }

            inline static constexpr std::size_t backyardLockCachelineMask = ~(64ull / BackyardBucketSize - 1);

            inline void lockBackyard(std::size_t i1, std::size_t i2) {
                if constexpr (Threaded) {
                    i1 &= backyardLockCachelineMask;
                    i2 &= backyardLockCachelineMask;
                    if (i1 == i2) { 
                        backyard[i1].lock();
                        return;
                    }
                    if (i1 > i2) std::swap(i1, i2);
                    backyard[i1].lock();
                    backyard[i2].lock();
                }
            }

            inline void unlockBackyard(std::size_t i1, std::size_t i2) {
                if constexpr (Threaded) {
                    i1 &= backyardLockCachelineMask;
                    i2 &= backyardLockCachelineMask;
                    if (i1 == i2) { 
                        backyard[i1].unlock();
                        return;
                    }
                    // if (i1 > i2) std::swap(i1, i2);
                    backyard[i2].unlock();
                    backyard[i1].unlock();
                }
            }
            
            std::uint64_t R;
            // std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> backyardToFrontyard; //Comment this out when done with testing I guess?
            // std::vector<size_t> overflows;
            inline FrontyardQRContainerType getQRPairFromHash(std::uint64_t hash) {
                if constexpr (DEBUG) {
                    FrontyardQRContainerType f = FrontyardQRContainerType(hash >> SizeRemainders, hash & HashMask);
                    assert(f.bucketIndex < frontyard.size());
#ifdef CUCKOO_HASH
                    BackyardQRContainerType fb1(f, 0, R, backyard.size());
                    BackyardQRContainerType fb2(f, 1, R, backyard.size());
#else
                    BackyardQRContainerType fb1(f, 0, R);
                    BackyardQRContainerType fb2(f, 1, R);
#endif

                    assert(fb1.bucketIndex < backyard.size() && fb2.bucketIndex < backyard.size());
                }
                return FrontyardQRContainerType(hash >> RealRemainderSize, hash & HashMask);
            }

            static constexpr std::size_t BulkRelocationDepth = 3;
            static constexpr std::size_t BulkNumCrumbs =
                    2 * BackyardQRContainerType::ConsolidationFactorP2;

            // Everything a relocation search needs to know about one full backyard
            // bucket. An entry's crumb records which front-yard bucket sent it, so
            // the crumb alone determines that entry's other legal bucket, and all
            // entries sharing a crumb share it. Only one entry per crumb is
            // therefore worth considering, which caps a scan at 2C candidates
            // rather than BackyardBucketCapacity of them. Inverting the rotating
            // hash needs the bucket index divided by R, which depends on the bucket
            // and not on the entry, so it is computed once per scan.
            struct BulkBucketScan {
                std::size_t divR;
                std::size_t modR;
                std::uint32_t crumbsPresent;
                std::uint8_t firstSlotOfCrumb[BulkNumCrumbs];
            };

            // The rotating hash has a special case that the inversion below does not
            // undo, so under NEW_HASH only second-hash entries can be relocated.
            static constexpr std::uint32_t BulkUsableCrumbMask =
                    NEW_HASH ? ~((1u << BackyardQRContainerType::ConsolidationFactorP2) - 1)
                             : ~0u;

            // The front-yard bucket that sent an entry with this crumb to this bucket.
            inline std::size_t bulkSourceFrontyardBucket(std::size_t backyardBucketIndex,
                                                         const BulkBucketScan& scan,
                                                         std::size_t crumb) const {
                if (crumb >= BackyardQRContainerType::ConsolidationFactorP2) {
                    // Second hash: bucket = f / C and crumb = (f mod C) + C.
                    return backyardBucketIndex * FrontyardToBackyardRatio +
                           (crumb - BackyardQRContainerType::ConsolidationFactorP2);
                }
                // Rotating hash: writing f in base C as (f3, f2, f1), the bucket is
                // f3 + f1*R and the crumb is f2.
                return (scan.modR * FrontyardToBackyardRatio + crumb) *
                               FrontyardToBackyardRatio +
                       scan.divR;
            }

            // The other backyard bucket that entry also hashes to.
            inline std::size_t bulkAlternativeBucket(std::size_t backyardBucketIndex,
                                                     const BulkBucketScan& scan,
                                                     std::size_t crumb) const {
                if (crumb >= BackyardQRContainerType::ConsolidationFactorP2) {
                    // The source is f = backyardBucketIndex*C + (crumb - C), and the
                    // rotating hash sends it to f/C^2 + (f mod C)*R.
                    return backyardBucketIndex / FrontyardToBackyardRatio +
                           (crumb - BackyardQRContainerType::ConsolidationFactorP2) * R;
                }
                // The second hash of the source is just f / C.
                return scan.modR * FrontyardToBackyardRatio + crumb;
            }

            // Move one entry to the other backyard bucket it hashes to. Lookup,
            // remove and promotion all probe both buckets, so no other operation can
            // tell the difference.
            inline bool moveBulkEntry(std::size_t backyardBucketIndex,
                                      const BulkBucketScan& scan,
                                      std::size_t crumb,
                                      std::size_t alternativeBucketIndex) {
                const std::size_t keyIndex = scan.firstSlotOfCrumb[crumb];
                const bool usedSecondHash =
                        crumb >= BackyardQRContainerType::ConsolidationFactorP2;
                const std::size_t miniBucket =
                        backyard[backyardBucketIndex].queryWhichMiniBucket(keyIndex);
                const std::size_t frontyardBucketIndex =
                        bulkSourceFrontyardBucket(backyardBucketIndex, scan, crumb);
                assert(frontyardBucketIndex < frontyard.size());
                assert(miniBucket < BucketNumMiniBuckets);

                FrontyardQRContainerType frontyardQR(
                        frontyardBucketIndex * BucketNumMiniBuckets + miniBucket,
                        backyard[backyardBucketIndex].remainderStore.get(keyIndex) & HashMask);
                BackyardQRContainerType current(frontyardQR, usedSecondHash, R);
                BackyardQRContainerType alternative(frontyardQR, !usedSecondHash, R);
                assert(current.bucketIndex == backyardBucketIndex);
                assert(alternative.bucketIndex == alternativeBucketIndex);

                backyard[backyardBucketIndex].remainderStoreRemoveReturn(keyIndex, miniBucket);
                if (backyard[alternativeBucketIndex].insert(alternative).miniBucketIndex ==
                    -1ull) {
                    return true;
                }

                // Unreachable while the bulk builder has the filter to itself, since
                // the caller established the vacancy. A concurrent writer could take
                // it, so put the entry back rather than lose it.
                backyard[backyardBucketIndex].insert(current);
                return false;
            }

            // Free one slot in backyardBucketIndex by moving one of its entries to
            // the other bucket that entry already hashes to, chaining up to depth
            // moves. Nothing moves unless the whole chain succeeds.
            inline bool makeBulkRelocationRoom(
                    std::size_t backyardBucketIndex,
                    std::size_t depth,
                    std::size_t (&visited)[BulkRelocationDepth],
                    std::size_t visitedCount) {
                if (!backyard[backyardBucketIndex].full()) {
                    return true;
                }
#ifdef CUCKOO_HASH
                // The cuckoo-hash experiment does not retain enough information to
                // invert a backyard entry without searching the front yard.
                (void) depth;
                (void) visited;
                (void) visitedCount;
                return false;
#else
                if (depth == 0) {
                    return false;
                }

                BulkBucketScan scan;
                scan.divR = backyardBucketIndex / R;
                scan.modR = backyardBucketIndex - scan.divR * R;
                scan.crumbsPresent = 0;

                // A one-hop move is both the cheapest to find and the cheapest to
                // perform, so every alternative gets a chance at one before any of
                // them is expanded. Searching depth first instead lets a bucket
                // early in the scan drag the whole chain to its depth limit while a
                // later alternative had a free slot all along. The scan doubles as
                // that pass so that it can stop at the first alternative with room.
                // Only full buckets get here, so every slot holds a key.
                for (std::size_t keyIndex = 0; keyIndex < BackyardBucketCapacity; ++keyIndex) {
                    const std::size_t crumb =
                            backyard[backyardBucketIndex].remainderStore.get4BitPart(keyIndex);
                    const std::uint32_t crumbBit = 1u << crumb;
                    if ((scan.crumbsPresent & crumbBit) != 0) {
                        continue;
                    }
                    scan.crumbsPresent |= crumbBit;
                    scan.firstSlotOfCrumb[crumb] = static_cast<std::uint8_t>(keyIndex);
                    if ((crumbBit & BulkUsableCrumbMask) == 0) {
                        continue;
                    }

                    const std::size_t alternativeBucketIndex =
                            bulkAlternativeBucket(backyardBucketIndex, scan, crumb);
                    if (alternativeBucketIndex == backyardBucketIndex ||
                        alternativeBucketIndex >= backyard.size()) {
                        continue;
                    }
                    if (!backyard[alternativeBucketIndex].full()) {
                        return moveBulkEntry(backyardBucketIndex, scan, crumb,
                                             alternativeBucketIndex);
                    }
                }

                if (depth == 1) {
                    return false;
                }

                // Every bucket on the chain is full, so a deeper hop must not try to
                // free one of them by moving an entry back into another.
                visited[visitedCount++] = backyardBucketIndex;
                for (std::uint32_t crumbs = scan.crumbsPresent & BulkUsableCrumbMask;
                     crumbs != 0; crumbs &= crumbs - 1) {
                    const std::size_t crumb = __builtin_ctz(crumbs);
                    const std::size_t alternativeBucketIndex =
                            bulkAlternativeBucket(backyardBucketIndex, scan, crumb);
                    if (alternativeBucketIndex == backyardBucketIndex ||
                        alternativeBucketIndex >= backyard.size()) {
                        continue;
                    }
                    bool onChain = false;
                    for (std::size_t i = 0; i < visitedCount; ++i) {
                        onChain |= visited[i] == alternativeBucketIndex;
                    }
                    if (onChain) {
                        continue;
                    }
                    if (makeBulkRelocationRoom(alternativeBucketIndex, depth - 1,
                                               visited, visitedCount) &&
                        moveBulkEntry(backyardBucketIndex, scan, crumb,
                                      alternativeBucketIndex)) {
                        return true;
                    }
                }
                return false;
#endif
            }

            inline bool insertOverflowBulk(
                    FrontyardQRContainerType overflow,
                    BackyardQRContainerType firstBackyardQR,
                    BackyardQRContainerType secondBackyardQR) {
                if (backyard[firstBackyardQR.bucketIndex].full() &&
                    backyard[secondBackyardQR.bucketIndex].full()) {
                    std::size_t visited[BulkRelocationDepth];
                    const bool madeRoom =
                            makeBulkRelocationRoom(firstBackyardQR.bucketIndex,
                                                   BulkRelocationDepth, visited, 0) ||
                            makeBulkRelocationRoom(secondBackyardQR.bucketIndex,
                                                   BulkRelocationDepth, visited, 0);
                    if (!madeRoom) {
                        if constexpr (DIAGNOSTICS) {
                            insertFailure = true;
                            failureFB = overflow.bucketIndex;
                            failureBucket1 = firstBackyardQR.bucketIndex;
                            failureBucket2 = secondBackyardQR.bucketIndex;
                            failureWFB = firstBackyardQR.whichFrontyardBucket;
                        }
                        return false;
                    }
                }

                return insertOverflow(
                        overflow, firstBackyardQR, secondBackyardQR);
            }

            inline bool insertOverflow(FrontyardQRContainerType overflow, BackyardQRContainerType firstBackyardQR, BackyardQRContainerType secondBackyardQR) {
                std::size_t fillOfFirstBackyardBucket = backyard[firstBackyardQR.bucketIndex].countKeys();
                std::size_t fillOfSecondBackyardBucket = backyard[secondBackyardQR.bucketIndex].countKeys();
                
                if(fillOfFirstBackyardBucket < fillOfSecondBackyardBucket) {
                    if constexpr (PARTIAL_DEBUG || DEBUG)
                        assert(backyard[firstBackyardQR.bucketIndex].insert(firstBackyardQR).miniBucketIndex == -1ull); //Failing this would be *really* bad, as it is the main unproven assumption this algo relies on
                    else {
                        if constexpr (DIAGNOSTICS) {
                            bool success = backyard[firstBackyardQR.bucketIndex].insert(firstBackyardQR).miniBucketIndex == -1ull;
                            failureFB = overflow.bucketIndex;
                            failureBucket1 = firstBackyardQR.bucketIndex;
                            failureBucket2 = secondBackyardQR.bucketIndex;
                            failureWFB = firstBackyardQR.whichFrontyardBucket;
                            insertFailure = !success;
                            return success;
                        }
                        else {
                            backyard[firstBackyardQR.bucketIndex].insert(firstBackyardQR);
                        }
                    }
                }
                else {
                    if constexpr (PARTIAL_DEBUG || DEBUG)
                        assert(backyard[secondBackyardQR.bucketIndex].insert(secondBackyardQR).miniBucketIndex == -1ull);
                    else {
                        if constexpr (DIAGNOSTICS) {
                            bool success = backyard[secondBackyardQR.bucketIndex].insert(secondBackyardQR).miniBucketIndex == -1ull;
                            insertFailure = !success;
                            failureFB = overflow.bucketIndex;
                            failureBucket1 = firstBackyardQR.bucketIndex;
                            failureBucket2 = secondBackyardQR.bucketIndex;
                            failureWFB = firstBackyardQR.whichFrontyardBucket;
                            if(!success) {
                                std::cerr << firstBackyardQR.bucketIndex << " " << secondBackyardQR.bucketIndex << std::endl;
                            }
                            return success;
                        }
                        else {
                            return backyard[secondBackyardQR.bucketIndex].insert(secondBackyardQR).miniBucketIndex == -1ull;
                        }
                    }
                }
                return true;
            }

            inline std::uint64_t queryBackyard(FrontyardQRContainerType overflow, BackyardQRContainerType firstBackyardQR, BackyardQRContainerType secondBackyardQR) {
                return backyard[firstBackyardQR.bucketIndex].querySimple(firstBackyardQR) || backyard[secondBackyardQR.bucketIndex].querySimple(secondBackyardQR);
            }

            inline bool removeFromBackyard(FrontyardQRContainerType frontyardQR, BackyardQRContainerType firstBackyardQR, BackyardQRContainerType secondBackyardQR, bool elementInFrontyard) {
                if (elementInFrontyard) { //In the case we removed it from the frontyard bucket, we need to bring back an element from the backyard (if there is one)
                    //Pretty messy since need to figure out who in the backyard has the key with a smaller miniBucket, since we want to bring the key with the smallest miniBucket index back into the frontyard
                    std::size_t fillOfFirstBackyardBucket = backyard[firstBackyardQR.bucketIndex].countKeys();
                    std::size_t fillOfSecondBackyardBucket = backyard[secondBackyardQR.bucketIndex].countKeys();
                    if(fillOfFirstBackyardBucket==0 && fillOfSecondBackyardBucket==0) return true;
                    // todo which frontyard bucket
                    std::uint64_t keysFromFrontyardInFirstBackyard = backyard[firstBackyardQR.bucketIndex].remainderStore.query4BitPartMask(firstBackyardQR.whichFrontyardBucket, (1ull << fillOfFirstBackyardBucket) - 1);
                    std::uint64_t keysFromFrontyardInSecondBackyard = backyard[secondBackyardQR.bucketIndex].remainderStore.query4BitPartMask(secondBackyardQR.whichFrontyardBucket, (1ull << fillOfSecondBackyardBucket) - 1);
                    if constexpr (DEBUG) {
                        assert(firstBackyardQR.whichFrontyardBucket == firstBackyardQR.remainder >> SizeRemainders);
                        assert(secondBackyardQR.whichFrontyardBucket == secondBackyardQR.remainder >> SizeRemainders);
                    }
                    if(keysFromFrontyardInFirstBackyard == 0 && keysFromFrontyardInSecondBackyard == 0) return true;
                    std::uint64_t firstKeyBackyard = __builtin_ctzll(keysFromFrontyardInFirstBackyard);
                    std::uint64_t secondKeyBackyard = __builtin_ctzll(keysFromFrontyardInSecondBackyard);
                    std::uint64_t firstMiniBucketBackyard = backyard[firstBackyardQR.bucketIndex].queryWhichMiniBucket(firstKeyBackyard);
                    std::uint64_t secondMiniBucketBackyard = backyard[secondBackyardQR.bucketIndex].queryWhichMiniBucket(secondKeyBackyard);
                    if constexpr (DEBUG) {
                        assert(firstMiniBucketBackyard >= frontyard[frontyardQR.bucketIndex].queryWhichMiniBucket(FrontyardBucketCapacity-2) && secondMiniBucketBackyard >= frontyard[frontyardQR.bucketIndex].queryWhichMiniBucket(FrontyardBucketCapacity-2));
                    }
                    if(firstMiniBucketBackyard < secondMiniBucketBackyard) {
                        frontyardQR.miniBucketIndex = firstMiniBucketBackyard;
                        frontyardQR.remainder = backyard[firstBackyardQR.bucketIndex].remainderStoreRemoveReturn(firstKeyBackyard, firstMiniBucketBackyard) & HashMask;
                    }
                    else {
                        frontyardQR.miniBucketIndex = secondMiniBucketBackyard;
                        frontyardQR.remainder = backyard[secondBackyardQR.bucketIndex].remainderStoreRemoveReturn(secondKeyBackyard, secondMiniBucketBackyard) & HashMask;
                    }
                    frontyard[frontyardQR.bucketIndex].insert(frontyardQR);
                }
                else {
                    if (!backyard[firstBackyardQR.bucketIndex].remove(firstBackyardQR)) {
                        return backyard[secondBackyardQR.bucketIndex].remove(secondBackyardQR);
                    }
                }
                return true;
            }

            inline bool insertInner(FrontyardQRContainerType frontyardQR) {
                FrontyardQRContainerType overflow = frontyard[frontyardQR.bucketIndex].insert(frontyardQR);
                if constexpr (DEBUG) {
                    assert((uint64_t)(&frontyard[frontyardQR.bucketIndex]) % FrontyardBucketSize == 0);
                }
                if(overflow.miniBucketIndex != -1ull) {
#ifdef CUCKOO_HASH
                    BackyardQRContainerType firstBackyardQR(overflow, 0, R, backyard.size());
                    BackyardQRContainerType secondBackyardQR(overflow, 1, R, backyard.size());
#else
                    BackyardQRContainerType firstBackyardQR(overflow, 0, R);
                    BackyardQRContainerType secondBackyardQR(overflow, 1, R);
#endif
                    lockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);

                    bool retval = insertOverflow(overflow, firstBackyardQR, secondBackyardQR);

                    unlockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);
                    return retval;
                }
                return true;
            }
            inline std::uint64_t queryWhereInner(FrontyardQRContainerType frontyardQR) {
                std::uint64_t frontyardQuery = frontyard[frontyardQR.bucketIndex].query(frontyardQR);
                if(frontyardQuery != 2) return frontyardQuery;

                if constexpr (DIAGNOSTICS) {
                    backyardLookupCount ++;
                }
#ifdef CUCKOO_HASH
                BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R, backyard.size());
                BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R, backyard.size());
#else
                BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R);
                BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R);
#endif
                lockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);

                std::uint64_t retval = queryBackyard(frontyardQR, firstBackyardQR, secondBackyardQR);

                unlockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);

                return retval | 2;
            }
            inline bool queryInner(FrontyardQRContainerType frontyardQR) {
                std::uint64_t frontyardQuery = frontyard[frontyardQR.bucketIndex].query(frontyardQR);
                if(frontyardQuery != 2) return frontyardQuery;

                if constexpr (DIAGNOSTICS) {
                    backyardLookupCount ++;
                }
#ifdef CUCKOO_HASH
                BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R, backyard.size());
                BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R, backyard.size());
#else
                BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R);
                BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R);
#endif
                lockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);

                bool retval = queryBackyard(frontyardQR, firstBackyardQR, secondBackyardQR);

                unlockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);

                return retval;
            }

            inline bool removeInner(FrontyardQRContainerType frontyardQR) {
#ifdef CUCKOO_HASH
                BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R, backyard.size());
                BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R, backyard.size());
#else
                BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R);
                BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R);
#endif

                bool frontyardBucketFull = frontyard[frontyardQR.bucketIndex].full();
                bool elementInFrontyard = frontyard[frontyardQR.bucketIndex].remove(frontyardQR);
                if(!frontyardBucketFull) {
                    return elementInFrontyard;
                }
                else {
#ifdef CUCKOO_HASH
                    BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R, backyard.size());
                    BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R, backyard.size());
#else
                    BackyardQRContainerType firstBackyardQR(frontyardQR, 0, R);
                    BackyardQRContainerType secondBackyardQR(frontyardQR, 1, R);
#endif

                    lockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);

                    bool retval = removeFromBackyard(frontyardQR, firstBackyardQR, secondBackyardQR, elementInFrontyard);

                    unlockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);
                    return retval;
                }
                return true;
            }
        


        public:
            bool insertFailure = false;
            std::size_t failureFB = 0;
            std::size_t failureBucket1 = 0;
            std::size_t failureBucket2 = 0;
            std::size_t failureWFB = 0;
            std::size_t backyardLookupCount = 0;

            // std::size_t normalizedCapacity;
            std::size_t capacity;
            std::size_t range;

            //AllocateFrontyard is false when a bulk build will hand the front yard its memory
            //through setFrontyardArray, in which case every bucket is written before it is read.
            PartitionQuotientFilter(std::size_t N, bool Normalize = true, bool AllocateFrontyard = true): 
                RealRemainderSize{SizeRemainders},
                HashMask{(1ull << SizeRemainders) - 1},
                capacity{Normalize ? static_cast<size_t>(N/NormalizingFactor) : N},
                range{capacity << SizeRemainders},
                frontyard((capacity+BucketNumMiniBuckets-1)/BucketNumMiniBuckets, AllocateFrontyard),
                backyard((frontyard.size()+FrontyardToBackyardRatio-1)/FrontyardToBackyardRatio + FrontyardToBackyardRatio*2)
            {
                R = frontyard.size() / FrontyardToBackyardRatio / FrontyardToBackyardRatio + 1;
                if(R % (FrontyardToBackyardRatio - 1) == 0) R++;
            }

            using FrontyardMiniFilterType = typename FrontyardBucketType::TypeOfMiniFilter;

            static constexpr std::size_t FrontyardHeaderBytes = FrontyardMiniFilterType::NumBytes;

            inline std::size_t getNumFrontyardBuckets() const {
                return frontyard.size();
            }

            //Puts the front yard in memory the filter does not own, which has to be aligned to a
            //bucket and hold getNumFrontyardBuckets() of them.
            inline void setFrontyardArray(void* arr) {
                assert(reinterpret_cast<std::uintptr_t>(arr) % FrontyardBucketSize == 0);
                frontyard.setExternalStorage(static_cast<FrontyardBucketType*>(arr));
            }

            // Byte access to the mini filter and the remainders of one front-yard bucket, so
            // that a bulk build can write a whole bucket in place. The mini filter has to sit
            // at the beginning of the bucket, so its address is also the bucket's.
            inline std::uint8_t* getFrontyardHeaderByIndex(std::size_t bucketIndex) {
                assert(bucketIndex < frontyard.size());
                return frontyard[bucketIndex].miniFilter.filterBytes.data();
            }

            inline std::uint8_t* getFrontyardBodyByIndex(std::size_t bucketIndex) {
                assert(bucketIndex < frontyard.size());
                return reinterpret_cast<std::uint8_t*>(
                        frontyard[bucketIndex].remainderStore.remainders.data());
            }

            // Requires exclusive access to the filter. Relocation may touch
            // buckets beyond the two initial candidates, so this bulk-only path
            // is intentionally not a concurrent insertion primitive.
            inline bool bulkInsertOverflowPacked(std::uint64_t packedValue) {
                constexpr std::size_t MiniBucketBits = std::bit_width(BucketNumMiniBuckets - 1);
                constexpr std::size_t FingerprintBits = SizeRemainders + MiniBucketBits;
                constexpr std::uint64_t RemainderMask = (std::uint64_t{1} << SizeRemainders) - 1;
                constexpr std::uint64_t MiniBucketMask = (std::uint64_t{1} << MiniBucketBits) - 1;
                constexpr std::uint64_t FingerprintMask = (std::uint64_t{1} << FingerprintBits) - 1;

                const std::size_t frontyardBucket = packedValue >> FingerprintBits;
                const std::uint64_t fingerprint = packedValue & FingerprintMask;
                const std::size_t miniBucket =
                        (fingerprint >> SizeRemainders) & MiniBucketMask;
                const std::uint64_t remainder = fingerprint & RemainderMask;

                assert(frontyardBucket < frontyard.size());
                assert(miniBucket < BucketNumMiniBuckets);

                FrontyardQRContainerType overflow(
                        frontyardBucket * BucketNumMiniBuckets + miniBucket, remainder);
#ifdef CUCKOO_HASH
                BackyardQRContainerType firstBackyardQR(overflow, 0, R, backyard.size());
                BackyardQRContainerType secondBackyardQR(overflow, 1, R, backyard.size());
#else
                BackyardQRContainerType firstBackyardQR(overflow, 0, R);
                BackyardQRContainerType secondBackyardQR(overflow, 1, R);
#endif

                lockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);
                // std::cout << "first=" << firstBackyardQR.bucketIndex << std::endl;
                // std::cout << "second=" << secondBackyardQR.bucketIndex << std::endl;
                const bool inserted = insertOverflowBulk(
                        overflow, firstBackyardQR, secondBackyardQR);
                unlockBackyard(firstBackyardQR.bucketIndex, secondBackyardQR.bucketIndex);
                return inserted;
            }



             //create new PQF by merging
            PartitionQuotientFilter(const PartitionQuotientFilter& a, const PartitionQuotientFilter& b, std::optional<std::vector<size_t>> verifykeys = {}) :
                RealRemainderSize{a.RealRemainderSize-1},
                HashMask{(1ull << RealRemainderSize) - 1},
                capacity{a.capacity+b.capacity},
                range{a.range},
                frontyard(a.frontyard.size() + b.frontyard.size()),
                backyard(a.backyard.size() + b.backyard.size())
            {
                R = frontyard.size() / FrontyardToBackyardRatio / FrontyardToBackyardRatio + 1;
                if(R % (FrontyardToBackyardRatio - 1) == 0) R++;
                
                if(a.RealRemainderSize != b.RealRemainderSize || (a.capacity != b.capacity) || (a.range != b.range)) {
                    throw std::invalid_argument("Merges must be of filters with the exact same properties");
                }
                

                std::vector<std::pair<uint64_t, uint64_t>> afrontkeys, bfrontkeys;
                afrontkeys.reserve(FrontyardBucketCapacity);
                bfrontkeys.reserve(FrontyardBucketCapacity);
                std::vector<std::pair<uint64_t, uint64_t>> aback1keys, bback1keys;
                std::vector<std::pair<uint64_t, uint64_t>> aback2keys, bback2keys;
                aback1keys.reserve(BackyardBucketCapacity);
                aback2keys.reserve(BackyardBucketCapacity);
                bback1keys.reserve(BackyardBucketCapacity);
                bback2keys.reserve(BackyardBucketCapacity);
                std::vector<std::pair<uint64_t, uint64_t>> allKeys;
                allKeys.reserve(2*FrontyardBucketCapacity + 4*BackyardBucketCapacity + 5);
                // static constexpr size_t AllKeysSize = 2*FrontyardBucketCapacity + 4*BackyardBucketCapacity + 5;
                // std::array<std::pair<uint64_t, uint64_t>, AllKeysSize> allKeys;

                std::multiset<uint64_t> keyset;
                if(verifykeys) {
                    keyset.insert(verifykeys->begin(), verifykeys->end());
                }

                std::vector<FrontyardQRContainerType> overflow;
                std::vector<uint64_t> insertedKeys;

                for(size_t i=0; i < a.frontyard.size(); i++) {
                    a.frontyard[i].deconstruct(afrontkeys);
                    b.frontyard[i].deconstruct(bfrontkeys);
                    
                    FrontyardQRContainerType frontyardQR(i*BucketNumMiniBuckets, 0);
#ifdef CUCKOO_HASH
                    BackyardQRContainerType firstBackyardQR(frontyardQR, 0, a.R, backyard.size());
                    BackyardQRContainerType secondBackyardQR(frontyardQR, 1, a.R, backyard.size());
#else
                    BackyardQRContainerType firstBackyardQR(frontyardQR, 0, a.R);
                    BackyardQRContainerType secondBackyardQR(frontyardQR, 1, a.R);
#endif

                    a.backyard[firstBackyardQR.bucketIndex].deconstruct(aback1keys);
                    b.backyard[firstBackyardQR.bucketIndex].deconstruct(bback1keys);
                    a.backyard[secondBackyardQR.bucketIndex].deconstruct(aback2keys);
                    b.backyard[secondBackyardQR.bucketIndex].deconstruct(bback2keys);

                    auto filterbackyard = [&a] (auto& v, std::vector<std::pair<uint64_t, uint64_t>>& o, BackyardQRContainerType b) {
                        for(auto x: v) {
                            if((x.second & (~a.HashMask)) == b.remainder) {
                                o.push_back(std::make_pair(x.first, x.second & a.HashMask));
                            }
                        }
                    };

                    allKeys.insert(allKeys.end(), afrontkeys.begin(), afrontkeys.end());
                    allKeys.insert(allKeys.end(), bfrontkeys.begin(), bfrontkeys.end());

                    filterbackyard(aback1keys, allKeys, firstBackyardQR);
                    filterbackyard(bback1keys, allKeys, firstBackyardQR);
                    filterbackyard(aback2keys, allKeys, secondBackyardQR);
                    filterbackyard(bback2keys, allKeys, secondBackyardQR);

                    FrontyardQRContainerType temp(0, 0);
                    for(size_t j=0; j < allKeys.size(); j++) {
                        auto x = allKeys[j];
                        if(x.first == BucketNumMiniBuckets) {
                            continue;
                        }
                        uint64_t key = x.second + ((x.first + BucketNumMiniBuckets * i) << a.RealRemainderSize);
                        FrontyardQRContainerType frontyardQR = getQRPairFromHash(key);
                        if(verifykeys) {
                            insertedKeys.push_back(key);
                            if(keyset.count(key) > 0) {
                                keyset.extract(key);
                            }
                            else {
                                std::cerr << "FAILED TO MERGE " << i << " " << j << std::endl;
                                exit(-1);
                            }
                        }
                        auto overflowQR = frontyard[frontyardQR.bucketIndex].insert(frontyardQR);
                        if(overflowQR.miniBucketIndex != -1ull) {
                            overflow.push_back(overflowQR);
                        }
                        else {
                            if(verifykeys && (!query(key))) {
                                std::cerr << "Nani? " << i << " " << j << std::endl;
                                exit(-1);
                            }
                        }
                    }

                    afrontkeys.resize(0);
                    bfrontkeys.resize(0);
                    aback1keys.resize(0);
                    aback2keys.resize(0);
                    bback1keys.resize(0);
                    bback2keys.resize(0);
                    allKeys.resize(0);
                }

                auto generator = std::mt19937_64(std::random_device()());
                std::shuffle(overflow.begin(), overflow.end(), generator);
                for(size_t i=0; i < overflow.size(); i++) {
                    auto qr = overflow[i];
#ifdef CUCKOO_HASH
                    BackyardQRContainerType firstBackyardQR(qr, 0, R, backyard.size());
                    BackyardQRContainerType secondBackyardQR(qr, 1, R, backyard.size());
#else
                    BackyardQRContainerType firstBackyardQR(qr, 0, R);
                    BackyardQRContainerType secondBackyardQR(qr, 1, R);
#endif

                    if(!insertOverflow(qr, firstBackyardQR, secondBackyardQR)) {
                        std::cerr << "Backyard merging failed" << std::endl;
                        exit(-1);
                    }

                    if(verifykeys) {
                        uint64_t key = qr.remainder + ((qr.miniBucketIndex + BucketNumMiniBuckets * qr.bucketIndex) << RealRemainderSize);
                        insertedKeys.push_back(key);
                        if(!query(key)) {
                            std::cerr << "Nani? " << i << std::endl;
                            exit(-1);
                        }
                    }
                }
                
                if(keyset.size() > 0) {
                    std::cerr << "MISSED KEYS. Keys left: " << keyset.size() << std::endl;
                    exit(-1);
                }

                for(size_t i=0; i < insertedKeys.size(); i++) {
                    if(!query(insertedKeys[i])) {
                        std::cerr << "NNaaanii?? " << i << std::endl;
                        exit(-1);
                    }
                }
            }

            bool insert(std::uint64_t hash) {
                FrontyardQRContainerType frontyardQR = getQRPairFromHash(hash);
                lockFrontyard(frontyardQR.bucketIndex);

                bool retval = insertInner(frontyardQR);

                unlockFrontyard(frontyardQR.bucketIndex);

                return retval;
            }
            
            void insertBatch(const std::vector<size_t>& hashes, std::vector<bool>& status, const uint64_t num_keys){
                constexpr size_t bsize = 16;
                for (size_t j=0; j < num_keys; j+=bsize) {
                    for (size_t i=j; i < std::min(num_keys, j+bsize); i++) {
                        FrontyardQRContainerType frontyardQR = getQRPairFromHash(hashes[i]);
                        __builtin_prefetch(&frontyard[frontyardQR.bucketIndex]);
                    }
                    for(size_t i=j; i < std::min(num_keys, j+bsize); i++) {
                        status[i] = insert(hashes[i]);
                    }
                }
            }

            //also queries where the item is (backyard or frontyard)
            std::uint64_t queryWhere(std::uint64_t hash) {
                FrontyardQRContainerType frontyardQR = getQRPairFromHash(hash);
                lockFrontyard(frontyardQR.bucketIndex);

                std::uint64_t retval = queryWhereInner(frontyardQR);

                unlockFrontyard(frontyardQR.bucketIndex);

                return retval;
            }

            bool query(std::uint64_t hash) {
                FrontyardQRContainerType frontyardQR = getQRPairFromHash(hash);
                lockFrontyard(frontyardQR.bucketIndex);

                bool retval = queryInner(frontyardQR);

                unlockFrontyard(frontyardQR.bucketIndex);
                
                return retval;
            }

            void queryBatch(const std::vector<size_t>& hashes, std::vector<bool>& status, const uint64_t num_keys) {
                constexpr size_t bsize = 16;
                for (size_t j=0; j < num_keys; j+=bsize) {
                    for (size_t i=j; i < std::min(num_keys, j+bsize); i++) {
                        FrontyardQRContainerType frontyardQR = getQRPairFromHash(hashes[i]);
                        __builtin_prefetch(&frontyard[frontyardQR.bucketIndex]);
                    }
                    for (size_t i=j; i < std::min(num_keys, j+bsize); i++) {
                        status[i] = query(hashes[i]);
                    }
                }
            }

            std::uint64_t sizeFilter()  {
                return (frontyard.size()*sizeof(FrontyardBucketType)) + (backyard.size()*sizeof(BackyardBucketType));
            }

            bool remove(std::uint64_t hash) {
                if constexpr (DEBUG)
                    assert(query(hash));
                FrontyardQRContainerType frontyardQR = getQRPairFromHash(hash);
                lockFrontyard(frontyardQR.bucketIndex);

                bool retval = removeInner(frontyardQR);

                unlockFrontyard(frontyardQR.bucketIndex);

                return retval;
            }

            void removeBatch(const std::vector<size_t>& hashes, std::vector<bool>& status, const uint64_t num_keys) {
                constexpr size_t bsize = 16;
                for (size_t j=0; j < num_keys; j+=bsize) {
                    for (size_t i=j; i < std::min(num_keys, j+bsize); i++) {
                        FrontyardQRContainerType frontyardQR = getQRPairFromHash(hashes[i]);
                        __builtin_prefetch(&frontyard[frontyardQR.bucketIndex]);
                    }
                    for (size_t i=j; i < std::min(num_keys, j+bsize); i++) {
                        status[i] = remove(hashes[i]);
                    }
                }
            }

            size_t getNumBuckets() {
                return frontyard.size() + backyard.size();
            }
        
        private:
            AlignedVector<FrontyardBucketType, 64> frontyard;
            AlignedVector<BackyardBucketType, 64> backyard;

    };


    using PQF_8_22 = PartitionQuotientFilter<8, 22, 26, 18, 8, 32, 32, false, false>;
    using PQF_8_22_FRQ = PartitionQuotientFilter<8, 22, 26, 18, 8, 32, 32, true, false>;
    using PQF_8_22BB = PartitionQuotientFilter<8, 22, 26, 37, 8, 32, 64, false, false>;
    using PQF_8_22BB_FRQ = PartitionQuotientFilter<8, 22, 26, 37, 8, 32, 64, true, false>;
    using PQF_8_3 = PartitionQuotientFilter<8, 3, 6, 6, 8, 32, 32, false, false>;

    using PQF_8_31 = PartitionQuotientFilter<8, 31, 25, 17, 8, 32, 32, false, false>;
    using PQF_8_31_FRQ = PartitionQuotientFilter<8, 31, 25, 17, 8, 32, 32, true, false>;

    using PQF_8_62 = PartitionQuotientFilter<8, 62, 50, 34, 8, 64, 64, false, false>;
    using PQF_8_62_FRQ = PartitionQuotientFilter<8, 62, 50, 34, 8, 64, 64, true, false>;

    using PQF_8_53 = PartitionQuotientFilter<8, 53, 51, 35, 8, 64, 64, false, false>;
    using PQF_8_53_FRQ = PartitionQuotientFilter<8, 53, 51, 35, 8, 64, 64, true, false>;

    using PQF_16_36 = PartitionQuotientFilter<16, 36, 28, 22, 8, 64, 64, false, false>;
    using PQF_16_36_FRQ = PartitionQuotientFilter<16, 36, 28, 22, 8, 64, 64, true, false>;


    using PQF_8_21_T = PartitionQuotientFilter<8, 21, 26, 18, 8, 32, 32, false, true>;
    using PQF_8_21_FRQ_T = PartitionQuotientFilter<8, 21, 26, 18, 8, 32, 32, true, true>;

    using PQF_8_52_T = PartitionQuotientFilter<8, 52, 51, 35, 8, 64, 64, false, true>;
    using PQF_8_52_FRQ_T = PartitionQuotientFilter<8, 52, 51, 35, 8, 64, 64, true, true>;

    using PQF_16_35_T = PartitionQuotientFilter<16, 35, 28, 22, 8, 64, 64, false, true>;
    using PQF_16_35_FRQ_T = PartitionQuotientFilter<16, 35, 28, 22, 8, 64, 64, false, true>;
}

#endif
