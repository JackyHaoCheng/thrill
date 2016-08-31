/*******************************************************************************
 * thrill/common/hash.hpp
 *
 * Part of Project Thrill - http://project-thrill.org
 *
 * Copyright (C) 2016 Lorenz Hübschle-Schneider <lorenz@4z2.de>
 *
 * All rights reserved. Published under the BSD-2 license in the LICENSE file.
 ******************************************************************************/

#pragma once
#ifndef THRILL_COMMON_HASH_HEADER
#define THRILL_COMMON_HASH_HEADER

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

#include "config.hpp"

#if defined(THRILL_HAVE_AVX2)
#include <highwayhash/highway_tree_hash.h>
#elif defined(THRILL_HAVE_SSE4_1)
#include <highwayhash/sse41_highway_tree_hash.h>
#else
#include <highwayhash/scalar_highway_tree_hash.h>
#endif

#ifdef THRILL_HAVE_SSE4_2
#include <smmintrin.h> // crc32 instructions
#endif

namespace thrill {
namespace common {

/*
 * A reinterpret_cast that doesn't violate the strict aliasing rule.  Zero
 * overhead with any reasonable compiler (g++ -O1 or higher, clang++ -O2 or
 * higher)
 */
template<typename To, typename From>
struct alias_cast_t {
    static_assert(sizeof(To) == sizeof(From),
                  "Cannot cast types of different sizes");
    union {
        From * in;
        To * out;
    };
};

template<typename To, typename From>
To & alias_cast(From & raw_data) {
    alias_cast_t<To, From> ac;
    ac.in = &raw_data;
    return *ac.out;
}

template<typename To, typename From>
const To & alias_cast(const From & raw_data) {
    alias_cast_t<const To, const From> ac;
    ac.in = &raw_data;
    return *ac.out;
}


//! This is the Hash128to64 function from Google's cityhash (available under the
//! MIT License).
static inline uint64_t Hash128to64(const uint64_t upper, const uint64_t lower) {
    // Murmur-inspired hashing.
    const uint64_t k = 0x9DDFEA08EB382D69ull;
    uint64_t a = (lower ^ upper) * k;
    a ^= (a >> 47);
    uint64_t b = (upper ^ a) * k;
    b ^= (b >> 47);
    b *= k;
    return b;
}

/*!
 * Returns a uint32_t hash of a uint64_t.
 *
 * This comes from http://www.concentric.net/~ttwang/tech/inthash.htm
 *
 * This hash gives no guarantees on the cryptographic suitability nor the
 * quality of randomness produced, and the mapping may change in the future.
 */
static inline uint32_t hash64To32(uint64_t key) {
    key = (~key) + (key << 18);
    key = key ^ (key >> 31);
    key = key * 21;
    key = key ^ (key >> 11);
    key = key + (key << 6);
    key = key ^ (key >> 22);
    return (uint32_t) key;
}


#ifdef THRILL_HAVE_SSE4_2
/**
 * A CRC32C hasher using SSE4.2 intrinsics
 */
template <typename ValueType>
struct hash_crc32_intel {
    // Hash data with Intel's CRC32C instructions
    // Copyright 2008,2009,2010 Massachusetts Institute of Technology.
    uint32_t hash_bytes(const void* data, size_t length, uint32_t crc = 0xffffffff) {
        const char* p_buf = (const char*) data;
        // The 64-bit crc32 instruction returns a 64-bit value (even though a
        // CRC32 hash has - well - 32 bits. Whatever.
        uint64_t crc_carry = crc;
        for (size_t i = 0; i < length / sizeof(uint64_t); i++) {
            crc_carry = _mm_crc32_u64(crc_carry, *(const uint64_t*) p_buf);
            p_buf += sizeof(uint64_t);
        }
        crc = (uint32_t) crc_carry; // discard the rest
        length &= 7; // remaining length

        // ugly switch statement, faster than a loop-based version
        switch (length) {
        case 7:
            crc = _mm_crc32_u8(crc, *p_buf++);
        case 6:
            crc = _mm_crc32_u16(crc, *(const uint16_t*) p_buf);
            p_buf += 2;
            // case 5 is below: 4 + 1
        case 4:
            crc = _mm_crc32_u32(crc, *(const uint32_t*) p_buf);
            break;
        case 3:
            crc = _mm_crc32_u8(crc, *p_buf++);
        case 2:
            crc = _mm_crc32_u16(crc, *(const uint16_t*) p_buf);
            break;
        case 5:
            crc = _mm_crc32_u32(crc, *(const uint32_t*) p_buf);
            p_buf += 4;
        case 1:
            crc = _mm_crc32_u8(crc, *p_buf);
            break;
        case 0:
            break;
        default: // wat
            assert(false);
        }
        return crc;
    }

