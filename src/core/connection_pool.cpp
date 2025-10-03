#include "core/connection_pool.hpp"
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

namespace ultrabalancer {

Connection::Connection(int fd, const sockaddr_storage& addr)
    : fd_(fd), addr_(addr), alive_(true), last_used_(std::chrono::steady_clock::now()) {
    int flag = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
}

Connection::~Connection() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool Connection::is_alive() const {
    if (!alive_) return false;

    char buf;
    int ret = recv(fd_, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // Don't modify alive_ in const function - let caller handle this
        return false;
    }
    return true;
}

void Connection::reset() {
    last_used_ = std::chrono::steady_clock::now();
}

ConnectionPool::ConnectionPool(size_t max_size, size_t max_idle)
    : max_size_(max_size), max_idle_(max_idle), active_(0) {}

ConnectionPool::~ConnectionPool() {
    cleanup_idle(std::chrono::seconds(0)); // Clean up all idle connections
}

std::shared_ptr<Connection> ConnectionPool::acquire(const server_t* server) {
    std::unique_lock<std::mutex> lock(mutex_);

    ServerKey key{server->hostname ? server->hostname : "", server->port};

    auto& queue = idle_queue_[key];
    while (!queue.empty()) {
        auto conn = queue.front();
        queue.pop();

        if (conn->is_alive()) {
            active_++;
            return conn;
        }
    }

    cv_.wait(lock, [this] { return active_ < max_size_; });

    auto conn = create_connection(server);
    if (conn) {
        active_++;
    }
    return conn;
}

void ConnectionPool::release(std::shared_ptr<Connection> conn) {
    if (!conn) return;

    std::lock_guard<std::mutex> lock(mutex_);
    active_--;

    if (!conn->is_alive()) {
        cv_.notify_one();
        return;
    }

    conn->reset();

    for (auto& [key, queue] : idle_queue_) {
        if (queue.size() < max_idle_) {
            queue.push(conn);
            cv_.notify_one();
            return;
        }
    }

    cv_.notify_one();
}

void ConnectionPool::set_health_check(std::function<bool(Connection*)> checker) {
    health_checker_ = checker;
}

void ConnectionPool::cleanup_idle(std::chrono::seconds idle_timeout) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    for (auto& [key, queue] : idle_queue_) {
        std::queue<std::shared_ptr<Connection>> new_queue;

        while (!queue.empty()) {
            auto conn = queue.front();
            queue.pop();

            auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
                now - conn->last_used());

            if (idle_time < idle_timeout && conn->is_alive()) {
                new_queue.push(conn);
            }
        }

        queue = std::move(new_queue);
    }

    cv_.notify_all();
}

std::shared_ptr<Connection> ConnectionPool::create_connection(const server_t* server) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return nullptr;

    struct sockaddr_in* sin = (struct sockaddr_in*)&server->addr;
    if (connect(fd, (struct sockaddr*)sin, sizeof(*sin)) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return nullptr;
        }
    }

    return std::make_shared<Connection>(fd, server->addr);
}

ConnectionManager& ConnectionManager::instance() {
    static ConnectionManager instance;
    return instance;
}

ConnectionPool* ConnectionManager::get_pool(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(name);
    return it != pools_.end() ? it->second.get() : nullptr;
}

void ConnectionManager::register_pool(const std::string& name,
                                     std::unique_ptr<ConnectionPool> pool) {
    std::lock_guard<std::mutex> lock(mutex_);
    pools_[name] = std::move(pool);
}

void ConnectionManager::cleanup_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    pools_.clear();
}

}

extern "C" {

void* create_connection_pool(size_t max_size, size_t max_idle) {
    auto pool = std::make_unique<ultrabalancer::ConnectionPool>(max_size, max_idle);
    return pool.release();
}

void destroy_connection_pool(void* pool) {
    delete static_cast<ultrabalancer::ConnectionPool*>(pool);
}

int acquire_connection(void* pool, struct server* srv) {
    auto cpp_pool = static_cast<ultrabalancer::ConnectionPool*>(pool);
    auto conn = cpp_pool->acquire(srv);
    return conn ? conn->get_fd() : -1;
}

void release_connection(void* pool, int fd) {
}

size_t get_pool_active_connections(void* pool) {
    auto cpp_pool = static_cast<ultrabalancer::ConnectionPool*>(pool);
    return cpp_pool->active_connections();
}

size_t get_pool_idle_connections(void* pool) {
    auto cpp_pool = static_cast<ultrabalancer::ConnectionPool*>(pool);
    return cpp_pool->idle_connections();
}

}