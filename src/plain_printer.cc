/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cmath>
#include <numeric>
#include <ratio>
#include <string_view> // for string_view
#include <utility>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/ostream.h>

#include "stats_printer.h"

namespace
{
template <typename N, typename D>
auto print_ratio(N num, D denom)
{
  if (denom > 0) {
    return fmt::format("{:.4g}", std::ceil(num) / std::ceil(denom));
  }
  return std::string{"-"};
}
} // namespace

std::vector<std::string> champsim::plain_printer::format(O3_CPU::stats_type stats)
{
  constexpr std::array types{branch_type::BRANCH_DIRECT_JUMP, branch_type::BRANCH_INDIRECT,      branch_type::BRANCH_CONDITIONAL,
                             branch_type::BRANCH_DIRECT_CALL, branch_type::BRANCH_INDIRECT_CALL, branch_type::BRANCH_RETURN};
  auto total_branch = std::ceil(
      std::accumulate(std::begin(types), std::end(types), 0LL, [tbt = stats.total_branch_types](auto acc, auto next) { return acc + tbt.value_or(next, 0); }));
  auto total_mispredictions = std::ceil(
      std::accumulate(std::begin(types), std::end(types), 0LL, [btm = stats.branch_type_misses](auto acc, auto next) { return acc + btm.value_or(next, 0); }));

  std::vector<std::string> lines{};
  lines.push_back(fmt::format("{} cumulative IPC: {} instructions: {} cycles: {}", stats.name, ::print_ratio(stats.instrs(), stats.cycles()), stats.instrs(),
                              stats.cycles()));

  lines.push_back(fmt::format("{} Branch Prediction Accuracy: {}% MPKI: {} Average ROB Occupancy at Mispredict: {}", stats.name,
                              ::print_ratio(100 * (total_branch - total_mispredictions), total_branch),
                              ::print_ratio(std::kilo::num * total_mispredictions, stats.instrs()),
                              ::print_ratio(stats.total_rob_occupancy_at_branch_mispredict, total_mispredictions)));

  lines.emplace_back("Branch type MPKI");
  for (auto idx : types) {
    lines.push_back(fmt::format("{}: {}", branch_type_names.at(champsim::to_underlying(idx)),
                                ::print_ratio(std::kilo::num * stats.branch_type_misses.value_or(idx, 0), stats.instrs())));
  }

  return lines;
}

