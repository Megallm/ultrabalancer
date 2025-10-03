# How UltraBalancer Works

## Architecture Deep Dive

UltraBalancer is a high-performance layer 4/7 load balancer designed to handle millions of concurrent connections with minimal latency. Here's how it achieves HAProxy-level performance:

## 1. Network I/O Architecture

### Event-Driven Model
```
Client Connection → epoll → Worker Thread → Backend Selection → Server Connection
```

UltraBalancer uses **epoll** (Linux) for scalable event notification:
- Single epoll instance per worker thread
- Edge-triggered mode for efficiency
- Non-blocking I/O on all sockets

### Zero-Copy Data Transfer
Instead of copying data through userspace:
```c
// Traditional approach (slow):
read(client_fd, buffer, size);
write(server_fd, buffer, size);

// UltraBalancer approach (fast):
splice(client_fd, NULL, pipe[1], NULL, size, SPLICE_F_MOVE);
splice(pipe[0], NULL, server_fd, NULL, size, SPLICE_F_MOVE);
```

This moves data directly in kernel space without copying to userspace.

## 2. Connection Flow

### TCP Load Balancing
```
1. Client connects to frontend port
2. Accept connection in worker thread
3. Select backend using algorithm
4. Establish backend connection
5. Splice data bidirectionally
6. Handle connection close
```

### HTTP Load Balancing
```
1. Accept client connection
2. Parse HTTP request headers
3. Apply ACL rules
4. Select backend based on:
   - URL path
   - Headers
   - Cookies
   - Source IP
5. Modify headers (add X-Forwarded-For, etc.)
6. Forward to backend
7. Parse response
8. Apply response rules
9. Send to client
```

## 3. Load Balancing Algorithms

### Round-Robin
```c
backend = backends[counter++ % backend_count];
```

### Least Connections
```c
for each backend:
    if backend.active_conns < min_conns:
        selected = backend
```

### Consistent Hashing
- Uses MurmurHash3 for distribution
- Virtual nodes for better balance
- Minimal disruption on backend changes

### Source IP Hash
```c
hash = client_ip
backend = backends[hash % backend_count]
```

## 4. Session Persistence (Stick Tables)

Stick tables maintain client-server affinity:

```
Client IP → Hash → Bucket → Entry → Server ID
```

Features:
- Configurable expiration
- Multiple data types (counters, rates, etc.)
- Replication support

## 5. Health Checking

### Check Flow
```
1. Timer triggers check
2. Connect to backend
3. Send check data (TCP/HTTP)
4. Verify response
5. Update server state
6. Adjust check interval
```

States:
- **UP**: Server healthy, accepting traffic
- **DOWN**: Failed checks exceeded threshold
- **DRAIN**: Gracefully removing from rotation
- **MAINT**: Maintenance mode

## 6. HTTP Processing

### Request Pipeline
```
Raw Data → Parser → Headers → ACLs → Backend → Modify → Forward
```

### HTTP/2 Support
- Multiplexing multiple streams
- Header compression (HPACK)
- Server push capability
- Priority handling

## 7. ACL Engine

Pattern matching for routing decisions:

```
acl is_api path_beg /api/
acl is_secure src 10.0.0.0/8
use_backend api_servers if is_api is_secure
```

ACL Types:
- Path matching (exact, prefix, suffix, regex)
- Header matching
- Source IP matching
- Method matching
- SSL/TLS properties

## 8. Caching Layer

### Cache Key Generation
```
Method + Host + URI + Vary Headers = Cache Key
```

### Cache Storage
```
Hash Table → Bucket → Entry List → LRU Management
```

### Eviction Policy
- LRU (Least Recently Used)
- TTL expiration
- Size limits

## 9. SSL/TLS Termination

### Handshake Flow
```
1. Client Hello with SNI
2. Select certificate based on SNI
3. Complete handshake
4. Decrypt client data
5. Forward to backend (plain or re-encrypted)
6. Encrypt response
7. Send to client
```

Features:
- SNI-based routing
- ALPN for protocol negotiation
- Session resumption
- OCSP stapling

## 10. Memory Management

### Custom Memory Pools
```c
Memory Pool → Chunks → Free List → Allocation
```

