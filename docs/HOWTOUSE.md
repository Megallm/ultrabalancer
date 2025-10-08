# UltraBalancer - High-Performance Load Balancer

A production-grade, high-performance load balancer written in C, designed to rival HAProxy in features and performance. UltraBalancer provides enterprise-level traffic management with minimal latency and maximum throughput.

## Features

### Core Load Balancing
- **Multiple Algorithms**: Round-robin, least connections, IP hash, consistent hash, weighted, response time-based
- **Zero-copy networking**: Using splice() and sendfile() for optimal performance
- **Lock-free data structures**: For minimal contention in multi-threaded environments
- **NUMA-aware memory allocation**: Optimized for modern multi-socket systems
- **CPU affinity**: Worker threads pinned to specific CPUs

### Protocol Support
- **HTTP/1.0, HTTP/1.1**: Full protocol support with keep-alive and pipelining
- **HTTP/2**: Multiplexing, server push, header compression (HPACK)
- **WebSocket**: Full bidirectional support with automatic protocol upgrade
- **TCP**: Layer 4 load balancing for any TCP protocol
- **SSL/TLS**: SNI, ALPN, session resumption, OCSP stapling
- **gRPC**: Native support with HTTP/2

### Advanced Features
- **Health Checks**: TCP, HTTP, HTTPS, custom scripts
- **Stick Tables**: Session persistence with multiple methods
- **ACLs**: Powerful rule engine for traffic control
- **Rate Limiting**: Per-IP, per-backend, global limits
- **Compression**: gzip, deflate, brotli
- **Caching**: Intelligent HTTP caching layer
- **Hot Reload**: Configuration changes without dropping connections
- **Circuit Breaker**: Automatic backend failure detection
- **Request/Response Rewriting**: Headers, URLs, content modification

### Monitoring & Observability
- **Real-time Statistics**: Connections, requests, bytes, response times
- **Prometheus Metrics**: Native export format
- **JSON API**: RESTful API for stats and control
- **Access Logs**: Customizable format, syslog support
- **Error Tracking**: Detailed error logging and alerting

## Quick Start

### Building from Source

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install -y build-essential cmake libssl-dev libpcre3-dev \
                        zlib1g-dev libbrotli-dev libjemalloc-dev

# Clone and build
git clone https://github.com/Megallm/ultrabalancer.git
cd ultrabalancer
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

### Basic Usage

```bash
# Start with default configuration
ultrabalancer -f /etc/ultrabalancer/ultrabalancer.cfg

# Start with specific options
ultrabalancer -p 80 -a round-robin \
              -b server1.example.com:8080 \
              -b server2.example.com:8080 \
              -b server3.example.com:8080

# Run in daemon mode
ultrabalancer -D -f /etc/ultrabalancer/ultrabalancer.cfg

# Test configuration
ultrabalancer -c -f /etc/ultrabalancer/ultrabalancer.cfg
```

## Configuration

### Basic Configuration Example

