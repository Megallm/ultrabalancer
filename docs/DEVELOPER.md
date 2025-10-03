# UltraBalancer Developer Guide

## Architecture Overview

UltraBalancer is built with a modular architecture optimized for high performance and scalability.

### Core Components

#### 1. Network Layer (`src/network/`)
- **Event-driven I/O**: Uses epoll (Linux) for efficient connection handling
- **Zero-copy**: Utilizes splice() and sendfile() for optimal data transfer
- **Connection pooling**: Reuses connections to reduce overhead

#### 2. Load Balancing Algorithms (`src/core/`)
- Round-robin
- Least connections
- Source IP hash
- Consistent hashing
- Weighted distribution
- Response time based

#### 3. Protocol Support (`src/http/`)
- HTTP/1.0, HTTP/1.1
- HTTP/2 with multiplexing
- WebSocket
- TCP pass-through

#### 4. Health Checking (`src/health/`)
- TCP connectivity checks
- HTTP/HTTPS health endpoints
- Database-specific checks (MySQL, PostgreSQL, Redis)
- Custom script execution

#### 5. Session Persistence (`src/stick_tables.c`)
- IP-based affinity
- Cookie-based sessions
- Consistent hashing for cache-friendly distribution

#### 6. Caching Layer (`src/cache/`)
- LRU eviction
- TTL-based expiration
- Compression support (gzip, brotli)

## Building from Source

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt-get install -y build-essential libssl-dev libpcre3-dev \
                        zlib1g-dev libbrotli-dev libjemalloc-dev

# CentOS/RHEL
sudo yum install -y gcc make openssl-devel pcre-devel \
                    zlib-devel brotli-devel jemalloc-devel
```

### Compilation
```bash
make clean
make -j$(nproc)
make test
sudo make install
```

### Build Options
```bash
make USE_SYSTEMD=1    # Enable systemd integration
make USE_PCRE2=1      # Use PCRE2 for regex
make DEBUG=1          # Debug build with symbols
```

## Configuration

### Basic Structure
```
global
    # Global settings

defaults
    # Default settings for all proxies

frontend <name>
    # Frontend configuration

backend <name>
    # Backend configuration

listen <name>
    # Combined frontend/backend
```

### ACL Rules
```
acl <name> <criterion> <pattern>

# Examples:
acl is_static path_end .jpg .css .js
acl is_api path_beg /api/
acl is_secure src 10.0.0.0/8
```

### Load Balancing
```
balance <algorithm>
    roundrobin       # Default
    leastconn        # Least connections
    source           # Source IP hash
    uri              # URI hash
    url_param <name> # URL parameter hash
```

## Performance Tuning

### System Limits
```bash
# /etc/security/limits.conf
* soft nofile 1000000
* hard nofile 1000000
* soft nproc 64000
* hard nproc 64000
```

### Kernel Parameters
```bash
# /etc/sysctl.conf
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_tw_reuse = 1
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535
```

### Configuration Tuning
```
global
    maxconn 100000
    nbthread 16
    tune.bufsize 32768
    tune.maxrewrite 16384
```

## API Reference

### Stats API

#### GET /stats
Returns statistics in JSON format

#### GET /stats?csv
Returns statistics in CSV format

#### GET /metrics
Returns Prometheus-compatible metrics

### Admin API

#### POST /server/:backend/:server/disable
Disable a server

#### POST /server/:backend/:server/enable
Enable a server

#### POST /server/:backend/:server/weight/:value
Set server weight

## Monitoring

### Metrics
- Connection metrics: current, total, rate
- Request metrics: rate, total, errors
- Backend metrics: up/down status, response time
- Cache metrics: hits, misses, evictions

### Health Checks
```
option httpchk GET /health
http-check expect status 200
http-check expect rstring healthy
```

### Logging
```
log 127.0.0.1:514 local0
log-tag ultrabalancer

# Log formats
option httplog    # HTTP format
option tcplog     # TCP format
```

## Troubleshooting

### Debug Mode
```bash
ultrabalancer -d -vvv -f config.cfg
```

### Common Issues

1. **Port already in use**
   ```bash
   netstat -tlnp | grep 8080
   kill $(lsof -t -i:8080)
   ```

2. **Too many open files**
   ```bash
   ulimit -n 1000000
   ```

3. **Backend connection failures**
   - Check network connectivity
   - Verify backend health
   - Review timeout settings

### Performance Analysis
```bash
# CPU profiling
perf record -g ./ultrabalancer -f config.cfg
perf report

# Memory profiling
valgrind --leak-check=full ./ultrabalancer -f config.cfg
```

## Development

### Code Structure
```
src/
├── core/        # Core proxy and server logic
├── network/     # Network I/O
├── http/        # HTTP protocol handling
├── ssl/         # SSL/TLS support
├── health/      # Health checking
├── acl/         # ACL engine
├── cache/       # Caching layer
├── stats/       # Statistics
├── utils/       # Utilities
└── config/      # Configuration parser

include/         # Header files
tests/          # Test suites
examples/       # Example configurations
docs/           # Documentation
```

### Testing
```bash
# Unit tests
make test

# Integration tests
python3 tests/integration_test.py

# Load testing
ab -n 10000 -c 100 http://localhost:8080/
```

### Contributing
1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests
5. Submit a pull request

## License
MIT License - See LICENSE file for details