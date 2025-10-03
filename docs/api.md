# 🔌 UltraBalancer API Reference

## Table of Contents
1. [REST API](#rest-api)
2. [Admin Socket API](#admin-socket-api)
3. [C API](#c-api)
4. [C++23 API](#c23-api)
5. [WebSocket API](#websocket-api)
6. [gRPC API](#grpc-api)

## REST API

### Base URL
```
http://localhost:8080/api/v1
```

### Authentication
```http
Authorization: Bearer <token>
X-API-Key: <api-key>
```

### Endpoints

#### Health & Status

##### GET /health
Check service health

**Response:**
```json
{
  "status": "healthy",
  "uptime": 3600,
  "version": "1.0.0",
  "pid": 1234,
  "memory_usage_mb": 156,
  "cpu_usage_percent": 12.5
}
```

##### GET /ready
Readiness probe

**Response:**
```json
{
  "ready": true,
  "backends_healthy": 5,
  "backends_total": 6
}
```

##### GET /stats
Get comprehensive statistics

**Response:**
```json
{
  "global": {
    "total_requests": 1234567,
    "requests_per_second": 5432,
    "active_connections": 234,
    "total_bytes_in": 123456789,
    "total_bytes_out": 987654321
  },
  "frontends": {
    "web": {
      "requests": 234567,
      "bytes_in": 34567890,
      "bytes_out": 87654321,
      "response_times": {
        "p50": 1.2,
        "p95": 5.4,
        "p99": 12.3
      }
    }
  },
  "backends": {
    "servers": {
      "active_connections": 123,
      "total_requests": 345678,
      "errors": 12,
      "servers": [
        {
          "name": "web1",
          "status": "up",
          "weight": 100,
          "active_connections": 45,
          "total_requests": 123456
        }
      ]
    }
  }
}
```

#### Configuration Management

##### GET /config
Get current configuration

**Response:**
```json
{
  "global": {
    "maxconn": 100000,
    "nbthread": 32
  },
  "frontends": [...],
  "backends": [...]
}
```

##### PUT /config
Update configuration (hot reload)

**Request:**
```json
{
  "backend": "servers",
  "server": "web1",
  "weight": 50
}
```

**Response:**
```json
{
  "success": true,
  "message": "Configuration updated"
}
```

#### Backend Management

##### GET /backends
List all backends

**Response:**
```json
[
  {
    "name": "servers",
    "mode": "http",
    "algorithm": "roundrobin",
    "servers": 3,
    "active_connections": 123
  }
]
```

##### GET /backends/{name}
Get backend details

**Response:**
```json
{
  "name": "servers",
  "mode": "http",
  "algorithm": "roundrobin",
  "servers": [
    {
      "id": "web1",
      "address": "192.168.1.10:8080",
      "status": "up",
      "weight": 100,
      "active_connections": 45,
      "total_requests": 123456,
      "errors": 12,
      "last_check": "2024-01-01T12:00:00Z",
      "check_duration_ms": 2
    }
  ]
}
```

##### POST /backends/{name}/servers
Add server to backend

**Request:**
```json
{
  "id": "web4",
  "address": "192.168.1.13:8080",
  "weight": 100,
  "check": true
}
```

##### PUT /backends/{name}/servers/{id}
Update server

**Request:**
```json
{
  "weight": 50,
  "status": "drain"
}
```

##### DELETE /backends/{name}/servers/{id}
Remove server

#### Session Management

##### GET /sessions
Get active sessions

**Response:**
```json
{
  "total": 234,
  "sessions": [
    {
      "id": "abc123",
      "client": "192.168.1.100:54321",
      "backend": "servers",
      "server": "web1",
      "created_at": "2024-01-01T12:00:00Z",
      "bytes_in": 12345,
      "bytes_out": 54321
    }
  ]
}
```

##### DELETE /sessions/{id}
Terminate session

#### ACL Management

##### GET /acls
List ACLs

##### POST /acls
Create ACL

**Request:**
```json
{
  "name": "blocked_ips",
  "type": "src",
  "values": ["192.168.1.100", "10.0.0.0/8"]
}
```

#### Rate Limiting

##### GET /ratelimits
Get rate limit rules

##### POST /ratelimits
Create rate limit rule

**Request:**
```json
{
  "name": "api_limit",
  "path": "/api/*",
  "requests_per_second": 100,
  "burst": 10,
  "key": "src_ip"
}
```

## Admin Socket API

### Connection
```bash
socat - /var/run/ultrabalancer.sock
```

### Commands

#### show info
Display general information
```
> show info
Name: UltraBalancer
Version: 1.0.0
Uptime: 2d 3h 15m
Process_num: 1
Pid: 1234
```

#### show stat
Display statistics
```
> show stat
# pxname,svname,qcur,qmax,scur,smax,stot,...
web,FRONTEND,,,0,1,12345,...
servers,web1,0,0,0,10,5432,...
```

#### set weight
Change server weight
```
> set weight servers/web1 50
```

#### disable server
Disable server
```
> disable server servers/web1
```

#### enable server
Enable server
```
> enable server servers/web1
```

## C API

### Core Functions

```c
#include <ultrabalancer.h>

// Initialize
int ub_init(const char *config_file);
void ub_shutdown(void);

// Frontend Management
ub_frontend_t* ub_frontend_create(const char *name);
int ub_frontend_bind(ub_frontend_t *fe, const char *address, int port);
int ub_frontend_set_mode(ub_frontend_t *fe, ub_mode_t mode);
void ub_frontend_destroy(ub_frontend_t *fe);

// Backend Management
ub_backend_t* ub_backend_create(const char *name);
int ub_backend_add_server(ub_backend_t *be, const char *name,
                          const char *address, int port);
int ub_backend_set_algorithm(ub_backend_t *be, ub_algorithm_t algo);
void ub_backend_destroy(ub_backend_t *be);

// Server Management
ub_server_t* ub_server_create(const char *name, const char *address, int port);
int ub_server_set_weight(ub_server_t *srv, int weight);
int ub_server_set_state(ub_server_t *srv, ub_state_t state);
int ub_server_check_health(ub_server_t *srv);

// Session Management
ub_session_t* ub_session_get(uint64_t id);
int ub_session_terminate(ub_session_t *sess);

// Statistics
typedef struct {
    uint64_t total_requests;
    uint64_t active_connections;
    uint64_t bytes_in;
    uint64_t bytes_out;
    double avg_response_time_ms;
} ub_stats_t;

int ub_get_stats(ub_stats_t *stats);

// Health Checks
typedef int (*ub_health_check_fn)(ub_server_t *srv, void *ctx);
int ub_register_health_check(const char *name, ub_health_check_fn fn);

// ACL
ub_acl_t* ub_acl_create(const char *name);
int ub_acl_add_pattern(ub_acl_t *acl, const char *pattern);
int ub_acl_match(ub_acl_t *acl, const char *value);

// Callbacks
typedef enum {
    UB_EVT_CONN_ACCEPT,
    UB_EVT_CONN_CLOSE,
    UB_EVT_REQUEST,
    UB_EVT_RESPONSE,
    UB_EVT_SERVER_UP,
    UB_EVT_SERVER_DOWN
} ub_event_t;

typedef void (*ub_event_handler_t)(ub_event_t evt, void *data, void *ctx);
int ub_register_event_handler(ub_event_t evt, ub_event_handler_t handler, void *ctx);
```

### Example Usage

```c
#include <ultrabalancer.h>

int main() {
    // Initialize
    if (ub_init("/etc/ultrabalancer/config.yaml") < 0) {
        return 1;
    }

    // Create frontend
    ub_frontend_t *fe = ub_frontend_create("web");
    ub_frontend_bind(fe, "0.0.0.0", 80);
    ub_frontend_set_mode(fe, UB_MODE_HTTP);

    // Create backend
    ub_backend_t *be = ub_backend_create("servers");
    ub_backend_set_algorithm(be, UB_ALGO_ROUNDROBIN);

    // Add servers
    ub_backend_add_server(be, "web1", "192.168.1.10", 8080);
    ub_backend_add_server(be, "web2", "192.168.1.11", 8080);

    // Register event handler
    ub_register_event_handler(UB_EVT_SERVER_DOWN, server_down_handler, NULL);

    // Run
    ub_run();

    // Cleanup
    ub_shutdown();
    return 0;
}
```

## C++23 API

### Connection Pool

```cpp
#include <ultrabalancer/connection_pool.hpp>

namespace ub = ultrabalancer;

// Create pool
auto pool = std::make_unique<ub::ConnectionPool>(1000, 100);

// Set health checker
pool->set_health_check([](ub::Connection* conn) -> bool {
    return conn->is_alive();
});

// Acquire connection
if (auto conn = pool->acquire(server); conn) {
    // Use connection
    conn->send(request);
    auto response = conn->receive();

    // Connection automatically returned to pool
}

// Cleanup idle connections
pool->cleanup_idle(std::chrono::seconds(60));

// Get statistics
auto active = pool->active_connections();
auto idle = pool->idle_connections();
```

### Request Router

```cpp
#include <ultrabalancer/request_router.hpp>

// Create router
auto router = std::make_shared<ub::RequestRouter>();

// Create route
auto route = std::make_shared<ub::Route>("api");
route->set_priority(100);

// Add rules
route->add_rule(std::make_shared<ub::RouteRule>(
    ub::RouteRule::MatchType::PREFIX, "/api/v2"
));

// Add targets
route->add_target(std::make_shared<ub::RouteTarget>("api_backend", 100));

// Enable circuit breaker
route->enable_circuit_breaker(50, std::chrono::seconds(30));

// Add route to router
router->add_route(route);

// Set default backend
router->set_default_backend("default_backend");

// Enable rate limiting
router->enable_rate_limiting("api", 1000);

// Route request
std::unordered_map<std::string, std::string> headers{
    {"Host", "api.example.com"},
    {"X-API-Version", "2"}
};

if (auto target = router->route_request("GET", "/api/v2/users", headers)) {
    std::cout << "Routed to: " << target->get_backend() << std::endl;
}

// Get statistics
auto stats = router->get_stats();
```

### Metrics Aggregator

```cpp
#include <ultrabalancer/metrics_aggregator.hpp>

// Get singleton instance
auto& metrics = ub::MetricsAggregator::instance();

// Increment counter
metrics.increment_counter("requests.total");

// Set gauge
metrics.set_gauge("connections.active", 123);

// Record timer
{
    ub::ScopedTimer timer("request.latency");
    // Process request...
} // Timer automatically recorded

// Get specific metric
if (auto metric = metrics.get_metric("requests.total")) {
    std::cout << "Total requests: " << metric->get_count() << std::endl;
    std::cout << "Mean time: " << metric->get_mean() << " ms" << std::endl;
}

// Get percentiles
if (auto metric = metrics.get_metric("request.latency")) {
    auto percentiles = metric->get_percentiles({50, 95, 99});
    std::cout << "P50: " << percentiles[0] << " ms" << std::endl;
    std::cout << "P95: " << percentiles[1] << " ms" << std::endl;
    std::cout << "P99: " << percentiles[2] << " ms" << std::endl;
}

// Get aggregated stats
auto stats = metrics.get_stats();
std::cout << "Total requests: " << stats.total_requests << std::endl;
std::cout << "Success rate: "
          << (stats.successful_requests * 100.0 / stats.total_requests)
          << "%" << std::endl;
```

### Coroutine Support (C++23)

```cpp
#include <ultrabalancer/async.hpp>

// Async request handler
ub::Task<ub::Response> handle_request_async(ub::Request req) {
    // Acquire connection from pool
    auto conn = co_await pool->acquire_async(server);

    // Send request
    co_await conn->send_async(req);

    // Receive response
    auto response = co_await conn->receive_async();

    // Process with timeout
    auto result = co_await ub::with_timeout(
        process_response(response),
        std::chrono::seconds(5)
    );

    co_return result.value_or(ub::Response::timeout());
}

// Parallel requests
ub::Task<std::vector<ub::Response>> fanout_request(ub::Request req) {
    auto backends = {"backend1", "backend2", "backend3"};

    // Launch parallel requests
    std::vector<ub::Task<ub::Response>> tasks;
    for (const auto& backend : backends) {
        tasks.push_back(send_to_backend(backend, req));
    }

    // Wait for all
    co_return co_await ub::when_all(std::move(tasks));
}
```

## WebSocket API

### Connection
```javascript
const ws = new WebSocket('ws://localhost:8080/ws');

ws.onopen = () => {
    // Subscribe to events
    ws.send(JSON.stringify({
        type: 'subscribe',
        events: ['stats', 'health', 'config']
    }));
};

ws.onmessage = (event) => {
    const data = JSON.parse(event.data);

    switch(data.type) {
        case 'stats':
            updateStats(data.payload);
            break;
        case 'health':
            updateHealth(data.payload);
            break;
        case 'config':
            updateConfig(data.payload);
            break;
    }
};
```

### Commands

#### Subscribe to Events
```json
{
  "type": "subscribe",
  "events": ["stats", "health", "config", "logs"]
}
```

#### Execute Command
```json
{
  "type": "command",
  "command": "set weight",
  "args": {
    "backend": "servers",
    "server": "web1",
    "weight": 50
  }
}
```

#### Real-time Metrics Stream
```json
{
  "type": "metrics",
  "interval": 1000,
  "metrics": ["requests_per_second", "response_time_p99", "active_connections"]
}
```

## gRPC API

### Protocol Buffer Definition

```protobuf
syntax = "proto3";

package ultrabalancer.v1;

service LoadBalancer {
  // Health
  rpc GetHealth(Empty) returns (HealthResponse);

  // Stats
  rpc GetStats(StatsRequest) returns (StatsResponse);
  rpc StreamStats(StreamStatsRequest) returns (stream StatsResponse);

  // Backend Management
  rpc ListBackends(Empty) returns (ListBackendsResponse);
  rpc GetBackend(GetBackendRequest) returns (Backend);
  rpc UpdateServer(UpdateServerRequest) returns (UpdateServerResponse);

  // Configuration
  rpc GetConfig(Empty) returns (Config);
  rpc UpdateConfig(UpdateConfigRequest) returns (UpdateConfigResponse);

  // Sessions
  rpc ListSessions(ListSessionsRequest) returns (ListSessionsResponse);
  rpc TerminateSession(TerminateSessionRequest) returns (Empty);
}

message HealthResponse {
  enum Status {
    UNKNOWN = 0;
    HEALTHY = 1;
    DEGRADED = 2;
    UNHEALTHY = 3;
  }

  Status status = 1;
  int64 uptime_seconds = 2;
  string version = 3;
}

message Backend {
  string name = 1;
  string algorithm = 2;
  repeated Server servers = 3;
}

message Server {
  string id = 1;
  string address = 2;
  enum Status {
    UNKNOWN = 0;
    UP = 1;
    DOWN = 2;
    DRAIN = 3;
  }
  Status status = 3;
  int32 weight = 4;
  int32 active_connections = 5;
}
```

### Client Example

```cpp
#include <grpcpp/grpcpp.h>
#include "ultrabalancer.grpc.pb.h"

using namespace ultrabalancer::v1;

class LoadBalancerClient {
    std::unique_ptr<LoadBalancer::Stub> stub_;

public:
    LoadBalancerClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(LoadBalancer::NewStub(channel)) {}

    HealthResponse GetHealth() {
        Empty request;
        HealthResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetHealth(&context, request, &response);
        if (!status.ok()) {
            throw std::runtime_error(status.error_message());
        }

        return response;
    }

    void StreamStats() {
        StreamStatsRequest request;
        request.set_interval_ms(1000);

        grpc::ClientContext context;
        auto reader = stub_->StreamStats(&context, request);

        StatsResponse response;
        while (reader->Read(&response)) {
            std::cout << "RPS: " << response.requests_per_second() << std::endl;
            std::cout << "Connections: " << response.active_connections() << std::endl;
        }
    }
};
```

## Error Codes

### HTTP Status Codes
- `200` - Success
- `400` - Bad Request
- `401` - Unauthorized
- `403` - Forbidden
- `404` - Not Found
- `409` - Conflict
- `429` - Too Many Requests
- `500` - Internal Server Error
- `502` - Bad Gateway
- `503` - Service Unavailable
- `504` - Gateway Timeout

### Custom Error Codes
- `1001` - Invalid configuration
- `1002` - Backend not found
- `1003` - Server not found
- `1004` - Health check failed
- `1005` - Rate limit exceeded
- `1006` - Circuit breaker open
- `1007` - Connection pool exhausted
- `1008` - Invalid ACL pattern
- `1009` - Session not found
- `1010` - Metrics unavailable