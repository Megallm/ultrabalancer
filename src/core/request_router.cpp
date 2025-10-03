#include "core/request_router.hpp"
#include <algorithm>
#include <random>
#include <mutex>

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

        case MatchType::PREFIX:
            return path.substr(0, pattern_.length()) == pattern_;

        case MatchType::REGEX:
            return std::regex_match(path, regex_);

        case MatchType::HEADER: {
            auto pos = pattern_.find(':');
            if (pos != std::string::npos) {
                std::string header_name = pattern_.substr(0, pos);
                std::string header_value = pattern_.substr(pos + 1);
                auto it = headers.find(header_name);
                return it != headers.end() && it->second == header_value;
            }
            return false;
        }

        case MatchType::METHOD:
            return headers.count("method") && headers.at("method") == pattern_;

        case MatchType::QUERY_PARAM: {
            auto query_pos = path.find('?');
            if (query_pos == std::string::npos) return false;

            std::string query = path.substr(query_pos + 1);
            return query.find(pattern_) != std::string::npos;
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

Route::Route(const std::string& name) : name_(name) {}

void Route::add_rule(std::shared_ptr<RouteRule> rule) {
    rules_.push_back(rule);
}

void Route::add_target(std::shared_ptr<RouteTarget> target) {
    targets_.push_back(target);
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

    int total_weight = 0;
    for (const auto& target : targets_) {
        total_weight += target->get_weight();
    }

    if (total_weight == 0) return nullptr;

    static thread_local std::mt19937 gen(std::random_device{}());
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
    std::shared_lock<std::shared_mutex> lock(circuit_mutex_);

    if (circuit_open_) {
        auto now = std::chrono::steady_clock::now();
        if (now - circuit_open_time_ > circuit_reset_timeout_) {
            lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(circuit_mutex_);
            circuit_open_ = false;
            errors_ = 0;
            return false;
        }
        return true;
    }

    if (errors_ >= error_threshold_) {
        lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(circuit_mutex_);
        circuit_open_ = true;
        circuit_open_time_ = std::chrono::steady_clock::now();
        return true;
    }

    return false;
}

RequestRouter::RequestRouter() {}

void RequestRouter::add_route(std::shared_ptr<Route> route) {
    std::unique_lock<std::shared_mutex> lock(routes_mutex_);
    routes_.push_back(route);
    std::sort(routes_.begin(), routes_.end(),
             [](const auto& a, const auto& b) {
                 return a->get_priority() > b->get_priority();
             });
}

void RequestRouter::remove_route(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(routes_mutex_);
    routes_.erase(
        std::remove_if(routes_.begin(), routes_.end(),
                      [&name](const auto& route) {
                          return route->get_priority() == 0;
                      }),
        routes_.end());
}

std::shared_ptr<RouteTarget> RequestRouter::route_request(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers) {

    std::shared_lock<std::shared_mutex> lock(routes_mutex_);

    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_requests++;
    }

    for (const auto& route : routes_) {
        if (route->matches(path, headers)) {
            auto target = route->select_target();
            if (target) {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.routed_requests++;
                stats_.backend_selections[target->get_backend()]++;
                return target;
            }
        }
    }

    if (!default_backend_.empty()) {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.default_route_hits++;
        return std::make_shared<RouteTarget>(default_backend_);
    }

    return nullptr;
}

void RequestRouter::set_default_backend(const std::string& backend) {
    default_backend_ = backend;
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
    std::lock_guard<std::mutex> lock(rate_limiters_mutex_);

    auto it = rate_limiters_.find(route_name);
    if (it == rate_limiters_.end()) {
        return true;
    }

    auto* limiter = it->second.get();
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
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void RequestRouter::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = RoutingStats{};
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