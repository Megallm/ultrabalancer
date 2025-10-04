#ifndef CORE_CONNECTION_POOL_HPP
#define CORE_CONNECTION_POOL_HPP

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <unordered_map>

#include "core/common.h"
#include "core/proxy.h"

namespace ultrabalancer {

class Connection {
public:
    Connection(int fd, const sockaddr_storage& addr);
    ~Connection();

    bool is_alive() const;
    void reset();
    int get_fd() const { return fd_; }
    std::chrono::steady_clock::time_point last_used() const { return last_used_; }

private:
    int fd_;
    sockaddr_storage addr_;
    mutable std::atomic<bool> alive_;
    std::chrono::steady_clock::time_point last_used_;
};

class ConnectionPool {
public:
    ConnectionPool(size_t max_size, size_t max_idle);
    ~ConnectionPool();

    std::shared_ptr<Connection> acquire(const server_t* server);
    void release(std::shared_ptr<Connection> conn);

    void set_health_check(std::function<bool(Connection*)> checker);
    void cleanup_idle(std::chrono::seconds idle_timeout);

    size_t active_connections() const { return active_; }
    size_t idle_connections() const { return idle_queue_.size(); }

private:
    struct ServerKey {
        std::string host;
        uint16_t port;

        bool operator==(const ServerKey& other) const {
            return host == other.host && port == other.port;
        }
    };

    struct ServerKeyHash {
        size_t operator()(const ServerKey& key) const {
            return std::hash<std::string>{}(key.host) ^
                   (std::hash<uint16_t>{}(key.port) << 1);
        }
    };

    size_t max_size_;
    size_t max_idle_;
    std::atomic<size_t> active_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::unordered_map<ServerKey, std::queue<std::shared_ptr<Connection>>, ServerKeyHash> idle_queue_;
    std::function<bool(Connection*)> health_checker_;

    std::shared_ptr<Connection> create_connection(const server_t* server);
};

class ConnectionManager {
public:
    static ConnectionManager& instance();

    ConnectionPool* get_pool(const std::string& name);
    void register_pool(const std::string& name, std::unique_ptr<ConnectionPool> pool);

    void cleanup_all();

    // Delete copy and move to enforce singleton
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    ConnectionManager(ConnectionManager&&) = delete;
    ConnectionManager& operator=(ConnectionManager&&) = delete;

private:
    ConnectionManager() {}
    ~ConnectionManager() = default;

    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<ConnectionPool>> pools_;
};

}

#endif