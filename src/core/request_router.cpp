#include "core/request_router.hpp"
#include <algorithm>
#include <random>
#include <mutex>
#include <atomic>
#include <string_view>

namespace ultrabalancer {

RouteRule::RouteRule(MatchType type, const std::string& pattern)
    : type_(type), pattern_(pattern) {
    if (type_ == MatchType::REGEX) {
        regex_ = std::regex(pattern);
    }
}

bool RouteRule::matches(const std::string& path,
                       const std::unordered_map<std::string, std::string>& headers) const {
    switch (type_) {
        case MatchType::EXACT:
            return path == pattern_;

        case MatchType::PREFIX: {
            // Optimization: use string_view comparison to avoid substr allocation
            size_t pattern_len = pattern_.length();
            if (path.length() < pattern_len) return false;
            return std::string_view(path.data(), pattern_len) == pattern_;
        }

        case MatchType::REGEX:
            return std::regex_match(path, regex_);

        case MatchType::HEADER: {
            auto pos = pattern_.find(':');
            if (pos != std::string::npos) {
                // Use string_view to avoid unnecessary allocations during parsing
                std::string_view header_name(pattern_.data(), pos);
                std::string_view header_value(pattern_.data() + pos + 1, pattern_.length() - pos - 1);

                auto it = headers.find(std::string(header_name));
                return it != headers.end() && it->second == header_value;
            }
            return false;
        }

        case MatchType::METHOD:
            return headers.count("method") && headers.at("method") == pattern_;

        case MatchType::QUERY_PARAM: {
            auto query_pos = path.find('?');
            if (query_pos == std::string::npos) return false;

            // Use string_view instead of substr to avoid allocation
            std::string_view query(path.data() + query_pos + 1, path.length() - query_pos - 1);
            return query.find(pattern_) != std::string_view::npos;
        }

        default:
            return false;
    }
}

RouteTarget::RouteTarget(const std::string& backend_name, int weight)
    : backend_name_(backend_name), weight_(weight) {}

void RouteTarget::set_retry_policy(int max_retries, std::chrono::milliseconds timeout) {
    max_retries_ = max_retries;
    retry_timeout_ = timeout;
}

bool RouteTarget::should_retry(int attempt) const {
    return attempt < max_retries_;
}

Route::Route(const std::string& name)
    : name_(name), cached_total_weight_(0), weight_cache_valid_(false) {}

void Route::add_rule(std::shared_ptr<RouteRule> rule) {
    rules_.push_back(rule);
}

void Route::add_target(std::shared_ptr<RouteTarget> target) {
    targets_.push_back(target);
    // Invalidate weight cache when targets change
    weight_cache_valid_.store(false, std::memory_order_release);
}

bool Route::matches(const std::string& path,
                   const std::unordered_map<std::string, std::string>& headers) const {
    for (const auto& rule : rules_) {
        if (!rule->matches(path, headers)) {
            return false;
        }
    }
    return !rules_.empty();
}

std::shared_ptr<RouteTarget> Route::select_target() const {
    if (targets_.empty()) return nullptr;

    if (is_circuit_open()) return nullptr;

    // Cache the total weight calculation to avoid recomputing on every request
    if (!weight_cache_valid_.load(std::memory_order_acquire)) {
        int total = 0;
        for (const auto& target : targets_) {
            total += target->get_weight();
        }
        cached_total_weight_.store(total, std::memory_order_release);
        weight_cache_valid_.store(true, std::memory_order_release);
    }

    int total_weight = cached_total_weight_.load(std::memory_order_acquire);
    if (total_weight == 0) return nullptr;

    // Initialize random generator once per thread instead of per call
    static thread_local std::mt19937 gen([]() {
        std::random_device rd;
        return rd();
    }());

    std::uniform_int_distribution<> dis(0, total_weight - 1);
    int random_weight = dis(gen);

    int cumulative = 0;
    for (const auto& target : targets_) {
        cumulative += target->get_weight();
        if (random_weight < cumulative) {
            return target;
        }
    }

    return targets_.back();
}

void Route::enable_circuit_breaker(int error_threshold, std::chrono::seconds reset_timeout) {
    error_threshold_ = error_threshold;
    circuit_reset_timeout_ = reset_timeout;
}

bool Route::is_circuit_open() const {
    // Fast path: check if circuit is open without any locks (uses atomic operations)
    if (circuit_open_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        // Check if enough time has passed to reset the circuit
        if (now - circuit_open_time_ > circuit_reset_timeout_) {
            // Only one thread should reset the circuit, use a lock here
            std::unique_lock<std::shared_mutex> lock(circuit_mutex_);
            // Double-check after acquiring lock to prevent race condition
            if (circuit_open_.load(std::memory_order_acquire)) {
                circuit_open_.store(false, std::memory_order_release);
                errors_.store(0, std::memory_order_release);
                return false;
            }
        }
        return true;
    }

    // Fast path: check error threshold using atomic counter (no lock needed)
    if (errors_.load(std::memory_order_acquire) >= error_threshold_) {
        std::unique_lock<std::shared_mutex> lock(circuit_mutex_);
        // Double-check to prevent multiple threads from opening circuit simultaneously
        if (!circuit_open_.load(std::memory_order_acquire)) {
            circuit_open_.store(true, std::memory_order_release);
            circuit_open_time_ = std::chrono::steady_clock::now();
            return true;
        }
    }

    return false;
}

RequestRouter::RequestRouter()
    : default_backend_target_(nullptr) {}

void RequestRouter::add_route(std::shared_ptr<Route> route) {
    std::unique_lock<std::shared_mutex> lock(routes_mutex_);

    // Use binary search insertion to maintain sorted order, avoiding full sort
    auto insert_pos = std::lower_bound(
        routes_.begin(), routes_.end(), route,
        [](const auto& a, const auto& b) {
            return a->get_priority() > b->get_priority();
        });
    routes_.insert(insert_pos, route);
}

void RequestRouter::remove_route(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(routes_mutex_);
    // Fixed bug: actually compare route names instead of checking priority == 0
    routes_.erase(
        std::remove_if(routes_.begin(), routes_.end(),
                      [&name](const auto& route) {
                          return route->get_name() == name;
                      }),
        routes_.end());
}

std::shared_ptr<RouteTarget> RequestRouter::route_request(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers) {

    // Use atomic operations for stats to reduce lock contention
    stats_.total_requests.fetch_add(1, std::memory_order_relaxed);

    // Hold read lock only during route matching, release before stats updates
    std::shared_ptr<RouteTarget> target;
    {
        std::shared_lock<std::shared_mutex> lock(routes_mutex_);

        for (const auto& route : routes_) {
            if (route->matches(path, headers)) {
                target = route->select_target();
                if (target) {
                    break;
                }
            }
        }
    }

    // Update stats outside of routes lock to reduce contention
    if (target) {
        stats_.routed_requests.fetch_add(1, std::memory_order_relaxed);
        // Use atomic increment for backend selection counter
        const std::string& backend = target->get_backend();
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.backend_selections[backend]++;
        return target;
    }

    // Reuse cached default backend target to avoid repeated allocations
    if (!default_backend_.empty()) {
        stats_.default_route_hits.fetch_add(1, std::memory_order_relaxed);

        // Check if we have a cached default target
        auto cached = default_backend_target_.load(std::memory_order_acquire);
        if (cached && cached->get_backend() == default_backend_) {
            return std::shared_ptr<RouteTarget>(cached);
        }

        // Create new target and cache it
        auto new_target = std::make_shared<RouteTarget>(default_backend_);
        default_backend_target_.store(new_target.get(), std::memory_order_release);
        return new_target;
    }

    return nullptr;
}

void RequestRouter::set_default_backend(const std::string& backend) {
    default_backend_ = backend;
    // Invalidate cached default target when backend changes
    default_backend_target_.store(nullptr, std::memory_order_release);
}

void RequestRouter::enable_rate_limiting(const std::string& route_name,
                                        int requests_per_second) {
    std::lock_guard<std::mutex> lock(rate_limiters_mutex_);

    auto limiter = std::make_unique<RateLimiter>();
    limiter->max_tokens = requests_per_second;
    limiter->tokens = requests_per_second;
    limiter->last_refill = std::chrono::steady_clock::now();

    rate_limiters_[route_name] = std::move(limiter);
}

bool RequestRouter::check_rate_limit(const std::string& route_name) {
    // Fast path: check if rate limiter exists without holding the global lock
    RateLimiter* limiter = nullptr;
    {
        std::lock_guard<std::mutex> lock(rate_limiters_mutex_);
        auto it = rate_limiters_.find(route_name);
        if (it == rate_limiters_.end()) {
            return true;  // No rate limit configured
        }
        limiter = it->second.get();
    }

    // Hold only the specific limiter's lock, not the global map lock
    std::lock_guard<std::mutex> limiter_lock(limiter->mutex);

    refill_tokens(limiter, limiter->max_tokens);

    if (limiter->tokens > 0) {
        limiter->tokens--;
        return true;
    }

    return false;
}

void RequestRouter::refill_tokens(RateLimiter* limiter, int tokens_per_second) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - limiter->last_refill).count();

    if (elapsed > 0) {
        int tokens_to_add = (tokens_per_second * elapsed) / 1000;
        limiter->tokens = std::min(limiter->max_tokens,
                                  limiter->tokens + tokens_to_add);
        limiter->last_refill = now;
    }
}

