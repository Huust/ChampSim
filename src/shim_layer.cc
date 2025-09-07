#include "shim_layer.h"

#include <cfenv>
#include <fmt/core.h>

#include "util/bits.h"

SHIM_LAYER::SHIM_LAYER(std::vector<channel_type*>&& ll, std::size_t rq_size,
             std::size_t wq_size, champsim::channel *ul, std::size_t enable_dram, std::size_t enable_cxl)
  :WQ(wq_size), RQ(rq_size), ul(ul), ll_queues(ll)
{
  this->mode = get_operate_mode(enable_dram, enable_cxl);
}

// 1. 将旧的responses从internal channel搬运到和LLC相连的champsim channel
// 2. 从lower level搬运新的responses到internal channel
long SHIM_LAYER::handle_responses() {
  long progress{0};
  // 1. Move the response from local queue into upper level's champsim channel
  while (!RespQ.empty()) {
    auto ret = RespQ.front();
    ul->returned.push_back(ret);
    RespQ.pop_front();
  }

  // 2. Move the response from lower level's champsim channel into local queue
  for (auto lower_level: ll_queues) {
    auto ret = lower_level->returned.front();
    RespQ.push_back(ret);
    lower_level->returned.pop_front();
  }

  return progress;
}

long SHIM_LAYER::route() {
  long progress{0};
  // Propagate all requests into same (the only) lower level memory device
  if (mode == SHIM_LAYER::MODE::DRAM_ONLY ||
      mode == SHIM_LAYER::MODE::CXL_ONLY) {
      auto lower_level = ll_queues.front();
      auto drain_and_send = [&](auto& queue, auto add_method_ptr) {
        while (!queue.empty()) {
          (lower_level->*add_method_ptr)(queue.front());
          queue.pop_front();
        }
      };
      
      drain_and_send(WQ, &channel_type::add_wq);
      drain_and_send(RQ, &channel_type::add_rq);
      drain_and_send(PQ, &channel_type::add_pq);
  } else {
      auto drain_and_send = [&](auto& queue, auto add_method_ptr) {
        auto dram_ll = ll_queues[0];
        auto cxl_ll = ll_queues[1];
        while (!queue.empty()) {
          if (queue.front().address.template to<uint64_t>() % 2 != 0)
            (dram_ll->*add_method_ptr)(queue.front());
          else
            (cxl_ll->*add_method_ptr)(queue.front());

          queue.pop_front();
        }
      };
      
      drain_and_send(WQ, &channel_type::add_wq);
      drain_and_send(RQ, &channel_type::add_rq);
      drain_and_send(PQ, &channel_type::add_pq);
    }

  return progress;
}

// Populate local queues with requests from upper level
long SHIM_LAYER::populate_requests() {
  long progress{0};

  auto populate = [](auto queue){
    while (!queue.empty()) {
      auto request = queue.front();
      queue.push_back(request);
      queue.pop_front();
    }
  };
  
  populate(ul->WQ);
  populate(ul->RQ);
  populate(ul->PQ);

  return progress;
}

SHIM_LAYER::MODE SHIM_LAYER::get_operate_mode(std::size_t enable_dram, std::size_t enable_cxl) {
  if (enable_dram >= 1 && enable_cxl >= 1)
      return SHIM_LAYER::MODE::HYBRID;
  else if (enable_dram >= 1 && enable_cxl < 1)
      return SHIM_LAYER::MODE::DRAM_ONLY;
  else if (enable_dram < 1 && enable_cxl >= 1)
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

void SHIM_LAYER::initialize() {
  using namespace champsim::data::data_literals;
  using namespace std::literals::chrono_literals;
  // auto sz = this->size();
  // if (champsim::data::gibibytes gb_sz{sz}; gb_sz > 1_gib) {
  //   fmt::print("off-chip dram size: {}", gb_sz);
  // } else if (champsim::data::mebibytes mb_sz{sz}; mb_sz > 1_mib) {
  //   fmt::print("off-chip dram size: {}", mb_sz);
  // } else if (champsim::data::kibibytes kb_sz{sz}; kb_sz > 1_kib) {
  //   fmt::print("off-chip dram size: {}", kb_sz);
  // } else {
  //   fmt::print("off-chip dram size: {}", sz);
  // }
  // fmt::print(" channels: {} width: {}-bit data rate: {} mt/s\n", std::size(channels), champsim::data::bits_per_byte * channel_width.count(),
             // 1us / (data_bus_period));
}
void SHIM_LAYER::begin_phase() {
  // std::size_t chan_idx = 0;

  // CXL_CHANNEL::stats_type new_stats;
  // new_stats.name = "Channel " + std::to_string(chan_idx);
  // channel.sim_stats = new_stats;
  // channel.warmup = warmup;

  channel_type::stats_type ul_new_roi_stats;
  channel_type::stats_type ul_new_sim_stats;
  ul->roi_stats = ul_new_roi_stats;
  ul->sim_stats = ul_new_sim_stats;
}

void SHIM_LAYER::end_phase(unsigned /*cpu*/) {
  /* roi_stats = sim_stats; */ 
}

void SHIM_LAYER::print_deadlock() {}
