# 🗄️ Database Load Balancing with UltraBalancer

## Overview

UltraBalancer now includes intelligent database load balancing with connection pooling, automatic read/write splitting, protocol-aware routing, and health-based failover. This allows you to maximize database throughput, reduce connection overhead, and ensure high availability without modifying application code.

## Key Features

### 🔄 Read/Write Splitting
Automatically routes queries to appropriate backends:
- **Primary (Write)** - `INSERT`, `UPDATE`, `DELETE`, `BEGIN`, `SET`
- **Replicas (Read)** - `SELECT`, `SHOW` queries
- **Intelligent Fallback** - Routes to primary if no healthy replicas

### 🔌 Connection Pooling
Reduces connection overhead dramatically:
- **Persistent Connections** - Reuse database connections across requests
- **Lazy Connection Creation** - Only creates connections when needed
- **Automatic Cleanup** - Removes stale and idle connections
- **Per-Backend Pools** - Separate pools for primary and replicas

### 🛡️ Health Checks & Failover
Continuous monitoring with automatic failover:
- **TCP Health Checks** - Verify backend connectivity
- **Replication Lag Detection** - Monitor replica lag (PostgreSQL/MySQL)
- **Automatic Demotion** - Removes lagging replicas (>5s default)
- **Auto-Recovery** - Promotes backends when healthy

### 📊 Protocol Support
Native protocol parsers for:
- **PostgreSQL** - Wire protocol with query detection
- **MySQL** - MySQL protocol with command parsing
- **Redis** - RESP protocol with command routing

### 🧠 Session Awareness
Transaction and session sticky routing:
- **Transaction Tracking** - Keeps `BEGIN...COMMIT` on same backend
- **Session Variables** - Routes `SET` statements consistently
- **Session Pooling** - Maintains client-to-backend affinity

---

## Quick Start

### PostgreSQL Setup

#### 1. Configure Primary and Replicas

**Primary (192.168.1.10:5432)**
```bash
ultrabalancer -p 5432 \
  --db-mode postgres \
  --db-primary 192.168.1.10:5432 \
  --db-replica 192.168.1.11:5432 \
  --db-replica 192.168.1.12:5432 \
  --db-pool-size 200 \
  --db-health-check-interval 10000
```

#### 2. Connect Your Application
```python
import psycopg2

conn = psycopg2.connect(
    host="localhost",
    port=5432,
    database="mydb",
    user="myuser",
    password="mypass"
)

cursor = conn.cursor()
cursor.execute("SELECT * FROM users")
rows = cursor.fetchall()

cursor.execute("UPDATE users SET active=true WHERE id=1")
conn.commit()
```

**What happens:**
- `SELECT` → Routed to replica (192.168.1.11 or .12 with least connections)
- `UPDATE` → Routed to primary (192.168.1.10)
- All transparent to application!

---

### MySQL Setup

#### 1. Start UltraBalancer for MySQL

```bash
ultrabalancer -p 3306 \
  --db-mode mysql \
  --db-primary 192.168.1.20:3306 \
  --db-replica 192.168.1.21:3306 \
  --db-replica 192.168.1.22:3306 \
  --db-max-lag 3000 \
  --db-pool-min-idle 10 \
  --db-pool-max-idle 50
```

#### 2. Connect Your Application
```javascript
const mysql = require('mysql2/promise');

const pool = mysql.createPool({
  host: 'localhost',
  port: 3306,
  user: 'root',
  password: 'password',
  database: 'mydb',
  waitForConnections: true,
  connectionLimit: 10
});

const [rows] = await pool.query('SELECT * FROM products');
await pool.query('INSERT INTO orders (user_id, total) VALUES (?, ?)', [123, 99.99]);
```

**Benefits:**
- **10x Connection Capacity** - MySQL connections are expensive (each ~256KB memory)
- **Lower Latency** - Connection reuse eliminates handshake overhead
- **Load Distribution** - Reads spread across replicas

---

### Redis Setup

#### 1. Configure Redis Cluster

```bash
ultrabalancer -p 6379 \
  --db-mode redis \
  --db-primary 192.168.1.30:6379 \
  --db-replica 192.168.1.31:6379 \
  --db-replica 192.168.1.32:6379 \
  --db-pool-size 500
```

#### 2. Connect Your Application
```python
import redis

r = redis.Redis(host='localhost', port=6379, decode_responses=True)

r.set('user:123', 'John Doe')
value = r.get('user:123')

r.lpush('queue:tasks', 'task1', 'task2')
task = r.rpop('queue:tasks')
```

**Routing:**
- `GET`, `MGET`, `HGETALL` → Replicas
- `SET`, `LPUSH`, `HSET` → Primary
- Pipelining and transactions supported

---

## Configuration Reference

### CLI Options

