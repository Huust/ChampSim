#include "shim_layer.h"

#include <cassert>
#include <cfenv>
#include <fstream>
#include <fmt/core.h>
#include "access_type.h"
#include "deadlock.h"
#include "operable.h"
#include "util/bits.h"
#include "dram_controller.h"
#include "ramulator_controller.h"
#include "heatmap.h"
#include "environment.h"
#include "ooo_cpu.h"

// Shim layer sits between LLC and memory controllers. LLC's max_tag_check=1
// limits its output to ~1-2 requests/cycle, so even small values for queue
// sizes and bandwidth here won't bottleneck. Current defaults (64) are safe.
SHIM_LAYER::SHIM_LAYER(champsim::chrono::picoseconds clock_period, champsim::channel *ul, std::vector<channel_type*>&& ll,
                       std::size_t rq_size, std::size_t wq_size, std::size_t pq_size,
                       long int max_upper_bw, long int max_lower_bw,
                       champsim::environment* env_ptr
                      )
  : champsim::operable(clock_period),
    ul(ul), ll_queues(ll), WQ(wq_size), RQ(rq_size), PQ(pq_size),
    env_ptr(env_ptr),
    UPPER_STREAM_MAX_BW{max_upper_bw}, LOWER_STREAM_MAX_BW{max_lower_bw}
{
  this->mode = get_operate_mode(env_ptr->has_dram(), env_ptr->has_cxl());
}

// 1. Move the response from lower level's champsim channel (connected to potential memory) into local queue
// 2. Move the response from local queue into upper level's champsim channel (connected with LLC)
long SHIM_LAYER::handle_responses() {
  long progress{0};
  // 1
  for (size_t i = 0; i < ll_queues.size(); ++i) {
    auto lower_level = ll_queues[i];
    bool is_cxl_channel = (i == 1); // ll_queues[0] = DRAM, ll_queues[1] = CXL

    for (auto& ret : lower_level->returned) {
        ret.is_cxl_memory = is_cxl_channel;
        RespQ.push_back(ret);
        progress++;

        // Track bandwidth: count read responses
        if (!warmup) {
          if (is_cxl_channel) {
            cxl_read_responses++;
          } else {
            dram_read_responses++;
          }
        }
    }
    lower_level->returned.clear();
  }

  // 2
  for (auto& ret : RespQ) {
    ret.is_llc_miss = true;
    ul->returned.push_back(ret);
    progress++;
  }
  RespQ.clear();

  return progress;
}

// Route requests from internal channels into corresponding lower levels
long SHIM_LAYER::route() {
  long progress{0};
  champsim::bandwidth lower_bw{LOWER_STREAM_MAX_BW};

  auto process_queue = [&](auto& queue, auto stats_func_dram, auto stats_func_cxl, char queue_type) {
    for (auto it = std::begin(queue); it != std::end(queue); ++it) {
      if (!it->has_value()) {
        continue;
      }

      if (!lower_bw.has_remaining()) {
        sim_stats.lower_bw_congestion_cycles++;
        break;
      }

      auto& pkt = it->value();
      bool is_cxl_address;
      if (mode == MODE::CXL_ONLY) {
        is_cxl_address = true;
      } else if (mode == MODE::HYBRID) {
        // Get DRAM size from whichever controller is used for physical memory
        champsim::data::bytes dram_size{0};
        if (auto* ramulator_dram = env_ptr->ramulator_dram_view()) {
          // Physical memory using Ramulator
          dram_size = ramulator_dram->size();
        } else if (auto* builtin_dram = env_ptr->builtin_dram_view()) {
          // Physical memory using builtin DRAM controller
          dram_size = builtin_dram->size();
        } else {
          // No physical DRAM controller found - this should not happen
          assert(false && "No physical DRAM controller view available");
          abort();
        }
        is_cxl_address = pkt.address.template to<uint64_t>() >= static_cast<uint64_t>(dram_size.count());
      } else if (mode == MODE::DRAM_ONLY) {
        is_cxl_address = false;
      } else {
        assert(0);
        abort();
      }

      champsim::channel* dest_channel = ll_queues[0];
      if (mode == MODE::HYBRID)
        dest_channel = is_cxl_address ? ll_queues[1] : ll_queues[0];
      
      bool success = false;
      
      switch(queue_type) {
        case 'R': success = dest_channel->add_rq(pkt); break;
        case 'W': success = dest_channel->add_wq(pkt); break;
        case 'P': success = dest_channel->add_pq(pkt); break;
      }
      
      if (success) {
        if (is_cxl_address) {
          stats_func_cxl();
          if (!warmup && pkt.type == access_type::TRANSLATION)
            sim_stats.cxl_requests_translation++;
        } else {
          stats_func_dram();
          if (!warmup && pkt.type == access_type::TRANSLATION)
            sim_stats.dram_requests_translation++;
        }
        it->reset();
        lower_bw.consume();
        progress++;
      }
    }
  };

  process_queue(RQ, [&]{ sim_stats.dram_requests_read++; sim_stats.dram_requests_total++; },
                    [&]{ sim_stats.cxl_requests_read++; sim_stats.cxl_requests_total++; }, 'R');
  process_queue(WQ, [&]{ sim_stats.dram_requests_write++; sim_stats.dram_requests_total++; dram_write_requests++; },
                    [&]{ sim_stats.cxl_requests_write++; sim_stats.cxl_requests_total++; cxl_write_requests++; }, 'W');
  process_queue(PQ, [&]{ sim_stats.dram_requests_prefetch++; sim_stats.dram_requests_total++; },
                    [&]{ sim_stats.cxl_requests_prefetch++; sim_stats.cxl_requests_total++; }, 'P');
  
  return progress;
}