std::vector<std::string> champsim::plain_printer::format(CACHE::stats_type stats)
{
  using hits_value_type = typename decltype(stats.hits)::value_type;
  using misses_value_type = typename decltype(stats.misses)::value_type;
  using mshr_merge_value_type = typename decltype(stats.mshr_merge)::value_type;
  using mshr_return_value_type = typename decltype(stats.mshr_return)::value_type;

  std::vector<std::size_t> cpus;

  // build a vector of all existing cpus
  auto stat_keys = {stats.hits.get_keys(), stats.misses.get_keys(), stats.mshr_merge.get_keys(), stats.mshr_return.get_keys()};
  for (auto keys : stat_keys) {
    std::transform(std::begin(keys), std::end(keys), std::back_inserter(cpus), [](auto val) { return val.second; });
  }
  std::sort(std::begin(cpus), std::end(cpus));
  auto uniq_end = std::unique(std::begin(cpus), std::end(cpus));
  cpus.erase(uniq_end, std::end(cpus));

  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
    for (auto cpu : cpus) {
      stats.hits.allocate(std::pair{type, cpu});
      stats.misses.allocate(std::pair{type, cpu});
      stats.mshr_merge.allocate(std::pair{type, cpu});
      stats.mshr_return.allocate(std::pair{type, cpu});
    }
  }

  std::vector<std::string> lines{};
  for (auto cpu : cpus) {
    hits_value_type total_hits = 0;
    misses_value_type total_misses = 0;
    mshr_merge_value_type total_mshr_merge = 0;
    mshr_return_value_type total_mshr_return = 0;
    for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
      total_hits += stats.hits.value_or(std::pair{type, cpu}, hits_value_type{});
      total_misses += stats.misses.value_or(std::pair{type, cpu}, misses_value_type{});
      total_mshr_merge += stats.mshr_merge.value_or(std::pair{type, cpu}, mshr_merge_value_type{});
      total_mshr_return += stats.mshr_return.value_or(std::pair{type, cpu}, mshr_merge_value_type{});
    }

    fmt::format_string<std::string_view, std::string_view, int, int, int> hitmiss_fmtstr{
        "cpu{}->{} {:<12s} ACCESS: {:10d} HIT: {:10d} MISS: {:10d} MSHR_MERGE: {:10d}"};
    lines.push_back(fmt::format(hitmiss_fmtstr, cpu, stats.name, "TOTAL", total_hits + total_misses, total_hits, total_misses, total_mshr_merge));
    for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
      lines.push_back(
          fmt::format(hitmiss_fmtstr, cpu, stats.name, access_type_names.at(champsim::to_underlying(type)),
                      stats.hits.value_or(std::pair{type, cpu}, hits_value_type{}) + stats.misses.value_or(std::pair{type, cpu}, misses_value_type{}),
                      stats.hits.value_or(std::pair{type, cpu}, hits_value_type{}), stats.misses.value_or(std::pair{type, cpu}, misses_value_type{}),
                      stats.mshr_merge.value_or(std::pair{type, cpu}, mshr_merge_value_type{})));
    }

    lines.push_back(fmt::format("cpu{}->{} PREFETCH REQUESTED: {:10} ISSUED: {:10} USEFUL: {:10} USELESS: {:10}", cpu, stats.name, stats.pf_requested,
                                stats.pf_issued, stats.pf_useful, stats.pf_useless));

    uint64_t total_downstream_demands = total_mshr_return - stats.mshr_return.value_or(std::pair{access_type::PREFETCH, cpu}, mshr_return_value_type{});
    lines.push_back(
        fmt::format("cpu{}->{} AVERAGE MISS LATENCY: {} cycles", cpu, stats.name, ::print_ratio(stats.total_miss_latency_cycles, total_downstream_demands)));

    // Print CXL/DRAM split statistics
    if (stats.misses_to_dram > 0 || stats.misses_to_cxl > 0) {
      uint64_t actual_requests_to_dram = stats.misses_to_dram - stats.mshr_merge_to_dram;
      uint64_t actual_requests_to_cxl = stats.misses_to_cxl - stats.mshr_merge_to_cxl;

      lines.push_back(
          fmt::format("cpu{}->{} DRAM: {} misses, {} MSHR merges, {} actual requests sent", cpu, stats.name,
                      stats.misses_to_dram, stats.mshr_merge_to_dram, actual_requests_to_dram));
      lines.push_back(
          fmt::format("cpu{}->{} CXL: {} misses, {} MSHR merges, {} actual requests sent", cpu, stats.name,
                      stats.misses_to_cxl, stats.mshr_merge_to_cxl, actual_requests_to_cxl));
      lines.push_back(
          fmt::format("cpu{}->{} AVERAGE MISS LATENCY TO DRAM: {} cycles", cpu, stats.name,
                      ::print_ratio(stats.total_miss_latency_cycles_dram, stats.misses_to_dram)));
      lines.push_back(
          fmt::format("cpu{}->{} AVERAGE MISS LATENCY TO CXL: {} cycles", cpu, stats.name,
                      ::print_ratio(stats.total_miss_latency_cycles_cxl, stats.misses_to_cxl)));
    }

    lines.push_back(
        fmt::format("cpu{}->{} MSHR CONGESTION CYCLES: {:10} ({}% of total cycles)", cpu, stats.name, stats.mshr_congestion_cycles,
                    stats.mshr_congestion_cycles * 100.0 / std::max(uint64_t{1}, stats.mshr_congestion_cycles + total_hits + total_misses)));
  }

  return lines;
}

