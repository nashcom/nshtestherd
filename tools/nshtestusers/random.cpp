// random.cpp - see random.h

#include "random.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace
{

// Reads n bytes from the operating system.
bool OsRandom(unsigned char *buf, size_t n, std::string &err)
{
    if (0 != BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))
    {
        err = "BCryptGenRandom failed";
        return false;
    }

    return true;
}

} // namespace

#else

#include <cstdio>

namespace
{

// Reads n bytes from the operating system.
bool OsRandom(unsigned char *buf, size_t n, std::string &err)
{
    FILE *f = std::fopen("/dev/urandom", "rb");

    if (NULL == f)
    {
        err = "cannot open /dev/urandom";
        return false;
    }

    size_t got = std::fread(buf, 1, n, f);
    std::fclose(f);

    if (got != n)
    {
        err = "cannot read /dev/urandom";
        return false;
    }

    return true;
}

} // namespace

#endif

namespace
{

// The operating system is read in blocks, not once per number: a list of 100000 random passwords needs
// millions of random numbers, and one open/read/close of /dev/urandom each took minutes. Used bytes are
// wiped from the block. (One thread: this tool does not need more.)
const size_t POOL_SIZE = 65536;

unsigned char g_pool[POOL_SIZE];
size_t        g_poolPos = POOL_SIZE; // everything used: the first call fills the block
size_t        g_osReads = 0;

} // namespace

bool FillRandom(unsigned char *buf, size_t n, std::string &err)
{
    while (n > 0)
    {
        if (g_poolPos >= POOL_SIZE)
        {
            if (!OsRandom(g_pool, POOL_SIZE, err))
                return false;

            g_poolPos = 0;
            g_osReads++;
        }

        size_t take = std::min(n, POOL_SIZE - g_poolPos);

        std::memcpy(buf, g_pool + g_poolPos, take);
        std::memset(g_pool + g_poolPos, 0, take);

        g_poolPos += take;
        buf += take;
        n -= take;
    }

    return true;
}

size_t RandomOsReads()
{
    return g_osReads;
}

bool SecureBelow(size_t n, size_t &out, std::string &err)
{
    // Rejection sampling: numbers above the largest multiple of n are thrown away.
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % (uint64_t)n);

    for (;;)
    {
        unsigned char b[8];

        if (!FillRandom(b, sizeof(b), err))
            return false;

        uint64_t v = 0;

        for (int i = 0; i < 8; i++)
            v = (v << 8) | b[i];

        if (v < limit)
        {
            out = (size_t)(v % (uint64_t)n);
            return true;
        }
    }
}

uint64_t SeededRandom::Next()
{
    state_ += 0x9E3779B97F4A7C15ULL;

    uint64_t z = state_;
    z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z          = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;

    return z ^ (z >> 31);
}

size_t SeededRandom::Below(size_t n)
{
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % (uint64_t)n);

    for (;;)
    {
        uint64_t v = Next();

        if (v < limit)
            return (size_t)(v % (uint64_t)n);
    }
}