Benefits:
- Reduced malloc overhead
- Better cache locality
- Predictable performance

### Lock-Free Structures
- Atomic operations for counters
- Spinlocks for short critical sections
- RCU for read-heavy structures

## 11. Thread Architecture

### Worker Thread Model
```
Main Thread
    ├── Worker Thread 1 → epoll → Connections
    ├── Worker Thread 2 → epoll → Connections
    ├── Worker Thread N → epoll → Connections
    ├── Health Check Thread
    └── Stats Thread
```

### CPU Affinity
Each worker thread is pinned to a CPU core:
```c
CPU_SET(cpu_id, &cpuset);
pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
```

## 12. Statistics & Monitoring

### Metrics Collection
```
Per-Request → Atomic Counters → Aggregation → Export
```

### Export Formats
- JSON API
- Prometheus metrics
- CSV stats
- HTML dashboard

## 13. Configuration Processing

### Parse Flow
```
Config File → Lexer → Parser → Validation → Runtime Structure
```

### Hot Reload
```
1. Parse new config
2. Validate configuration
3. Create new proxy structures
4. Start accepting on new config
5. Drain old connections
6. Free old structures
```

## 14. Performance Optimizations

### System Tuning
- **TCP_NODELAY**: Disable Nagle's algorithm
- **SO_REUSEPORT**: Multiple threads on same port
- **TCP_DEFER_ACCEPT**: Accept only with data
- **Huge Pages**: Reduced TLB misses

### Compiler Optimizations
- Link-time optimization (LTO)
- Profile-guided optimization
- CPU-specific instructions

## 15. Request Flow Example

Let's trace a request through the system:

```
1. Client connects to port 80
   ↓
2. Worker thread epoll_wait() returns
   ↓
3. Accept connection, set non-blocking
   ↓
4. Read HTTP request headers
   ↓
5. Parse request line: "GET /api/users HTTP/1.1"
   ↓
6. Match ACL: path_beg /api/ → true
   ↓
7. Select backend pool: api_servers
   ↓
8. Choose server: least connections → server2
   ↓
9. Check stick table for existing session
   ↓
10. Connect to backend server2:3000
    ↓
11. Add headers: X-Forwarded-For, X-Real-IP
    ↓
12. Send request to backend
    ↓
13. Read response headers
    ↓
14. Check if cacheable (GET, 200 OK)
    ↓
15. Stream response to client using splice()
    ↓
16. Update statistics
    ↓
17. Store in cache if applicable
    ↓
18. Close connections or keep-alive
```

## Performance Characteristics

### Throughput
- 100,000+ requests/second per core
- 1,000,000+ concurrent connections
- 10 Gbps+ data throughput

### Latency
- < 100μs added latency
- < 1ms health check response
- < 10ms configuration reload

### Resource Usage
- ~50MB base memory
- ~1KB per connection
- < 5% CPU at 10K req/s

## Comparison with HAProxy

| Feature | UltraBalancer | HAProxy |
|---------|--------------|---------|
| Zero-copy networking | ✓ splice() | ✓ splice() |
| HTTP/2 | ✓ | ✓ |
| ACLs | ✓ | ✓ |
| Stick tables | ✓ | ✓ |
| Health checks | ✓ | ✓ |
| SSL/TLS | ✓ | ✓ |
| Hot reload | ✓ | ✓ |
| Lua scripting | ✗ | ✓ |
| QUIC/HTTP3 | ✗ | Experimental |

## Bottlenecks & Solutions

### Connection Acceptance
**Problem**: Single accept thread bottleneck
**Solution**: SO_REUSEPORT for multiple accept threads

### Lock Contention
**Problem**: Global locks cause contention
**Solution**: Per-CPU structures, lock-free algorithms

### Memory Allocation
**Problem**: malloc() overhead
**Solution**: Custom memory pools

### Cache Misses
**Problem**: Random memory access
**Solution**: Cache-aligned structures, prefetching

## Future Enhancements

1. **io_uring** support for even better performance
2. **eBPF** for programmable data plane
3. **QUIC/HTTP3** support
4. **Distributed** mode for horizontal scaling
5. **Machine learning** for predictive load balancing