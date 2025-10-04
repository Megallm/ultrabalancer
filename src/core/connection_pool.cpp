#include "core/connection_pool.hpp"
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <shared_mutex>

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
    // check atomic flag first before syscall
    if (!alive_.load(std::memory_order_acquire)) return false;

    char buf;
    int ret = recv(fd_, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // connection is dead update atomic flag for future checks
        alive_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void Connection::reset() {
    last_used_ = std::chrono::steady_clock::now();
    // Ensure connection is marked alive when reset (reused from pool)
    alive_.store(true, std::memory_order_release);
}

ConnectionPool::ConnectionPool(size_t max_size, size_t max_idle)
    : max_size_(max_size), max_idle_(max_idle), active_(0) {}

ConnectionPool::~ConnectionPool() {
    cleanup_idle(std::chrono::seconds(0)); // idle connection cleanup 
}

std::shared_ptr<Connection> ConnectionPool::acquire(const server_t* server) {
    ServerKey key{server->hostname ? server->hostname : "", server->port};

    // Try to acquire from idle queue with minimal lock time
    {
        std::unique_lock<std::mutex> lock(mutex_);

        auto it = idle_queue_.find(key);
        if (it != idle_queue_.end()) {
            auto& queue = it->second;

            // Search for a live connection in the queue
            while (!queue.empty()) {
                auto conn = queue.front();
                queue.pop();

                // Check liveness without holding the lock for syscall
                lock.unlock();
                bool alive = conn->is_alive();
                lock.lock();

                if (alive) {
                    active_.fetch_add(1, std::memory_order_relaxed);
                    conn->reset();
                    return conn;
                }
                // Dead connection - let it be destroyed
            }
        }

        // No idle connection available - check if we can create new one
        size_t current_active = active_.load(std::memory_order_relaxed);
        if (current_active >= max_size_) {
            // Wait for a connection to be released
            cv_.wait(lock, [this] {
                return active_.load(std::memory_order_relaxed) < max_size_;
            });
        }

        active_.fetch_add(1, std::memory_order_relaxed);
    }

    // Create connection outside of lock to avoid blocking other threads
    auto conn = create_connection(server);
    if (!conn) {
        // Failed to create - decrement active counter
        active_.fetch_sub(1, std::memory_order_relaxed);
        cv_.notify_one();
    }
    return conn;
}

void ConnectionPool::release(std::shared_ptr<Connection> conn) {
    if (!conn) return;

    // Decrement active counter immediately using atomic operation
    active_.fetch_sub(1, std::memory_order_relaxed);

    // Check if connection is still alive before returning to pool
    if (!conn->is_alive()) {
        cv_.notify_one();
        return;
    }

    conn->reset();

    // Determine the server key for this connection
    // We need to find which queue this connection belongs to
    std::lock_guard<std::mutex> lock(mutex_);

    // Simple heuristic: add to first queue that has space
    // In production, we'd want to track the original server key
    for (auto& [key, queue] : idle_queue_) {
        if (queue.size() < max_idle_) {
            queue.push(conn);
            cv_.notify_one();
            return;
        }
    }

    // If all queues are full, connection is dropped (destructor handles cleanup)
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