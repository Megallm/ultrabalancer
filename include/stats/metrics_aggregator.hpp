#ifndef STATS_METRICS_AGGREGATOR_HPP
#define STATS_METRICS_AGGREGATOR_HPP

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <algorithm>

namespace ultrabalancer {

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t size)
        : size_(size), buffer_(size), head_(0), tail_(0), full_(false) {}

    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_[head_] = item;

        if (full_) {
            tail_ = (tail_ + 1) % size_;
        }

        head_ = (head_ + 1) % size_;
        full_ = head_ == tail_;
    }

    std::vector<T> get_all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> result;

        if (full_) {
            result.reserve(size_);
            for (size_t i = tail_; i != head_; i = (i + 1) % size_) {
                result.push_back(buffer_[i]);
            }
        } else {
            result.reserve(head_);
            for (size_t i = 0; i < head_; ++i) {
                result.push_back(buffer_[i]);
            }
        }

        return result;
    }

private:
    size_t size_;
    std::vector<T> buffer_;
    size_t head_;
    size_t tail_;
    bool full_;
    mutable std::mutex mutex_;
};

struct TimeSeries {
    std::chrono::steady_clock::time_point timestamp;
    double value;
};

class Metric {
public:
    enum Type {
        COUNTER,
        GAUGE,
        HISTOGRAM,
        TIMER
    };

    Metric(const std::string& name, Type type)
        : name_(name), type_(type), count_(0), sum_(0), min_(0), max_(0) {}

    void increment(double value = 1.0) {
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(static_cast<uint64_t>(value * 1000000), std::memory_order_relaxed);
        update_min_max(value);
    }

    void set(double value) {
        gauge_value_.store(value, std::memory_order_relaxed);
    }

    void record_time(std::chrono::nanoseconds duration) {
        double ms = duration.count() / 1000000.0;
        increment(ms);
        time_series_.push({std::chrono::steady_clock::now(), ms});
    }

    double get_mean() const {
        uint64_t c = count_.load(std::memory_order_relaxed);
        if (c == 0) return 0.0;
        return (sum_.load(std::memory_order_relaxed) / 1000000.0) / c;
    }

    uint64_t get_count() const {
        return count_.load(std::memory_order_relaxed);
    }

    double get_gauge() const {
        return gauge_value_.load(std::memory_order_relaxed);
    }

    std::vector<TimeSeries> get_time_series() const {
        return time_series_.get_all();
    }

    std::vector<double> get_percentiles(const std::vector<double>& percentiles) const {
        auto series = time_series_.get_all();
        if (series.empty()) return std::vector<double>(percentiles.size(), 0.0);

        std::vector<double> values;
        values.reserve(series.size());
        for (const auto& ts : series) {
            values.push_back(ts.value);
        }

        std::sort(values.begin(), values.end());

        std::vector<double> result;
        for (double p : percentiles) {
            size_t idx = static_cast<size_t>(values.size() * p / 100.0);
            if (idx >= values.size()) idx = values.size() - 1;
            result.push_back(values[idx]);
        }

        return result;
    }

private:
    void update_min_max(double value) {
        double current_min = min_.load(std::memory_order_relaxed);
        while (current_min > value &&
               !min_.compare_exchange_weak(current_min, value, std::memory_order_relaxed));

        double current_max = max_.load(std::memory_order_relaxed);
        while (current_max < value &&
               !max_.compare_exchange_weak(current_max, value, std::memory_order_relaxed));
    }

    std::string name_;
    Type type_;
    std::atomic<uint64_t> count_;
    std::atomic<uint64_t> sum_;
    std::atomic<double> min_;
    std::atomic<double> max_;
    std::atomic<double> gauge_value_;
    RingBuffer<TimeSeries> time_series_{10000};
};

class MetricsAggregator {
public:
    static MetricsAggregator& instance();

    void increment_counter(const std::string& name, double value = 1.0);
    void set_gauge(const std::string& name, double value);
    void record_timer(const std::string& name, std::chrono::nanoseconds duration);

    std::shared_ptr<Metric> get_metric(const std::string& name);
    std::unordered_map<std::string, std::shared_ptr<Metric>> get_all_metrics();

    struct Stats {
        uint64_t total_requests;
        uint64_t successful_requests;
        uint64_t failed_requests;
        double avg_response_time_ms;
        double p50_response_time_ms;
        double p95_response_time_ms;
        double p99_response_time_ms;
        uint64_t active_connections;
        uint64_t total_bytes_in;
        uint64_t total_bytes_out;
    };

    Stats get_stats();
    void reset_stats();

private:
    MetricsAggregator();
    ~MetricsAggregator() = default;

    MetricsAggregator(const MetricsAggregator&) = delete;
    MetricsAggregator& operator=(const MetricsAggregator&) = delete;

    std::shared_ptr<Metric> get_or_create(const std::string& name, Metric::Type type);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Metric>> metrics_;
};

class ScopedTimer {
public:
    ScopedTimer(const std::string& metric_name)
        : metric_name_(metric_name),
          start_time_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        auto duration = std::chrono::steady_clock::now() - start_time_;
        MetricsAggregator::instance().record_timer(metric_name_,
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
    }

private:
    std::string metric_name_;
    std::chrono::steady_clock::time_point start_time_;
};

}

#endif