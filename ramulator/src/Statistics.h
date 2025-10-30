#ifndef __STATISTICS_H
#define __STATISTICS_H

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cassert>
#include <limits>

// Include the compatibility header
#include "StatType.h"

// Forward declare for compatibility
namespace RamulatorStats {
    extern uint64_t curTick;
}

/*
  SIMPLIFIED STATISTICS IMPLEMENTATION

  This version provides a simplified statistics implementation that maintains
  the same interface as the original gem5-based version but removes the
  complex inheritance hierarchy that was causing runtime errors.

  The following statistics classes are provided:
  - ScalarStat: Single scalar value
  - AverageStat: Average value over time
  - VectorStat: Vector of scalar values
  - AverageVectorStat: Vector of average values
  - DistributionStat: Distribution of values
  - HistogramStat: Histogram of values
  - StandardDeviationStat: Standard deviation computation
  - AverageDeviationStat: Average deviation computation

  All stats are automatically named "ramulator.<your name>" and dumped
  at the end of simulation.
*/

namespace ramulator {

// Forward declarations
class StatBase;

// Statistics storage and management
extern std::vector<StatBase*>& get_all_stats();
extern std::ofstream stats_output;
extern uint64_t current_tick;

void reset_stats();
void print_stats(const std::string& filename);

// Base class for all statistics
class StatBase {
protected:
    std::string stat_name;
    std::string stat_desc;
    int stat_precision = 1;
    bool display_flag = true;
    bool nozero_flag = false;

public:
    StatBase() {
        get_all_stats().push_back(this);
    }

    virtual ~StatBase() = default;

    StatBase& name(const std::string& n) {
        stat_name = n;
        return *this;
    }

    const std::string& name() const {
        return stat_name;
    }

    StatBase& desc(const std::string& d) {
        stat_desc = d;
        return *this;
    }

    StatBase& precision(int p) {
        stat_precision = p;
        return *this;
    }

    StatBase& flags(uint16_t f) {
        display_flag = !(f & 0x0100); // nozero flag inverted
        nozero_flag = (f & 0x0100);
        return *this;
    }

    template<class T>
    StatBase& prereq(const T& /*prereq_stat*/) {
        // Simplified - ignore prerequisites
        return *this;
    }

    virtual void print(std::ofstream& file) = 0;
    virtual void reset() = 0;
    virtual bool zero() const = 0;
    virtual void prepare() {}

    bool should_display() const {
        return display_flag && (!nozero_flag || !zero());
    }
};

// ScalarStat: Simple counter/value
class ScalarStat : public StatBase {
private:
    double value_;

public:
    ScalarStat() : value_(0.0) {}

    double value() const { return value_; }

    // Assignment operators
    template<typename T>
    ScalarStat& operator=(const T& v) {
        value_ = static_cast<double>(v);
        return *this;
    }

    // Increment/decrement operators
    ScalarStat& operator++() {
        ++value_;
        return *this;
    }

    ScalarStat& operator++(int) {
        value_++;
        return *this;
    }

    ScalarStat& operator--() {
        --value_;
        return *this;
    }

    ScalarStat& operator--(int) {
        value_--;
        return *this;
    }

    // Arithmetic assignment operators
    template<typename T>
    ScalarStat& operator+=(const T& v) {
        value_ += static_cast<double>(v);
        return *this;
    }

    template<typename T>
    ScalarStat& operator-=(const T& v) {
        value_ -= static_cast<double>(v);
        return *this;
    }

    void print(std::ofstream& file) override {
        std::string stat_full_name = "ramulator." + stat_name;
        file << std::setw(40) << std::right << stat_full_name;
        file << std::setw(20) << std::right << std::fixed;
        file.precision(stat_precision);
        file << value_;
        if (!stat_desc.empty()) {
            file << std::setw(40) << std::right << " # " << stat_desc;
        }
        file << std::endl;
    }

    void reset() override {
        value_ = 0.0;
    }

    bool zero() const override {
        return std::abs(value_) < 1e-8;
    }
};

// AverageStat: Average value over time
class AverageStat : public StatBase {
private:
    double current_value_;
    double total_value_;
    uint64_t last_tick_;
    uint64_t start_tick_;

    uint64_t get_current_tick() const {
        // Use RamulatorStats::curTick for compatibility
        return RamulatorStats::curTick;
    }

public:
    AverageStat() : current_value_(0.0), total_value_(0.0) {
        last_tick_ = get_current_tick();
        start_tick_ = get_current_tick();
    }

