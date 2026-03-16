#ifndef CACHE_STATS_H
#define CACHE_STATS_H

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "channel.h"
#include "event_counter.h"

struct cache_stats {
  std::string name;
  // prefetch stats
  uint64_t pf_requested = 0;
  uint64_t pf_issued = 0;
  uint64_t pf_useful = 0;
  uint64_t pf_useless = 0;
  uint64_t pf_fill = 0;

  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> hits = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> misses = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> mshr_merge = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> mshr_return = {};

  long total_miss_latency_cycles{};
  uint64_t mshr_congestion_cycles = 0;  // Cycles when MSHR was full and requests were blocked

  // Perforated page statistics
  uint64_t perf_total = 0;
  uint64_t perf_non_hole = 0;
  uint64_t perf_hole = 0;
  uint64_t perf_coarse_filtered = 0;

  // CXL/DRAM split statistics (only meaningful when heatmap allocation is enabled)
  long total_miss_latency_cycles_dram{};  // Latency for requests served by DRAM
  long total_miss_latency_cycles_cxl{};   // Latency for requests served by CXL
  uint64_t misses_to_dram = 0;            // Number of misses that went to DRAM
  uint64_t misses_to_cxl = 0;             // Number of misses that went to CXL
  uint64_t mshr_merge_to_dram = 0;        // Number of MSHR merges for DRAM requests
  uint64_t mshr_merge_to_cxl = 0;         // Number of MSHR merges for CXL requests
};

cache_stats operator-(cache_stats lhs, cache_stats rhs);

#endif
