#ifndef DB_POOL_HPP
#define DB_POOL_HPP

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <optional>
#include <variant>
#include "db_protocol.h"

template<typename E>
class unexpected {
    E error_;
public:
    explicit unexpected(E error) : error_(std::move(error)) {}
    const E& value() const { return error_; }
};

template<typename E>
unexpected<E> make_unexpected(E error) {
    return unexpected<E>(std::move(error));
}

template<typename T, typename E>
class expected {
    std::variant<T, E> data_;
    bool has_value_;

public:
    expected(const T& value) : data_(value), has_value_(true) {}
    expected(T&& value) : data_(std::move(value)), has_value_(true) {}
    expected(const unexpected<E>& unexp) : data_(unexp.value()), has_value_(false) {}
    expected(unexpected<E>&& unexp) : data_(std::move(unexp.value())), has_value_(false) {}

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    const T& value() const & { return std::get<T>(data_); }
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::move(std::get<T>(data_)); }

    const E& error() const & { return std::get<E>(data_); }
    E& error() & { return std::get<E>(data_); }

    T* operator->() { return &std::get<T>(data_); }
    const T* operator->() const { return &std::get<T>(data_); }
};

namespace ultrabalancer {
namespace database {

enum class ConnectionState {
    Idle,
    Active,
    Validating,
    Closing
};

enum class BackendRole {
    Primary,
    Replica,
    Down
};

struct ConnectionStatsSnapshot {
    uint64_t total_acquired;
    uint64_t total_released;
    uint64_t total_created;
    uint64_t total_closed;
    uint64_t total_validation_failures;
};

struct ConnectionStats {
    std::atomic<uint64_t> total_acquired{0};
    std::atomic<uint64_t> total_released{0};
    std::atomic<uint64_t> total_created{0};
    std::atomic<uint64_t> total_closed{0};
    std::atomic<uint64_t> total_validation_failures{0};
};

class Connection {
public:
    Connection(int fd, db_protocol_type_t protocol, uint64_t backend_id);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] db_protocol_type_t protocol() const noexcept { return protocol_; }
    [[nodiscard]] uint64_t backend_id() const noexcept { return backend_id_; }
    [[nodiscard]] bool in_transaction() const noexcept { return in_transaction_; }
    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] auto age() const noexcept -> std::chrono::seconds;
    [[nodiscard]] auto idle_time() const noexcept -> std::chrono::seconds;

    void mark_used() noexcept;
    void set_transaction(bool in_tx) noexcept { in_transaction_ = in_tx; }
    [[nodiscard]] bool validate();

private:
    int fd_;
    db_protocol_type_t protocol_;
    uint64_t backend_id_;
    bool in_transaction_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point last_used_;
    ConnectionState state_;
};

class Backend {
public:
    Backend(uint64_t id, std::string host, uint16_t port,
            BackendRole role, db_protocol_type_t protocol);

    [[nodiscard]] uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] uint16_t port() const noexcept { return port_; }
    [[nodiscard]] BackendRole role() const noexcept { return role_; }
    [[nodiscard]] db_protocol_type_t protocol() const noexcept { return protocol_; }
    [[nodiscard]] bool is_healthy() const noexcept { return is_healthy_.load(); }
    [[nodiscard]] uint64_t replication_lag_ms() const noexcept { return replication_lag_ms_.load(); }
    [[nodiscard]] uint32_t active_connections() const noexcept { return active_connections_.load(); }

    void set_healthy(bool healthy) noexcept { is_healthy_.store(healthy); }
    void set_replication_lag(uint64_t lag_ms) noexcept { replication_lag_ms_.store(lag_ms); }
    void increment_connections() noexcept { active_connections_.fetch_add(1); }
    void decrement_connections() noexcept { active_connections_.fetch_sub(1); }
    void set_role(BackendRole role) noexcept { role_ = role; }

    [[nodiscard]] expected<int, std::string> create_connection();

private:
    uint64_t id_;
    std::string host_;
    uint16_t port_;
    BackendRole role_;
    db_protocol_type_t protocol_;
    std::atomic<bool> is_healthy_{true};
    std::atomic<uint64_t> replication_lag_ms_{0};
    std::atomic<uint32_t> active_connections_{0};
};

class DatabasePool {
public:
    DatabasePool(uint32_t max_connections, uint32_t min_idle, uint32_t max_idle,
                 std::chrono::seconds max_lifetime, std::chrono::seconds idle_timeout);
    ~DatabasePool();

    DatabasePool(const DatabasePool&) = delete;
    DatabasePool& operator=(const DatabasePool&) = delete;

    [[nodiscard]] uint64_t add_backend(const std::string& host, uint16_t port,
                                       BackendRole role, db_protocol_type_t protocol);

    [[nodiscard]] expected<std::unique_ptr<Connection>, std::string>
    acquire(db_query_type_t query_type, bool in_transaction = false,
            std::optional<uint64_t> session_backend_id = std::nullopt);

    void release(std::unique_ptr<Connection> conn);

    [[nodiscard]] std::optional<Backend*> select_backend(db_query_type_t query_type);

    void cleanup_idle_connections();

    [[nodiscard]] ConnectionStatsSnapshot get_stats() const noexcept;

    [[nodiscard]] std::string get_stats_json() const;

    [[nodiscard]] Backend* get_backend_by_id(uint64_t id);

private:
    [[nodiscard]] std::unique_ptr<Connection> create_new_connection(Backend* backend);
    [[nodiscard]] Backend* select_primary();
    [[nodiscard]] Backend* select_replica();

    std::vector<std::unique_ptr<Backend>> backends_;
    std::unordered_map<uint64_t, std::vector<std::unique_ptr<Connection>>> idle_connections_;

    uint32_t max_connections_;
    uint32_t min_idle_;
    uint32_t max_idle_;
    std::chrono::seconds max_lifetime_;
    std::chrono::seconds idle_timeout_;

    std::atomic<uint64_t> next_backend_id_{1};
    std::atomic<uint32_t> total_connections_{0};

    ConnectionStats stats_;
    mutable std::shared_mutex mutex_;
};

}
}

#endif
