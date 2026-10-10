// random.h - the operating system's random generator (passwords) and a seedable generator (names)

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Fills buf with n random bytes from the operating system (/dev/urandom, BCryptGenRandom), read in blocks.
// Used for passwords; there is no fallback to a weaker source.
bool FillRandom(unsigned char *buf, size_t n, std::string &err);

// How often the operating system was read so far (it is read in blocks of 64 KB, not once per number).
size_t RandomOsReads();

// Uniform number 0..n-1 from the operating system's generator (no modulo bias). n must be > 0.
bool SecureBelow(size_t n, size_t &out, std::string &err);

// A small seedable generator (splitmix64): the same seed gives the same sequence on every platform
// and compiler. For the name lists only, never for passwords.
class SeededRandom
{
public:
    explicit SeededRandom(uint64_t seed) : state_(seed) {}

    uint64_t Next();

    // Uniform number 0..n-1 (no modulo bias). n must be > 0.
    size_t Below(size_t n);

private:
    uint64_t state_;
};
