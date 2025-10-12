#include "shim_layer.h"

#include <cassert>
#include <cfenv>
#include <stdexcept>
#include <fmt/core.h>
#include "access_type.h"
#include "deadlock.h"
#include "operable.h"
#include "util/bits.h"
#include "dram_controller.h"
#include "heatmap.h"
#include "environment.h"
#include "ooo_cpu.h"

SHIM_LAYER::SHIM_LAYER(champsim::chrono::picoseconds clock_period, champsim::channel *ul, std::vector<channel_type*>&& ll,
                       std::size_t rq_size, std::size_t wq_size, std::size_t pq_size,
                       long int max_upper_bw, long int max_lower_bw,
                       MEMORY_CONTROLLER* dram_ptr, MEMORY_CONTROLLER* cxl_ptr,
                       champsim::environment* env_ptr
                      )
  : champsim::operable(clock_period),
    ul(ul), ll_queues(ll), WQ(wq_size), RQ(rq_size), PQ(pq_size),
    dram_ptr(dram_ptr), cxl_ptr(cxl_ptr), env_ptr(env_ptr),
    UPPER_STREAM_MAX_BW{max_upper_bw}, LOWER_STREAM_MAX_BW{max_lower_bw}
{
  this->mode = get_operate_mode(dram_ptr != nullptr, cxl_ptr != nullptr);
}

// 1. Move the response from lower level's champsim channel (connected to potential memory) into local queue
// 2. Move the response from local queue into upper level's champsim channel (connected with LLC)
long SHIM_LAYER::handle_responses() {
  long progress{0};
  // 1
  for (auto lower_level : ll_queues) {
    for (auto& ret : lower_level->returned) {
        RespQ.push_back(ret);
        progress++;
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
        is_cxl_address = pkt.address.template to<uint64_t>() >= dram_ptr->size().count();
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
        } else {
          stats_func_dram();
        }
        it->reset();
        lower_bw.consume();
        progress++;
      }
    }
  };

  process_queue(RQ, [&]{ sim_stats.dram_requests_read++; sim_stats.dram_requests_total++; }, 
                    [&]{ sim_stats.cxl_requests_read++; sim_stats.cxl_requests_total++; }, 'R');
  process_queue(WQ, [&]{ sim_stats.dram_requests_write++; sim_stats.dram_requests_total++; }, 
                    [&]{ sim_stats.cxl_requests_write++; sim_stats.cxl_requests_total++; }, 'W');
  process_queue(PQ, [&]{ sim_stats.dram_requests_prefetch++; sim_stats.dram_requests_total++; }, 
                    [&]{ sim_stats.cxl_requests_prefetch++; sim_stats.cxl_requests_total++; }, 'P');
  
  return progress;
}

// Warmup mode: direct pass-through without bandwidth/queue limits (from upper level to lower level)
long SHIM_LAYER::warmup_fast_forward() {
  long progress{0};

  // Process all upstream requests directly
  auto process_direct_transfer = [&](auto& from_queue, char queue_type) {
    while (!from_queue.empty()) {
      auto req = from_queue.front();
      from_queue.pop_front();

      // Route to appropriate lower level
      bool is_cxl_address;
      if (mode == MODE::CXL_ONLY) {
        is_cxl_address = true;
      } else if (mode == MODE::HYBRID) {
        is_cxl_address = req.address.template to<uint64_t>() >= dram_ptr->size().count();
      } else if (mode == MODE::DRAM_ONLY) {
        is_cxl_address = false;
      } else {
        assert(0);
        abort();
      }

      champsim::channel* dest_channel = ll_queues[0];
      if (mode == MODE::HYBRID)
        dest_channel = is_cxl_address ? ll_queues[1] : ll_queues[0];

      // Direct transfer without checking success
      switch(queue_type) {
        case 'R': dest_channel->add_rq(req); break;
        case 'W': dest_channel->add_wq(req); break;
        case 'P': dest_channel->add_pq(req); break;
      }

      progress++;
    }
  };

  process_direct_transfer(ul->RQ, 'R');
  process_direct_transfer(ul->WQ, 'W');
  process_direct_transfer(ul->PQ, 'P');

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
         (*rq_it)->type == access_type::TRANSLATION || (*rq_it)->type == access_type::PREFETCH)) {
      champsim::heatmap::track_llc_miss((*rq_it)->v_address);

      // Set is_llc_miss in corresponding ROB entry as true
      // RFOs always come from write request, and at this moment write has been retired
      // If the translation comes from write, at this moment write has been retired
      // No matter what situation, rob_entry is gone
      // For load request, it should always be within the ROB before reposoen before reposoen before reposoen before response is reached

      // Anyway, the possible situation can be:
      // All loads
      // Translation triggered by load requests
      if ((*rq_it)->type == access_type::LOAD || (*rq_it)->type == access_type::TRANSLATION) {
        auto rob_entry = get_rob_entry((*rq_it)->cpu, (*rq_it)->instr_id);
        if (rob_entry != nullptr) {
          rob_entry->is_llc_miss = true;
          // Track the source memory address that caused this LLC miss
          rob_entry->llc_miss_source_memory.push_back((*rq_it)->v_address);
        } else
          assert((*rq_it)->type != access_type::LOAD);
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

  // Search through ROB to find matching instruction ID
  auto rob_entry = std::partition_point(cpu.ROB.begin(), cpu.ROB.end(), ooo_model_instr::precedes(instr_id));
  assert(rob_entry != cpu.ROB.end());

  return (rob_entry->instr_id == instr_id) ? &(*rob_entry) : nullptr;
}
