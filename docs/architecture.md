# 🏗️ UltraBalancer Architecture Overview

## Table of Contents
1. [Core Design Philosophy](#core-design-philosophy)
2. [System Architecture](#system-architecture)
3. [Component Overview](#component-overview)
4. [Data Flow](#data-flow)
5. [Memory Architecture](#memory-architecture)
6. [Threading Model](#threading-model)
7. [C23/C++23 Integration](#c23c23-integration)

## Core Design Philosophy

UltraBalancer is built on three fundamental principles:

1. **Zero-Copy Performance**: Minimize data movement through kernel bypass and splice operations
2. **Lock-Free Concurrency**: Use atomic operations and lock-free data structures wherever possible
3. **NUMA Awareness**: Optimize memory allocation and thread placement for multi-socket systems

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Control Plane (C++23)                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐ │
│  │ Config       │  │ Metrics      │  │ Admin API                │ │
│  │ Manager      │  │ Aggregator   │  │ (REST/gRPC)              │ │
│  └──────────────┘  └──────────────┘  └──────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
                                  │
┌─────────────────────────────────────────────────────────────────────┐
│                          Data Plane (C23)                           │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                     Event Loop (epoll/io_uring)              │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                  │                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐    │
│  │ Connection   │  │ Protocol     │  │ Load Balancer        │    │
│  │ Acceptor     │  │ Handlers     │  │ Engine               │    │
│  └──────────────┘  └──────────────┘  └──────────────────────┘    │
│                                  │                                  │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                    Backend Connection Pool                   │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Component Overview

### Frontend Components

#### Listener Manager
- **Purpose**: Manages all listening sockets
- **Language**: C23
- **Key Features**:
  - SO_REUSEPORT for multi-thread scaling
  - TCP_DEFER_ACCEPT for performance
  - IP_TRANSPARENT for direct server return

#### Connection Acceptor
- **Purpose**: Accept and distribute new connections
- **Language**: C23
- **Key Features**:
  - Lock-free connection distribution
  - CPU affinity pinning
  - Accept4() with flags for optimization

#### Protocol Detector
- **Purpose**: Auto-detect incoming protocol
- **Language**: C23
- **Key Features**:
  - HTTP/1.x, HTTP/2, WebSocket detection
  - SSL/TLS SNI-based routing
  - Protocol upgrade handling

### Core Engine Components

#### Connection Pool (C++23)
```cpp
class ConnectionPool {
    using ConnectionPtr = std::unique_ptr<Connection, ConnectionDeleter>;
    using Clock = std::chrono::steady_clock;

    std::atomic<size_t> active_connections_{0};
    std::pmr::synchronized_pool_resource pool_resource_;
    folly::MPMCQueue<ConnectionPtr> idle_queue_;

public:
    std::expected<ConnectionPtr, Error> acquire() noexcept;
    void release(ConnectionPtr conn) noexcept;
};
```

#### Request Router (C++23)
```cpp
class RequestRouter {
    using RouteTable = std::flat_map<std::string_view, RouteTarget>;
    using Matcher = std::variant<ExactMatcher, RegexMatcher, PrefixMatcher>;

    std::shared_mutex routes_mutex_;
    RouteTable routes_;

public:
    [[nodiscard]] std::optional<RouteTarget> route(const Request& req) const noexcept;
};
```

#### Metrics Aggregator (C++23)
```cpp
class MetricsAggregator {
    using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
    using Histogram = folly::Histogram<int64_t>;

    struct alignas(64) Counter {
        std::atomic<uint64_t> value{0};
    };

    std::array<Counter, MAX_METRICS> counters_;

public:
    void increment(MetricType type, uint64_t delta = 1) noexcept;
    [[nodiscard]] MetricSnapshot snapshot() const noexcept;
};
```

### Backend Components

#### Server Manager
- **Purpose**: Track and manage backend servers
- **Language**: C23
- **Key Features**:
  - Dynamic server addition/removal
  - Weight-based distribution
  - Drain mode support

#### Health Checker
- **Purpose**: Monitor backend health
- **Language**: C23 with C++23 async
- **Key Features**:
  - TCP/HTTP/Custom checks
  - Exponential backoff on failure
  - Circuit breaker integration

## Data Flow

### Request Processing Pipeline

```mermaid
graph LR
    A[Client Request] --> B[Accept]
    B --> C[Protocol Detection]
    C --> D[Request Parsing]
    D --> E[Routing Decision]
    E --> F[Backend Selection]
    F --> G[Connection Pool]
    G --> H[Backend Request]
    H --> I[Response Processing]
    I --> J[Client Response]
```

### Zero-Copy Data Path

1. **Receive Path**:
   - `recv()` with `MSG_PEEK` for protocol detection
   - `splice()` for direct kernel-to-kernel transfer
   - `recvmmsg()` for batch receiving

2. **Send Path**:
   - `sendfile()` for static content
   - `writev()` for gathered writes
   - `sendmmsg()` for batch sending

## Memory Architecture

### NUMA-Aware Allocation

```c
// C23 code with attributes
[[gnu::hot]] [[gnu::aligned(64)]]
static _Thread_local struct {
    struct connection *free_list;
    size_t count;
    int numa_node;
} connection_cache = {
    .numa_node = numa_node_of_cpu(sched_getcpu())
};
```

### Memory Pools

- **Connection Pool**: Per-thread, NUMA-local
- **Buffer Pool**: Tiered (small/medium/large)
- **Session Pool**: Lock-free, per-CPU

## Threading Model

### Thread Types

1. **Acceptor Threads** (1-2)
   - Accept new connections
   - Distribute to worker threads

2. **Worker Threads** (N = CPU cores)
   - Handle request/response
   - CPU-pinned for cache locality

3. **Health Check Thread** (1)
   - Async health monitoring
   - Low priority scheduling

4. **Metrics Thread** (1)
   - Aggregate statistics
   - Export to monitoring systems

### Synchronization

```c
// C23 atomic operations
typedef struct {
    _Atomic(uint64_t) requests;
    _Atomic(uint64_t) bytes_in;
    _Atomic(uint64_t) bytes_out;
    alignas(64) char padding[40];  // Prevent false sharing
} thread_stats_t;
```

## C23/C++23 Integration

### C23 Features Used

```c
// Type-generic programming
#define container_of(ptr, type, member) \
    _Generic((ptr), \
        const typeof(((type *)0)->member) *: \
            ((const type *)((const char *)(ptr) - offsetof(type, member))), \
        default: \
            ((type *)((char *)(ptr) - offsetof(type, member))))

// Bit-precise integers
typedef _BitInt(48) connection_id_t;

// Enhanced enums
enum class lb_algorithm : uint8_t {
    ROUND_ROBIN,
    LEAST_CONN,
    IP_HASH,
    RANDOM,
    [[deprecated]] WEIGHTED_RR
};
```

### C++23 Features Used

```cpp
// Deducing this
template<typename Self>
class Connection {
    auto process(this Self&& self, Request req) -> Response {
        if constexpr (std::is_const_v<std::remove_reference_t<Self>>) {
            return self.process_const(req);
        } else {
            return self.process_mutable(req);
        }
    }
};

// std::expected for error handling
std::expected<Response, Error> handle_request(Request req) noexcept;

// Ranges and views
auto active_backends = backends
    | std::views::filter(&Backend::is_healthy)
    | std::views::take(max_backends);
```

### FFI Bridge

```cpp
// C++23 side
extern "C" {
    [[gnu::visibility("default")]]
    void* create_router() noexcept {
        return new(std::nothrow) RequestRouter{};
    }

    [[gnu::visibility("default")]]
    void destroy_router(void* router) noexcept {
        delete static_cast<RequestRouter*>(router);
    }
}
```

## Performance Optimizations

### CPU Optimizations
- Branch prediction hints via `[[likely]]`/`[[unlikely]]`
- SIMD operations for header parsing
- Prefetching for linked list traversal

### Memory Optimizations
- Huge pages (2MB) for connection pools
- Cache-line aligned structures
- NUMA-local allocation

### Network Optimizations
- TCP_NODELAY for latency-sensitive traffic
- SO_ZEROCOPY for large transfers
- Kernel bypass via DPDK (optional)

## Scalability

### Horizontal Scaling
- Shared-nothing architecture
- Per-CPU data structures
- Lock-free algorithms

### Vertical Scaling
- NUMA awareness
- CPU affinity
- Interrupt steering

## Security Considerations

### DDoS Protection
- SYN cookies
- Rate limiting per IP
- Connection limits

### TLS/SSL
- Hardware acceleration via AES-NI
- Session resumption
- OCSP stapling

## Monitoring & Observability

### Metrics Export
- Prometheus format
- StatsD protocol
- OpenTelemetry traces

### Health Endpoints
- `/health` - Basic health
- `/ready` - Readiness probe
- `/metrics` - Prometheus metrics

## Future Architecture Plans

1. **io_uring Integration** - Linux 5.1+ kernel interface
2. **eBPF Programs** - Kernel-level filtering
3. **QUIC/HTTP3** - Next-gen protocol support
4. **WebAssembly Plugins** - Custom logic injection
5. **Distributed Mode** - Multi-node clustering