    // Hash large or oddly-sized types
    template <typename T = ValueType>
    uint32_t operator()(const T& val, uint32_t crc = 0xffffffff,
                        typename std::enable_if<
                        (sizeof(T) > 8) || (sizeof(T) > 4 && sizeof(T) < 8) || sizeof(T) == 3
                        >::type* = 0) {
        return hash_bytes((const void*)&val, sizeof(T), crc);
    }

    // Specializations for {8,4,2,1}-byte types avoiding unnecessary branches
    template <typename T = ValueType>
    uint32_t operator()(const T& val, uint32_t crc = 0xffffffff,
                        typename std::enable_if<sizeof(T) == 8>::type* = 0) {
        // For Intel reasons, the 64-bit version returns a 64-bit int
        uint64_t res = _mm_crc32_u64(crc, *alias_cast<const uint64_t*>(&val));
        return static_cast<uint32_t>(res);
    }

    template <typename T = ValueType>
    uint32_t operator()(const T& val, uint32_t crc = 0xffffffff,
                        typename std::enable_if<sizeof(T) == 4>::type* = 0) {
        return _mm_crc32_u32(crc, *alias_cast<const uint32_t*>(&val));
    }

    template <typename T = ValueType>
    uint32_t operator()(const T& val, uint32_t crc = 0xffffffff,
                        typename std::enable_if<sizeof(T) == 2>::type* = 0) {
        return _mm_crc32_u16(crc, *alias_cast<const uint16_t*>(&val));
    }

    template <typename T = ValueType>
    uint32_t operator()(const T& val, const uint64_t crc = 0xffffffff,
                        typename std::enable_if<sizeof(T) == 1>::type* = 0) {
        return _mm_crc32_u8(crc, *alias_cast<const uint8_t*>(&val));
    }
};
#endif

// CRC32C, adapted from Evan Jones' BSD-licensed implementation at
// http://www.evanjones.ca/crc32c.html
uint32_t crc32_slicing_by_8(uint32_t crc, const void* data, size_t length);

/**
 * Fallback CRC32C implementation in software.
 */
template <typename ValueType>
struct hash_crc32_fallback {
    uint32_t operator()(const ValueType& val, const uint32_t crc = 0xffffffff) {
        return crc32_slicing_by_8(crc, (const void*)&val, sizeof(ValueType));
    }
};


// If SSE4.2 is available, use the hardware implementation, which is roughly
// four to five times faster than the software fallback (less for small sizes).
#ifdef THRILL_HAVE_SSE4_2
template <typename T>
using hash_crc32 = hash_crc32_intel<T>;
#else
template <typename T>
using hash_crc32 = hash_crc32_fallback<T>;
#endif


template <typename ValueType>
struct hash_highway {
#if defined(THRILL_HAVE_AVX2)
    using state_t = highwayhash::HighwayTreeHashState;
    using key_t = highwayhash::HighwayTreeHashState::Key;
#elif defined(THRILL_HAVE_SSE4_1)
    using state_t = highwayhash::SSE41HighwayTreeHashState;
    using key_t = highwayhash::SSE41HighwayTreeHashState::Key;
#else
    using state_t = highwayhash::ScalarHighwayTreeHashState;
    using key_t = highwayhash::ScalarHighwayTreeHashState::Key;
#endif

    // Default key from highwayhash's sip_hash_main.cc
    hash_highway() {
        key_[0] = 0x0706050403020100ULL;
        key_[1] = 0x0F0E0D0C0B0A0908ULL;
        key_[2] = 0x1716151413121110ULL;
        key_[3] = 0x1F1E1D1C1B1A1918ULL;
    }

    hash_highway(uint64_t key[4]) {
        key_[0] = key[0];
        key_[1] = key[1];
        key_[2] = key[2];
        key_[3] = key[3];
    }

    uint64_t hash_bytes(const char* bytes, const size_t size) {
        return highwayhash::ComputeHash<state_t>(key_, bytes, size);
    }

    uint64_t operator()(const ValueType& val) {
        return hash_bytes((const char*)&val, sizeof(ValueType));
    }

private:
    key_t key_;
};


} // namespace common
} // namespace thrill

#endif // !THRILL_COMMON_HASH_HEADER

/******************************************************************************/
