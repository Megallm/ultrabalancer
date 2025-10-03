# 🎯 Load Balancing Algorithms

## Table of Contents
1. [Algorithm Overview](#algorithm-overview)
2. [Round Robin](#round-robin)
3. [Weighted Round Robin](#weighted-round-robin)
4. [Least Connections](#least-connections)
5. [Weighted Least Connections](#weighted-least-connections)
6. [Source IP Hash](#source-ip-hash)
7. [URI Hash](#uri-hash)
8. [Random](#random)
9. [Custom Algorithms](#custom-algorithms)
10. [Algorithm Selection Guide](#algorithm-selection-guide)

## Algorithm Overview

UltraBalancer implements multiple load balancing algorithms optimized for different scenarios:

| Algorithm | Use Case | Pros | Cons |
|-----------|----------|------|------|
| Round Robin | General purpose | Simple, fair distribution | Ignores server load |
| Weighted Round Robin | Variable capacity servers | Accounts for server capacity | Static weights |
| Least Connections | Long-lived connections | Balances active load | Requires connection tracking |
| Source IP Hash | Session persistence | Client affinity | Uneven distribution possible |
| URI Hash | Content-based routing | Cache efficiency | Limited to HTTP |
| Random | Simple distribution | Low overhead | Unpredictable |

## Round Robin

### Description
Distributes requests sequentially across all available servers.

### Implementation (C23)
```c
typedef struct {
    _Atomic(uint32_t) current_index;
    server_t **servers;
    uint32_t server_count;
} roundrobin_state_t;

server_t* select_roundrobin(roundrobin_state_t *state) {
    uint32_t idx = atomic_fetch_add(&state->current_index, 1);
    return state->servers[idx % state->server_count];
}
```

### Configuration
```yaml
backend servers:
  balance: roundrobin
```

### Characteristics
- **Time Complexity**: O(1)
- **Space Complexity**: O(1)
- **Distribution**: Uniform
- **Session Persistence**: No
- **Health Check Aware**: Yes

## Weighted Round Robin

### Description
Distributes requests based on server weights, allowing for capacity-based distribution.

### Implementation (C23)
```c
typedef struct {
    server_t *server;
    int weight;
    int current_weight;
    int effective_weight;
} weighted_server_t;

server_t* select_weighted_roundrobin(weighted_server_t *servers, int count) {
    int total = 0;
    weighted_server_t *selected = NULL;

    for (int i = 0; i < count; i++) {
        servers[i].current_weight += servers[i].effective_weight;
        total += servers[i].effective_weight;

        if (!selected || servers[i].current_weight > selected->current_weight) {
            selected = &servers[i];
        }
    }

    if (selected) {
        selected->current_weight -= total;
        return selected->server;
    }

    return NULL;
}
```

### Configuration
```yaml
backend servers:
  balance: roundrobin

  server:
    - web1 192.168.1.10:8080 weight=100
    - web2 192.168.1.11:8080 weight=200
    - web3 192.168.1.12:8080 weight=50
```

### Smooth Weighted Round Robin (Nginx-style)
Ensures smooth distribution without clustering:

```c
// Smooth distribution algorithm
// For weights [5, 1, 1], produces: a,a,b,a,c,a,a
// Instead of: a,a,a,a,a,b,c
```

## Least Connections

### Description
Routes requests to the server with the fewest active connections.

### Implementation (C++23)
```cpp
class LeastConnections {
    struct ServerStats {
        std::atomic<int32_t> active_connections{0};
        server_t* server;

        bool operator<(const ServerStats& other) const {
            return active_connections.load() < other.active_connections.load();
        }
    };

    std::vector<ServerStats> servers_;

public:
    server_t* select() {
        return std::ranges::min_element(servers_)->server;
    }

    void on_connect(server_t* srv) {
        auto it = std::ranges::find(servers_, srv,
                                   &ServerStats::server);
        if (it != servers_.end()) {
            it->active_connections.fetch_add(1);
        }
    }

    void on_disconnect(server_t* srv) {
        auto it = std::ranges::find(servers_, srv,
                                   &ServerStats::server);
        if (it != servers_.end()) {
            it->active_connections.fetch_sub(1);
        }
    }
};
```

### Configuration
```yaml
backend servers:
  balance: leastconn

  # Optional: consider weight
  option: lb-leastconn-weighted
```

### Characteristics
- **Time Complexity**: O(n)
- **Space Complexity**: O(n)
- **Distribution**: Dynamic
- **Session Persistence**: No
- **Best For**: Long-lived connections

## Weighted Least Connections

### Description
Combines least connections with server weights.

### Formula
```
score = active_connections / weight
```

### Implementation (C23)
```c
server_t* select_weighted_leastconn(server_t **servers, int count) {
    server_t *best = NULL;
    float min_score = FLT_MAX;

    for (int i = 0; i < count; i++) {
        if (!server_is_available(servers[i]))
            continue;

        float score = (float)servers[i]->cur_conns / servers[i]->weight;

        if (score < min_score) {
            min_score = score;
            best = servers[i];
        }
    }

    return best;
}
```

## Source IP Hash

### Description
Routes requests from the same client IP to the same server.

### Implementation (C23)
```c
server_t* select_source_hash(uint32_t client_ip, server_t **servers, int count) {
    // Use consistent hashing for better distribution
    uint32_t hash = xxhash32(&client_ip, sizeof(client_ip), 0);
    return servers[hash % count];
}
```

### Consistent Hashing (C++23)
```cpp
class ConsistentHash {
    static constexpr int VIRTUAL_NODES = 150;
    std::map<size_t, server_t*> ring_;

public:
    void add_server(server_t* srv) {
        for (int i = 0; i < VIRTUAL_NODES; i++) {
            std::string key = fmt::format("{}#{}", srv->name, i);
            size_t hash = std::hash<std::string>{}(key);
            ring_[hash] = srv;
        }
    }

    server_t* get_server(uint32_t client_ip) {
        if (ring_.empty()) return nullptr;

        size_t hash = std::hash<uint32_t>{}(client_ip);
        auto it = ring_.lower_bound(hash);

        if (it == ring_.end()) {
            it = ring_.begin();
        }

        return it->second;
    }
};
```

### Configuration
```yaml
backend servers:
  balance: source

  # Optional: use consistent hashing
  hash-type: consistent

  # Optional: hash on different fields
  hash-on: hdr(X-Forwarded-For)
```

## URI Hash

### Description
Routes requests based on URI, ensuring same URI always goes to same server.

### Implementation (C23)
```c
server_t* select_uri_hash(const char *uri, server_t **servers, int count) {
    uint32_t hash = 0;

    // DJB2 hash algorithm
    while (*uri) {
        hash = ((hash << 5) + hash) + *uri++;
    }

    return servers[hash % count];
}
```

### Advanced URI Hashing (C++23)
```cpp
class URIHashBalancer {
    enum class HashType {
        FULL_URI,
        PATH_ONLY,
        QUERY_PARAMS,
        CUSTOM_PATTERN
    };

    HashType hash_type_;
    std::regex pattern_;

public:
    server_t* select(std::string_view uri) {
        std::string hash_input;

        switch (hash_type_) {
            case HashType::PATH_ONLY:
                hash_input = extract_path(uri);
                break;

            case HashType::QUERY_PARAMS:
                hash_input = extract_query(uri);
                break;

            case HashType::CUSTOM_PATTERN:
                if (std::smatch m; std::regex_search(uri.begin(), uri.end(), m, pattern_)) {
                    hash_input = m[1];
                }
                break;

            default:
                hash_input = uri;
        }

        return consistent_hash(hash_input);
    }
};
```

### Configuration
```yaml
backend servers:
  balance: uri

  # Hash parameters
  hash-type: map-based
  hash-on: path

  # Optional: only hash part of URI
  uri-hash-len: 32
  uri-hash-depth: 3  # /a/b/c -> only hash first 3 levels
```

## Random

### Description
Randomly selects an available server.

### Implementation (C23)
```c
server_t* select_random(server_t **servers, int count) {
    // Use thread-local random for performance
    static _Thread_local struct {
        uint64_t state;
        bool initialized;
    } rng = {0};

    if (!rng.initialized) {
        rng.state = time(NULL) ^ pthread_self();
        rng.initialized = true;
    }

    // xorshift64
    rng.state ^= rng.state << 13;
    rng.state ^= rng.state >> 7;
    rng.state ^= rng.state << 17;

    return servers[rng.state % count];
}
```

### Weighted Random (C++23)
```cpp
class WeightedRandom {
    std::discrete_distribution<> distribution_;
    std::mt19937 generator_{std::random_device{}()};

public:
    WeightedRandom(const std::vector<int>& weights)
        : distribution_(weights.begin(), weights.end()) {}

    size_t select() {
        return distribution_(generator_);
    }
};
```

## Custom Algorithms

### Plugin Interface (C23)
```c
typedef struct {
    const char *name;
    void* (*init)(const char *params);
    server_t* (*select)(void *state, server_t **servers, int count, void *context);
    void (*destroy)(void *state);
    void (*on_connect)(void *state, server_t *srv);
    void (*on_disconnect)(void *state, server_t *srv);
} lb_algorithm_plugin_t;

// Register custom algorithm
int register_lb_algorithm(const lb_algorithm_plugin_t *plugin);
```

### Example: Adaptive Load Balancing (C++23)
```cpp
class AdaptiveLoadBalancer {
    struct ServerMetrics {
        std::atomic<double> response_time_ema{0.0};
        std::atomic<uint32_t> error_rate{0};
        std::atomic<uint32_t> success_rate{0};

        double score() const {
            double rt = response_time_ema.load();
            double er = error_rate.load();
            double sr = success_rate.load();

            // Lower is better
            return (rt * 0.5) + (er * 100) - (sr * 0.1);
        }
    };

    std::unordered_map<server_t*, ServerMetrics> metrics_;

public:
    server_t* select() {
        return std::ranges::min_element(metrics_,
            [](const auto& a, const auto& b) {
                return a.second.score() < b.second.score();
            })->first;
    }

    void update_metrics(server_t* srv, double response_time, bool success) {
        auto& m = metrics_[srv];

        // Exponential moving average
        double alpha = 0.2;
        double current = m.response_time_ema.load();
        m.response_time_ema = current * (1 - alpha) + response_time * alpha;

        if (success) {
            m.success_rate.fetch_add(1);
        } else {
            m.error_rate.fetch_add(1);
        }
    }
};
```

### Machine Learning Based (C++23)
```cpp
class MLLoadBalancer {
    struct Features {
        float cpu_usage;
        float memory_usage;
        float response_time_p99;
        float error_rate;
        float active_connections;
    };

    std::unique_ptr<TensorFlowModel> model_;

public:
    server_t* select(const Request& req) {
        std::vector<Features> server_features;

        for (const auto& srv : servers_) {
            server_features.push_back(get_features(srv));
        }

        // Predict best server using neural network
        auto predictions = model_->predict(server_features);
        size_t best_idx = std::distance(predictions.begin(),
                                       std::max_element(predictions.begin(),
                                                      predictions.end()));

        return servers_[best_idx];
    }
};
```

## Algorithm Selection Guide

### Decision Tree

```
Start
│
├─ Need session persistence?
│  ├─ Yes → Source IP Hash or Cookie-based
│  └─ No → Continue
│
├─ Servers have different capacities?
│  ├─ Yes → Weighted algorithms
│  └─ No → Continue
│
├─ Connection duration?
│  ├─ Long-lived → Least Connections
│  ├─ Short-lived → Round Robin
│  └─ Mixed → Adaptive
│
├─ Traffic pattern?
│  ├─ Uniform → Round Robin
│  ├─ Bursty → Least Connections
│  └─ Unknown → Random
│
└─ Cache efficiency important?
   ├─ Yes → URI Hash
   └─ No → Any algorithm
```

### Performance Comparison

| Algorithm | Throughput | Latency | CPU Usage | Memory |
|-----------|------------|---------|-----------|---------|
| Round Robin | ★★★★★ | ★★★★☆ | ★★★★★ | ★★★★★ |
| Weighted RR | ★★★★☆ | ★★★★☆ | ★★★★☆ | ★★★★☆ |
| Least Conn | ★★★☆☆ | ★★★★★ | ★★★☆☆ | ★★★☆☆ |
| Source Hash | ★★★★☆ | ★★★☆☆ | ★★★★☆ | ★★★★☆ |
| URI Hash | ★★★★☆ | ★★★☆☆ | ★★★★☆ | ★★★☆☆ |
| Random | ★★★★★ | ★★★☆☆ | ★★★★★ | ★★★★★ |

### Recommendations by Use Case

#### Web Applications
- **Static Content**: URI Hash (cache efficiency)
- **Dynamic Content**: Least Connections
- **Mixed**: Weighted Round Robin

#### API Services
- **Stateless**: Round Robin
- **Stateful**: Source IP Hash
- **Rate Limited**: Least Connections

#### WebSocket/Long Polling
- **Primary**: Least Connections
- **Fallback**: Weighted Least Connections

#### Microservices
- **Service Mesh**: Adaptive/ML-based
- **Simple**: Random with circuit breaker

#### Database Connections
- **Read Replicas**: Weighted Round Robin
- **Connection Pooling**: Least Connections

## Algorithm Tuning

### Round Robin Optimization
```yaml
backend servers:
  balance: roundrobin

  # Skip failed servers quickly
  option: allbackups
  option: redispatch
  retries: 3
```

### Least Connections Tuning
```yaml
backend servers:
  balance: leastconn

  # Consider queue depth
  option: lb-agent-check

  # Slow start for new servers
  server web1 192.168.1.10:8080 slowstart=30s
```

### Source Hash Optimization
```yaml
backend servers:
  balance: source

  # Use consistent hashing
  hash-type: consistent

  # Increase hash table size
  hash-balance-factor: 150

  # Rehash on server changes
  option: rehash
```

## Monitoring Algorithm Performance

### Metrics to Track
- Distribution uniformity
- Response time by server
- Error rate by server
- Connection count by server
- Cache hit rate (for hash-based)

### Example Metrics Query
```sql
SELECT
  server_name,
  COUNT(*) as requests,
  AVG(response_time) as avg_response,
  PERCENTILE_CONT(0.99) WITHIN GROUP (ORDER BY response_time) as p99,
  SUM(CASE WHEN status >= 500 THEN 1 ELSE 0 END) as errors
FROM requests
WHERE timestamp > NOW() - INTERVAL '5 minutes'
GROUP BY server_name
ORDER BY requests DESC;
```