```bash
--db-mode <protocol>
  Protocol: postgres, mysql, redis
  Default: none

--db-primary <host:port>
  Primary database backend (required)

--db-replica <host:port>
  Read replica backend (can specify multiple)

--db-pool-size <num>
  Maximum connections in pool
  Default: 200

--db-pool-min-idle <num>
  Minimum idle connections to maintain
  Default: 10

--db-pool-max-idle <num>
  Maximum idle connections to keep
  Default: 50

--db-pool-max-lifetime <seconds>
  Maximum connection lifetime
  Default: 3600 (1 hour)

--db-pool-idle-timeout <seconds>
  Idle connection timeout
  Default: 300 (5 minutes)

--db-health-check-interval <ms>
  Health check interval in milliseconds
  Default: 10000 (10 seconds)

--db-health-check-timeout <ms>
  Health check timeout
  Default: 5000 (5 seconds)

--db-max-lag <ms>
  Maximum replication lag before demotion
  Default: 5000 (5 seconds)

--db-max-sessions <num>
  Maximum concurrent sessions
  Default: 10000
```

### Configuration File (YAML)

```yaml
database:
  mode: postgres

  primary:
    host: 192.168.1.10
    port: 5432

  replicas:
    - host: 192.168.1.11
      port: 5432
    - host: 192.168.1.12
      port: 5432

  pool:
    max_connections: 200
    min_idle: 10
    max_idle: 50
    max_lifetime: 3600s
    idle_timeout: 300s

  health_check:
    interval: 10s
    timeout: 5s
    max_replication_lag: 5s

  routing:
    read_preference: least_connections
    write_consistency: primary_only
    transaction_sticky: true
```

---

## Architecture

### Connection Flow

```
┌─────────────┐
│ Application │
└──────┬──────┘
       │
       │ SELECT * FROM users
       │
┌──────▼────────────────┐
│   UltraBalancer       │
│  ┌────────────────┐   │
│  │ Protocol Parser│   │
│  │ (PostgreSQL)   │   │
│  └────────┬───────┘   │
│           │           │
│  ┌────────▼───────┐   │
│  │  Query Router  │   │
│  │ (READ detected)│   │
│  └────────┬───────┘   │
│           │           │
│  ┌────────▼───────┐   │
│  │Connection Pool │   │
│  │ (Acquire conn) │   │
│  └────────┬───────┘   │
└───────────┼───────────┘
            │
    ┌───────▼────────┐
    │   Replica 1    │
    │ (Least Conns)  │
    └────────────────┘
```

### Read/Write Splitting Logic

```c
Query Classification:
  ├─ SELECT, SHOW → DB_QUERY_READ → Replica (least connections + lag < 5s)
  ├─ INSERT, UPDATE, DELETE → DB_QUERY_WRITE → Primary
  ├─ BEGIN, START → DB_QUERY_TRANSACTION_BEGIN → Primary + Sticky Session
  ├─ COMMIT, ROLLBACK → DB_QUERY_TRANSACTION_END → Release Session
  └─ SET → DB_QUERY_SESSION_VAR → Primary + Sticky Session

Replica Selection Algorithm:
  1. Filter: is_healthy && replication_lag_ms < max_lag
  2. Sort by: active_connections ASC, replication_lag_ms ASC
  3. Return: Top candidate
  4. Fallback: Primary if no replicas available
```

### Session Stickiness

```
Transaction Example:
  Client sends: BEGIN
    → UltraBalancer creates session
    → Routes to Primary (backend_id=1)
    → Stores session mapping

  Client sends: SELECT * FROM orders
    → Session exists → Routes to backend_id=1 (same primary)

  Client sends: INSERT INTO orders...
    → Session exists → Routes to backend_id=1

  Client sends: COMMIT
    → Session exists → Routes to backend_id=1
    → Clears session mapping

  Client sends: SELECT * FROM users
    → No session → Routes to Replica
```

---

## Performance Tuning

### Connection Pool Sizing

**Calculate Optimal Pool Size:**
```
pool_size = ((core_count * 2) + effective_spindle_count)
```

**Examples:**
- **PostgreSQL** - 8 cores, 4 disks → `(8*2) + 4 = 20` connections
- **MySQL** - 16 cores, SSD → `(16*2) + 5 = 37` connections
- **Redis** - Always high (500-1000) since Redis is single-threaded

### Replication Lag Tolerance

**Low Latency Reads (Financial, Real-time)**
```bash
--db-max-lag 1000  # 1 second max lag
```

**Eventual Consistency OK (Analytics, Reporting)**
```bash
--db-max-lag 30000  # 30 seconds max lag
```

### Idle Connection Management

**High Traffic (Sustained Load)**
```bash
--db-pool-min-idle 50     # Keep warm connections
--db-pool-idle-timeout 600 # 10 minutes
```

**Bursty Traffic (Occasional Spikes)**
```bash
--db-pool-min-idle 5       # Minimal idle
--db-pool-idle-timeout 60  # 1 minute cleanup
```

---

## Monitoring

### Stats Endpoint

```bash
curl http://localhost:8080/db/stats
```

