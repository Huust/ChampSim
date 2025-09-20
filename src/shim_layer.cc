#include "shim_layer.h"

#include <cfenv>
#include <fmt/core.h>
#include "deadlock.h"
#include "util/bits.h"
#include "dram_controller.h"

SHIM_LAYER::SHIM_LAYER(champsim::channel *ul, std::vector<channel_type*>&& ll,
                       std::size_t rq_size, std::size_t wq_size, std::size_t pq_size,
                       long int max_upper_bw, long int max_lower_bw,
                       MEMORY_CONTROLLER* dram_ptr, MEMORY_CONTROLLER* cxl_ptr
                      )
  :ul(ul), ll_queues(ll), WQ(wq_size), RQ(rq_size), PQ(pq_size),
   dram_ptr(dram_ptr), cxl_ptr(cxl_ptr),
   UPPER_STREAM_MAX_BW{max_upper_bw}, LOWER_STREAM_MAX_BW{max_lower_bw}
{
  this->mode = get_operate_mode(dram_ptr != nullptr, cxl_ptr != nullptr);
}

// 1. Move the response from lower level's champsim channel (connected to potential memory) into local queue
// 2. Move the response from local queue into upper level's champsim channel (connected with LLC)
long SHIM_LAYER::handle_responses() {
  long progress{0};
  // 1
  for (auto* lower_level : ll_queues) {
    for (auto& ret : lower_level->returned) {
        RespQ.push_back(ret);
        progress++;
    }
    lower_level->returned.clear();
  }
  
  // 2
  for (auto& ret : RespQ) {
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
        if (!warmup) {
          sim_stats.lower_bw_congestion_cycles++;
        }
        break;
      }

      auto& pkt = it->value();
      bool is_cxl_address = pkt.address.template to<uint64_t>() >= dram_ptr->size().count();
      champsim::channel* dest_channel;
      if (this->mode == MODE::DRAM_ONLY)
        dest_channel = ll_queues[0];
      else if (this->mode == MODE::CXL_ONLY)
        dest_channel = ll_queues[1];
      else
        dest_channel = is_cxl_address ? ll_queues[1] : ll_queues[0];
      
      bool success = false;
      
      switch(queue_type) {
        case 'R': success = dest_channel->add_rq(pkt); break;
        case 'W': success = dest_channel->add_wq(pkt); break;
        case 'P': success = dest_channel->add_pq(pkt); break;
      }
      
      if (success) {
        if (!warmup) {
          if (is_cxl_address) {
            stats_func_cxl();
          } else {
            stats_func_dram();
          }
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

// Populate local queues with requests from upper level
long SHIM_LAYER::populate_requests() {
  long progress{0};
  champsim::bandwidth upper_bw{UPPER_STREAM_MAX_BW};

  // RQ processing
  auto rq_it = std::find_if_not(std::begin(RQ), std::end(RQ), [](const auto& pkt){ return pkt.has_value(); });
  
  while (!ul->RQ.empty()) {  // Still have upstream requests
    // First check bandwidth limitation
    if (!upper_bw.has_remaining()) {
      // Bandwidth exhausted
      if (!warmup) {
        sim_stats.upper_bw_congestion_cycles++;  // Upper bandwidth congestion
      }
      break;
    }
    
    // Then check internal buffer availability
    if (rq_it == std::end(RQ)) {
      // Internal RQ is full
      if (!warmup) {
        sim_stats.rq_full++;  // Buffer congestion
      }
      break;
    }
    
    // Both conditions satisfied, transfer the request
    *rq_it = ul->RQ.front();
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
      if (!warmup) {
        sim_stats.upper_bw_congestion_cycles++;  // Upper bandwidth congestion
      }
      break;
    }
    
    // Then check internal buffer availability
    if (wq_it == std::end(WQ)) {
      // Internal WQ is full
      if (!warmup) {
        sim_stats.wq_full++;  // Buffer congestion
      }
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
      if (!warmup) {
        sim_stats.upper_bw_congestion_cycles++;  // Upper bandwidth congestion
      }
      break;
    }
    
    // Then check internal buffer availability
    if (pq_it == std::end(PQ)) {
      // Internal PQ is full
      if (!warmup) {
        sim_stats.pq_full++;  // Buffer congestion
      }
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
  progress += route();
  progress += populate_requests();
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
  for (auto levels : {std::vector{ul}, ll_queues}) {
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
