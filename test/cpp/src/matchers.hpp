#ifndef TEST_MATCHERS_H
#define TEST_MATCHERS_H

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_templated.hpp>
#include <limits>
#include <numeric>

#include "address.h"

namespace champsim::test {
template <typename Addr>
struct StrideMatcher : Catch::Matchers::MatcherGenericBase {
  typename Addr::difference_type stride;

  explicit StrideMatcher(typename Addr::difference_type s) : stride(s) {}

    template<typename Range>
    bool match(const Range& range) const {
      std::vector<decltype(stride)> diffs;
      return std::adjacent_find(std::cbegin(range), std::cend(range), [stride=stride](const auto& x, const auto& y){ return champsim::offset(Addr{x}, Addr{y}) != stride; }) == std::cend(range);
    }

    std::string describe() const override {
        return "has stride " + std::to_string(stride);
    }
};


struct RelativeReturnedMatcher : Catch::Matchers::MatcherGenericBase {
  long issue_time;
  long epsilon;

  RelativeReturnedMatcher(long issue_time_, long epsilon_) : issue_time(issue_time_), epsilon(epsilon_) {}

  template <typename T>
  RelativeReturnedMatcher(const T& other, long cycles, long epsilon_) : issue_time(other.issue_time + cycles), epsilon(epsilon_) {}

  template <typename T>
  bool match(const T& to_match) const {
    bool not_early = (to_match.return_time >= issue_time - epsilon);
    bool not_late = (to_match.return_time <= issue_time + epsilon);
    return not_early && not_late;
  }

  std::string describe() const override {
    if (epsilon > 0) {
      return "Returned between cycle " + std::to_string(issue_time-epsilon) + " and " + std::to_string(issue_time+epsilon);
    }
    return "Returned at cycle " + std::to_string(issue_time);
  }
};

struct ReturnedMatcher : Catch::Matchers::MatcherGenericBase {
  long cycles;
  long epsilon;

  ReturnedMatcher(long cycles_, long epsilon_) : cycles(cycles_), epsilon(epsilon_) {}

  template <typename T>
  bool match(const T& to_match) const {
    bool not_early = (to_match.return_time >= to_match.issue_time + cycles - epsilon);
    bool not_late = (to_match.return_time <= to_match.issue_time + cycles + epsilon);
    return not_early && not_late;
  }

  std::string describe() const override {
    if (epsilon > 0) {
      return "Returned after between cycles " + std::to_string(cycles-epsilon) + " and " + std::to_string(cycles+epsilon);
    }
    return "Returned after cycles " + std::to_string(cycles);
  }
};

template <typename Range>
struct DisjunctMatcher : Catch::Matchers::MatcherGenericBase
{
  DisjunctMatcher(Range const& range_) : range{ range_ } {}

  template<typename OtherRange>
    bool match(OtherRange const& other) const {
      return std::none_of(std::begin(other), std::end(other), [&](const auto other_elem){
          return std::find(std::begin(range), std::end(range), other_elem) != std::end(range);
      });
    }

  std::string describe() const override {
    return "Contains none of: " + Catch::rangeToString(range);
  }

  private:
  Range const& range;
};

struct MonotonicallyIncreasingMatcher : Catch::Matchers::MatcherGenericBase {
    template<typename Range>
    bool match(Range const& range) const {
        return std::adjacent_find(std::begin(range), std::end(range), std::greater_equal<typename Range::value_type>{}) == std::end(range);
    }

    std::string describe() const override {
        return "Increases monotonically";
    }
};

/**
 * Matcher that checks if response latency falls within a left-closed, right-open interval [min_sum, max_sum).
 * Both bounds are calculated as the sum of their respective component values.
 * 
 * Usage:
 *   // Check latency in range [t_cxl+dram_latency, max_reasonable)
 *   REQUIRE_THAT(response, LatencyRangeMatcher({t_cxl, dram_latency}, {max_reasonable}));
 * 
 *   // Check latency >= minimum (set upper bound to max)
 *   REQUIRE_THAT(response, LatencyRangeMatcher({t_cxl, dram_latency}, {std::numeric_limits<long>::max()}));
 * 
 *   // Check latency < maximum (set lower bound to 0)
 *   REQUIRE_THAT(response, LatencyRangeMatcher({0}, {max_latency}));
 * 
 *   // Exact range with multiple components
 *   REQUIRE_THAT(response, LatencyRangeMatcher({t_cxl, dram_latency, overhead}, {t_cxl, dram_latency, overhead, tolerance}));
 */
struct LatencyRangeMatcher : Catch::Matchers::MatcherGenericBase {
  std::vector<long> min_components;  // Components for lower bound (inclusive)
  std::vector<long> max_components;  // Components for upper bound (exclusive)
  
  LatencyRangeMatcher(std::initializer_list<long> min_list, std::initializer_list<long> max_list) 
    : min_components(min_list), max_components(max_list) {}
  
  template <typename T>
  bool match(const T& to_match) const {
    long latency = to_match.return_time - to_match.issue_time;
    
    long min_sum = std::accumulate(min_components.begin(), min_components.end(), 0L);
    long max_sum = std::accumulate(max_components.begin(), max_components.end(), 0L);
    
    return latency >= min_sum && latency < max_sum;
  }
  
  std::string describe() const override {
    long min_sum = std::accumulate(min_components.begin(), min_components.end(), 0L);
    long max_sum = std::accumulate(max_components.begin(), max_components.end(), 0L);
    
    if (max_sum == std::numeric_limits<long>::max()) {
      return "Has latency >= " + std::to_string(min_sum) + " cycles";
    }
    return "Has latency in range [" + std::to_string(min_sum) + ", " + std::to_string(max_sum) + ") cycles";
  }
};
}

#endif