**Response:**
```json
{
  "total_acquired": 125000,
  "total_released": 124950,
  "total_created": 200,
  "total_closed": 45,
  "total_connections": 155,
  "validation_failures": 12,
  "backends": [
    {
      "id": 1,
      "host": "192.168.1.10",
      "port": 5432,
      "role": "primary",
      "healthy": true,
      "active_connections": 25,
      "replication_lag_ms": 0
    },
    {
      "id": 2,
      "host": "192.168.1.11",
      "port": 5432,
      "role": "replica",
      "healthy": true,
      "active_connections": 65,
      "replication_lag_ms": 120
    },
    {
      "id": 3,
      "host": "192.168.1.12",
      "port": 5432,
      "role": "replica",
      "healthy": false,
      "active_connections": 0,
      "replication_lag_ms": 8500
    }
  ]
}
```

### Prometheus Metrics

```prometheus
ultrabalancer_db_pool_connections_active 155
ultrabalancer_db_pool_connections_idle 45
ultrabalancer_db_pool_connections_total 200
ultrabalancer_db_pool_connections_created_total 200
ultrabalancer_db_pool_connections_closed_total 45
ultrabalancer_db_backend_healthy{backend="primary",id="1"} 1
ultrabalancer_db_backend_healthy{backend="replica",id="2"} 1
ultrabalancer_db_backend_healthy{backend="replica",id="3"} 0
ultrabalancer_db_backend_lag_ms{backend="replica",id="2"} 120
ultrabalancer_db_backend_lag_ms{backend="replica",id="3"} 8500
```

---

## Troubleshooting

### Issue: Connection Pool Exhausted

**Symptoms:**
```
Error: Connection pool exhausted
```

**Solutions:**
1. Increase pool size: `--db-pool-size 500`
2. Check for connection leaks in application
3. Reduce connection idle timeout: `--db-pool-idle-timeout 60`

### Issue: High Replication Lag

**Symptoms:**
```
Warning: Backend replica-1 replication lag: 12000ms
```

**Solutions:**
1. Increase lag tolerance: `--db-max-lag 15000`
2. Investigate database replication bottleneck
3. Add more replicas to distribute load

### Issue: All Replicas Marked Down

**Symptoms:**
```
Warning: No healthy replicas, routing reads to primary
```

**Solutions:**
1. Check replica health: `curl http://localhost:8080/db/stats`
2. Verify network connectivity to replicas
3. Check replication lag on database side

---

## Best Practices

### 1. Use Separate Pools for Read-Heavy vs Write-Heavy
```bash
ultrabalancer -p 5432 --db-pool-size 200   # Write pool
ultrabalancer -p 5433 --db-pool-size 1000  # Read pool (larger)
```

### 2. Monitor Replication Lag Continuously
```bash
watch -n 1 'curl -s http://localhost:8080/db/stats | jq .backends[].replication_lag_ms'
```

### 3. Use Connection Pooling at Application Layer Too
```python
psycopg2.pool.ThreadedConnectionPool(minconn=5, maxconn=20, ...)
```

### 4. Set Proper Timeouts
```yaml
database:
  pool:
    connect_timeout: 5s
    query_timeout: 30s
    idle_timeout: 300s
```

### 5. Use Health Checks Aggressively
```bash
--db-health-check-interval 5000  # Check every 5 seconds
```

---

## Advanced Examples

### Multi-Datacenter Setup

```bash
ultrabalancer -p 5432 \
  --db-primary dc1.database.local:5432 \
  --db-replica dc1-replica.database.local:5432 \
  --db-replica dc2-replica.database.local:5432 \
  --db-max-lag 2000 \
  --db-health-check-interval 5000
```

### Read-Only Load Balancer

```bash
ultrabalancer -p 5432 \
  --db-mode postgres \
  --db-replica 192.168.1.11:5432 \
  --db-replica 192.168.1.12:5432 \
  --db-replica 192.168.1.13:5432 \
  --db-read-only true
```

### Development Environment (Single Backend)

```bash
ultrabalancer -p 5432 \
  --db-mode postgres \
  --db-primary localhost:5433 \
  --db-pool-size 20
```

---

## Comparison with Other Solutions

| Feature | UltraBalancer | HAProxy | ProxySQL | PgBouncer |
|---------|---------------|---------|----------|-----------|
| **Protocol Parsing** | ✅ | ❌ | ✅ | ✅ |
| **Read/Write Split** | ✅ | ❌ | ✅ | ❌ |
| **Connection Pooling** | ✅ | ❌ | ✅ | ✅ |
| **Health Checks** | ✅ | ✅ | ✅ | ✅ |
| **Lag Detection** | ✅ | ❌ | ✅ | ❌ |
| **Multi-Protocol** | ✅ | ✅ | ❌ | ❌ |
| **Zero Config** | ✅ | ❌ | ❌ | ❌ |

---

## What's Next?

### Upcoming Features
- [ ] Query caching for repeated `SELECT` statements
- [ ] Prepared statement support
- [ ] SSL/TLS termination for database connections
- [ ] Query rewriting and normalization
- [ ] Sharding support (horizontal partitioning)
- [ ] Machine learning-based query routing

---

## Support

**Issues:** [GitHub Issues](https://github.com/Megallm/ultrabalancer/issues)
**Discussions:** [GitHub Discussions](https://github.com/Megallm/ultrabalancer/discussions)
**Email:** kiraa@tuta.io
