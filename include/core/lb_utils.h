#ifndef LB_UTILS_H
#define LB_UTILS_H

#include <stdint.h>
#include <time.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline void atomic_min(atomic_uint_fast64_t* var, uint64_t val) {
    uint64_t old = atomic_load(var);
    while (old > val && !atomic_compare_exchange_weak(var, &old, val));
}

uint64_t murmur3_64(const void* key, size_t len, uint64_t seed);

#endif