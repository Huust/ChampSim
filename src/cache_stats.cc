#include "cache_stats.h"

cache_stats operator-(cache_stats lhs, cache_stats rhs)
{
  cache_stats result;
  result.pf_requested = lhs.pf_requested - rhs.pf_requested;
  result.pf_issued = lhs.pf_issued - rhs.pf_issued;
  result.pf_useful = lhs.pf_useful - rhs.pf_useful;
  result.pf_useless = lhs.pf_useless - rhs.pf_useless;
  result.pf_fill = lhs.pf_fill - rhs.pf_fill;

  result.hits = lhs.hits - rhs.hits;
  result.misses = lhs.misses - rhs.misses;
  result.mshr_merge = lhs.mshr_merge - rhs.mshr_merge;
  result.mshr_return = lhs.mshr_return - rhs.mshr_return;

  result.total_miss_latency_cycles = lhs.total_miss_latency_cycles - rhs.total_miss_latency_cycles;
  result.mshr_congestion_cycles = lhs.mshr_congestion_cycles - rhs.mshr_congestion_cycles;

  // CXL/DRAM split statistics
  result.total_miss_latency_cycles_dram = lhs.total_miss_latency_cycles_dram - rhs.total_miss_latency_cycles_dram;
  result.total_miss_latency_cycles_cxl = lhs.total_miss_latency_cycles_cxl - rhs.total_miss_latency_cycles_cxl;
  result.misses_to_dram = lhs.misses_to_dram - rhs.misses_to_dram;
  result.misses_to_cxl = lhs.misses_to_cxl - rhs.misses_to_cxl;
  result.mshr_merge_to_dram = lhs.mshr_merge_to_dram - rhs.mshr_merge_to_dram;
  result.mshr_merge_to_cxl = lhs.mshr_merge_to_cxl - rhs.mshr_merge_to_cxl;

  return result;
}