    double value() const { return current_value_; }

    void set(double val) {
        uint64_t cur_tick = get_current_tick();
        total_value_ += current_value_ * (cur_tick - last_tick_);
        last_tick_ = cur_tick;
        current_value_ = val;
    }

    template<typename T>
    AverageStat& operator=(const T& v) {
        set(static_cast<double>(v));
        return *this;
    }

    template<typename T>
    AverageStat& operator+=(const T& v) {
        set(current_value_ + static_cast<double>(v));
        return *this;
    }

    template<typename T>
    AverageStat& operator-=(const T& v) {
        set(current_value_ - static_cast<double>(v));
        return *this;
    }

    AverageStat& operator++() {
        set(current_value_ + 1.0);
        return *this;
    }

    AverageStat& operator++(int) {
        set(current_value_ + 1.0);
        return *this;
    }

    AverageStat& operator--() {
        set(current_value_ - 1.0);
        return *this;
    }

    AverageStat& operator--(int) {
        set(current_value_ - 1.0);
        return *this;
    }

    double result() const {
        uint64_t cur_tick = get_current_tick();
        uint64_t total_ticks = cur_tick - start_tick_ + 1;
        if (total_ticks == 0) return 0.0;
        return (total_value_ + current_value_) / total_ticks;
    }

    void print(std::ofstream& file) override {
        std::string stat_full_name = "ramulator." + stat_name;
        file << std::setw(40) << std::right << stat_full_name;
        file << std::setw(20) << std::right << std::fixed;
        file.precision(stat_precision);
        file << result();
        if (!stat_desc.empty()) {
            file << std::setw(40) << std::right << " # " << stat_desc;
        }
        file << std::endl;
    }

    void reset() override {
        current_value_ = 0.0;
        total_value_ = 0.0;
        uint64_t cur_tick = get_current_tick();
        last_tick_ = cur_tick;
        start_tick_ = cur_tick;
    }

    bool zero() const override {
        return std::abs(total_value_) < 1e-8 && std::abs(current_value_) < 1e-8;
    }
};

// Element class for VectorStat
class VectorElement {
private:
    double value_;

public:
    VectorElement() : value_(0.0) {}

    double value() const { return value_; }

    template<typename T>
    VectorElement& operator=(const T& v) {
        value_ = static_cast<double>(v);
        return *this;
    }

    VectorElement& operator++() {
        ++value_;
        return *this;
    }

    VectorElement& operator++(int) {
        value_++;
        return *this;
    }

    template<typename T>
    VectorElement& operator+=(const T& v) {
        value_ += static_cast<double>(v);
        return *this;
    }

    template<typename T>
    VectorElement& operator-=(const T& v) {
        value_ -= static_cast<double>(v);
        return *this;
    }

    void reset() {
        value_ = 0.0;
    }

    bool zero() const {
        return std::abs(value_) < 1e-8;
    }
};

// VectorStat: Vector of scalar values
class VectorStat : public StatBase {
private:
    std::vector<VectorElement> elements_;
    std::vector<std::string> subnames_;
    std::vector<std::string> subdescs_;

public:
    VectorStat() {}

    VectorStat& init(size_t size) {
        elements_.resize(size);
        subnames_.resize(size);
        subdescs_.resize(size);
        return *this;
    }

    VectorElement& operator[](size_t index) {
        if (index >= elements_.size()) {
            elements_.resize(index + 1);
            subnames_.resize(index + 1);
            subdescs_.resize(index + 1);
        }
        return elements_[index];
    }

    size_t size() const {
        return elements_.size();
    }

    VectorStat& subname(size_t index, const std::string& name) {
        if (index >= subnames_.size()) {
            subnames_.resize(index + 1);
        }
        subnames_[index] = name;
        return *this;
    }

    VectorStat& subdesc(size_t index, const std::string& desc) {
        if (index >= subdescs_.size()) {
            subdescs_.resize(index + 1);
        }
        subdescs_[index] = desc;
        return *this;
    }

    void value(std::vector<double>& vec) const {
        vec.resize(elements_.size());
        for (size_t i = 0; i < elements_.size(); ++i) {
            vec[i] = elements_[i].value();
        }
    }