std::vector<std::string> champsim::plain_printer::format(DRAM_CHANNEL::stats_type stats)
{
  std::vector<std::string> lines{};
  lines.push_back(fmt::format("{} RQ ROW_BUFFER_HIT: {:10}", stats.name, stats.RQ_ROW_BUFFER_HIT));
  lines.push_back(fmt::format("  ROW_BUFFER_MISS: {:10}", stats.RQ_ROW_BUFFER_MISS));
  lines.push_back(fmt::format("  AVG DBUS CONGESTED CYCLE: {}", ::print_ratio(stats.dbus_cycle_congested, stats.dbus_count_congested)));
  lines.push_back(fmt::format("{} WQ ROW_BUFFER_HIT: {:10}", stats.name, stats.WQ_ROW_BUFFER_HIT));
  lines.push_back(fmt::format("  ROW_BUFFER_MISS: {:10}", stats.WQ_ROW_BUFFER_MISS));
  lines.push_back(fmt::format("  FULL: {:10}", stats.WQ_FULL));

  if (stats.refresh_cycles > 0)
    lines.push_back(fmt::format("{} REFRESHES ISSUED: {:10}", stats.name, stats.refresh_cycles));
  else
    lines.push_back(fmt::format("{} REFRESHES ISSUED: -", stats.name));

  return lines;
}

std::vector<std::string> champsim::plain_printer::format(SHIM_LAYER::stats_type stats)
{
  std::vector<std::string> lines{};

  // DRAM request statistics
  lines.push_back(fmt::format("{} DRAM REQUESTS", stats.name));
  lines.push_back(fmt::format("  READ: {:10}", stats.dram_requests_read));
  lines.push_back(fmt::format("  WRITE: {:10}", stats.dram_requests_write));
  lines.push_back(fmt::format("  PREFETCH: {:10}", stats.dram_requests_prefetch));
  lines.push_back(fmt::format("  TRANSLATION: {:10}", stats.dram_requests_translation));
  lines.push_back(fmt::format("  TOTAL: {:10}", stats.dram_requests_total));

  // CXL request statistics
  lines.push_back(fmt::format("{} CXL REQUESTS", stats.name));
  lines.push_back(fmt::format("  READ: {:10}", stats.cxl_requests_read));
  lines.push_back(fmt::format("  WRITE: {:10}", stats.cxl_requests_write));
  lines.push_back(fmt::format("  PREFETCH: {:10}", stats.cxl_requests_prefetch));
  lines.push_back(fmt::format("  TRANSLATION: {:10}", stats.cxl_requests_translation));
  lines.push_back(fmt::format("  TOTAL: {:10}", stats.cxl_requests_total));

  // Buffer congestion statistics
  lines.push_back(fmt::format("{} BUFFER CONGESTION", stats.name));
  lines.push_back(fmt::format("  RQ FULL: {:10}", stats.rq_full));
  lines.push_back(fmt::format("  WQ FULL: {:10}", stats.wq_full));
  lines.push_back(fmt::format("  PQ FULL: {:10}", stats.pq_full));

  // Bandwidth congestion statistics
  lines.push_back(fmt::format("{} BANDWIDTH CONGESTION", stats.name));
  lines.push_back(fmt::format("  UPPER BW CONGESTION CYCLES: {:10}", stats.upper_bw_congestion_cycles));
  lines.push_back(fmt::format("  LOWER BW CONGESTION CYCLES: {:10}", stats.lower_bw_congestion_cycles));

  return lines;
}