// Warmup mode: directly convert requests to responses (like DRAM controller)
long SHIM_LAYER::warmup_fast_forward() {
  long progress{0};

  // Process read requests (RQ and PQ): convert to responses immediately
  auto process_read_queue = [&](auto& from_queue) {
    while (!from_queue.empty()) {
      auto req = from_queue.front();
      from_queue.pop_front();

      // Directly create response and return it
      champsim::channel::response_type response{req.address, req.v_address, req.data, req.pf_metadata, req.instr_depend_on_me};
      ul->returned.push_back(response);

      progress++;
    }
  };

  // Process write requests (WQ): just discard them
  auto process_write_queue = [&](auto& from_queue) {
    while (!from_queue.empty()) {
      from_queue.pop_front();
      progress++;
    }
  };

  process_read_queue(ul->RQ);
  process_write_queue(ul->WQ);
  process_read_queue(ul->PQ);

  return progress;
}

long SHIM_LAYER::populate_requests() {
  long progress{0};
  champsim::bandwidth upper_bw{UPPER_STREAM_MAX_BW};

  // RQ processing
  auto rq_it = std::find_if_not(std::begin(RQ), std::end(RQ), [](const auto& pkt){ return pkt.has_value(); });
  
  while (!ul->RQ.empty()) {  // Still have upstream requests
    // First check bandwidth limitation
    if (!upper_bw.has_remaining()) {
      // Bandwidth exhausted
      sim_stats.upper_bw_congestion_cycles++;
      break;
    }
    
    // Then check internal buffer availability
    if (rq_it == std::end(RQ)) {
      // Internal RQ is full
      sim_stats.rq_full++;
      break;
    }
    
    *rq_it = ul->RQ.front();
    // Track four cases of access type
    if (!warmup && champsim::heatmap::is_heatmap_generation_enabled() &&
        ((*rq_it)->type == access_type::LOAD || (*rq_it)->type == access_type::RFO ||
         (*rq_it)->type == access_type::TRANSLATION)) {
      champsim::heatmap::track_llc_miss((*rq_it)->v_address);

      // Set is_llc_miss in corresponding ROB entry as true
      if ((*rq_it)->type == access_type::LOAD || (*rq_it)->type == access_type::TRANSLATION) {
        // Several cases for get_rob_entry() return different values
        // 1. Load instr (or its translation), no branch prediction, return entry pointer
        // 2. Load instr (or its translation),    branch prediction, return nullptr
        // 3. Translation from write instr, return nullptr
        auto rob_entry = get_rob_entry((*rq_it)->cpu, (*rq_it)->instr_id);
        if (rob_entry != nullptr && rob_entry->caused_rob_stall == false) {
          if ((*rq_it)->type == access_type::LOAD)
            rob_entry->is_load_llc_miss = true;
          else {
            rob_entry->is_trans_llc_miss = true;
            rob_entry->translation_stall_source_memory.insert((*rq_it)->v_address.to<uint64_t>());
          }
          // Track the source memory address that caused this LLC miss (no duplicates)
          rob_entry->llc_miss_source_memory.insert((*rq_it)->v_address.to<uint64_t>());
        }
      }
    }

    ul->RQ.pop_front();
    ++rq_it;
    upper_bw.consume();
    progress++;
    
    // Find next available slot
    rq_it = std::find_if_not(rq_it, std::end(RQ), [](const auto& pkt){ return pkt.has_value(); });
  }

  // WQ processing
  auto wq_it = std::find_if_not(std::begin(WQ), std::end(WQ), [](const auto& pkt){ return pkt.has_value(); });
  
  while (!ul->WQ.empty()) {  // Still have upstream requests
    // First check bandwidth limitation
    if (!upper_bw.has_remaining()) {
      // Bandwidth exhausted
      sim_stats.upper_bw_congestion_cycles++;
      break;
    }
    
    // Then check internal buffer availability
    if (wq_it == std::end(WQ)) {
      // Internal WQ is full
      sim_stats.wq_full++;
      break;
    }
    
    // Both conditions satisfied, transfer the request
    *wq_it = ul->WQ.front();
    ul->WQ.pop_front();
    ++wq_it;
    upper_bw.consume();
    progress++;
    
    // Find next available slot
    wq_it = std::find_if_not(wq_it, std::end(WQ), [](const auto& pkt){ return pkt.has_value(); });
  }
  
  // PQ processing
  auto pq_it = std::find_if_not(std::begin(PQ), std::end(PQ), [](const auto& pkt){ return pkt.has_value(); });
  
  while (!ul->PQ.empty()) {  // Still have upstream requests
    // First check bandwidth limitation
    if (!upper_bw.has_remaining()) {
      // Bandwidth exhausted
      sim_stats.upper_bw_congestion_cycles++;
      break;
    }
    
    // Then check internal buffer availability
    if (pq_it == std::end(PQ)) {
      // Internal PQ is full
      sim_stats.pq_full++;
      break;
    }
    
    // Both conditions satisfied, transfer the request
    *pq_it = ul->PQ.front();
    ul->PQ.pop_front();
    ++pq_it;
    upper_bw.consume();
    progress++;
    
    // Find next available slot
    pq_it = std::find_if_not(pq_it, std::end(PQ), [](const auto& pkt){ return pkt.has_value(); });
  }

  return progress;
}