    double total() const {
        double sum = 0.0;
        for (const auto& elem : elements_) {
            sum += elem.value();
        }
        return sum;
    }

    void print(std::ofstream& file) override {
        for (size_t i = 0; i < elements_.size(); ++i) {
            std::string stat_full_name = "ramulator." + stat_name;
            if (!subnames_[i].empty()) {
                stat_full_name += "::" + subnames_[i];
            } else {
                stat_full_name += "_" + std::to_string(i);
            }
            file << std::setw(40) << std::right << stat_full_name;
            file << std::setw(20) << std::right << std::fixed;
            file.precision(stat_precision);
            file << elements_[i].value();
            if (!subdescs_[i].empty()) {
                file << std::setw(40) << std::right << " # " << subdescs_[i];
            } else if (!stat_desc.empty()) {
                file << std::setw(40) << std::right << " # " << stat_desc << " (element " << i << ")";
            }
            file << std::endl;
        }
    }

    void reset() override {
        for (auto& elem : elements_) {
            elem.reset();
        }
    }

    bool zero() const override {
        for (const auto& elem : elements_) {
            if (!elem.zero()) return false;
        }
        return true;
    }
};

// Element class for AverageVectorStat
class AverageElement {
private:
    double current_value_;
    double total_value_;
    uint64_t last_tick_;
    uint64_t start_tick_;

    uint64_t get_current_tick() const {
        return RamulatorStats::curTick;
    }

public:
    AverageElement() : current_value_(0.0), total_value_(0.0) {
        last_tick_ = get_current_tick();
        start_tick_ = get_current_tick();
    }

    double value() const { return current_value_; }

    void set(double val) {
        uint64_t cur_tick = get_current_tick();
        total_value_ += current_value_ * (cur_tick - last_tick_);
        last_tick_ = cur_tick;
        current_value_ = val;
    }

    template<typename T>
    AverageElement& operator=(const T& v) {
        set(static_cast<double>(v));
        return *this;
    }

    template<typename T>
    AverageElement& operator+=(const T& v) {
        set(current_value_ + static_cast<double>(v));
        return *this;
    }

    AverageElement& operator++() {
        set(current_value_ + 1.0);
        return *this;
    }

    AverageElement& operator++(int) {
        set(current_value_ + 1.0);
        return *this;
    }

    double result() const {
        uint64_t cur_tick = get_current_tick();
        uint64_t total_ticks = cur_tick - start_tick_ + 1;
        if (total_ticks == 0) return 0.0;
        return (total_value_ + current_value_) / total_ticks;
    }

    void reset() {
        current_value_ = 0.0;
        total_value_ = 0.0;
        uint64_t cur_tick = get_current_tick();
        last_tick_ = cur_tick;
        start_tick_ = cur_tick;
    }

    bool zero() const {
        return std::abs(total_value_) < 1e-8 && std::abs(current_value_) < 1e-8;
    }
};

// AverageVectorStat: Vector of average values
class AverageVectorStat : public StatBase {
private:
    std::vector<AverageElement> elements_;
    std::vector<std::string> subnames_;
    std::vector<std::string> subdescs_;

public:
    AverageVectorStat() {}

    AverageVectorStat& init(size_t size) {
        elements_.resize(size);
        subnames_.resize(size);
        subdescs_.resize(size);
        return *this;
    }

    AverageElement& operator[](size_t index) {
        if (index >= elements_.size()) {
            elements_.resize(index + 1);
            subnames_.resize(index + 1);
            subdescs_.resize(index + 1);
        }
        return elements_[index];
    }

    size_t size() const {
        return elements_.size();
    }

    AverageVectorStat& subname(size_t index, const std::string& name) {
        if (index >= subnames_.size()) {
            subnames_.resize(index + 1);
        }
        subnames_[index] = name;
        return *this;
    }

    AverageVectorStat& subdesc(size_t index, const std::string& desc) {
        if (index >= subdescs_.size()) {
            subdescs_.resize(index + 1);
        }
        subdescs_[index] = desc;
        return *this;
    }

