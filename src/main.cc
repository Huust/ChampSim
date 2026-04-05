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

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>
#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include "cache.h" // for CACHE
#include "champsim.h"
#ifndef CHAMPSIM_TEST_BUILD
#include "core_inst.inc"
#endif
#include "defaults.hpp"
#include "environment.h"
#include "ooo_cpu.h" // for O3_CPU
#include "phase_info.h"
#include "stats_printer.h"
#include "tracereader.h"
#include "vmem.h"
#include "heatmap.h"
#include "ramulator_controller.h"

namespace champsim
{
std::vector<phase_stats> main(environment& env, std::vector<phase_info>& phases, std::vector<tracereader>& traces);
}

#ifndef CHAMPSIM_TEST_BUILD
using configured_environment = champsim::configured::generated_environment<CHAMPSIM_BUILD>;

const std::size_t NUM_CPUS = configured_environment::num_cpus;

const unsigned BLOCK_SIZE = configured_environment::block_size;
const unsigned PAGE_SIZE = configured_environment::page_size;
#endif
const unsigned LOG2_BLOCK_SIZE = champsim::lg2(BLOCK_SIZE);
const unsigned LOG2_PAGE_SIZE = champsim::lg2(PAGE_SIZE);

#ifndef CHAMPSIM_TEST_BUILD
int main(int argc, char** argv) // NOLINT(bugprone-exception-escape)
{
  configured_environment gen_environment{};

  CLI::App app{"A microarchitecture simulator for research and education"};

  bool knob_cloudsuite{false};
  long long warmup_instructions = 0;
  long long simulation_instructions = std::numeric_limits<long long>::max();
  std::string json_file_name;
  std::vector<std::string> trace_names;

  std::string generate_heatmap_path;
  std::string use_heatmap_path;
  bool sort_by_criticality = false;
  std::string ratio_str = "1:3";
  std::string save_bandwidth_path;
  std::string track_interleaving_path;
  std::string use_allocation_path;
  std::string generate_pmap_path;
  std::string use_pmap_path;
  std::string use_policy_path;
  std::string page_mode_str;

  auto set_heartbeat_callback = [&](auto) {
    for (O3_CPU& cpu : gen_environment.cpu_view()) {
      cpu.show_heartbeat = false;
    }
  };

  app.add_flag("-c,--cloudsuite", knob_cloudsuite, "Read all traces using the cloudsuite format");
  app.add_flag("--hide-heartbeat", set_heartbeat_callback, "Hide the heartbeat output");
  auto* warmup_instr_option = app.add_option("-w,--warmup-instructions", warmup_instructions, "The number of instructions in the warmup phase");
  auto* deprec_warmup_instr_option =
      app.add_option("--warmup_instructions", warmup_instructions, "[deprecated] use --warmup-instructions instead")->excludes(warmup_instr_option);
  auto* sim_instr_option = app.add_option("-i,--simulation-instructions", simulation_instructions,
                                          "The number of instructions in the detailed phase. If not specified, run to the end of the trace.");
  auto* deprec_sim_instr_option =
      app.add_option("--simulation_instructions", simulation_instructions, "[deprecated] use --simulation-instructions instead")->excludes(sim_instr_option);

  auto* json_option =
      app.add_option("--json", json_file_name, "The name of the file to receive JSON output. If no name is specified, stdout will be used")->expected(0, 1);

  app.add_option("--generate-heatmap", generate_heatmap_path, "Generate heatmap data and save to specified file path");
  app.add_option("--use-heatmap", use_heatmap_path, "Use existing heatmap file for memory allocation during simulation");
  app.add_flag("--sort-by-criticality", sort_by_criticality, "Sort virtual pages by criticality when allocating (for simulation mode)");
  app.add_option("--ratio", ratio_str, "Ratio for memory allocation (format: N:M, for simulation mode)")->default_val("1:3");
  app.add_option("--save-bandwidth", save_bandwidth_path, "Save bandwidth statistics to specified file path");
  app.add_option("--generate-interleaving", track_interleaving_path, "Track page allocations in interleaving mode and save to specified file");
  app.add_option("--use-interleaving", use_allocation_path, "Use precomputed allocation mapping (CSV format: vpage,device)");
  app.add_option("--generate-pmap", generate_pmap_path, "Save VPN→PPN physical mapping at end of simulation");
  app.add_option("--use-pmap", use_pmap_path, "Load physical mapping for address replay");
  app.add_option("--use-policy", use_policy_path, "Load policy file (page types + per-subpage tier decisions)");
  app.add_option("--page-mode", page_mode_str, "Page size mode: 4kb, 2mb, or mixed (required)")->required()
    ->check(CLI::IsMember({"4kb", "2mb", "mixed"}));

  app.add_option("traces", trace_names, "The paths to the traces")->required()->expected(NUM_CPUS)->check(CLI::ExistingFile);

  CLI11_PARSE(app, argc, argv);

  const bool warmup_given = (warmup_instr_option->count() > 0) || (deprec_warmup_instr_option->count() > 0);
  const bool simulation_given = (sim_instr_option->count() > 0) || (deprec_sim_instr_option->count() > 0);

  if (deprec_warmup_instr_option->count() > 0) {
    fmt::print("WARNING: option --warmup_instructions is deprecated. Use --warmup-instructions instead.\n");
  }

  if (deprec_sim_instr_option->count() > 0) {
    fmt::print("WARNING: option --simulation_instructions is deprecated. Use --simulation-instructions instead.\n");
  }

  if (simulation_given && !warmup_given) {
    // Warmup is 20% by default
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    warmup_instructions = simulation_instructions / 5;
  }

  std::vector<champsim::tracereader> traces;
  std::transform(
      std::begin(trace_names), std::end(trace_names), std::back_inserter(traces),
      [knob_cloudsuite, repeat = simulation_given, i = uint8_t(0)](auto name) mutable { return get_tracereader(name, i++, knob_cloudsuite, repeat); });

  std::vector<champsim::phase_info> phases;

  if (!generate_heatmap_path.empty()) {
    // Generate heatmap mode: warmup + heatmap generation phase
    champsim::heatmap::enable_heatmap_generation();

    phases = {
      champsim::phase_info{"Warmup", true, warmup_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names},
      champsim::phase_info{"HeatmapGeneration", false, simulation_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names}
    };
  } else {
    // Normal simulation mode
    phases = {
      champsim::phase_info{"Warmup", true, warmup_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names},
      champsim::phase_info{"Simulation", false, simulation_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names}
    };

    // Load heatmap data if use-heatmap is provided (allocation deferred until after page mode is set)
    if (!use_heatmap_path.empty()) {
      // Check if both DRAM and CXL devices are available for heatmap usage
      if (!gen_environment.has_dram() || !gen_environment.has_cxl()) {
        fmt::print("ERROR: Heatmap functionality requires both DRAM and CXL devices to be enabled in configuration.\n");
        fmt::print("       Current configuration: DRAM={}, CXL={}\n",
                   gen_environment.has_dram() ? "enabled" : "disabled",
                   gen_environment.has_cxl() ? "enabled" : "disabled");
        std::abort();
      }

      champsim::heatmap::enable_hotness_allocation();
      champsim::heatmap::load(use_heatmap_path);
    }
  }

  for (auto& p : phases) {
    std::iota(std::begin(p.trace_index), std::end(p.trace_index), 0);
  }

  // Print simulation configuration
  if (!generate_heatmap_path.empty()) {
    fmt::print("\n*** ChampSim Heatmap Generation Mode ***\nWarmup Instructions: {}\nHeatmap Generation Instructions: {}\nNumber of CPUs: {}\nBase page size: {} (page mode: {})\nHeatmap Output: {}\n\n",
               phases.at(0).length, phases.at(1).length, std::size(gen_environment.cpu_view()), PAGE_SIZE, page_mode_str, generate_heatmap_path);
  } else {
    fmt::print("\n*** ChampSim Multicore Out-of-Order Simulator ***\nWarmup Instructions: {}\nSimulation Instructions: {}\nNumber of CPUs: {}\nBase page size: {} (page mode: {})\n",
               phases.at(0).length, phases.at(1).length, std::size(gen_environment.cpu_view()), PAGE_SIZE, page_mode_str);
    if (!use_heatmap_path.empty()) {
      fmt::print("Using Heatmap: {}\nMemory Allocation Ratio: {}\nSort by Criticality: {}\n",
                 use_heatmap_path, ratio_str, sort_by_criticality ? "Yes" : "No");
    }
    fmt::print("\n");
  }

  // Load precomputed allocation mapping if requested
  if (!use_allocation_path.empty()) {
    g_vmem->load_allocation_mapping(use_allocation_path);
  }

  // Set page mode and carve 2MB pool accordingly
  if (page_mode_str == "2mb") {
    g_vmem->set_default_page_size(PageSize::PAGE_2M);
    fmt::print("Page mode: 2mb (all pages use 2MB mapping)\n");
  } else if (page_mode_str == "mixed") {
    if (use_policy_path.empty()) {
      fmt::print("ERROR: --page-mode mixed requires --use-policy\n");
      return 1;
    }
    g_vmem->set_default_page_size(PageSize::PAGE_PERF);
    fmt::print("Page mode: mixed (page types from policy file)\n");
  } else {
    fmt::print("Page mode: 4kb (all pages use 4KB mapping)\n");
  }
  g_vmem->carve_2mb_pages();

  // Allocate VPNs by heatmap (must happen after page mode is set, so capacity
  // interpretation is correct: 4KB mode uses #capacity directly, 2MB mode uses entry count)
  if (!use_heatmap_path.empty()) {
    size_t colon_pos = ratio_str.find(':');
    if (colon_pos != std::string::npos) {
      uint32_t ratio_first = std::stoul(ratio_str.substr(0, colon_pos));
      uint32_t ratio_second = std::stoul(ratio_str.substr(colon_pos + 1));
      champsim::heatmap::allocate_vpns(sort_by_criticality, ratio_first, ratio_second);
      champsim::heatmap::enable_hotness_allocation();
    } else {
      fmt::print("ERROR: Invalid ratio format. Use N:M format (e.g., 1:3)\n");
      return 1;
    }
  }

  // Phase 1: force all allocations to DRAM when generating pmap
  if (!generate_pmap_path.empty()) {
    g_vmem->generating_physical_mapping = true;
    fmt::print("Physical mapping generation mode: all pages allocated to DRAM\n");
  }

  // Load physical mapping for address replay
  if (!use_pmap_path.empty()) {
    g_vmem->load_physical_mapping(use_pmap_path);
  }

  // Load policy file (page types + per-subpage tier decisions)
  if (!use_policy_path.empty()) {
    g_vmem->load_policy(use_policy_path);
  }

  // Warn if page-mode doesn't match binary's DTLB config
  // (mixed binary has smaller individual DTLBs; pure binary wastes entries in mixed mode)
  if (page_mode_str == "mixed") {
    fmt::print("NOTE: Ensure this binary was built with mixed DTLB config (e.g., 64+32)\n");
  }

  // Enable interleaving allocation tracking if requested
  if (!track_interleaving_path.empty()) {
    g_vmem->enable_interleaving_allocation_tracking(track_interleaving_path);
  }

  // in champsim.cc
  auto phase_stats = champsim::main(gen_environment, phases, traces);

  // Save heatmap if in generate mode
  if (!generate_heatmap_path.empty()) {
    champsim::heatmap::save(generate_heatmap_path);
    fmt::print("\nHeatmap generation completed and saved to: {}\n\n", generate_heatmap_path);
    // return 0;
  }

  // Champsim would print it after warmup and simulation are done
  fmt::print("\nChampSim completed all CPUs\n\n");

  champsim::plain_printer{std::cout}.print(phase_stats);

  for (CACHE& cache : gen_environment.cache_view()) {
    cache.impl_prefetcher_final_stats();
  }

  for (CACHE& cache : gen_environment.cache_view()) {
    cache.impl_replacement_final_stats();
  }

  if (json_option->count() > 0) {
    if (json_file_name.empty()) {
      champsim::json_printer{std::cout}.print(phase_stats);
    } else {
      std::ofstream json_file{json_file_name};
      champsim::json_printer{json_file}.print(phase_stats);
    }
  }

  // Save bandwidth statistics if requested
  if (!save_bandwidth_path.empty()) {
    gen_environment.router_view().save_bandwidth_samples(save_bandwidth_path);
  }

  // Save interleaving allocation tracking if requested
  if (!track_interleaving_path.empty()) {
    g_vmem->save_allocation_tracking();
  }

  // Save physical page mapping if requested (Phase 1 golden run)
  if (!generate_pmap_path.empty()) {
    g_vmem->save_physical_mapping(generate_pmap_path);
  }

  // Print Ramulator statistics after ChampSim statistics
  // This ensures proper output order: ChampSim stats first, then Ramulator stats
  if (auto* ramulator_dram = gen_environment.ramulator_dram_view(); ramulator_dram != nullptr) {
    fmt::print("\n---------- DRAM Ramulator Statistics ----------");
    ramulator_dram->print_ramulator_stats();
  }
  if (auto* ramulator_cxl = gen_environment.ramulator_cxl_dram_view(); ramulator_cxl != nullptr) {
    fmt::print("\n---------- CXL Ramulator Statistics ----------");
    ramulator_cxl->print_ramulator_stats();
  }

  return 0;
}
#endif
