#include "database/db_pool.hpp"
#include "database/db_pool.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace ultrabalancer {
namespace database {

Connection::Connection(int fd, db_protocol_type_t protocol, uint64_t backend_id)
    : fd_(fd),
      protocol_(protocol),
      backend_id_(backend_id),
      in_transaction_(false),
      created_at_(std::chrono::steady_clock::now()),
      last_used_(std::chrono::steady_clock::now()),
      state_(ConnectionState::Idle) {}

Connection::~Connection() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

Connection::Connection(Connection&& other) noexcept
    : fd_(other.fd_),
      protocol_(other.protocol_),
      backend_id_(other.backend_id_),
      in_transaction_(other.in_transaction_),
      created_at_(other.created_at_),
      last_used_(other.last_used_),
      state_(other.state_) {
    other.fd_ = -1;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = other.fd_;
        protocol_ = other.protocol_;
        backend_id_ = other.backend_id_;
        in_transaction_ = other.in_transaction_;
        created_at_ = other.created_at_;
        last_used_ = other.last_used_;
        state_ = other.state_;
        other.fd_ = -1;
    }
    return *this;
}

bool Connection::is_valid() const noexcept {
    if (fd_ < 0) return false;

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        return false;
    }
    return error == 0;
}

auto Connection::age() const noexcept -> std::chrono::seconds {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - created_at_);
}

auto Connection::idle_time() const noexcept -> std::chrono::seconds {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_used_);
}

void Connection::mark_used() noexcept {
    last_used_ = std::chrono::steady_clock::now();
}

bool Connection::validate() {
    if (!is_valid()) return false;

    char buf;
    int result = recv(fd_, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) return false;
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;

    return true;
}

Backend::Backend(uint64_t id, std::string host, uint16_t port,
                 BackendRole role, db_protocol_type_t protocol)
    : id_(id),
      host_(std::move(host)),
      port_(port),
      role_(role),
      protocol_(protocol) {}

expected<int, std::string> Backend::create_connection() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return make_unexpected(std::string("Failed to create socket: ") + strerror(errno));
    }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return make_unexpected(std::string("Invalid address: ") + host_);
    }

    int result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(fd);
        return make_unexpected(std::string("Connection failed: ") + strerror(errno));
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    result = select(fd + 1, nullptr, &write_fds, nullptr, &timeout);
    if (result <= 0) {
        close(fd);
        return make_unexpected(std::string("Connection timeout"));
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        close(fd);
        return make_unexpected(std::string("Connection error: ") + strerror(error));
    }

    return fd;
}

DatabasePool::DatabasePool(uint32_t max_connections, uint32_t min_idle, uint32_t max_idle,
                           std::chrono::seconds max_lifetime, std::chrono::seconds idle_timeout)
    : max_connections_(max_connections),
      min_idle_(min_idle),
      max_idle_(max_idle),
      max_lifetime_(max_lifetime),
      idle_timeout_(idle_timeout) {}

DatabasePool::~DatabasePool() {
    std::unique_lock lock(mutex_);
    idle_connections_.clear();
}

uint64_t DatabasePool::add_backend(const std::string& host, uint16_t port,
                                    BackendRole role, db_protocol_type_t protocol) {
    std::unique_lock lock(mutex_);

    uint64_t id = next_backend_id_.fetch_add(1);
    auto backend = std::make_unique<Backend>(id, host, port, role, protocol);
    backends_.push_back(std::move(backend));

    return id;
}

