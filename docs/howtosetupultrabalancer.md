# How to Setup UltraBalancer

## Quick Installation

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt update
sudo apt install build-essential libssl-dev libpcre3-dev zlib1g-dev \
                 libbrotli-dev libjemalloc-dev pkg-config

# CentOS/RHEL
sudo yum groupinstall "Development Tools"
sudo yum install openssl-devel pcre-devel zlib-devel brotli-devel \
                 jemalloc-devel

# macOS
brew install openssl pcre zlib brotli jemalloc
```

### Build from Source
```bash
git clone https://github.com/ultrabalancer/ultrabalancer.git
cd ultrabalancer
make clean
make all
sudo make install
```

## Configuration

### Basic Configuration File
```ini
# /etc/ultrabalancer/ultrabalancer.cfg

[global]
port = 8080
ssl_port = 8443
stats_port = 9090
worker_processes = auto
max_connections = 10000
log_level = info
log_file = /var/log/ultrabalancer.log
pid_file = /var/run/ultrabalancer.pid

[algorithm]
type = round_robin
sticky_sessions = false

[health_check]
enabled = true
interval = 5000
timeout = 2000
retries = 3
path = /health
expected_status = 200

[ssl]
enabled = false
cert_file = /etc/ssl/certs/ultrabalancer.crt
key_file = /etc/ssl/private/ultrabalancer.key
protocols = TLSv1.2,TLSv1.3
ciphers = ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256

[backend "web1"]
server = 192.168.1.10:3000
weight = 100
max_connections = 500
active = true
backup = false

[backend "web2"]
server = 192.168.1.11:3000
weight = 100
max_connections = 500
active = true
backup = false

[backend "web3"]
server = 192.168.1.12:3000
weight = 150
max_connections = 750
active = true
backup = false

[backend "backup1"]
server = 192.168.1.20:3000
weight = 50
max_connections = 250
active = true
backup = true
```

## Service Management

### Systemd Service
```ini
# /etc/systemd/system/ultrabalancer.service

[Unit]
Description=UltraBalancer High-Performance Load Balancer
After=network.target
Wants=network.target

[Service]
Type=forking
User=ultrabalancer
Group=ultrabalancer
ExecStartPre=/usr/local/bin/ultrabalancer -t
ExecStart=/usr/local/bin/ultrabalancer -f /etc/ultrabalancer/ultrabalancer.cfg
ExecReload=/bin/kill -USR1 $MAINPID
ExecStop=/bin/kill -TERM $MAINPID
PIDFile=/var/run/ultrabalancer.pid
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
LimitNPROC=32768

[Install]
WantedBy=multi-user.target
```

### Service Commands
```bash
# Create user
sudo useradd -r -s /bin/false ultrabalancer

# Set permissions
sudo mkdir -p /etc/ultrabalancer /var/log/ultrabalancer /var/run/ultrabalancer
sudo chown ultrabalancer:ultrabalancer /var/log/ultrabalancer /var/run/ultrabalancer
sudo chmod 755 /etc/ultrabalancer

# Enable and start service
sudo systemctl daemon-reload
sudo systemctl enable ultrabalancer
sudo systemctl start ultrabalancer

# Check status
sudo systemctl status ultrabalancer

# View logs
sudo journalctl -u ultrabalancer -f

# Reload configuration
sudo systemctl reload ultrabalancer
```

## Load Balancing Algorithms

### Round Robin
```ini
[algorithm]
type = round_robin
smooth_weighted = true
```

### Least Connections
```ini
[algorithm]
type = least_connections
connection_threshold = 100
```

### IP Hash
```ini
[algorithm]
type = ip_hash
hash_key = source_ip
consistent_hashing = true
```

### Weighted Round Robin
```ini
[algorithm]
type = weighted_round_robin
dynamic_weights = true
response_time_factor = 0.3
```

## Advanced Features

### SSL/TLS Configuration
```ini
[ssl]
enabled = true
cert_file = /etc/ssl/certs/domain.crt
key_file = /etc/ssl/private/domain.key
ca_file = /etc/ssl/certs/ca.crt
protocols = TLSv1.2,TLSv1.3
ciphers = ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256
honor_cipher_order = true
session_cache = shared:SSL:10m
session_timeout = 10m
stapling = true
stapling_verify = true
```

### Rate Limiting
```ini
[rate_limit]
enabled = true
requests_per_second = 1000
burst_size = 100
window_size = 60
block_duration = 300
whitelist = 127.0.0.1,10.0.0.0/8
```

### Health Monitoring
```ini
[health_check]
enabled = true
interval = 5000
timeout = 2000
retries = 3
rise = 2
fall = 3
path = /health
method = GET
expected_status = 200
expected_content = "OK"
headers = "X-Health-Check: true"
```

### Logging Configuration
```ini
[logging]
level = info
file = /var/log/ultrabalancer.log
format = combined
rotate = true
max_size = 100M
max_files = 10
compress = true
access_log = /var/log/ultrabalancer_access.log
error_log = /var/log/ultrabalancer_error.log
```

## Performance Tuning

### System Optimizations
```bash
# Increase file descriptor limits
echo "ultrabalancer soft nofile 65536" >> /etc/security/limits.conf
echo "ultrabalancer hard nofile 65536" >> /etc/security/limits.conf

