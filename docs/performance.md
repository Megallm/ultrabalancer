# 💡 UltraBalancer Performance Tuning Guide

## Table of Contents
1. [System Tuning](#system-tuning)
2. [Kernel Optimization](#kernel-optimization)
3. [Network Tuning](#network-tuning)
4. [CPU Optimization](#cpu-optimization)
5. [Memory Optimization](#memory-optimization)
6. [Application Tuning](#application-tuning)
7. [C23/C++23 Optimizations](#c23c23-optimizations)
8. [Benchmarking](#benchmarking)

## System Tuning

### OS Limits Configuration

```bash
# /etc/security/limits.conf
* soft nofile 1000000
* hard nofile 1000000
* soft nproc 65535
* hard nproc 65535
ultrabalancer soft memlock unlimited
ultrabalancer hard memlock unlimited
```

### Sysctl Settings

```bash
# /etc/sysctl.d/99-ultrabalancer.conf

# Network
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65536
net.core.rmem_default = 134217728
net.core.wmem_default = 134217728
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.core.optmem_max = 134217728

# TCP
net.ipv4.tcp_syncookies = 1
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 10
net.ipv4.tcp_keepalive_time = 120
net.ipv4.tcp_keepalive_intvl = 30
net.ipv4.tcp_keepalive_probes = 3
net.ipv4.tcp_max_syn_backlog = 65536
net.ipv4.tcp_max_tw_buckets = 2000000
net.ipv4.tcp_timestamps = 1
net.ipv4.tcp_slow_start_after_idle = 0
net.ipv4.tcp_mtu_probing = 1

# TCP Buffer Sizes (min, default, max)
net.ipv4.tcp_rmem = 4096 87380 134217728
net.ipv4.tcp_wmem = 4096 65536 134217728

# TCP Congestion Control
net.ipv4.tcp_congestion_control = bbr
net.core.default_qdisc = fq

# Connection Tracking (if using iptables)
net.netfilter.nf_conntrack_max = 2000000
net.netfilter.nf_conntrack_buckets = 500000
net.netfilter.nf_conntrack_tcp_timeout_established = 86400

# IP
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.ip_nonlocal_bind = 1

# Memory
vm.swappiness = 0
vm.max_map_count = 262144
```

## Kernel Optimization

### CPU Affinity

```yaml
# ultrabalancer.yaml
global:
  cpu-map:
    # Process 1 on CPU 0-3
    1: 0-3
    # Process 2 on CPU 4-7
    2: 4-7

  # Per-thread affinity
  thread-groups:
    - threads: 1-4
      cpus: 0-3
    - threads: 5-8
      cpus: 4-7
```

### IRQ Affinity

```bash
#!/bin/bash
# Set IRQ affinity for network interfaces

IFACE=eth0
CPUS=(4 5 6 7)  # Dedicate CPUs for network IRQs

# Find IRQs for network interface
IRQS=$(grep $IFACE /proc/interrupts | awk '{print $1}' | tr -d ':')

i=0
for IRQ in $IRQS; do
    CPU=${CPUS[$((i % ${#CPUS[@]}))]}
    echo $CPU > /proc/irq/$IRQ/smp_affinity_list
    i=$((i + 1))
done

# Disable irqbalance for these CPUs
systemctl stop irqbalance
```

### NUMA Optimization

```c
// C23 NUMA-aware memory allocation
#include <numa.h>

typedef struct {
    void *data;
    size_t size;
    int numa_node;
} numa_buffer_t;

numa_buffer_t* numa_buffer_alloc(size_t size) {
    numa_buffer_t *buf = malloc(sizeof(numa_buffer_t));

    // Get current CPU's NUMA node
    int cpu = sched_getcpu();
    buf->numa_node = numa_node_of_cpu(cpu);

    // Allocate on specific NUMA node
    buf->data = numa_alloc_onnode(size, buf->numa_node);
    buf->size = size;

    // Bind thread to NUMA node
    struct bitmask *mask = numa_allocate_nodemask();
    numa_bitmask_setbit(mask, buf->numa_node);
    numa_bind(mask);
    numa_free_nodemask(mask);

    return buf;
}
```

## Network Tuning

### Network Interface Optimization

```bash
# Increase ring buffer sizes
ethtool -G eth0 rx 4096 tx 4096

# Enable offloading features
ethtool -K eth0 gso on gro on tso on lro on

# Increase interrupt coalescence
ethtool -C eth0 rx-usecs 100 tx-usecs 100

# RSS (Receive Side Scaling)
ethtool -L eth0 combined 8

# XPS (Transmit Packet Steering)
echo f > /sys/class/net/eth0/queues/tx-0/xps_cpus
```

### TCP Optimization

```yaml
# ultrabalancer.yaml
global:
  # TCP Options
  tune.tcp.maxseg: 1460
  tune.tcp.nodelay: true
  tune.tcp.quickack: true
  tune.tcp.defer-accept: true

  # Socket Options
  tune.socket.rcvbuf: 2097152
  tune.socket.sndbuf: 2097152

  # Keep-alive
  tune.tcp.keepalive: true
  tune.tcp.keepidle: 120
  tune.tcp.keepintvl: 30
  tune.tcp.keepcnt: 3
```

### Zero-Copy Networking (C23)

```c
// Using splice for zero-copy
ssize_t zero_copy_transfer(int in_fd, int out_fd, size_t len) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    ssize_t total = 0;

    while (len > 0) {
        // Splice from socket to pipe
        ssize_t n = splice(in_fd, NULL, pipefd[1], NULL,
                          len, SPLICE_F_MOVE | SPLICE_F_MORE);
        if (n <= 0) break;

        // Splice from pipe to socket
        ssize_t m = splice(pipefd[0], NULL, out_fd, NULL,
                          n, SPLICE_F_MOVE | SPLICE_F_MORE);
        if (m < n) break;

        total += m;
        len -= m;
    }

    close(pipefd[0]);
    close(pipefd[1]);
    return total;
}
```

## CPU Optimization

### Compiler Optimizations

```makefile
# C23 Optimizations
CFLAGS += -O3 -march=native -mtune=native
CFLAGS += -flto=auto -fuse-linker-plugin
CFLAGS += -fomit-frame-pointer
CFLAGS += -funroll-loops
CFLAGS += -fprefetch-loop-arrays
CFLAGS += -ffast-math
CFLAGS += -fno-exceptions -fno-rtti

# C++23 Optimizations
CXXFLAGS += -O3 -march=native -mtune=native
CXXFLAGS += -flto=auto -fuse-linker-plugin
CXXFLAGS += -fomit-frame-pointer
CXXFLAGS += -fcoroutines
CXXFLAGS += -std=c++23
```

### CPU Features Detection (C++23)

```cpp
#include <cpuid.h>
#include <immintrin.h>

class CPUFeatures {
    static inline const auto features = []() {
        struct Features {
            bool sse42;
            bool avx;
            bool avx2;
            bool avx512;
            bool bmi2;
            bool popcnt;
        } f{};

        unsigned int eax, ebx, ecx, edx;

        __get_cpuid(1, &eax, &ebx, &ecx, &edx);
        f.sse42 = ecx & bit_SSE4_2;
        f.popcnt = ecx & bit_POPCNT;

        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        f.avx2 = ebx & bit_AVX2;
        f.bmi2 = ebx & bit_BMI2;

        return f;
    }();

public:
    template<typename T>
    static auto optimize_function() {
        if (features.avx512) {
            return avx512_implementation<T>;
        } else if (features.avx2) {
            return avx2_implementation<T>;
        } else if (features.sse42) {
            return sse42_implementation<T>;
        } else {
            return generic_implementation<T>;
        }
    }
};
```

### Lock-Free Data Structures (C++23)

```cpp
template<typename T, size_t Size>
class alignas(64) LockFreeQueue {
    struct alignas(64) Node {
        std::atomic<T*> data{nullptr};
        std::atomic<size_t> version{0};
    };

    std::array<Node, Size> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool enqueue(T item) noexcept {
        size_t tail = tail_.load(std::memory_order_relaxed);

        for (;;) {
            size_t next = (tail + 1) % Size;

            if (next == head_.load(std::memory_order_acquire)) {
                return false;  // Queue full
            }

            if (tail_.compare_exchange_weak(tail, next,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
                buffer_[tail].data.store(new T(std::move(item)),
                                        std::memory_order_release);
                return true;
            }
        }
    }

    std::optional<T> dequeue() noexcept {
        size_t head = head_.load(std::memory_order_relaxed);

        for (;;) {
            if (head == tail_.load(std::memory_order_acquire)) {
                return std::nullopt;  // Queue empty
            }

            T* data = buffer_[head].data.exchange(nullptr,
                                                 std::memory_order_acquire);

            if (data) {
                head_.store((head + 1) % Size, std::memory_order_release);
                T item = std::move(*data);
                delete data;
                return item;
            }
        }
    }
};
```

## Memory Optimization

### Huge Pages

```bash
# Enable transparent huge pages
echo always > /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/defrag

# Reserve huge pages
echo 1024 > /proc/sys/vm/nr_hugepages

# Mount hugetlbfs
mkdir -p /mnt/hugepages
mount -t hugetlbfs none /mnt/hugepages
```

### Memory Pool Implementation (C23)

```c
typedef struct memory_pool {
    void *base;
    size_t size;
    size_t used;
    _Atomic(size_t) allocated;

    // Free list for recycling
    struct free_node {
        struct free_node *next;
        size_t size;
    } *free_list;

    pthread_spinlock_t lock;
} memory_pool_t;

void* pool_alloc(memory_pool_t *pool, size_t size) {
    // Align to cache line
    size = (size + 63) & ~63;

    // Try free list first
    pthread_spin_lock(&pool->lock);
    struct free_node **prev = &pool->free_list;
    struct free_node *node = pool->free_list;

    while (node) {
        if (node->size >= size) {
            *prev = node->next;
            pthread_spin_unlock(&pool->lock);
            return node;
        }
        prev = &node->next;
        node = node->next;
    }
    pthread_spin_unlock(&pool->lock);

    // Allocate from pool
    size_t offset = atomic_fetch_add(&pool->allocated, size);
    if (offset + size > pool->size) {
        atomic_fetch_sub(&pool->allocated, size);
        return NULL;
    }

    return (char*)pool->base + offset;
}
```

### Memory Allocator Selection

```c
// Use jemalloc for better multi-threaded performance
// Link with -ljemalloc

// Or use tcmalloc
// Link with -ltcmalloc

// Custom allocator with NUMA awareness
void* numa_malloc(size_t size) {
    int node = numa_preferred();
    void *ptr = numa_alloc_onnode(size, node);

    if (!ptr) {
        // Fallback to regular malloc
        ptr = malloc(size);
    }

    return ptr;
}
```

## Application Tuning

### Connection Pool Tuning

```yaml
connection_pools:
  default:
    # Pool sizing
    min_size: 100
    max_size: 1000
    max_idle: 200

    # Timeouts
    acquire_timeout: 100ms
    idle_timeout: 300s
    max_lifetime: 3600s

    # Validation
    test_on_borrow: false  # Performance impact
    test_while_idle: true
    validation_interval: 30s

    # Pre-warming
    prewarm: true
    prewarm_size: 100
```

### Buffer Tuning

```yaml
global:
  # Buffer sizes
  tune.bufsize: 32768       # Larger for throughput
  tune.maxrewrite: 16384     # URL rewriting
  tune.headerlen: 16384      # Max header size

  # Pipeline depth
  tune.http.maxhdr: 128      # Max headers
  tune.http.cookielen: 4096  # Max cookie size

  # Compression
  tune.comp.maxlevel: 9      # Max compression
  tune.zlib.memlevel: 9      # Zlib memory
  tune.zlib.windowsize: 15   # Zlib window
```

### Thread Pool Configuration (C++23)

```cpp
class ThreadPool {
    struct Config {
        size_t min_threads = std::thread::hardware_concurrency();
        size_t max_threads = min_threads * 4;
        size_t queue_size = 10000;
        std::chrono::milliseconds keep_alive{60000};
        bool pin_threads = true;
    };

    Config config_;
    std::vector<std::jthread> threads_;
    LockFreeQueue<std::function<void()>> tasks_;

public:
    void optimize_for_workload(WorkloadType type) {
        switch (type) {
            case WorkloadType::CPU_INTENSIVE:
                config_.min_threads = std::thread::hardware_concurrency();
                config_.max_threads = config_.min_threads;
                config_.pin_threads = true;
                break;

            case WorkloadType::IO_INTENSIVE:
                config_.min_threads = std::thread::hardware_concurrency() * 2;
                config_.max_threads = config_.min_threads * 4;
                config_.pin_threads = false;
                break;

            case WorkloadType::MIXED:
                config_.min_threads = std::thread::hardware_concurrency();
                config_.max_threads = config_.min_threads * 2;
                config_.pin_threads = true;
                break;
        }

        resize_pool();
    }
};
```

## C23/C++23 Optimizations

### Branch Prediction Hints (C23)

```c
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

server_t* get_server_optimized(backend_t *backend) {
    // Fast path - servers available
    if (likely(backend->servers_up > 0)) {
        return select_server(backend);
    }

    // Slow path - no servers
    if (unlikely(backend->servers_up == 0)) {
        log_error("No servers available");
        return NULL;
    }
}
```

### SIMD Optimization (C++23)

```cpp
#include <immintrin.h>

// AVX2 optimized memcmp
bool avx2_memcmp(const void* a, const void* b, size_t n) {
    const __m256i* pa = static_cast<const __m256i*>(a);
    const __m256i* pb = static_cast<const __m256i*>(b);

    size_t chunks = n / 32;
    for (size_t i = 0; i < chunks; ++i) {
        __m256i va = _mm256_loadu_si256(pa + i);
        __m256i vb = _mm256_loadu_si256(pb + i);
        __m256i vcmp = _mm256_cmpeq_epi8(va, vb);

        if (_mm256_movemask_epi8(vcmp) != 0xFFFFFFFF) {
            return false;
        }
    }

    // Handle remainder
    size_t remainder = n % 32;
    if (remainder) {
        return memcmp((char*)a + chunks * 32,
                     (char*)b + chunks * 32,
                     remainder) == 0;
    }

    return true;
}
```

### Coroutine Optimization (C++23)

```cpp
// Optimized async I/O with coroutines
template<typename T>
class AsyncBuffer {
    struct promise_type {
        T value;
        std::coroutine_handle<> waiter;

        auto initial_suspend() noexcept {
            return std::suspend_never{};
        }

        auto final_suspend() noexcept {
            struct Awaiter {
                bool await_ready() noexcept { return false; }

                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    if (h.promise().waiter) {
                        h.promise().waiter.resume();
                    }
                }

                void await_resume() noexcept {}
            };
            return Awaiter{};
        }
    };
};
```

## Benchmarking

### Performance Testing Tools

```bash
# HTTP benchmarking
wrk -t12 -c400 -d30s --latency http://localhost/

# Advanced HTTP testing
h2load -n100000 -c100 -t10 --h1 http://localhost/

# TCP connection testing
tcpkali -c 10000 -r 10000 -T 30s localhost:80

# Network performance
iperf3 -c localhost -t 30 -P 10
```

### Custom Benchmark (C++23)

```cpp
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>

class Benchmark {
    using Clock = std::chrono::high_resolution_clock;
    using Duration = std::chrono::nanoseconds;

    struct Result {
        Duration min, max, mean, median, p95, p99;
        size_t iterations;
    };

public:
    template<typename Func>
    static Result run(Func&& f, size_t iterations = 1000000) {
        std::vector<Duration> measurements;
        measurements.reserve(iterations);

        // Warmup
        for (size_t i = 0; i < 1000; ++i) {
            f();
        }

        // Actual benchmark
        for (size_t i = 0; i < iterations; ++i) {
            auto start = Clock::now();
            f();
            auto end = Clock::now();
            measurements.push_back(end - start);
        }

        // Calculate statistics
        std::sort(measurements.begin(), measurements.end());

        Result r;
        r.iterations = iterations;
        r.min = measurements.front();
        r.max = measurements.back();
        r.mean = std::accumulate(measurements.begin(),
                               measurements.end(),
                               Duration{}) / iterations;
        r.median = measurements[iterations / 2];
        r.p95 = measurements[size_t(iterations * 0.95)];
        r.p99 = measurements[size_t(iterations * 0.99)];

        return r;
    }
};
```

### Monitoring Script

```bash
#!/bin/bash

# Real-time performance monitoring
watch -n 1 '
echo "=== CPU ==="
mpstat 1 1

echo -e "\n=== Memory ==="
free -h

echo -e "\n=== Network ==="
ss -s

echo -e "\n=== UltraBalancer Stats ==="
curl -s http://localhost:8080/stats | jq .

echo -e "\n=== Top Connections ==="
ss -tan | awk "{print \$4}" | cut -d: -f1 | sort | uniq -c | sort -rn | head -5
'
```

## Performance Targets

### Latency Goals
- P50: < 1ms
- P95: < 5ms
- P99: < 10ms
- P99.9: < 50ms

### Throughput Goals
- Small requests (< 1KB): 1M+ RPS
- Medium requests (1-10KB): 500K+ RPS
- Large requests (> 10KB): 100K+ RPS

### Resource Usage
- CPU: < 50% at peak load
- Memory: < 100MB per 10K connections
- Network: Line-rate performance

## Troubleshooting Performance

### Common Issues

1. **High CPU Usage**
   - Check for busy loops
   - Profile with `perf top`
   - Review lock contention

2. **Memory Leaks**
   - Use AddressSanitizer
   - Monitor with `valgrind`
   - Check pool statistics

3. **Network Bottlenecks**
   - Verify NIC settings
   - Check interrupt distribution
   - Monitor with `ethtool -S`

4. **Latency Spikes**
   - Check GC pauses (if using managed memory)
   - Review disk I/O
   - Monitor context switches