expected<std::unique_ptr<Connection>, std::string>
DatabasePool::acquire(db_query_type_t query_type, bool in_transaction,
                      std::optional<uint64_t> session_backend_id) {
    std::unique_lock lock(mutex_);

    Backend* backend = nullptr;

    if (session_backend_id.has_value()) {
        backend = get_backend_by_id(session_backend_id.value());
        if (!backend || !backend->is_healthy()) {
            return make_unexpected(std::string("Session backend unavailable"));
        }
    } else {
        auto opt_backend = select_backend(query_type);
        if (!opt_backend) {
            return make_unexpected(std::string("No healthy backend available"));
        }
        backend = *opt_backend;
    }

    auto& conns = idle_connections_[backend->id()];

    while (!conns.empty()) {
        auto conn = std::move(conns.back());
        conns.pop_back();

        if (conn->validate() && conn->age() < max_lifetime_) {
            conn->mark_used();
            conn->set_transaction(in_transaction);
            backend->increment_connections();
            stats_.total_acquired.fetch_add(1);
            return conn;
        } else {
            stats_.total_closed.fetch_add(1);
        }
    }

    if (total_connections_.load() >= max_connections_) {
        return make_unexpected(std::string("Connection pool exhausted"));
    }

    auto conn = create_new_connection(backend);
    if (conn) {
        conn->set_transaction(in_transaction);
        backend->increment_connections();
        total_connections_.fetch_add(1);
        stats_.total_created.fetch_add(1);
        stats_.total_acquired.fetch_add(1);
    }

    return conn;
}

void DatabasePool::release(std::unique_ptr<Connection> conn) {
    if (!conn) return;

    std::unique_lock lock(mutex_);

    auto* backend = get_backend_by_id(conn->backend_id());
    if (backend) {
        backend->decrement_connections();
    }

    // Check both idle time and age to ensure connections don't become stale
    if (!conn->validate() || conn->idle_time() > idle_timeout_ || conn->age() > max_lifetime_) {
        total_connections_.fetch_sub(1);
        stats_.total_closed.fetch_add(1);
        return;
    }

    auto& conns = idle_connections_[conn->backend_id()];
    if (conns.size() < max_idle_) {
        conn->set_transaction(false);
        conns.push_back(std::move(conn));
        stats_.total_released.fetch_add(1);
    } else {
        total_connections_.fetch_sub(1);
        stats_.total_closed.fetch_add(1);
    }
}

std::optional<Backend*> DatabasePool::select_backend(db_query_type_t query_type) {
    switch (query_type) {
        case DB_QUERY_WRITE:
        case DB_QUERY_TRANSACTION_BEGIN:
        case DB_QUERY_SESSION_VAR:
            return select_primary();

        case DB_QUERY_READ:
            return select_replica();

        default:
            return select_primary();
    }
}

void DatabasePool::cleanup_idle_connections() {
    std::unique_lock lock(mutex_);

    for (auto& [backend_id, conns] : idle_connections_) {
        auto it = std::remove_if(conns.begin(), conns.end(),
            [this](const std::unique_ptr<Connection>& conn) {
                bool should_remove = !conn->validate() ||
                                    conn->idle_time() > idle_timeout_ ||
                                    conn->age() > max_lifetime_;
                if (should_remove) {
                    total_connections_.fetch_sub(1);
                    stats_.total_closed.fetch_add(1);
                }
                return should_remove;
            });
        conns.erase(it, conns.end());
    }
}

ConnectionStatsSnapshot DatabasePool::get_stats() const noexcept {
    return ConnectionStatsSnapshot{
        stats_.total_acquired.load(),
        stats_.total_released.load(),
        stats_.total_created.load(),
        stats_.total_closed.load(),
        stats_.total_validation_failures.load()
    };
}