    void print(std::ofstream& file) override {
        for (size_t i = 0; i < elements_.size(); ++i) {
            std::string stat_full_name = "ramulator." + stat_name;
            if (!subnames_[i].empty()) {
                stat_full_name += "::" + subnames_[i];
            } else {
                stat_full_name += "_" + std::to_string(i);
            }
            file << std::setw(40) << std::right << stat_full_name;
            file << std::setw(20) << std::right << std::fixed;
            file.precision(stat_precision);
            file << elements_[i].result();
            if (!subdescs_[i].empty()) {
                file << std::setw(40) << std::right << " # " << subdescs_[i];
            } else if (!stat_desc.empty()) {
                file << std::setw(40) << std::right << " # " << stat_desc << " (element " << i << ")";
            }
            file << std::endl;
        }
    }

    void reset() override {
        for (auto& elem : elements_) {
            elem.reset();
        }
    }

    bool zero() const override {
        for (const auto& elem : elements_) {
            if (!elem.zero()) return false;
        }
        return true;
    }
};

class DistributionStat : public StatBase {
private:
    // Based on gem5's DistStor implementation
    double min_track_;      // Minimum value to track in buckets
    double max_track_;      // Maximum value to track in buckets  
    double bucket_size_;    // Size of each bucket
    
    double min_val_;        // Smallest value sampled
    double max_val_;        // Largest value sampled
    uint64_t underflow_;    // Count of values < min_track
    uint64_t overflow_;     // Count of values > max_track
    double sum_;            // Sum of all values
    double squares_;        // Sum of squares
    uint64_t samples_;      // Total number of samples
    std::vector<uint64_t> buckets_;  // Bucket counters
    
    size_t num_buckets_;

public:
    DistributionStat() 
        : min_track_(0), max_track_(0), bucket_size_(0),
          min_val_(std::numeric_limits<double>::max()),
          max_val_(std::numeric_limits<double>::lowest()),
          underflow_(0), overflow_(0), sum_(0), squares_(0), 
          samples_(0), num_buckets_(0) {}

    // Initialize with min, max, and bucket size (like gem5's DistStor)
    DistributionStat& init(double min_val, double max_val, size_t bucket_size) {
        min_track_ = min_val;
        max_track_ = max_val;
        bucket_size_ = bucket_size;
        
        // Calculate number of buckets (gem5 logic)
        num_buckets_ = static_cast<size_t>(std::ceil((max_val - min_val + 1.0) / bucket_size));
        buckets_.resize(num_buckets_, 0);
        
        return *this;
    }

    // Sample function (based on gem5's DistStor::sample)
    template<typename T>
    void sample(const T& val, int number = 1) {
        double value = static_cast<double>(val);
        
        // Handle underflow/overflow like gem5
        if (value < min_track_) {
            underflow_ += number;
        } else if (value > max_track_) {
            overflow_ += number;
        } else {
            // Calculate bucket index (gem5 logic)
            size_t bucket_idx = static_cast<size_t>(std::floor((value - min_track_) / bucket_size_));
            if (bucket_idx < buckets_.size()) {
                buckets_[bucket_idx] += number;
            }
        }

        // Update min/max sampled values
        if (value < min_val_) {
            min_val_ = value;
        }
        if (value > max_val_) {
            max_val_ = value;
        }

        // Update statistics
        sum_ += value * number;
        squares_ += value * value * number;
        samples_ += number;
    }

    // Accessor methods
    uint64_t total_samples() const { return samples_; }
    uint64_t underflow() const { return underflow_; }
    uint64_t overflow() const { return overflow_; }
    double mean() const { 
        return samples_ > 0 ? sum_ / samples_ : 0.0; 
    }
    double variance() const {
        if (samples_ <= 1) return 0.0;
        double m = mean();
        return (squares_ / samples_) - (m * m);
    }
    double stddev() const {
        return std::sqrt(variance());
    }

    // Get bucket value
    uint64_t bucket(size_t idx) const {
        return idx < buckets_.size() ? buckets_[idx] : 0;
    }

    size_t num_buckets() const { return num_buckets_; }
    double bucket_size() const { return bucket_size_; }
    double min_track() const { return min_track_; }
    double max_track() const { return max_track_; }