SHIM_LAYER::MODE SHIM_LAYER::get_operate_mode(bool is_dram_enabled, bool is_cxl_enabled) {
  if (is_dram_enabled && is_cxl_enabled)
      return SHIM_LAYER::MODE::HYBRID;
  else if (!is_dram_enabled && is_cxl_enabled)
      return SHIM_LAYER::MODE::CXL_ONLY;
  else return SHIM_LAYER::MODE::DRAM_ONLY;
}

long SHIM_LAYER::operate() {
  long progress{0};
  progress += handle_responses();

  if (warmup) {
    // Warmup mode: direct pass-through, skip bandwidth modeling
    progress += warmup_fast_forward();
  } else {
    // Normal mode: full bandwidth modeling and queue management
    progress += route();
    progress += populate_requests();

    // Check if we need to save a bandwidth sample
    sample_bandwidth();
  }

  return progress;
}

// print operating mode and queue size
void SHIM_LAYER::initialize() {
  fmt::print("address-based ROUTER, operating in ");
  switch (mode) {
    case SHIM_LAYER::MODE::DRAM_ONLY:
      fmt::print("DRAM_ONLY mode.\n");
      break;
    case SHIM_LAYER::MODE::CXL_ONLY:
      fmt::print("CXL_ONLY mode.\n");
      break;
    case SHIM_LAYER::MODE::HYBRID:
      fmt::print("HYBRID mode.\n");
      break;
  }

  fmt::print("ROUTER RQ size: {}, WQ size: {}, PQ size: {}\n", std::size(RQ), std::size(WQ), std::size(PQ));
}
void SHIM_LAYER::begin_phase() {
  shim_stats new_roi_stats;
  new_roi_stats.name = "router";
  roi_stats = new_roi_stats;

  // Reset bandwidth sampling counters
  current_sample_start_cycle = current_cycle();
  dram_read_responses = 0;
  dram_write_requests = 0;
  cxl_read_responses = 0;
  cxl_write_requests = 0;
  bandwidth_samples.clear();

  // Update upper level's and lower levels' channel stats
  for(auto levels : {std::vector{ul}, ll_queues}) {
    for (auto l: levels) {
      channel_type::stats_type new_level_roi_stats;
      channel_type::stats_type new_level_sim_stats;
      l->roi_stats = new_level_roi_stats;
      l->sim_stats = new_level_sim_stats;
    }
  }
}