# Network optimizations
echo "net.core.somaxconn = 65536" >> /etc/sysctl.conf
echo "net.core.rmem_max = 16777216" >> /etc/sysctl.conf
echo "net.core.wmem_max = 16777216" >> /etc/sysctl.conf
echo "net.ipv4.tcp_rmem = 4096 87380 16777216" >> /etc/sysctl.conf
echo "net.ipv4.tcp_wmem = 4096 65536 16777216" >> /etc/sysctl.conf
echo "net.ipv4.tcp_fin_timeout = 30" >> /etc/sysctl.conf
echo "net.ipv4.tcp_keepalive_time = 120" >> /etc/sysctl.conf
sudo sysctl -p
```

### UltraBalancer Performance Settings
```ini
[performance]
worker_processes = auto
worker_connections = 2048
worker_rlimit_nofile = 65536
multi_accept = on
use_epoll = true
sendfile = on
tcp_nopush = on
tcp_nodelay = on
keepalive_timeout = 65
keepalive_requests = 100000
client_body_timeout = 12
client_header_timeout = 12
send_timeout = 10
```

## Monitoring and Statistics

### Statistics API
```ini
[stats]
enabled = true
port = 9090
bind = 127.0.0.1
username = admin
password = secure_password
uri = /stats
format = json
refresh_interval = 5
```

### Prometheus Integration
```ini
[prometheus]
enabled = true
port = 9091
path = /metrics
include_backend_metrics = true
include_system_metrics = true
```

## Command Line Usage

### Basic Commands
```bash
# Start with config file
ultrabalancer -f /etc/ultrabalancer/ultrabalancer.cfg

# Test configuration
ultrabalancer -t -f /etc/ultrabalancer/ultrabalancer.cfg

# Start in foreground
ultrabalancer -D -f /etc/ultrabalancer/ultrabalancer.cfg

# Show version
ultrabalancer -v

# Show help
ultrabalancer -h

# Reload configuration
kill -USR1 $(cat /var/run/ultrabalancer.pid)

# Graceful shutdown
kill -TERM $(cat /var/run/ultrabalancer.pid)
```

### Runtime Management
```bash
# Add backend dynamically
curl -X POST http://localhost:9090/backends \
  -H "Content-Type: application/json" \
  -d '{"host": "192.168.1.13", "port": 3000, "weight": 100}'

# Remove backend
curl -X DELETE http://localhost:9090/backends/web4

# Update backend weight
curl -X PATCH http://localhost:9090/backends/web1 \
  -H "Content-Type: application/json" \
  -d '{"weight": 200}'

# Get statistics
curl http://localhost:9090/stats

# Health check
curl http://localhost:9090/health
```

## Troubleshooting

### Common Issues

#### Backend Connection Failures
```bash
# Check backend connectivity
telnet 192.168.1.10 3000

# Check health check logs
tail -f /var/log/ultrabalancer.log | grep health

# Verify backend health endpoint
curl http://192.168.1.10:3000/health
```

#### Performance Issues
```bash
# Check system resources
top -p $(pgrep ultrabalancer)

# Monitor connections
ss -tuln | grep :8080

# Check file descriptor usage
lsof -p $(pgrep ultrabalancer) | wc -l
```

#### Configuration Errors
```bash
# Validate configuration
ultrabalancer -t -f /etc/ultrabalancer/ultrabalancer.cfg

# Check syntax
ultrabalancer -T -f /etc/ultrabalancer/ultrabalancer.cfg

# View error logs
tail -f /var/log/ultrabalancer_error.log
```

## Security Hardening

### Access Control
```ini
[security]
allow_ips = 127.0.0.1,10.0.0.0/8,192.168.0.0/16
deny_ips =
max_request_size = 1M
timeout_client = 30
timeout_server = 30
hide_version = true
server_tokens = off
```

### DDoS Protection
```ini
[ddos_protection]
enabled = true
max_connections_per_ip = 50
max_requests_per_second = 100
block_duration = 600
detection_threshold = 10
```