    // StatBase interface implementation
    void print(std::ofstream &file) override {
        std::string base_name = "ramulator." + stat_name;

        // Print basic statistics
        file << std::setw(40) << std::left << (base_name + "::samples");
        file << std::setw(20) << std::right << samples_;
        if (!stat_desc.empty()) {
            file << " # " << stat_desc;
        }
        file << std::endl;

        if (samples_ > 0) {
            file << std::setw(40) << std::left << (base_name + "::mean");
            file << std::setw(20) << std::right << std::fixed;
            file.precision(6);
            file << mean() << std::endl;

            file << std::setw(40) << std::left << (base_name + "::stdev");
            file << std::setw(20) << std::right << std::fixed;
            file.precision(6);
            file << stddev() << std::endl;

            file << std::setw(40) << std::left << (base_name + "::underflows");
            file << std::setw(20) << std::right << underflow_;
            if (!stat_desc.empty()) {
                file << " # " << stat_desc;
            }
            file << std::endl;        

            file << std::setw(40) << std::left << (base_name + "::overflows");
            file << std::setw(20) << std::right << overflow_;
            if (!stat_desc.empty()) {
                file << " # " << stat_desc;
            }
            file << std::endl;


            file << std::setw(40) << std::left << (base_name + "::min_val_");
            file << std::setw(20) << std::right << min_val_;
            if (!stat_desc.empty()) {
                file << " # " << stat_desc;
            }
            file << std::endl;

            file << std::setw(40) << std::left << (base_name + "::max_val_");
            file << std::setw(20) << std::right << max_val_;
            if (!stat_desc.empty()) {
                file << " # " << stat_desc;
            }
            file << std::endl;
            
            // Print bucket distribution
            for (size_t i = 0; i < buckets_.size(); ++i) {
                if (buckets_[i] > 0 || !nozero_flag) {
                    double bucket_min = min_track_ + i * bucket_size_;
                    double bucket_max = bucket_min + bucket_size_ - 1;

                    std::string bucket_name = base_name + "::" + std::to_string(static_cast<int>(bucket_min)) + "-" +
                                              std::to_string(static_cast<int>(bucket_max));

                    file << std::setw(40) << std::left << bucket_name;
                    file << std::setw(20) << std::right << buckets_[i];
                    if (!stat_desc.empty()) {
                        file << " # " << stat_desc;
                    }
                    file << std::endl;
                }
            }
        }
    }

    void reset() override {
        min_val_ = std::numeric_limits<double>::max();
        max_val_ = std::numeric_limits<double>::lowest();
        underflow_ = overflow_ = samples_ = 0;
        sum_ = squares_ = 0.0;
        std::fill(buckets_.begin(), buckets_.end(), 0);
    }

    bool zero() const override {
        return samples_ == 0;
    }
};

class HistogramStat : public StatBase {
private:
    // Based on gem5's HistStor implementation
    double min_bucket_;     // Lower bound of the first bucket's range
    double max_bucket_;     // Lower bound of the last bucket's range  
    double bucket_size_;    // The number of entries in each bucket
    
    double sum_;            // The current sum
    double logs_;           // Sum of logarithms (for geometric mean)
    double squares_;        // Sum of squares
    uint64_t samples_;      // Number of samples
    std::vector<uint64_t> buckets_;  // Counter for each bucket
    
    size_t num_buckets_;

public:
    HistogramStat() 
        : min_bucket_(0), max_bucket_(0), bucket_size_(1),
          sum_(0), logs_(0), squares_(0), samples_(0), num_buckets_(0) {}

    // Initialize with number of buckets (like gem5's Histogram)
    HistogramStat& init(size_t num_buckets) {
        if (num_buckets < 2) {
            throw std::invalid_argument("There must be at least two buckets in a histogram");
        }
        
        num_buckets_ = num_buckets;
        buckets_.resize(num_buckets_, 0);
        
        // Initialize like gem5's HistStor::reset()
        min_bucket_ = 0;
        max_bucket_ = num_buckets - 1;
        bucket_size_ = 1;
        
        return *this;
    }

    // Sample function (based on gem5's HistStor::sample)
    template<typename T>
    void sample(const T& val, int number = 1) {
        double value = static_cast<double>(val);
        
        // Dynamic growth logic from gem5
        if (value < min_bucket_) {
            if (min_bucket_ == 0) {
                growDown();
            }
            while (value < min_bucket_) {
                growOut();
            }
        } else if (value >= max_bucket_ + bucket_size_) {
            if (min_bucket_ == 0) {
                while (value >= max_bucket_ + bucket_size_) {
                    growUp();
                }
            } else {
                while (value >= max_bucket_ + bucket_size_) {
                    growOut();
                }
            }
        }

        // Calculate bucket index (gem5 logic)
        size_t index = static_cast<size_t>(std::floor((value - min_bucket_) / bucket_size_));
        
        if (index < buckets_.size()) {
            buckets_[index] += number;
        }

        // Update statistics
        sum_ += value * number;
        squares_ += value * value * number;
        logs_ += std::log(value) * number;
        samples_ += number;
    }