```cfg
# /etc/ultrabalancer/ultrabalancer.cfg

global
    maxconn 100000
    nbproc 4
    nbthread 8
    cpu-map auto:1-8
    daemon
    log 127.0.0.1 local0
    stats socket /var/run/ultrabalancer.sock mode 600
    tune.ssl.default-dh-param 2048
    ssl-default-bind-ciphers ECDHE+AESGCM:ECDHE+AES256:!aNULL:!MD5:!DSS

defaults
    mode http
    timeout connect 5s
    timeout client 30s
    timeout server 30s
    timeout http-request 10s
    option httplog
    option dontlognull
    option http-server-close
    option forwardfor except 127.0.0.0/8
    option redispatch
    retries 3
    compression algo gzip
    compression type text/html text/css application/javascript

frontend web_frontend
    bind *:80
    bind *:443 ssl crt /etc/ssl/certs/site.pem alpn h2,http/1.1

    # ACL rules
    acl is_static path_end .jpg .jpeg .gif .png .css .js .ico
    acl is_api path_beg /api/
    acl is_websocket hdr(Upgrade) -i WebSocket

    # Routing rules
    use_backend static_servers if is_static
    use_backend api_servers if is_api
    use_backend websocket_servers if is_websocket
    default_backend web_servers

backend web_servers
    balance roundrobin
    option httpchk GET /health
    http-check expect status 200

    server web1 192.168.1.10:8080 check weight 100 maxconn 1000
    server web2 192.168.1.11:8080 check weight 100 maxconn 1000
    server web3 192.168.1.12:8080 check weight 100 maxconn 1000 backup

backend static_servers
    balance uri depth 3
    hash-type consistent

    server cdn1 cdn1.example.com:80 check
    server cdn2 cdn2.example.com:80 check

backend api_servers
    balance leastconn
    option httpchk GET /api/health
    http-check expect rstring {"status":"ok"}

    stick-table type ip size 100k expire 30m
    stick on src

    server api1 10.0.1.10:3000 check ssl verify none
    server api2 10.0.1.11:3000 check ssl verify none

backend websocket_servers
    balance source
    hash-type consistent

    server ws1 10.0.2.10:8080 check
    server ws2 10.0.2.11:8080 check

listen stats
    bind *:8080
    stats enable
    stats uri /stats
    stats refresh 30s
    stats show-node
    stats auth admin:password
```

### Advanced Configuration

```cfg
# TCP Load Balancing
listen mysql_cluster
    bind *:3306
    mode tcp
    balance leastconn
    option mysql-check user haproxy

    server mysql1 10.0.3.10:3306 check weight 100
    server mysql2 10.0.3.11:3306 check weight 100
    server mysql3 10.0.3.12:3306 check weight 50 backup

# Rate Limiting
frontend rate_limited
    bind *:80

    # Rate limit by IP
    stick-table type ip size 100k expire 30s store http_req_rate(10s)
    http-request track-sc0 src
    http-request deny if { sc_http_req_rate(0) gt 20 }

    # Rate limit by URL
    acl is_expensive path_beg /api/expensive/
    http-request deny if is_expensive { sc_http_req_rate(0) gt 5 }

# SSL/TLS Configuration
frontend https_frontend
    bind *:443 ssl crt-list /etc/ultrabalancer/certs.list alpn h2,http/1.1

    # HSTS
    http-response set-header Strict-Transport-Security "max-age=31536000; includeSubDomains; preload"

    # SSL Redirect
    redirect scheme https if !{ ssl_fc }

    # SNI Routing
    use_backend backend1 if { ssl_fc_sni app1.example.com }
    use_backend backend2 if { ssl_fc_sni app2.example.com }

# Cache Configuration
cache web_cache
    total-max-size 256
    max-object-size 10485760
    max-age 3600

backend cached_backend
    http-request cache-use web_cache
    http-response cache-store web_cache

    server web1 10.0.0.10:80 check
```

## Command Line Options

```bash
ultrabalancer [OPTIONS]

Options:
  -f, --config FILE        Configuration file path
  -c, --check             Check configuration and exit
  -D, --daemon            Run as daemon
  -p, --port PORT         Listen port (default: 80)
  -a, --algorithm ALGO    Load balancing algorithm
  -b, --backend HOST:PORT Add backend server
  -w, --workers NUM       Number of worker threads
  -n, --nbproc NUM        Number of processes
  -v, --verbose           Verbose output
  -V, --version           Show version
  -h, --help              Show help

Algorithms:
  round-robin       Round-robin (default)
  leastconn         Least connections
  source            Source IP hash
  uri               URI hash
  url_param         URL parameter hash
  hdr               Header hash
  random            Random

Examples:
  # Simple HTTP load balancer
  ultrabalancer -p 80 -b 10.0.0.1:8080 -b 10.0.0.2:8080

  # HTTPS with custom config
  ultrabalancer -f /etc/ultrabalancer/https.cfg -D

  # TCP load balancer for MySQL
  ultrabalancer -m tcp -p 3306 -b mysql1:3306 -b mysql2:3306
```

## Performance Tuning

### System Tuning