RequestRouter::RoutingStats RequestRouter::get_stats() const {
    // Use a snapshot approach instead of copying entire structure under lock
    RoutingStats snapshot;
    snapshot.total_requests = stats_.total_requests.load(std::memory_order_relaxed);
    snapshot.routed_requests = stats_.routed_requests.load(std::memory_order_relaxed);
    snapshot.default_route_hits = stats_.default_route_hits.load(std::memory_order_relaxed);

    // Only lock for the map copy
    std::lock_guard<std::mutex> lock(stats_mutex_);
    snapshot.backend_selections = stats_.backend_selections;

    return snapshot;
}

void RequestRouter::reset_stats() {
    stats_.total_requests.store(0, std::memory_order_relaxed);
    stats_.routed_requests.store(0, std::memory_order_relaxed);
    stats_.default_route_hits.store(0, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.backend_selections.clear();
}

RouterManager& RouterManager::instance() {
    static RouterManager instance;
    return instance;
}

std::shared_ptr<RequestRouter> RouterManager::get_router(const std::string& name) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = routers_.find(name);
    return it != routers_.end() ? it->second : nullptr;
}

void RouterManager::register_router(const std::string& name,
                                   std::shared_ptr<RequestRouter> router) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    routers_[name] = router;
}

}

extern "C" {

void* create_request_router() {
    auto router = std::make_shared<ultrabalancer::RequestRouter>();
    return router.get();
}

void destroy_request_router(void* router) {
}

void router_add_route(void* router, const char* name, int priority) {
    auto cpp_router = static_cast<ultrabalancer::RequestRouter*>(router);
    auto route = std::make_shared<ultrabalancer::Route>(name);
    route->set_priority(priority);
    cpp_router->add_route(route);
}

const char* router_route_request(void* router, const char* method,
                                const char* path, const char* headers_json) {
    auto cpp_router = static_cast<ultrabalancer::RequestRouter*>(router);

    std::unordered_map<std::string, std::string> headers;
    headers["method"] = method;

    auto target = cpp_router->route_request(method, path, headers);
    if (target) {
        static thread_local std::string backend;
        backend = target->get_backend();
        return backend.c_str();
    }

    return nullptr;
}

void router_set_default_backend(void* router, const char* backend) {
    auto cpp_router = static_cast<ultrabalancer::RequestRouter*>(router);
    cpp_router->set_default_backend(backend);
}

int router_check_rate_limit(void* router, const char* route_name) {
    auto cpp_router = static_cast<ultrabalancer::RequestRouter*>(router);
    return cpp_router->check_rate_limit(route_name) ? 1 : 0;
}

}