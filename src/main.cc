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

  bool generate_heatmap = false;
  std::string heatmap_dir;
  bool sort_by_criticality = false;
  std::string ratio_str;

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

  app.add_flag("--generate-heatmap", generate_heatmap, "Generate heatmap data by running part of trace instructions after warmup");
  app.add_option("--heatmap-dir", heatmap_dir, "Directory path for heatmap output file");
  app.add_flag("--sort-by-criticality", sort_by_criticality, "Sort virtual pages by criticality when allocating (for simulation mode)");
  app.add_option("--ratio", ratio_str, "Ratio for memory allocation (format: N:M, for simulation mode)");

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

  if (generate_heatmap) {
    // Generate heatmap mode: warmup + heatmap generation phase
    if (heatmap_dir.empty()) {
      fmt::print("ERROR: --heatmap-dir is required when using --generate-heatmap\n");
      return 1;
    }
    champsim::heatmap::enable(heatmap_dir);

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

    // Load heatmap and allocate VPNs if parameters are provided
    if (!heatmap_dir.empty()) {
      // For simulation mode, we need to set the file path first
      std::ifstream check_file(heatmap_dir);
      if (!check_file.good()) {
        fmt::print("ERROR: Cannot access heatmap file: {}\\n", heatmap_dir);
        return 1;
      }

      // Enable heatmap with the file path, then load the data
      champsim::heatmap::enable(heatmap_dir);
      champsim::heatmap::load();

      if (!ratio_str.empty()) {
        // Parse ratio string (format: "N:M")
        size_t colon_pos = ratio_str.find(':');
        if (colon_pos != std::string::npos) {
          uint32_t ratio_first = std::stoul(ratio_str.substr(0, colon_pos));
          uint32_t ratio_second = std::stoul(ratio_str.substr(colon_pos + 1));

          champsim::heatmap::allocate_vpns(sort_by_criticality, ratio_first, ratio_second);
          champsim::heatmap::launch_heatmap();
        } else {
          fmt::print("ERROR: Invalid ratio format. Use N:M format (e.g., 1:3)\\n");
          return 1;
        }
      }
    }
  }

  // std::iota 是 C++ 标准库 <numeric> 中提供的一个算法，用来为一个范围内的元素赋予连续递增的数值序列。具体来说：
	// 参数： 它接受三个参数：起始迭代器、结束迭代器和一个初始值。
  for (auto& p : phases) {
    std::iota(std::begin(p.trace_index), std::end(p.trace_index), 0);
  }

  // Print simulation configuration
  if (generate_heatmap) {
    fmt::print("\n*** ChampSim Heatmap Generation Mode ***\nWarmup Instructions: {}\nHeatmap Generation Instructions: {}\nNumber of CPUs: {}\nPage size: {}\nHeatmap Output: {}\n\n",
               phases.at(0).length, phases.at(1).length, std::size(gen_environment.cpu_view()), PAGE_SIZE, heatmap_dir);
  } else {
    fmt::print("\n*** ChampSim Multicore Out-of-Order Simulator ***\nWarmup Instructions: {}\nSimulation Instructions: {}\nNumber of CPUs: {}\nPage size: {}\n\n",
               phases.at(0).length, phases.at(1).length, std::size(gen_environment.cpu_view()), PAGE_SIZE);
  }

  // in champsim.cc
  auto phase_stats = champsim::main(gen_environment, phases, traces);

  // Save heatmap if in generate mode
  if (generate_heatmap) {
    champsim::heatmap::save();
    fmt::print("\nHeatmap generation completed and saved to: {}\n\n", heatmap_dir);
    return 0; // Exit after heatmap generation
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

  return 0;
}
#endif
