# How to Use UltraBalancer with Your Code

## Quick Integration

### Basic Setup
```c
#include "ultrabalancer.h"

int main() {
    lb_config_t config = {
        .port = 8080,
        .backend_count = 3,
        .algorithm = LB_ROUND_ROBIN
    };

    lb_instance_t *lb = lb_create(&config);
    lb_add_backend(lb, "192.168.1.10", 3000, 100);
    lb_add_backend(lb, "192.168.1.11", 3000, 100);
    lb_add_backend(lb, "192.168.1.12", 3000, 100);

    lb_start(lb);
    return 0;
}
```

### Health Checks
```c
lb_health_config_t health = {
    .interval = 5000,
    .timeout = 2000,
    .path = "/health",
    .expected_status = 200
};
lb_configure_health_checks(lb, &health);
```

### SSL/TLS Configuration
```c
lb_ssl_config_t ssl = {
    .cert_file = "/path/to/cert.pem",
    .key_file = "/path/to/key.pem",
    .ca_file = "/path/to/ca.pem"
};
lb_enable_ssl(lb, &ssl);
```

### Load Balancing Algorithms
- `LB_ROUND_ROBIN` - Equal distribution
- `LB_LEAST_CONN` - Route to least connected server
- `LB_WEIGHTED_RR` - Weighted round robin
- `LB_IP_HASH` - Consistent client routing

### Monitoring & Stats
```c
lb_stats_t stats;
lb_get_stats(lb, &stats);
printf("Active connections: %d\n", stats.active_connections);
printf("Total requests: %ld\n", stats.total_requests);
```

### Configuration File
```ini
[global]
port = 8080
worker_processes = auto
max_connections = 10000

[backend "web1"]
server = 192.168.1.10:3000
weight = 100
backup = false

[backend "web2"]
server = 192.168.1.11:3000
weight = 150
backup = false
```

### Advanced Features
- Session persistence via sticky tables
- Rate limiting and DDoS protection
- HTTP/2 and WebSocket support
- Real-time metrics and logging
- Dynamic backend management
- Circuit breaker patterns

### Performance Tuning
```c
lb_performance_config_t perf = {
    .worker_threads = 8,
    .connection_pool_size = 1000,
    .buffer_size = 16384,
    .keepalive_timeout = 60
};
lb_configure_performance(lb, &perf);
```

### Error Handling
```c
if (lb_get_status(lb) != LB_STATUS_RUNNING) {
    lb_error_t error = lb_get_last_error(lb);
    fprintf(stderr, "Error: %s\n", error.message);
}
```