```bash
# /etc/sysctl.conf
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 15
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.tcp_syncookies = 1
net.ipv4.tcp_timestamps = 1
net.ipv4.tcp_tw_recycle = 0
net.ipv4.tcp_congestion_control = bbr
net.core.default_qdisc = fq
net.ipv4.tcp_mtu_probing = 1
net.ipv4.tcp_rmem = 4096 87380 134217728
net.ipv4.tcp_wmem = 4096 65536 134217728
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.ipv4.tcp_keepalive_time = 60
net.ipv4.tcp_keepalive_intvl = 10
net.ipv4.tcp_keepalive_probes = 3
net.ipv4.tcp_no_metrics_save = 1
net.ipv4.ip_forward = 1

# Apply settings
sudo sysctl -p
```

### UltraBalancer Tuning

```cfg
global
    # CPU pinning
    cpu-map 1-4 0-3

    # Memory
    tune.bufsize 32768
    tune.maxrewrite 16384

    # SSL
    tune.ssl.maxrecord 16384
    tune.ssl.cachesize 100000
    tune.ssl.lifetime 600

    # HTTP/2
    tune.h2.initial-window-size 65536
    tune.h2.max-concurrent-streams 100

    # Compression
    tune.comp.maxlevel 9

    # Zero-copy
    tune.splice.auto on
```

## Monitoring

### Stats Interface

```bash
# Enable stats in config
listen stats
    bind *:8080
    stats enable
    stats uri /stats
    stats refresh 30s
    stats show-legends
    stats show-node

# Access stats
curl http://localhost:8080/stats

# JSON stats
curl http://localhost:8080/stats?json

# Prometheus metrics
curl http://localhost:8080/metrics
```

### Health Checks

```cfg
backend web_servers
    # Basic HTTP health check
    option httpchk GET /health HTTP/1.1\r\nHost:\ example.com
    http-check expect status 200

    # Advanced health check with conditions
    option httpchk
    http-check connect
    http-check send meth GET uri /health ver HTTP/1.1 hdr Host example.com
    http-check expect status 200
    http-check expect header Content-Type -m sub text/html
    http-check expect rstring healthy|ok

    # TCP health check
    option tcp-check
    tcp-check connect
    tcp-check send-binary 01000000
    tcp-check expect binary 01000001

    server web1 10.0.0.1:80 check inter 2s rise 3 fall 3
```

## API

### REST API

```bash
# Get stats
curl http://localhost:8080/api/stats

# Get backend status
curl http://localhost:8080/api/backends

# Disable server
curl -X POST http://localhost:8080/api/server/web_servers/web1/disable

# Enable server
curl -X POST http://localhost:8080/api/server/web_servers/web1/enable

# Set server weight
curl -X POST http://localhost:8080/api/server/web_servers/web1/weight/50

# Clear stick table
curl -X DELETE http://localhost:8080/api/sticktable/web_servers
```

### Unix Socket Commands

```bash
# Connect to socket
socat readline /var/run/ultrabalancer.sock

# Commands
> show info
> show stat
> show pools
> show table
> disable server backend/server1
> enable server backend/server1
> set weight backend/server1 50
> clear table web_servers
```

## Troubleshooting

### Debug Mode

```bash
# Run in debug mode
ultrabalancer -d -f /etc/ultrabalancer/ultrabalancer.cfg

# Increase verbosity
ultrabalancer -vvv -f /etc/ultrabalancer/ultrabalancer.cfg

# Test configuration
ultrabalancer -c -f /etc/ultrabalancer/ultrabalancer.cfg
```

### Common Issues

1. **High CPU Usage**
   - Check for SSL/TLS overhead
   - Verify compression settings
   - Review ACL complexity
   - Check health check frequency

2. **Connection Errors**
   - Verify backend availability
   - Check timeout settings
   - Review maxconn limits
   - Check system ulimits

3. **Memory Usage**
   - Tune buffer sizes
   - Review stick table sizes
   - Check cache configuration
   - Monitor connection pools

## License

MIT License - See LICENSE file for details

## Contributing

Please see CONTRIBUTING.md for guidelines

## Support

- Documentation: https://ultrabalancer.io/docs
- Issues: https://github.com/Megallm/ultrabalancer/issues
- Community: https://discord.gg/ultrabalancer