#ifndef CORE_REQUEST_ROUTER_HPP
#define CORE_REQUEST_ROUTER_HPP

#include <string>
#include <vector>
#include <memory>
#include <regex>
#include <functional>
#include <unordered_map>
#include <shared_mutex>

#include "core/proxy.h"

namespace ultrabalancer {

class RouteRule {
public:
    enum class MatchType {
        EXACT,
        PREFIX,
        REGEX,
        HEADER,
        METHOD,
        QUERY_PARAM
    };

    RouteRule(MatchType type, const std::string& pattern);
    ~RouteRule() = default;

    bool matches(const std::string& path, const std::unordered_map<std::string, std::string>& headers) const;

    void set_weight(int weight) { weight_ = weight; }
    int get_weight() const { return weight_; }

private:
    MatchType type_;
    std::string pattern_;
    std::regex regex_;
    int weight_{100};
};

class RouteTarget {
public:
    RouteTarget(const std::string& backend_name, int weight = 100);

    const std::string& get_backend() const { return backend_name_; }
    int get_weight() const { return weight_; }

    void set_retry_policy(int max_retries, std::chrono::milliseconds timeout);
    bool should_retry(int attempt) const;

private:
    std::string backend_name_;
    int weight_;
    int max_retries_{3};
    std::chrono::milliseconds retry_timeout_{1000};
};

class Route {
public:
    Route(const std::string& name);

    void add_rule(std::shared_ptr<RouteRule> rule);
    void add_target(std::shared_ptr<RouteTarget> target);

    bool matches(const std::string& path, const std::unordered_map<std::string, std::string>& headers) const;
    std::shared_ptr<RouteTarget> select_target() const;

    void set_priority(int priority) { priority_ = priority; }
    int get_priority() const { return priority_; }

    void enable_circuit_breaker(int error_threshold, std::chrono::seconds reset_timeout);
    bool is_circuit_open() const;

private:
    std::string name_;
    std::vector<std::shared_ptr<RouteRule>> rules_;
    std::vector<std::shared_ptr<RouteTarget>> targets_;
    int priority_{0};

    mutable std::atomic<int> errors_{0};
    int error_threshold_{50};
    mutable std::atomic<bool> circuit_open_{false};
    mutable std::chrono::steady_clock::time_point circuit_open_time_;
    std::chrono::seconds circuit_reset_timeout_{30};
    mutable std::shared_mutex circuit_mutex_;

    mutable std::atomic<size_t> round_robin_index_{0};
};

class RequestRouter {
public:
    RequestRouter();
    ~RequestRouter() = default;

    void add_route(std::shared_ptr<Route> route);
    void remove_route(const std::string& name);

    std::shared_ptr<RouteTarget> route_request(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers);

    void set_default_backend(const std::string& backend);

    void enable_rate_limiting(const std::string& route_name, int requests_per_second);
    bool check_rate_limit(const std::string& route_name);

    struct RoutingStats {
        std::unordered_map<std::string, uint64_t> route_hits;
        std::unordered_map<std::string, uint64_t> backend_selections;
        uint64_t total_requests;
        uint64_t routed_requests;
        uint64_t default_route_hits;
    };

    RoutingStats get_stats() const;
    void reset_stats();

private:
    std::vector<std::shared_ptr<Route>> routes_;
    std::string default_backend_;
    mutable std::shared_mutex routes_mutex_;

    struct RateLimiter {
        int tokens;
        int max_tokens;
        std::chrono::steady_clock::time_point last_refill;
        std::mutex mutex;
    };

    std::unordered_map<std::string, std::unique_ptr<RateLimiter>> rate_limiters_;
    mutable std::mutex rate_limiters_mutex_;

    mutable RoutingStats stats_;
    mutable std::mutex stats_mutex_;

    void refill_tokens(RateLimiter* limiter, int tokens_per_second);
};

class RouterManager {
public:
    static RouterManager& instance();

    std::shared_ptr<RequestRouter> get_router(const std::string& name);
    void register_router(const std::string& name, std::shared_ptr<RequestRouter> router);

    void configure_from_json(const std::string& json_config);
    std::string export_config_json() const;

private:
    RouterManager() = default;
    ~RouterManager() = default;

    RouterManager(const RouterManager&) = delete;
    RouterManager& operator=(const RouterManager&) = delete;

    std::unordered_map<std::string, std::shared_ptr<RequestRouter>> routers_;
    mutable std::shared_mutex mutex_;
};

}

#endif