std::string DatabasePool::get_stats_json() const {
    std::shared_lock lock(mutex_);

    std::ostringstream oss;
    oss << "{"
        << "\"total_acquired\":" << stats_.total_acquired.load() << ","
        << "\"total_released\":" << stats_.total_released.load() << ","
        << "\"total_created\":" << stats_.total_created.load() << ","
        << "\"total_closed\":" << stats_.total_closed.load() << ","
        << "\"total_connections\":" << total_connections_.load() << ","
        << "\"validation_failures\":" << stats_.total_validation_failures.load() << ","
        << "\"backends\":[";

    bool first = true;
    for (const auto& backend : backends_) {
        if (!first) oss << ",";
        first = false;
        oss << "{"
            << "\"id\":" << backend->id() << ","
            << "\"host\":\"" << backend->host() << "\","
            << "\"port\":" << backend->port() << ","
            << "\"role\":\"" << (backend->role() == BackendRole::Primary ? "primary" : "replica") << "\","
            << "\"healthy\":" << (backend->is_healthy() ? "true" : "false") << ","
            << "\"active_connections\":" << backend->active_connections() << ","
            << "\"replication_lag_ms\":" << backend->replication_lag_ms()
            << "}";
    }

    oss << "]}";
    return oss.str();
}

std::unique_ptr<Connection> DatabasePool::create_new_connection(Backend* backend) {
    auto result = backend->create_connection();
    if (!result) {
        return nullptr;
    }

    return std::make_unique<Connection>(result.value(), backend->protocol(), backend->id());
}

Backend* DatabasePool::select_primary() {
    for (auto& backend : backends_) {
        if (backend->role() == BackendRole::Primary && backend->is_healthy()) {
            return backend.get();
        }
    }
    return nullptr;
}

Backend* DatabasePool::select_replica() {
    Backend* best = nullptr;
    uint32_t min_connections = UINT32_MAX;
    uint64_t min_lag = UINT64_MAX;

    for (auto& backend : backends_) {
        if (backend->role() != BackendRole::Replica || !backend->is_healthy()) {
            continue;
        }

        uint64_t lag = backend->replication_lag_ms();
        if (lag > 5000) continue;

        uint32_t conns = backend->active_connections();
        if (conns < min_connections || (conns == min_connections && lag < min_lag)) {
            best = backend.get();
            min_connections = conns;
            min_lag = lag;
        }
    }

    if (!best) {
        return select_primary();
    }

    return best;
}

Backend* DatabasePool::get_backend_by_id(uint64_t id) const noexcept {
    std::shared_lock lock(mutex_);

    auto it = std::find_if(backends_.begin(), backends_.end(),
        [id](const std::unique_ptr<Backend>& b) { return b->id() == id; });

    return it != backends_.end() ? it->get() : nullptr;
}

}
}

