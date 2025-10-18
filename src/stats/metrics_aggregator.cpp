#include "stats/metrics_aggregator.hpp"
#include <cstring>

namespace ultrabalancer {

MetricsAggregator::MetricsAggregator() {
    get_or_create("requests.total", Metric::COUNTER);
    get_or_create("requests.success", Metric::COUNTER);
    get_or_create("requests.failed", Metric::COUNTER);
    get_or_create("response.time", Metric::TIMER);
    get_or_create("connections.active", Metric::GAUGE);
    get_or_create("bytes.in", Metric::COUNTER);
    get_or_create("bytes.out", Metric::COUNTER);
    get_or_create("backend.health", Metric::GAUGE);
    get_or_create("cache.hits", Metric::COUNTER);
    get_or_create("cache.misses", Metric::COUNTER);
}

MetricsAggregator& MetricsAggregator::instance() {
    static MetricsAggregator instance;
    return instance;
}

void MetricsAggregator::increment_counter(const std::string& name, double value) {
    auto metric = get_or_create(name, Metric::COUNTER);
    metric->increment(value);
}

void MetricsAggregator::set_gauge(const std::string& name, double value) {
    auto metric = get_or_create(name, Metric::GAUGE);
    metric->set(value);
}

void MetricsAggregator::record_timer(const std::string& name,
                                    std::chrono::nanoseconds duration) {
    auto metric = get_or_create(name, Metric::TIMER);
    metric->record_time(duration);
}

std::shared_ptr<Metric> MetricsAggregator::get_metric(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    return it != metrics_.end() ? it->second : nullptr;
}

std::unordered_map<std::string, std::shared_ptr<Metric>>
MetricsAggregator::get_all_metrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

MetricsAggregator::Stats MetricsAggregator::get_stats() {
    Stats stats{};

    if (auto m = get_metric("requests.total")) {
        stats.total_requests = m->get_count();
    }

    if (auto m = get_metric("requests.success")) {
        stats.successful_requests = m->get_count();
        // stats.failed_requests =
        //     stats.total_requests > stats.successful_requests
        //         ? stats.total_requests - stats.successful_requests
        //         : 0;
    }

    if (auto m = get_metric("requests.failed")) {
        stats.failed_requests = m->get_count();
    }

    if (auto m = get_metric("response.time")) {
        stats.avg_response_time_ms = m->get_mean();
        auto percentiles = m->get_percentiles({50, 95, 99});
        if (percentiles.size() >= 3) {
            stats.p50_response_time_ms = percentiles[0];
            stats.p95_response_time_ms = percentiles[1];
            stats.p99_response_time_ms = percentiles[2];
        }
    }

    if (auto m = get_metric("connections.active")) {
        stats.active_connections = static_cast<uint64_t>(m->get_gauge());
    }

    if (auto m = get_metric("bytes.in")) {
        stats.total_bytes_in = m->get_count();
    }

    if (auto m = get_metric("bytes.out")) {
        stats.total_bytes_out = m->get_count();
    }

    return stats;
}

void MetricsAggregator::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.clear();

    get_or_create("requests.total", Metric::COUNTER);
    get_or_create("requests.success", Metric::COUNTER);
    get_or_create("requests.failed", Metric::COUNTER);
    get_or_create("response.time", Metric::TIMER);
    get_or_create("connections.active", Metric::GAUGE);
    get_or_create("bytes.in", Metric::COUNTER);
    get_or_create("bytes.out", Metric::COUNTER);
    // get_all_metrics().clear();
}

std::shared_ptr<Metric> MetricsAggregator::get_or_create(const std::string& name,
                                                        Metric::Type type) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return it->second;
    }

    auto metric = std::make_shared<Metric>(name, type);
    metrics_[name] = metric;
    return metric;
}

}

extern "C" {

void metrics_increment_counter(const char* name, double value) {
    ultrabalancer::MetricsAggregator::instance().increment_counter(name, value);
}

void metrics_set_gauge(const char* name, double value) {
    ultrabalancer::MetricsAggregator::instance().set_gauge(name, value);
}

void metrics_record_timer_ns(const char* name, uint64_t nanoseconds) {
    ultrabalancer::MetricsAggregator::instance().record_timer(
        name, std::chrono::nanoseconds(nanoseconds));
}

void metrics_get_stats(void* stats_struct) {
    auto stats = ultrabalancer::MetricsAggregator::instance().get_stats();
    if (stats_struct) {
        memcpy(stats_struct, &stats, sizeof(stats));
    }
}

void metrics_reset() {
    ultrabalancer::MetricsAggregator::instance().reset_stats();
}

uint64_t metrics_get_counter(const char* name) {
    auto metric = ultrabalancer::MetricsAggregator::instance().get_metric(name);
    return metric ? metric->get_count() : 0;
}

double metrics_get_gauge(const char* name) {
    auto metric = ultrabalancer::MetricsAggregator::instance().get_metric(name);
    return metric ? metric->get_gauge() : 0.0;
}

double metrics_get_timer_mean(const char* name) {
    auto metric = ultrabalancer::MetricsAggregator::instance().get_metric(name);
    return metric ? metric->get_mean() : 0.0;
}

void metrics_get_percentiles(const char* name, double* p50, double* p95, double* p99) {
    auto metric = ultrabalancer::MetricsAggregator::instance().get_metric(name);
    if (metric) {
        auto percentiles = metric->get_percentiles({50, 95, 99});
        if (percentiles.size() >= 3) {
            if (p50) *p50 = percentiles[0];
            if (p95) *p95 = percentiles[1];
            if (p99) *p99 = percentiles[2];
        }
    }
}

}
