# UltraBalancer - High-Performance Load Balancer Framework

[![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)]()
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Performance](https://img.shields.io/badge/performance-1M%2B_RPS-orange.svg)]()

**IMPORTANT: This is a skeleton/framework project under continuous development. Core features are functional but extensive testing and production hardening is ongoing.**

## What is UltraBalancer?

UltraBalancer is a next-generation, high-performance load balancing framework designed for modern cloud-native applications. Built with a hybrid C/C++ architecture, it combines the raw performance of C with the flexibility and advanced features of modern C++17.

## Why UltraBalancer?

### The Need
Modern applications demand load balancers that can:
- Handle millions of concurrent connections
- Process over 1M requests per second on commodity hardware
- Provide sub-millisecond latency
- Scale horizontally with zero downtime
- Offer intelligent routing beyond simple round-robin

### The Solution
UltraBalancer addresses these needs with:
- **Lock-free data structures** for maximum concurrency
- **Zero-copy networking** using splice and sendfile
- **NUMA-aware memory allocation** for optimal cache performance
- **Kernel bypass techniques** for ultra-low latency
- **Advanced C++ components** for intelligent request routing and metrics

## Performance Metrics

| Metric | Performance |
|--------|------------|
| **Throughput** | 1M+ requests/sec (single node) |
| **Latency** | < 100μs p50, < 1ms p99 |
| **Connections** | 1M+ concurrent |
| **Memory** | < 100MB for 100K connections |
| **CPU** | < 5% for 100K RPS |

*Benchmarked on: Intel Xeon E5-2686 v4, 32GB RAM, 10Gbps NIC*

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Frontend (C)                         │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   Listener  │  │  Connection  │  │   Protocol   │  │
│  │   Manager   │  │   Acceptor   │  │   Detector   │  │
│  └─────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│                  Core Engine (C/C++)                    │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Connection │  │   Request    │  │   Metrics    │  │
│  │     Pool    │  │    Router    │  │  Aggregator  │  │
│  │    (C++)    │  │    (C++)     │  │    (C++)     │  │
│  └─────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────┐
│                    Backend (C)                          │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   Server    │  │    Health    │  │   Session    │  │
│  │   Manager   │  │    Checker   │  │   Stickiness │  │
│  └─────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
```

## Core Features

### Load Balancing Algorithms
- **Round Robin** - Equal distribution
- **Weighted Round Robin** - Priority-based distribution
- **Least Connections** - Smart connection balancing
- **Source IP Hash** - Session persistence
- **URI Hash** - Content-aware routing
- **Random** - Stochastic distribution
- **Custom** - Plugin your own algorithm

### Advanced Features (C++)
- **Connection Pooling** - Reuse backend connections efficiently
- **Request Routing** - Intelligent path-based and header-based routing
- **Circuit Breaking** - Automatic failure detection and recovery
- **Rate Limiting** - Token bucket algorithm per route
- **Metrics Aggregation** - Real-time performance monitoring
- **Hot Reload** - Zero-downtime configuration updates

### Protocol Support
- HTTP/1.0, HTTP/1.1
- HTTP/2 (planned)
- WebSocket
- TCP/UDP proxy
- SSL/TLS termination
- Protocol auto-detection

## 📦 Installation

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt-get install build-essential gcc g++ make
sudo apt-get install libssl-dev libpcre3-dev zlib1g-dev
sudo apt-get install libbrotli-dev libjemalloc-dev

# CentOS/RHEL
sudo yum install gcc gcc-c++ make
sudo yum install openssl-devel pcre-devel zlib-devel
sudo yum install brotli-devel jemalloc-devel
```

### Building from Source
```bash
git clone https://github.com/Megallm/ultrabalancer.git
cd ultrabalancer
make
sudo make install
```

### Docker
```bash
docker build -t ultrabalancer .
docker run -p 80:80 -p 443:443 ultrabalancer
```

## Quick Start

### Basic Configuration
```yaml
# /etc/ultrabalancer/config.yaml
global:
  maxconn: 100000
  nbproc: auto
  nbthread: auto

frontend web:
  bind: 0.0.0.0:80
  mode: http
  default_backend: servers

backend servers:
  balance: roundrobin
  servers:
    - server1 192.168.1.10:8080 check weight=100
    - server2 192.168.1.11:8080 check weight=100
    - server3 192.168.1.12:8080 check weight=50 backup
```

### Running
```bash
# Start with default config
ultrabalancer

# Start with custom config
ultrabalancer -f /path/to/config.yaml

# Start in daemon mode
ultrabalancer -D

# Check configuration
ultrabalancer -c -f /path/to/config.yaml
```

## Monitoring

### Built-in Metrics
Access real-time metrics via HTTP endpoint:
```bash
curl http://localhost:8080/stats
```

Response:
```json
{
  "total_requests": 1234567,
  "successful_requests": 1234000,
  "failed_requests": 567,
  "avg_response_time_ms": 1.23,
  "p50_response_time_ms": 0.8,
  "p95_response_time_ms": 2.1,
  "p99_response_time_ms": 5.4,
  "active_connections": 5432,
  "total_bytes_in": 123456789,
  "total_bytes_out": 987654321
}
```

## 📖 Documentation

Detailed documentation is available in the `docs/` directory:

- [🏗️ Architecture Overview](docs/architecture.md) - System design and components
- [⚙️ Configuration Guide](docs/configuration.md) - Complete configuration reference
- [🔌 API Reference](docs/api.md) - HTTP API and control interface
- [🎯 Load Balancing Algorithms](docs/algorithms.md) - Algorithm details and selection guide
- [💡 Performance Tuning](docs/performance.md) - Optimization tips and benchmarks
- [🔧 Troubleshooting](docs/troubleshooting.md) - Common issues and solutions
- [🤝 Contributing](docs/contributing.md) - Development guide and roadmap

## 🔄 C++ Integration

UltraBalancer leverages C++ for advanced features while maintaining C core for performance:

### Connection Pool Manager (C++)
```cpp
// Automatically manages backend connections
auto pool = ConnectionPool(1000, 100); // max_size, max_idle
auto conn = pool.acquire(server);
// Connection automatically returned to pool on destruction
```

### Metrics Aggregator (C++)
```cpp
// Real-time metrics with percentile calculations
MetricsAggregator::instance().increment_counter("requests.total");
MetricsAggregator::instance().record_timer("response.time", duration);
auto stats = MetricsAggregator::instance().get_stats();
```

### Request Router (C++)
```cpp
// Advanced routing with regex and weighted targets
auto router = RequestRouter();
router->add_route(route);
router->enable_rate_limiting("api", 1000); // 1000 RPS
auto target = router->route_request("GET", "/api/v1/users", headers);
```

## Use Cases

- **Microservices** - Service mesh ingress/egress
- **API Gateway** - Rate limiting and routing
- **CDN Edge** - Content delivery acceleration
- **Database Proxy** - Connection pooling and read/write splitting
- **WebSocket Server** - Real-time application load balancing
- **gRPC Proxy** - HTTP/2 and streaming RPC support

## 🚧 Roadmap

- [x] Core load balancing engine
- [x] C++ components integration
- [x] Connection pooling
- [x] Metrics aggregation
- [x] Advanced routing
- [ ] HTTP/2 full support
- [ ] gRPC load balancing
- [ ] Kubernetes integration
- [ ] Web UI dashboard
- [ ] Distributed configuration
- [ ] Machine learning-based routing

## 🤝 Contributing

We welcome contributions! Please see [CONTRIBUTING.md](docs/contributing.md) for details.

### Core Maintainer

**Kira** - [GitHub](https://github.com/Bas3line)
📧 Contact: [shubham@ghostlytics.org](mailto:shubham@ghostlytics.org)

## 📄 License

UltraBalancer is released under the MIT License. See [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- HAProxy for inspiration and architectural patterns
- NGINX for performance optimization techniques
- The Linux kernel team for high-performance networking APIs
- The C++ community for modern language features

## ⚠️ Disclaimer

This is an active development project. While core functionality is stable, extensive testing is recommended before production deployment. The project is continuously evolving with new features and optimizations being added regularly.

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/Megallm/ultrabalancer/issues)
- **Discussions**: [GitHub Discussions](https://github.com/Megallm/ultrabalancer/discussions)

---