extern "C" {

using namespace ultrabalancer::database;

db_pool_t* db_pool_create(uint32_t max_connections, uint32_t min_idle, uint32_t max_idle) {
    db_pool_t* pool = (db_pool_t*)calloc(1, sizeof(db_pool_t));
    if (!pool) return nullptr;

    pool->max_connections = max_connections;
    pool->min_idle = min_idle;
    pool->max_idle = max_idle;
    pool->max_lifetime_seconds = 3600;
    pool->idle_timeout_seconds = 300;
    pool->initialized = true;

    auto cpp_pool = new DatabasePool(
        max_connections, min_idle, max_idle,
        std::chrono::seconds(3600),
        std::chrono::seconds(300)
    );

    pool->mutex = cpp_pool;
    return pool;
}

void db_pool_destroy(db_pool_t* pool) {
    if (!pool) return;
    if (pool->mutex) {
        delete static_cast<DatabasePool*>(pool->mutex);
    }
    free(pool);
}

int db_pool_add_backend(db_pool_t* pool, const char* host, uint16_t port,
                        db_backend_role_t role, db_protocol_type_t protocol) {
    if (!pool || !pool->mutex || !host) return -1;

    auto* cpp_pool = static_cast<DatabasePool*>(pool->mutex);
    BackendRole cpp_role = (role == DB_BACKEND_PRIMARY) ? BackendRole::Primary : BackendRole::Replica;

    [[maybe_unused]] uint64_t backend_id = cpp_pool->add_backend(host, port, cpp_role, protocol);
    return 0;
}

db_connection_t* db_pool_acquire(db_pool_t* pool, db_query_type_t query_type,
                                  bool in_transaction, uint64_t session_backend_id) {
    if (!pool || !pool->mutex) return nullptr;

    auto* cpp_pool = static_cast<DatabasePool*>(pool->mutex);

    std::optional<uint64_t> backend_opt =
        (session_backend_id > 0) ? std::optional<uint64_t>(session_backend_id) : std::nullopt;

    auto result = cpp_pool->acquire(query_type, in_transaction, backend_opt);
    if (!result) return nullptr;

    auto conn_ptr = std::move(result).value();
    db_connection_t* conn = (db_connection_t*)malloc(sizeof(db_connection_t));
    if (!conn) return nullptr;

    conn->fd = conn_ptr->fd();
    conn->protocol = conn_ptr->protocol();
    conn->backend_id = conn_ptr->backend_id();
    conn->in_use = true;
    conn->in_transaction = conn_ptr->in_transaction();
    conn->created_at = time(nullptr);
    conn->last_used = time(nullptr);
    conn->query_count = 0;
    conn->next = nullptr;

    // Fix: Properly get backend role instead of hardcoding to PRIMARY
    auto* backend = cpp_pool->get_backend_by_id(conn->backend_id);
    conn->backend_role = (backend && backend->role() == BackendRole::Primary) ?
                         DB_BACKEND_PRIMARY : DB_BACKEND_REPLICA;

    return conn;
}

void db_pool_release(db_pool_t* pool, db_connection_t* conn) {
    if (!pool || !pool->mutex || !conn) return;

    auto cpp_conn = std::make_unique<Connection>(
        conn->fd, conn->protocol, conn->backend_id
    );
    cpp_conn->set_transaction(conn->in_transaction);

    auto* cpp_pool = static_cast<DatabasePool*>(pool->mutex);
    cpp_pool->release(std::move(cpp_conn));

    free(conn);
}

db_backend_t* db_pool_select_backend(db_pool_t* pool, db_query_type_t query_type) {
    if (!pool || !pool->mutex) return nullptr;

    auto* cpp_pool = static_cast<DatabasePool*>(pool->mutex);
    auto backend_opt = cpp_pool->select_backend(query_type);

    if (!backend_opt) return nullptr;

    auto* backend = backend_opt.value();
    db_backend_t* c_backend = (db_backend_t*)malloc(sizeof(db_backend_t));
    if (!c_backend) return nullptr;

    c_backend->id = backend->id();
    strncpy(c_backend->host, backend->host().c_str(), sizeof(c_backend->host) - 1);
    c_backend->port = backend->port();
    c_backend->role = (backend->role() == BackendRole::Primary) ?
                      DB_BACKEND_PRIMARY : DB_BACKEND_REPLICA;
    c_backend->protocol = backend->protocol();
    c_backend->is_healthy = backend->is_healthy();
    c_backend->active_connections = backend->active_connections();
    c_backend->replication_lag_ms = backend->replication_lag_ms();
    c_backend->next = nullptr;

    return c_backend;
}

int db_pool_validate_connection(db_connection_t* conn) {
    if (!conn || conn->fd < 0) return -1;

    char buf;
    int result = recv(conn->fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) return -1;
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;

    return 0;
}

void db_pool_cleanup_idle(db_pool_t* pool) {
    if (!pool || !pool->mutex) return;

    auto* cpp_pool = static_cast<DatabasePool*>(pool->mutex);
    cpp_pool->cleanup_idle_connections();
}

int db_pool_get_stats(db_pool_t* pool, char* buffer, size_t buffer_size) {
    if (!pool || !pool->mutex || !buffer) return -1;

    auto* cpp_pool = static_cast<DatabasePool*>(pool->mutex);
    std::string stats_json = cpp_pool->get_stats_json();

    size_t copy_len = std::min(stats_json.length(), buffer_size - 1);
    memcpy(buffer, stats_json.c_str(), copy_len);
    buffer[copy_len] = '\0';

    return static_cast<int>(copy_len);
}

}
