#ifndef QRTRACKER_HPP
#define QRTRACKER_HPP

#include <cstddef>
#include <cstdint>
#include <immintrin.h>

namespace PQF {
    //Just a data structure to store bucket index, mini bucket index, and remainder trios
    //And also then allow you to get the same for the backyard/attic
    //Just use the global settings in Configuration.hpp for NumMiniBuckets?
    template<std::size_t NumMiniBuckets>
    struct FrontyardQRContainer {
        // std::size_t quotient;
        std::size_t bucketIndex;
        std::size_t miniBucketIndex;
        std::uint64_t remainder;

        //Here hash is assumed to already be in the range supported
        inline FrontyardQRContainer(std::size_t quotient, std::uint64_t remainder): /*quotient{quotient},*/ bucketIndex{quotient/NumMiniBuckets}, miniBucketIndex{quotient%NumMiniBuckets}, remainder{remainder} {
        }
    };

    //Consolidation bits is how many bits we use to refer to which bucket an item came from, one bit of which is used to specify the hashnum.
    template<std::size_t NumMiniBuckets, std::size_t RemainderBits, std::size_t ConsolidationFactor>
    struct BackyardQRContainer {
        // static const std::size_t ConsolidationFactorP2 = 1ull << (64-_lzcnt_u64(ConsolidationFactor-1));
        static constexpr std::size_t ConsolidationFactorP2 = 8; //Why can I not do this as in the above to do it dynamically? So frustrating. Like sure *maybe* I technically don't need it *right now,* but I really don't like having this fixed constant here. Like obviously I get it lzcnt is not a constexpr function, but like *why* is it not a constexpr function, and fine if its not constexpr like whatever, why does static const get typecast to static constexpr for no reason? Can I not initialize it when running the program then wtf?
        // std::size_t quotient; // Do we really need to store this?
        std::size_t realRemainder; //We imbue the remainder with the hash to make a bigger remainder, so this is just to store what was originally the remainder before the addition. Not used but just to be there
        std::size_t bucketIndex;
        std::size_t miniBucketIndex;
        std::uint64_t remainder;
        std::uint_fast8_t whichFrontyardBucket;

        inline void finishInit(std::uint64_t frontyardBucketIndex, bool hashNum, std::uint64_t R) {
            if (hashNum) {
                whichFrontyardBucket = (frontyardBucketIndex % ConsolidationFactor) + ConsolidationFactorP2;
                if constexpr (DEBUG)
                    assert(whichFrontyardBucket < 16);
                bucketIndex = frontyardBucketIndex / ConsolidationFactor;
                // todo why is this required?
                remainder += whichFrontyardBucket << RemainderBits;
            }
            else {
                std::uint64_t fbiMinusLowBits = frontyardBucketIndex / ConsolidationFactor; // f2
                std::uint64_t lowBits = frontyardBucketIndex % ConsolidationFactor; // l1
                bucketIndex = fbiMinusLowBits/ConsolidationFactor + lowBits*R;
                if (NEW_HASH && bucketIndex == fbiMinusLowBits) [[unlikely]] { //This case is the main problem!!! Should be uncommon so handling it should not use more time??
                    size_t l1 = (lowBits + 1)%ConsolidationFactor;
                    size_t f3 = l1*R/(ConsolidationFactor-1); //Should be right value? Cause l2 cannot be 7 since then l2 = 0 is also sol but l2 = 0 means that l1*R % (C-1) = 0 which then means that l1 = 0, as we set R so that it is *not* 0 mod ConsolidationFactor-1
                    size_t l2 = l1*R - f3*(ConsolidationFactor-1);
                    bucketIndex = f3 + l1*R;
                    whichFrontyardBucket = l2;
                }
                else {
                    whichFrontyardBucket = fbiMinusLowBits % ConsolidationFactor;
                }
                if constexpr (DEBUG) {
                    assert(whichFrontyardBucket < ConsolidationFactorP2);
                    assert(fbiMinusLowBits/ConsolidationFactor < R);
                }
                remainder += whichFrontyardBucket << RemainderBits;
            }
        }

        inline void finishInitCuckooHash(FrontyardQRContainer<NumMiniBuckets> frontQR, bool hashNum, size_t backyardSize, uint64_t R) {
            // just do a simple cuckoo hash. Maybe use murmur hash?
            if (hashNum) {
                // this should do the XOR
                const uint64_t PRIME = 16777619;
                std::uint64_t fbiMinusLowBits = frontQR.bucketIndex / ConsolidationFactor; // f2
                std::uint64_t lowBits = frontQR.bucketIndex % ConsolidationFactor; // l1
                uint64_t stored_hash = fbiMinusLowBits/ConsolidationFactor + lowBits*R;
                // Create a different hash value using rotation and prime multiplication
//                size_t transformed = ((stored_hash << 13) | (stored_hash >> 51)) * PRIME;
                // XOR the transformed value with original hash
               bucketIndex = (stored_hash ^ PRIME) % backyardSize;
		whichFrontyardBucket = (frontQR.bucketIndex % ConsolidationFactor) + ConsolidationFactorP2;
            } else {
                // compute hash using murmur64a
                // todo find out bucket size and hardcode it here for now
                std::uint64_t fbiMinusLowBits = frontQR.bucketIndex / ConsolidationFactor; // f2
                std::uint64_t lowBits = frontQR.bucketIndex % ConsolidationFactor; // l1
                bucketIndex = fbiMinusLowBits/ConsolidationFactor + lowBits*R;
whichFrontyardBucket = fbiMinusLowBits % ConsolidationFactor;
            }
        }

        inline BackyardQRContainer(std::size_t quotient, std::uint64_t remainder, bool hashNum, std::uint64_t R): /*quotient{quotient},*/ realRemainder{remainder}, miniBucketIndex{quotient%NumMiniBuckets}, remainder(remainder) {
            std::uint64_t frontyardBucketIndex = quotient/NumMiniBuckets;
            finishInit(frontyardBucketIndex, hashNum, R);
        }

#ifdef CUCKOO_HASH
        inline BackyardQRContainer(FrontyardQRContainer<NumMiniBuckets> frontQR, bool hashNum, std::uint64_t R, size_t backyardSize): /*quotient{frontQR.quotient},*/ realRemainder{frontQR.remainder}, miniBucketIndex{frontQR.miniBucketIndex}, remainder{frontQR.remainder} {
           finishInitCuckooHash(frontQR, hashNum, backyardSize, R);
        }
#else
        inline BackyardQRContainer(FrontyardQRContainer<NumMiniBuckets> frontQR, bool hashNum, std::uint64_t R): /*quotient{frontQR.quotient},*/ realRemainder{frontQR.remainder}, miniBucketIndex{frontQR.miniBucketIndex}, remainder{frontQR.remainder} {
            // todo this is called
            // an if condition here maybe?
            // or ifdef
            finishInit(frontQR.bucketIndex, hashNum, R);
        }
#endif
    };

}

#endif
