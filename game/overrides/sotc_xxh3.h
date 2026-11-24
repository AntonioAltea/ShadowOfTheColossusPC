// XXH3_64bits (default secret, seed 0), after the xxHash specification
// (Yann Collet, BSD-2); the reference scalar algorithm, inputs of 17 bytes and more.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace sotc_xxh3
{
    inline constexpr uint8_t kSecret[192] = {
        0xb8,0xfe,0x6c,0x39,0x23,0xa4,0x4b,0xbe,0x7c,0x01,0x81,0x2c,0xf7,0x21,0xad,0x1c,0xde,0xd4,0x6d,0xe9,0x83,0x90,0x97,0xdb,0x72,0x40,0xa4,0xa4,0xb7,0xb3,0x67,0x1f,
        0xcb,0x79,0xe6,0x4e,0xcc,0xc0,0xe5,0x78,0x82,0x5a,0xd0,0x7d,0xcc,0xff,0x72,0x21,0xb8,0x08,0x46,0x74,0xf7,0x43,0x24,0x8e,0xe0,0x35,0x90,0xe6,0x81,0x3a,0x26,0x4c,
        0x3c,0x28,0x52,0xbb,0x91,0xc3,0x00,0xcb,0x88,0xd0,0x65,0x8b,0x1b,0x53,0x2e,0xa3,0x71,0x64,0x48,0x97,0xa2,0x0d,0xf9,0x4e,0x38,0x19,0xef,0x46,0xa9,0xde,0xac,0xd8,
        0xa8,0xfa,0x76,0x3f,0xe3,0x9c,0x34,0x3f,0xf9,0xdc,0xbb,0xc7,0xc7,0x0b,0x4f,0x1d,0x8a,0x51,0xe0,0x4b,0xcd,0xb4,0x59,0x31,0xc8,0x9f,0x7e,0xc9,0xd9,0x78,0x73,0x64,
        0xea,0xc5,0xac,0x83,0x34,0xd3,0xeb,0xc3,0xc5,0x81,0xa0,0xff,0xfa,0x13,0x63,0xeb,0x17,0x0d,0xdd,0x51,0xb7,0xf0,0xda,0x49,0xd3,0x16,0x55,0x26,0x29,0xd4,0x68,0x9e,
        0x2b,0x16,0xbe,0x58,0x7d,0x47,0xa1,0xfc,0x8f,0xf8,0xb8,0xd1,0x7a,0xd0,0x31,0xce,0x45,0xcb,0x3a,0x8f,0x95,0x16,0x04,0x28,0xaf,0xd7,0xfb,0xca,0xbb,0x4b,0x40,0x7e};
    inline constexpr uint64_t P32_1 = 0x9E3779B1u, P32_2 = 0x85EBCA77u, P32_3 = 0xC2B2AE3Du;
    inline constexpr uint64_t P64_1 = 0x9E3779B185EBCA87ull, P64_2 = 0xC2B2AE3D27D4EB4Full, P64_3 = 0x165667B19E3779F9ull, P64_4 = 0x85EBCA77C2B2AE63ull, P64_5 = 0x27D4EB2F165667C5ull;
    inline uint64_t r64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
    inline uint64_t fold(uint64_t a, uint64_t b) { const unsigned __int128 m = static_cast<unsigned __int128>(a) * b; return static_cast<uint64_t>(m) ^ static_cast<uint64_t>(m >> 64); }
    inline uint64_t avalanche(uint64_t h) { h ^= h >> 37; h *= 0x165667919E3779F9ull; return h ^ (h >> 32); }
    inline uint64_t mix16(const uint8_t *p, const uint8_t *s) { return fold(r64(p) ^ r64(s), r64(p + 8) ^ r64(s + 8)); }
    inline uint64_t hash(const uint8_t *p, size_t n)
    {
        const uint8_t *s = kSecret;
        if (n < 17)
            return 0;
        if (n <= 128)
        {
            uint64_t acc = n * P64_1;
            if (n > 32)
            {
                if (n > 64)
                {
                    if (n > 96)
                        acc += mix16(p + 48, s + 96) + mix16(p + n - 64, s + 112);
                    acc += mix16(p + 32, s + 64) + mix16(p + n - 48, s + 80);
                }
                acc += mix16(p + 16, s + 32) + mix16(p + n - 32, s + 48);
            }
            acc += mix16(p, s) + mix16(p + n - 16, s + 16);
            return avalanche(acc);
        }
        if (n <= 240)
        {
            uint64_t acc = n * P64_1;
            const size_t rounds = n / 16;
            for (size_t i = 0; i < 8; ++i)
                acc += mix16(p + 16 * i, s + 16 * i);
            acc = avalanche(acc);
            for (size_t i = 8; i < rounds; ++i)
                acc += mix16(p + 16 * i, s + 16 * (i - 8) + 3);
            acc += mix16(p + n - 16, s + 136 - 17);
            return avalanche(acc);
        }
        uint64_t acc[8] = {P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1};
        const auto stripe = [&acc](const uint8_t *d, const uint8_t *k)
        {
            for (int i = 0; i < 8; ++i)
            {
                const uint64_t v = r64(d + 8 * i), x = v ^ r64(k + 8 * i);
                acc[i ^ 1] += v;
                acc[i] += (x & 0xFFFFFFFFu) * (x >> 32);
            }
        };
        const size_t perBlock = (192 - 64) / 8, block = 64 * perBlock, blocks = (n - 1) / block;
        for (size_t b = 0; b < blocks; ++b)
        {
            for (size_t i = 0; i < perBlock; ++i)
                stripe(p + b * block + i * 64, s + i * 8);
            for (int i = 0; i < 8; ++i)
                acc[i] = (acc[i] ^ (acc[i] >> 47) ^ r64(s + 192 - 64 + 8 * i)) * P32_1;
        }
        const size_t stripes = ((n - 1) - block * blocks) / 64;
        for (size_t i = 0; i < stripes; ++i)
            stripe(p + blocks * block + i * 64, s + i * 8);
        stripe(p + n - 64, s + 192 - 64 - 7);
        uint64_t r = n * P64_1;
        for (int i = 0; i < 4; ++i)
            r += fold(acc[2 * i] ^ r64(s + 11 + 16 * i), acc[2 * i + 1] ^ r64(s + 11 + 16 * i + 8));
        return avalanche(r);
    }
}