    // Accessor methods
    uint64_t total_samples() const { return samples_; }
    double mean() const { 
        return samples_ > 0 ? sum_ / samples_ : 0.0; 
    }
    double variance() const {
        if (samples_ <= 1) return 0.0;
        double m = mean();
        return (squares_ / samples_) - (m * m);
    }
    double stddev() const {
        return std::sqrt(variance());
    }
    double geometric_mean() const {
        return samples_ > 0 ? std::exp(logs_ / samples_) : 0.0;
    }

    // Get bucket value
    uint64_t bucket(size_t idx) const {
        return idx < buckets_.size() ? buckets_[idx] : 0;
    }

    size_t num_buckets() const { return num_buckets_; }
    double bucket_size() const { return bucket_size_; }
    double min_bucket() const { return min_bucket_; }
    double max_bucket() const { return max_bucket_; }

    // Get the range of a specific bucket
    std::pair<double, double> bucket_range(size_t idx) const {
        if (idx >= buckets_.size()) {
            return {0.0, 0.0};
        }
        double bucket_min = min_bucket_ + idx * bucket_size_;
        double bucket_max = bucket_min + bucket_size_ - 1;
        return {bucket_min, bucket_max};
    }

    // StatBase interface implementation
    void print(std::ofstream &file) override {
        std::string base_name = "ramulator." + stat_name;

        // Print basic statistics
        file << std::setw(40) << std::left << (base_name + "::samples");
        file << std::setw(20) << std::right << samples_;
        if (!stat_desc.empty()) {
            file << " # " << stat_desc;
        }
        file << std::endl;

        if (samples_ > 0) {
            file << std::setw(40) << std::left << (base_name + "::mean");
            file << std::setw(20) << std::right << std::fixed;
            file.precision(6);
            file << mean() << std::endl;

            file << std::setw(40) << std::left << (base_name + "::stdev");
            file << std::setw(20) << std::right << std::fixed;
            file.precision(6);
            file << stddev() << std::endl;

            // Print bucket distribution
            for (size_t i = 0; i < buckets_.size(); ++i) {
                if (buckets_[i] > 0 || !nozero_flag) {
                    auto range = bucket_range(i);

                    std::string bucket_name = base_name + "::" + std::to_string(static_cast<int>(range.first)) + "-" +
                                              std::to_string(static_cast<int>(range.second));

                    file << std::setw(40) << std::left << bucket_name;
                    file << std::setw(20) << std::right << buckets_[i];
                    if (!stat_desc.empty()) {
                        file << " # " << stat_desc;
                    }
                    file << std::endl;
                }
            }
        }
    }

    void reset() override {
        min_bucket_ = 0;
        max_bucket_ = num_buckets_ - 1;
        bucket_size_ = 1;
        sum_ = logs_ = squares_ = 0.0;
        samples_ = 0;
        std::fill(buckets_.begin(), buckets_.end(), 0);
    }

    bool zero() const override {
        return samples_ == 0;
    }

private:
    // Growth functions adapted from gem5's HistStor

    void growUp() {
        size_t size = buckets_.size();
        size_t half = (size + 1) / 2; // round up!

        size_t pair = 0;
        for (size_t i = 0; i < half; i++) {
            buckets_[i] = buckets_[pair];
            if (pair + 1 < size) {
                buckets_[i] += buckets_[pair + 1];
            }
            pair += 2;
        }

        for (size_t i = half; i < size; i++) {
            buckets_[i] = 0;
        }

        max_bucket_ *= 2;
        bucket_size_ *= 2;
    }

    void growOut() {
        size_t size = buckets_.size();
        size_t zero = size / 2; // round down!
        size_t top_half = zero + (size - zero + 1) / 2; // round up!
        size_t bottom_half = (size - zero) / 2; // round down!

        // grow down
        int low_pair = zero - 1;
        for (int i = zero - 1; i >= static_cast<int>(bottom_half); i--) {
            buckets_[i] = buckets_[low_pair];
            if (low_pair - 1 >= 0) {
                buckets_[i] += buckets_[low_pair - 1];
            }
            low_pair -= 2;
        }

        for (int i = bottom_half - 1; i >= 0; i--) {
            buckets_[i] = 0;
        }

        // grow up
        size_t high_pair = zero;
        for (size_t i = zero; i < top_half; i++) {
            buckets_[i] = buckets_[high_pair];
            if (high_pair + 1 < size) {
                buckets_[i] += buckets_[high_pair + 1];
            }
            high_pair += 2;
        }

        for (size_t i = top_half; i < size; i++) {
            buckets_[i] = 0;
        }

        max_bucket_ *= 2;
        min_bucket_ *= 2;
        bucket_size_ *= 2;
    }

