# How to Setup Round Robin Load Balancing

## Basic Round Robin Configuration

### Configuration File Setup
```ini
[global]
port = 8080
algorithm = round_robin
worker_processes = auto

[backend "server1"]
server = 192.168.1.10:3000
weight = 1
active = true

[backend "server2"]
server = 192.168.1.11:3000
weight = 1
active = true

[backend "server3"]
server = 192.168.1.12:3000
weight = 1
active = true
```

### Programmatic Setup
```c
#include "ultrabalancer.h"

int main() {
    lb_config_t config = {
        .port = 8080,
        .algorithm = LB_ROUND_ROBIN,
        .max_connections = 10000
    };

    lb_instance_t *lb = lb_create(&config);

    lb_add_backend(lb, "192.168.1.10", 3000, 1);
    lb_add_backend(lb, "192.168.1.11", 3000, 1);
    lb_add_backend(lb, "192.168.1.12", 3000, 1);

    lb_start(lb);
    return 0;
}
```

## Weighted Round Robin

### Equal Weights
```ini
[backend "server1"]
server = 192.168.1.10:3000
weight = 100

[backend "server2"]
server = 192.168.1.11:3000
weight = 100
```

### Unequal Weights (High Performance Server)
```ini
[backend "high_perf"]
server = 192.168.1.10:3000
weight = 300

[backend "standard1"]
server = 192.168.1.11:3000
weight = 100

[backend "standard2"]
server = 192.168.1.12:3000
weight = 100
```

### Dynamic Weight Adjustment
```c
lb_backend_t *backend = lb_get_backend(lb, "server1");
lb_set_backend_weight(backend, 200);
```

## Health Check Integration
```ini
[health_check]
interval = 5000
timeout = 2000
retries = 3
path = "/health"
expected_status = 200

[backend "server1"]
server = 192.168.1.10:3000
weight = 100
health_check = true
```

## Advanced Round Robin Features

### Smooth Weighted Round Robin
```c
lb_rr_config_t rr_config = {
    .type = LB_RR_SMOOTH_WEIGHTED,
    .auto_adjust_weights = true,
    .response_time_factor = 0.2
};
lb_configure_round_robin(lb, &rr_config);
```

### Backup Servers
```ini
[backend "primary1"]
server = 192.168.1.10:3000
weight = 100
backup = false

[backend "primary2"]
server = 192.168.1.11:3000
weight = 100
backup = false

[backend "backup1"]
server = 192.168.1.20:3000
weight = 50
backup = true
```

### Connection Limits
```c
lb_backend_config_t backend_config = {
    .max_connections = 500,
    .connection_timeout = 30,
    .retry_attempts = 3
};
lb_configure_backend(lb, "server1", &backend_config);
```

## Monitoring Round Robin Distribution

### Statistics
```c
lb_rr_stats_t rr_stats;
lb_get_rr_stats(lb, &rr_stats);

printf("Total requests distributed: %ld\n", rr_stats.total_requests);
for (int i = 0; i < rr_stats.backend_count; i++) {
    printf("Backend %d: %ld requests (%.2f%%)\n",
           i, rr_stats.backends[i].request_count,
           rr_stats.backends[i].percentage);
}
```

### Real-time Monitoring
```c
lb_enable_rr_monitoring(lb, true);
lb_set_rr_callback(lb, on_request_distributed, NULL);

void on_request_distributed(lb_backend_t *backend, void *user_data) {
    printf("Request sent to %s:%d\n", backend->host, backend->port);
}
```

## Performance Tuning

### Optimal Settings
```ini
[round_robin]
distribution_buffer_size = 1024
weight_calculation_interval = 1000
smooth_transition = true
failover_threshold = 3
```

### Load Testing
```bash
# Test round robin distribution
curl -s "http://localhost:8080/test" | grep "Server-ID"
curl -s "http://localhost:8080/test" | grep "Server-ID"
curl -s "http://localhost:8080/test" | grep "Server-ID"
```

## Troubleshooting

### Uneven Distribution
- Check backend weights
- Verify health check status
- Monitor connection limits
- Review sticky session settings

### Backend Failures
- Enable backup servers
- Configure proper health checks
- Set appropriate retry limits
- Monitor failover logs