# ⚙️ UltraBalancer Configuration Guide

## Table of Contents
1. [Configuration Format](#configuration-format)
2. [Global Settings](#global-settings)
3. [Frontend Configuration](#frontend-configuration)
4. [Backend Configuration](#backend-configuration)
5. [Health Checks](#health-checks)
6. [SSL/TLS Configuration](#ssltls-configuration)
7. [Advanced Features](#advanced-features)
8. [C++23 Component Configuration](#c23-component-configuration)

## Configuration Format

UltraBalancer supports multiple configuration formats:
- YAML (recommended)
- JSON
- TOML
- Environment variables

### Basic Structure

```yaml
# ultrabalancer.yaml
global:
  # Global settings

defaults:
  # Default values for all sections

frontend <name>:
  # Frontend configuration

backend <name>:
  # Backend configuration
```

## Global Settings

```yaml
global:
  # Performance
  maxconn: 100000              # Maximum global connections
  nbproc: auto                 # Number of processes (auto = CPU cores)
  nbthread: auto               # Threads per process
  cpu-map: auto                # CPU affinity mapping

  # Memory (C23 features)
  tune.bufsize: 16384          # Buffer size
  tune.maxrewrite: 8192        # Max URL rewrite size
  tune.ssl.cachesize: 100000   # SSL session cache

  # Network
  tune.rcvbuf.client: 0        # Client receive buffer (0 = auto)
  tune.sndbuf.client: 0        # Client send buffer
  tune.rcvbuf.server: 0        # Server receive buffer
  tune.sndbuf.server: 0        # Server send buffer

  # System
  chroot: /var/lib/ultrabalancer
  user: ultrabalancer
  group: ultrabalancer
  daemon: true
  pidfile: /var/run/ultrabalancer.pid

  # Logging
  log:
    - stdout local0
    - /var/log/ultrabalancer.log local0 notice

  # Stats
  stats:
    socket: /var/run/ultrabalancer.sock mode 660
    timeout: 30s
    maxconn: 10

  # C++23 Features
  cpp_features:
    connection_pool: true
    metrics_aggregator: true
    request_router: true
    use_coroutines: true
```

## Frontend Configuration

### Basic Frontend

```yaml
frontend web:
  bind:
    - 0.0.0.0:80
    - 0.0.0.0:443 ssl crt /etc/ssl/cert.pem
    - :::80 v6only
    - :::443 ssl crt /etc/ssl/cert.pem v6only

  mode: http
  option:
    - httplog
    - dontlognull
    - http-server-close
    - forwardfor except 127.0.0.0/8

  timeout:
    client: 30s
    http-request: 10s
    http-keep-alive: 30s

  maxconn: 50000

  # ACL Rules
  acl:
    is_static: path_beg /static /images /css /js
    is_api: path_beg /api /v1 /v2
    is_websocket: hdr(Upgrade) -i websocket
    is_admin: src 10.0.0.0/8 192.168.0.0/16

  # Routing Rules
  use_backend:
    - static if is_static
    - api if is_api
    - websocket if is_websocket
    - admin if is_admin

  default_backend: servers
```

### Advanced Frontend Features

```yaml
frontend advanced:
  bind:
    - 0.0.0.0:80 process 1-4
    - 0.0.0.0:443 ssl crt /etc/ssl/ alpn h2,http/1.1

  # Rate Limiting
  stick-table:
    type: ip
    size: 100k
    expire: 30s
    store:
      - gpc0
      - conn_rate(10s)
      - http_req_rate(10s)

  # Track requests
  tcp-request:
    - connection track-sc0 src
    - connection reject if { src_conn_rate gt 100 }

  http-request:
    - track-sc0 src
    - deny if { sc_http_req_rate(0) gt 20 }
    - set-header X-Forwarded-Proto https if { ssl_fc }
    - set-header X-Real-IP %[src]
    - del-header X-Forwarded-For
    - set-header X-Forwarded-For %[src]

  # Response modifications
  http-response:
    - set-header X-Server UltraBalancer
    - set-header Strict-Transport-Security "max-age=31536000"
    - del-header Server

  # Compression
  compression:
    algo: gzip br
    type:
      - text/html
      - text/css
      - application/javascript
      - application/json
    offload: true
```

## Backend Configuration

### Basic Backend

```yaml
backend servers:
  mode: http
  balance: roundrobin

  option:
    - httpchk GET /health HTTP/1.1\r\nHost:\ localhost
    - log-health-checks
    - redispatch
    - prefer-last-server

  timeout:
    connect: 5s
    server: 30s
    queue: 30s

  server:
    - web1 192.168.1.10:8080 check weight=100
    - web2 192.168.1.11:8080 check weight=100
    - web3 192.168.1.12:8080 check weight=50 backup
```

### Advanced Backend Features

```yaml
backend advanced:
  # Load Balancing Algorithm
  balance: leastconn
  # Alternative: roundrobin, source, uri, url_param, hdr, random

  # Session Persistence
  stick-table:
    type: string
    len: 64
    size: 100k
    expire: 30m

  stick:
    - on src
    - store-response set-cookie(JSESSIONID)
    - match set-cookie(JSESSIONID)

  # Connection Pooling (C++23)
  connection_pool:
    enabled: true
    max_connections: 1000
    max_idle: 100
    idle_timeout: 60s
    health_check_interval: 5s

  # Circuit Breaker
  circuit_breaker:
    enabled: true
    error_threshold: 50
    success_threshold: 5
    timeout: 30s
    half_open_requests: 10

  # Retry Policy
  retry_on:
    - all-retryable-errors
    - 503
    - 504
  retries: 3

  # Dynamic Servers
  server-template:
    - web 10 _http._tcp.service.consul resolvers consul check

  # Server Options
  server:
    - name: web1
      address: 192.168.1.10:8080
      options:
        - check inter 2000 rise 3 fall 3
        - weight 100
        - maxconn 100
        - slowstart 60s
        - observe layer7
        - on-marked-down shutdown-sessions
        - on-marked-up shutdown-backup-sessions
```

## Health Checks

### HTTP Health Checks

```yaml
backend api:
  option httpchk
  http-check:
    - send meth GET uri /health ver HTTP/1.1
    - send hdr Host api.example.com
    - send hdr User-Agent UltraBalancer-HealthCheck
    - expect status 200-399
    - expect header Content-Type -m sub application/json
    - expect string {"status":"healthy"}
```

### TCP Health Checks

```yaml
backend tcp_service:
  option tcp-check
  tcp-check:
    - connect
    - send-binary 0x01000000
    - expect binary 0x01000000
    - send PING\r\n
    - expect string +PONG
```

### Custom Health Checks (C++23)

```yaml
backend custom:
  health_check:
    type: custom
    script: |
      auto check = []() -> std::expected<bool, std::string> {
          auto conn = co_await connect_async(server);
          auto response = co_await conn.send("HEALTH");
          co_return response == "OK";
      };
    interval: 5s
    timeout: 2s
    rise: 3
    fall: 3
```

## SSL/TLS Configuration

### Basic SSL

```yaml
frontend https:
  bind:
    - 0.0.0.0:443 ssl crt /etc/ssl/cert.pem

  # SSL Options
  ssl-default-bind-options:
    - no-sslv3
    - no-tlsv10
    - no-tlsv11
    - no-tls-tickets

  ssl-default-bind-ciphers: ECDHE+AESGCM:ECDHE+AES256:!aNULL:!MD5:!DSS
  ssl-default-bind-ciphersuites: TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256
```

### Advanced SSL

```yaml
global:
  ssl:
    # Performance
    tune.ssl.default-dh-param: 2048
    tune.ssl.maxrecord: 1400
    tune.ssl.cachesize: 100000
    tune.ssl.lifetime: 600

    # Security
    ssl-default-server-options:
      - no-sslv3
      - no-tlsv10
      - no-tlsv11

    # Engine (C++23 integration)
    ssl-engine:
      - rdrand
      - aesni
      - async

frontend https_advanced:
  bind:
    - 0.0.0.0:443 ssl crt-list /etc/ssl/crt-list.txt alpn h2,http/1.1

  # SNI-based routing
  use_backend:
    - api if { ssl_fc_sni api.example.com }
    - admin if { ssl_fc_sni admin.example.com }

  # Client certificate verification
  bind:
    - 0.0.0.0:8443 ssl crt /etc/ssl/cert.pem ca-file /etc/ssl/ca.pem verify required

  acl:
    - valid_client ssl_c_verify 0
    - client_cert_cn ssl_c_s_dn(CN) -m str admin

  http-request:
    - deny unless valid_client
    - set-header X-Client-CN %[ssl_c_s_dn(CN)]
```

## Advanced Features

### Request Router Configuration (C++23)

```yaml
request_router:
  enabled: true

  routes:
    - name: api_v2
      priority: 100
      rules:
        - type: prefix
          pattern: /api/v2
        - type: header
          pattern: "X-API-Version: 2"
      targets:
        - backend: api_v2_primary
          weight: 80
        - backend: api_v2_canary
          weight: 20

      circuit_breaker:
        enabled: true
        error_threshold: 50
        reset_timeout: 30s

      rate_limit:
        requests_per_second: 1000
        burst: 100

    - name: websocket
      priority: 90
      rules:
        - type: header
          pattern: "Upgrade: websocket"
      targets:
        - backend: ws_cluster
      options:
        - sticky_sessions
        - no_buffer
```

### Metrics Configuration (C++23)

```yaml
metrics:
  enabled: true

  exporters:
    - type: prometheus
      port: 9090
      path: /metrics

    - type: statsd
      host: localhost
      port: 8125
      prefix: ultrabalancer

    - type: opentelemetry
      endpoint: http://otel-collector:4318
      service_name: ultrabalancer

  collection:
    interval: 10s
    percentiles: [50, 95, 99, 99.9]
    histogram_buckets:
      latency: [0.001, 0.01, 0.1, 1, 10]
      request_size: [100, 1000, 10000, 100000]
```

### Connection Pool Configuration (C++23)

```yaml
connection_pools:
  default:
    max_size: 1000
    max_idle: 100
    min_idle: 10
    max_lifetime: 3600s
    idle_timeout: 300s

    validation:
      test_on_borrow: true
      test_on_return: false
      test_while_idle: true
      validation_query: "PING"
      validation_timeout: 1s

    timeouts:
      connect: 5s
      socket: 30s
      request: 60s
```

### Cache Configuration

```yaml
cache:
  enabled: true

  storage:
    type: memory  # or redis, memcached
    size: 1GB
    max_object_size: 10MB

  rules:
    - name: static_assets
      path_pattern: \.(jpg|png|css|js)$
      ttl: 86400
      vary:
        - Accept-Encoding
        - Accept

    - name: api_responses
      path_pattern: ^/api/
      ttl: 300
      key: "%{req.uri}%{req.headers.accept}"
      conditions:
        - method: GET
        - status: 200
```

### Lua Scripting

```yaml
lua:
  enabled: true

  scripts:
    - name: rate_limit
      on: request
      code: |
        local redis = require("redis")
        local client = redis.connect("localhost", 6379)

        local key = "rate:" .. txn.req:get_header("X-API-Key")
        local count = client:incr(key)

        if count == 1 then
          client:expire(key, 60)
        end

        if count > 100 then
          txn.res:set_status(429)
          txn.res:set_header("X-RateLimit-Limit", "100")
          txn.res:set_header("X-RateLimit-Remaining", "0")
          txn.done()
        end
```

## Environment Variables

All configuration values can be overridden via environment variables:

```bash
# Global settings
ULTRABALANCER_GLOBAL_MAXCONN=200000
ULTRABALANCER_GLOBAL_NBTHREAD=32

# Frontend settings
ULTRABALANCER_FRONTEND_WEB_BIND=0.0.0.0:8080
ULTRABALANCER_FRONTEND_WEB_MAXCONN=100000

# Backend settings
ULTRABALANCER_BACKEND_SERVERS_SERVER_1=web1:192.168.1.10:8080:weight=100

# Feature flags
ULTRABALANCER_CPP_CONNECTION_POOL=true
ULTRABALANCER_CPP_METRICS=true
ULTRABALANCER_CPP_ROUTER=true
```

## Hot Reload

Configuration can be reloaded without downtime:

```bash
# Validate configuration
ultrabalancer -c -f /etc/ultrabalancer/config.yaml

# Reload configuration
kill -USR2 $(cat /var/run/ultrabalancer.pid)

# Or using systemctl
systemctl reload ultrabalancer
```

## Configuration Best Practices

1. **Start Simple**: Begin with minimal configuration
2. **Use Defaults**: Leverage sensible defaults
3. **Monitor Everything**: Enable comprehensive metrics
4. **Test Changes**: Always validate before reload
5. **Version Control**: Keep configurations in Git
6. **Environment-Specific**: Use separate configs per environment
7. **Security First**: Always use TLS in production
8. **Documentation**: Comment complex configurations