    void growDown() {
        const size_t size = buckets_.size();
        const size_t zero = size / 2; // round down!
        const bool even = ((size - 1) % 2) == 0;

        // Make sure that zero becomes the lower bound of the middle bucket
        int pair = size - 1;
        if (even) {
            pair--;
        }
        for (int i = pair; i >= static_cast<int>(zero); --i) {
            buckets_[i] = buckets_[pair];
            if (pair - 1 >= 0) {
                buckets_[i] += buckets_[pair - 1];
            }
            pair -= 2;
        }

        for (int i = zero - 1; i >= 0; i--) {
            buckets_[i] = 0;
        }

        // Double the range by using the negative of the lower bound of the last
        // bucket as the new lower bound of the first bucket
        min_bucket_ = -max_bucket_;

        // A special case must be handled when there is an odd number of
        // buckets so that zero is kept as the lower bound of the middle bucket
        if (!even) {
            min_bucket_ -= bucket_size_;
            max_bucket_ -= bucket_size_;
        }

        // Only update the bucket size once the range has been updated
        bucket_size_ *= 2;
    }
};

// StandardDeviationStat: Standard deviation computation
class StandardDeviationStat : public StatBase {
private:
    uint64_t num_samples_;
    double sum_, sum_squares_;

public:
    StandardDeviationStat() : num_samples_(0), sum_(0), sum_squares_(0) {}

    template<typename T>
    void sample(const T& value, int count = 1) {
        double val = static_cast<double>(value);
        num_samples_ += count;
        sum_ += val * count;
        sum_squares_ += val * val * count;
    }

    double mean() const {
        return num_samples_ > 0 ? sum_ / num_samples_ : 0.0;
    }

    double stddev() const {
        if (num_samples_ <= 1) return 0.0;
        double m = mean();
        return std::sqrt((sum_squares_ / num_samples_) - (m * m));
    }

    void print(std::ofstream& file) override {
        if (num_samples_ > 0) {
            std::string stat_full_name = "ramulator." + stat_name + ".stddev";
            file << std::setw(40) << std::right << stat_full_name;
            file << std::setw(20) << std::right << std::fixed;
            file.precision(stat_precision);
            file << stddev();
            if (!stat_desc.empty()) {
                file << std::setw(40) << std::right << " # " << stat_desc;
            }
            file << std::endl;
        }
    }

    void reset() override {
        num_samples_ = 0;
        sum_ = sum_squares_ = 0.0;
    }

    bool zero() const override {
        return num_samples_ == 0;
    }
};

// AverageDeviationStat: Average deviation computation
class AverageDeviationStat : public StatBase {
private:
    uint64_t num_samples_;
    double sum_, sum_squares_;

public:
    AverageDeviationStat() : num_samples_(0), sum_(0), sum_squares_(0) {}

    template<typename T>
    void sample(const T& value, int count = 1) {
        double val = static_cast<double>(value);
        num_samples_ += count;
        sum_ += val * count;
        sum_squares_ += val * val * count;
    }

    double mean() const {
        return num_samples_ > 0 ? sum_ / num_samples_ : 0.0;
    }

    void print(std::ofstream& file) override {
        if (num_samples_ > 0) {
            std::string stat_full_name = "ramulator." + stat_name + ".mean";
            file << std::setw(40) << std::right << stat_full_name;
            file << std::setw(20) << std::right << std::fixed;
            file.precision(stat_precision);
            file << mean();
            if (!stat_desc.empty()) {
                file << std::setw(40) << std::right << " # " << stat_desc;
            }
            file << std::endl;
        }
    }

    void reset() override {
        num_samples_ = 0;
        sum_ = sum_squares_ = 0.0;
    }

    bool zero() const override {
        return num_samples_ == 0;
    }
};

} /* namespace ramulator */

#endif