void SHIM_LAYER::end_phase(unsigned /*cpu*/) {
  roi_stats = sim_stats;
}

void SHIM_LAYER::print_deadlock() {
  // Generic printer
  std::string_view q_writer{"instr_id: {} address: {:#x} v_addr: {:#x} type: {}"};
  auto q_entry_pack = [](const auto& entry) {
    return std::tuple{entry->instr_id, entry->address, entry->v_address, access_type_names.at(champsim::to_underlying(entry->type))};
  };

  champsim::range_print_deadlock(RQ, "ROUTER_RQ", q_writer, q_entry_pack);
  champsim::range_print_deadlock(WQ, "ROUTER_WQ", q_writer, q_entry_pack);
  champsim::range_print_deadlock(PQ, "ROUTER_PQ", q_writer, q_entry_pack);
  // RespQ is deque, so doesn't need deference
  champsim::range_print_deadlock(RespQ, "ROUTER_RespQ", q_writer, [](const auto& entry){
    return std::tuple{entry.address, entry.v_address};
  });
}

// ROB access function - finds and returns reference to ROB entry by instruction ID
ooo_model_instr* SHIM_LAYER::get_rob_entry(uint32_t cpu_id, uint64_t instr_id) {
  assert(env_ptr != nullptr);
  auto cpu_view = env_ptr->cpu_view();

  assert(cpu_id < cpu_view.size());
  O3_CPU& cpu = cpu_view[cpu_id];

  auto rob_entry = std::partition_point(cpu.ROB.begin(), cpu.ROB.end(), ooo_model_instr::precedes(instr_id));
  return (rob_entry != cpu.ROB.end() && rob_entry->instr_id == instr_id) ? &(*rob_entry) : nullptr;
}

// Check if it's time to save a bandwidth sample
void SHIM_LAYER::sample_bandwidth() {
  uint64_t cycle = current_cycle();
  uint64_t elapsed = cycle - current_sample_start_cycle;

  if (elapsed >= bandwidth_sample_interval) {
    // Save current sample
    BandwidthSample sample;
    sample.cycle_start = current_sample_start_cycle;
    sample.cycle_end = cycle;
    sample.dram_read_count = dram_read_responses;
    sample.dram_write_count = dram_write_requests;
    sample.cxl_read_count = cxl_read_responses;
    sample.cxl_write_count = cxl_write_requests;
    bandwidth_samples.push_back(sample);

    // Reset for next interval
    current_sample_start_cycle = cycle;
    dram_read_responses = 0;
    dram_write_requests = 0;
    cxl_read_responses = 0;
    cxl_write_requests = 0;
  }
}

// Save bandwidth samples to file
void SHIM_LAYER::save_bandwidth_samples(const std::string& filename) {
  std::ofstream outfile(filename);
  if (!outfile.is_open()) {
    fmt::print("Warning: Cannot open bandwidth samples file: {}\n", filename);
    return;
  }

  // Header
  outfile << "cycle_start,cycle_end,dram_read_bw_GBps,dram_total_bw_GBps,cxl_read_bw_GBps,cxl_write_bw_GBps\n";

  // Calculate frequency in GHz: 1000 ps = 1 ns = 1 GHz
  double frequency_GHz = 1000.0 / clock_period.count();

  // Data: calculate bandwidth in GB/s
  for (const auto& sample : bandwidth_samples) {
    uint64_t cycles = sample.cycle_end - sample.cycle_start;
    if (cycles == 0) continue;

    // Bandwidth (bytes/cycle) × frequency (GHz) = Bandwidth (GB/s)
    double dram_read_bw = (sample.dram_read_count * BLOCK_SIZE * frequency_GHz) / cycles;
    double dram_total_bw = ((sample.dram_read_count + sample.dram_write_count) * BLOCK_SIZE * frequency_GHz) / cycles;
    double cxl_read_bw = (sample.cxl_read_count * BLOCK_SIZE * frequency_GHz) / cycles;
    double cxl_write_bw = (sample.cxl_write_count * BLOCK_SIZE * frequency_GHz) / cycles;

    outfile << sample.cycle_start << ","
            << sample.cycle_end << ","
            << dram_read_bw << ","
            << dram_total_bw << ","
            << cxl_read_bw << ","
            << cxl_write_bw << "\n";
  }

  outfile.close();
  fmt::print("Bandwidth samples saved to: {} ({} samples)\n", filename, bandwidth_samples.size());
}