std::vector<std::string> champsim::plain_printer::format(CXL_CHANNEL::stats_type stats)
{
  std::vector<std::string> lines{};

  lines.push_back(fmt::format("{} CXL CHANNEL STATISTICS", stats.name));
  lines.push_back(fmt::format("  READ BUS BUSY CYCLES: {:10}", stats.bus_cycles_rd_busy));
  lines.push_back(fmt::format("  WRITE BUS BUSY CYCLES: {:10}", stats.bus_cycles_wr_busy));
  lines.push_back(fmt::format("  TOTAL OPERATING CYCLES: {:10}", stats.total_operating_cycles));

  // Calculate utilization percentages
  if (stats.total_operating_cycles > 0) {
    lines.push_back(fmt::format("  READ BUS UTILIZATION: {}%",
                                ::print_ratio(100 * stats.bus_cycles_rd_busy, stats.total_operating_cycles)));
    lines.push_back(fmt::format("  WRITE BUS UTILIZATION: {}%",
                                ::print_ratio(100 * stats.bus_cycles_wr_busy, stats.total_operating_cycles)));
  } else {
    lines.push_back(fmt::format("  READ BUS UTILIZATION: -"));
    lines.push_back(fmt::format("  WRITE BUS UTILIZATION: -"));
  }

  return lines;
}

void champsim::plain_printer::print(champsim::phase_stats& stats)
{
  auto lines = format(stats);
  std::copy(std::begin(lines), std::end(lines), std::ostream_iterator<std::string>(stream, "\n"));
}

std::vector<std::string> champsim::plain_printer::format(champsim::phase_stats& stats)
{
  std::vector<std::string> lines{};
  lines.push_back(fmt::format("=== {} ===", stats.name));

  int i = 0;
  for (auto tn : stats.trace_names) {
    lines.push_back(fmt::format("CPU {} runs {}", i++, tn));
  }

  if (NUM_CPUS > 1) {
    lines.emplace_back("");
    lines.emplace_back("Total Simulation Statistics (not including warmup)");

    for (const auto& stat : stats.sim_cpu_stats) {
      auto sublines = format(stat);
      lines.emplace_back("");
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
      lines.emplace_back("");
    }

    for (const auto& stat : stats.sim_cache_stats) {
      auto sublines = format(stat);
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    }
  }

  lines.emplace_back("");
  lines.emplace_back("Region of Interest Statistics");

  for (const auto& stat : stats.roi_cpu_stats) {
    auto sublines = format(stat);
    lines.emplace_back("");
    std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    lines.emplace_back("");
  }

  for (const auto& stat : stats.roi_cache_stats) {
    auto sublines = format(stat);
    std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
  }

  // SHIM_LAYER Statistics
  if (!stats.roi_shim_layer_stats.empty()) {
    lines.emplace_back("");
    lines.emplace_back("SHIM_LAYER Statistics");
    for (const auto& stat : stats.roi_shim_layer_stats) {
      auto sublines = format(stat);
      lines.emplace_back("");
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    }
  }

  // DRAM Statistics - only display if DRAM stats exist
  if (!stats.roi_dram_stats.empty()) {
    lines.emplace_back("");
    lines.emplace_back("DRAM Statistics");
    for (const auto& stat : stats.roi_dram_stats) {
      auto sublines = format(stat);
      lines.emplace_back("");
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    }
  }

  // CXL Statistics - only display if CXL stats exist
  if (!stats.roi_cxl_stats.empty()) {
    lines.emplace_back("");
    lines.emplace_back("CXL Statistics");
    for (const auto& stat : stats.roi_cxl_stats) {
      auto sublines = format(stat);
      lines.emplace_back("");
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    }
  }

  // CXL DRAM Statistics - only display if CXL DRAM stats exist
  if (!stats.roi_cxl_dram_stats.empty()) {
    lines.emplace_back("");
    lines.emplace_back("CXL DRAM Statistics");
    for (const auto& stat : stats.roi_cxl_dram_stats) {
      auto sublines = format(stat);
      lines.emplace_back("");
      std::move(std::begin(sublines), std::end(sublines), std::back_inserter(lines));
    }
  }

  return lines;
}

void champsim::plain_printer::print(std::vector<phase_stats>& stats)
{
  for (auto p : stats) {
    print(p);
  }
}
