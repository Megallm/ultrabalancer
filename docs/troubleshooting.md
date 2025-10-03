# 🔧 UltraBalancer Troubleshooting Guide

## Table of Contents
1. [Common Issues](#common-issues)
2. [Debugging Tools](#debugging-tools)
3. [Error Messages](#error-messages)
4. [Performance Issues](#performance-issues)
5. [Connection Problems](#connection-problems)
6. [Configuration Issues](#configuration-issues)
7. [Health Check Failures](#health-check-failures)
8. [Memory Issues](#memory-issues)
9. [Advanced Debugging](#advanced-debugging)

## Common Issues

### Service Won't Start

#### Symptom
```bash
$ systemctl start ultrabalancer
Job for ultrabalancer.service failed because the control process exited with error code.
```

#### Diagnosis
```bash
# Check system logs
journalctl -u ultrabalancer -n 50

# Check configuration
ultrabalancer -c -f /etc/ultrabalancer/config.yaml

# Check permissions
ls -la /etc/ultrabalancer/
ls -la /var/run/ultrabalancer/
```

#### Common Causes

1. **Port Already in Use**
```bash
# Check if port is in use
ss -tlnp | grep :80
lsof -i :80
```

Solution:
```bash
# Kill process using port
kill -9 $(lsof -t -i:80)
# Or change port in config
```

2. **Configuration Syntax Error**
```yaml
# Fix: Validate YAML syntax
yamllint /etc/ultrabalancer/config.yaml
```

3. **Insufficient Permissions**
```bash
# Fix: Run with proper permissions
sudo chown -R ultrabalancer:ultrabalancer /etc/ultrabalancer
sudo chmod 640 /etc/ultrabalancer/config.yaml
```

### High Memory Usage

#### Diagnosis
```bash
# Memory profiling
pmap -x $(pidof ultrabalancer)

# Check for memory leaks
valgrind --leak-check=full ultrabalancer -f config.yaml

# Monitor memory growth
while true; do
    ps aux | grep ultrabalancer | grep -v grep
    sleep 5
done
```

#### Solutions

1. **Tune Connection Pools**
```yaml
connection_pools:
  default:
    max_idle: 50  # Reduce from 100
    idle_timeout: 60s  # Reduce from 300s
```

2. **Adjust Buffer Sizes**
```yaml
global:
  tune.bufsize: 8192  # Reduce from 32768
```

3. **Enable Memory Limits**
```bash
# systemd service
[Service]
MemoryMax=2G
MemoryHigh=1.5G
```

### CPU Spikes

#### Diagnosis
```bash
# CPU profiling
perf top -p $(pidof ultrabalancer)

# Thread analysis
top -H -p $(pidof ultrabalancer)

# System call tracing
strace -c -p $(pidof ultrabalancer)
```

#### Common Causes

1. **Busy Loop in Health Checks**
```yaml
# Fix: Increase check interval
backend servers:
  server web1 192.168.1.10:8080 check inter 5000  # 5s instead of 1s
```

2. **Inefficient ACL Rules**
```yaml
# Optimize regex patterns
acl:
  # Bad: Complex regex
  bad_pattern: path_reg ^/api/v[0-9]+/users/[0-9]+/.*

  # Good: Simple patterns
  good_pattern: path_beg /api/
```

## Debugging Tools

### Built-in Debug Mode

```bash
# Enable debug logging
ultrabalancer -d -f config.yaml

# Verbose mode
ultrabalancer -vvv -f config.yaml
```

### Debug Configuration

```yaml
global:
  debug: true
  log-level: debug

  # Enable core dumps
  tune.core.enabled: true
  tune.core.unlimited: true

  # Debug stats
  stats:
    enabled: true
    uri: /debug/stats
    refresh: 1s
```

### GDB Debugging

```bash
# Attach to running process
gdb -p $(pidof ultrabalancer)

# Common commands
(gdb) thread apply all bt       # All thread backtraces
(gdb) info threads              # List threads
(gdb) thread 2                  # Switch to thread 2
(gdb) frame 3                   # Switch to frame 3
(gdb) print *connection         # Print structure
(gdb) watch connection->state   # Watch variable changes
```

### SystemTap Scripts

```stap
# connection_tracking.stp
probe process("ultrabalancer").function("accept_connection") {
    printf("New connection from %s\n", user_string($addr))
}

probe process("ultrabalancer").function("close_connection") {
    printf("Closing connection %d\n", $fd)
}
```

## Error Messages

### Connection Errors

#### "Too many open files"
```bash
# Check current limits
ulimit -n

# Fix: Increase file descriptor limit
ulimit -n 100000

# Permanent fix in /etc/security/limits.conf
* soft nofile 100000
* hard nofile 100000
```

#### "Cannot allocate memory"
```bash
# Check memory usage
free -h
cat /proc/meminfo

# Fix: Reduce connection limits
global:
  maxconn: 50000  # Reduce from 100000
```

#### "Connection refused"
```bash
# Check if service is listening
ss -tlnp | grep ultrabalancer

# Check firewall rules
iptables -L -n
firewall-cmd --list-all

# Fix: Open firewall ports
firewall-cmd --permanent --add-port=80/tcp
firewall-cmd --reload
```

### Backend Errors

#### "No servers available"
```bash
# Check server status
echo "show servers state" | socat - /var/run/ultrabalancer.sock

# Manual health check
curl -I http://192.168.1.10:8080/health

# Fix: Enable servers
echo "enable server backend/server1" | socat - /var/run/ultrabalancer.sock
```

#### "503 Service Unavailable"
Diagnosis:
```bash
# Check backend health
curl http://localhost:8080/stats | jq '.backends'

# Check logs for health check failures
grep "Health check failed" /var/log/ultrabalancer.log
```

Solution:
```yaml
backend servers:
  # Increase health check tolerance
  option httpchk GET /health
  server web1 192.168.1.10:8080 check rise 2 fall 5
```

## Performance Issues

### Slow Response Times

#### Diagnosis
```bash
# Trace request flow
tcpdump -i any -w trace.pcap host 192.168.1.10
wireshark trace.pcap  # Analyze in Wireshark

# Check latency
curl -w "@curl-format.txt" -o /dev/null -s http://localhost/
```

curl-format.txt:
```
time_namelookup:  %{time_namelookup}\n
time_connect:  %{time_connect}\n
time_appconnect:  %{time_appconnect}\n
time_pretransfer:  %{time_pretransfer}\n
time_redirect:  %{time_redirect}\n
time_starttransfer:  %{time_starttransfer}\n
time_total:  %{time_total}\n
```

#### Solutions

1. **Enable TCP optimizations**
```yaml
global:
  tune.tcp.nodelay: true
  tune.tcp.quickack: true
```

2. **Reduce buffer copies**
```yaml
global:
  option splice-auto
  option splice-request
  option splice-response
```

### Connection Timeouts

#### Diagnosis
```bash
# Check timeout settings
grep -i timeout /etc/ultrabalancer/config.yaml

# Monitor connection states
ss -tan state time-wait | wc -l
ss -tan state close-wait | wc -l
```

#### Solutions
```yaml
global:
  # Adjust timeouts
  timeout:
    connect: 10s    # Increase from 5s
    client: 60s     # Increase from 30s
    server: 60s     # Increase from 30s
    queue: 60s      # Increase from 30s
```

## Connection Problems

### SSL/TLS Issues

#### "SSL handshake failure"
```bash
# Test SSL connection
openssl s_client -connect localhost:443 -showcerts

# Check cipher support
nmap --script ssl-enum-ciphers -p 443 localhost

# Fix: Update SSL configuration
```

```yaml
frontend https:
  bind 0.0.0.0:443 ssl crt /etc/ssl/cert.pem

  # Modern SSL configuration
  ssl-default-bind-ciphers ECDHE+AESGCM:ECDHE+CHACHA20
  ssl-default-bind-options no-sslv3 no-tlsv10 no-tlsv11
```

### WebSocket Issues

#### "WebSocket connection failed"
```bash
# Test WebSocket
wscat -c ws://localhost/ws

# Check headers
curl -H "Connection: Upgrade" \
     -H "Upgrade: websocket" \
     -H "Sec-WebSocket-Version: 13" \
     -H "Sec-WebSocket-Key: test" \
     http://localhost/ws
```

Solution:
```yaml
frontend web:
  # WebSocket support
  acl is_websocket hdr(Upgrade) -i websocket
  use_backend websocket_backend if is_websocket

  timeout tunnel 1h  # Long timeout for WebSocket
```

## Configuration Issues

### Validation Errors

```bash
# Validate configuration
ultrabalancer -c -f config.yaml

# Common validation errors:
# - Unknown keyword
# - Invalid value
# - Missing required field
```

### Hot Reload Failures

```bash
# Check if reload is possible
kill -USR1 $(pidof ultrabalancer)  # Check config
kill -USR2 $(pidof ultrabalancer)  # Reload if valid

# Monitor reload
tail -f /var/log/ultrabalancer.log
```

## Health Check Failures

### Debugging Health Checks

```bash
# Enable health check logging
global:
  log-health-checks: true

# Manual health check test
curl -v http://backend-server:8080/health

# Monitor health check packets
tcpdump -i any -s0 -A 'port 8080 and host backend-server'
```

### Common Health Check Issues

1. **Incorrect HTTP version**
```yaml
# Fix: Specify HTTP version
option httpchk GET /health HTTP/1.1\r\nHost:\ localhost
```

2. **Authentication required**
```yaml
# Fix: Add authentication header
http-check send hdr Authorization "Bearer token"
```

3. **Wrong response code expected**
```yaml
# Fix: Accept multiple codes
http-check expect status 200-399
```

## Memory Issues

### Memory Leak Detection

```bash
# Using AddressSanitizer
gcc -fsanitize=address -g ultrabalancer.c
ASAN_OPTIONS=detect_leaks=1 ./ultrabalancer

# Using Valgrind
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --verbose \
         --log-file=valgrind.log \
         ultrabalancer

# Using Heaptrack
heaptrack ultrabalancer
heaptrack --analyze heaptrack.ultrabalancer.*.gz
```

### Memory Profiling (C++23)

```cpp
class MemoryProfiler {
    struct Allocation {
        void* ptr;
        size_t size;
        std::source_location loc;
        std::chrono::time_point<std::chrono::steady_clock> time;
    };

    static inline std::unordered_map<void*, Allocation> allocations;
    static inline std::mutex mutex;

public:
    static void* track_alloc(size_t size,
                           std::source_location loc = std::source_location::current()) {
        void* ptr = malloc(size);

        std::lock_guard lock(mutex);
        allocations[ptr] = {ptr, size, loc, std::chrono::steady_clock::now()};

        return ptr;
    }

    static void track_free(void* ptr) {
        std::lock_guard lock(mutex);
        allocations.erase(ptr);
        free(ptr);
    }

    static void report() {
        std::lock_guard lock(mutex);

        size_t total = 0;
        for (const auto& [ptr, alloc] : allocations) {
            std::cout << std::format("Leak: {} bytes at {}:{}\n",
                                    alloc.size,
                                    alloc.loc.file_name(),
                                    alloc.loc.line());
            total += alloc.size;
        }

        std::cout << std::format("Total leaked: {} bytes\n", total);
    }
};

#define MALLOC(size) MemoryProfiler::track_alloc(size)
#define FREE(ptr) MemoryProfiler::track_free(ptr)
```

## Advanced Debugging

### Core Dump Analysis

```bash
# Enable core dumps
ulimit -c unlimited
echo "/tmp/core-%e-%p-%t" > /proc/sys/kernel/core_pattern

# Analyze core dump
gdb ultrabalancer /tmp/core-ultrabalancer-12345-1234567890

(gdb) bt full              # Full backtrace
(gdb) info registers       # Register values
(gdb) x/100x $rsp         # Stack contents
(gdb) disas $rip-32,$rip+32  # Disassembly around crash
```

### Network Tracing

```bash
# Capture all traffic
tcpdump -i any -w capture.pcap

# Filter specific traffic
tcpdump -i any 'tcp port 80 and (tcp[tcpflags] & tcp-syn != 0)'

# Analyze with tshark
tshark -r capture.pcap -Y "http.request" -T fields \
       -e ip.src -e http.request.method -e http.request.uri
```

### Performance Profiling

```bash
# CPU profiling with perf
perf record -g -p $(pidof ultrabalancer)
perf report

# Flame graphs
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg

# Memory profiling
perf record -e task-clock,page-faults,cache-misses -p $(pidof ultrabalancer)
perf report
```

### Custom Debug Functions (C23)

```c
#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) \
    do { \
        fprintf(stderr, "[%s:%d:%s] " fmt "\n", \
                __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    } while (0)

#define DEBUG_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "Assertion failed: %s\n", msg); \
            abort(); \
        } \
    } while (0)

#define DEBUG_TRACE_ENTER() \
    DEBUG_PRINT("Entering function")

#define DEBUG_TRACE_EXIT() \
    DEBUG_PRINT("Exiting function")
#else
#define DEBUG_PRINT(fmt, ...)
#define DEBUG_ASSERT(cond, msg)
#define DEBUG_TRACE_ENTER()
#define DEBUG_TRACE_EXIT()
#endif
```

## Recovery Procedures

### Emergency Restart

```bash
#!/bin/bash
# emergency_restart.sh

# Kill all instances
killall -9 ultrabalancer

# Clear shared memory
ipcrm -a

# Clear socket files
rm -f /var/run/ultrabalancer.sock

# Reset network settings
sysctl -w net.core.somaxconn=65535
sysctl -w net.ipv4.tcp_tw_reuse=1

# Start with minimal config
ultrabalancer -f /etc/ultrabalancer/minimal.yaml
```

### Rollback Procedure

```bash
#!/bin/bash
# rollback.sh

VERSION=$1
BACKUP_DIR="/var/backups/ultrabalancer"

# Stop service
systemctl stop ultrabalancer

# Restore configuration
cp $BACKUP_DIR/config-$VERSION.yaml /etc/ultrabalancer/config.yaml

# Restore binary
cp $BACKUP_DIR/ultrabalancer-$VERSION /usr/sbin/ultrabalancer

# Start service
systemctl start ultrabalancer

# Verify
sleep 5
curl -f http://localhost/health || {
    echo "Rollback failed! Check logs."
    exit 1
}
```

## Getting Help

### Log Collection Script

```bash
#!/bin/bash
# collect_debug_info.sh

OUTPUT_DIR="/tmp/ultrabalancer-debug-$(date +%Y%m%d-%H%M%S)"
mkdir -p $OUTPUT_DIR

# Collect logs
journalctl -u ultrabalancer -n 10000 > $OUTPUT_DIR/journal.log
cp /var/log/ultrabalancer.log $OUTPUT_DIR/ 2>/dev/null

# Collect configuration
cp /etc/ultrabalancer/config.yaml $OUTPUT_DIR/

# Collect system info
uname -a > $OUTPUT_DIR/system.txt
free -h >> $OUTPUT_DIR/system.txt
df -h >> $OUTPUT_DIR/system.txt
ps aux | grep ultrabalancer >> $OUTPUT_DIR/system.txt

# Collect network info
ss -s > $OUTPUT_DIR/network.txt
ss -tlnp >> $OUTPUT_DIR/network.txt
ip a >> $OUTPUT_DIR/network.txt

# Create archive
tar czf $OUTPUT_DIR.tar.gz $OUTPUT_DIR/
echo "Debug info collected: $OUTPUT_DIR.tar.gz"
```

### Community Resources

- GitHub Issues: https://github.com/Bas3line/ultrabalancer/issues
- Discord: https://discord.gg/ultrabalancer
- Stack Overflow: Tag `ultrabalancer`
- Documentation: https://docs.ultrabalancer.io