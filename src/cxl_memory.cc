#include "cxl_memory.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <fmt/core.h>

#include "champsim.h"
#include "deadlock.h"
#include "instruction.h"
#include "operable.h"
#include "util/bits.h"
#include "util/span.h"
#include "util/units.h"

CXL_CONTROLLER::CXL_CONTROLLER(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl,
                               std::vector<channel_type*>&& ul, std::size_t rq_size, std::size_t wq_size,
                               champsim::data::bytes chan_width, double rx_bw, double tx_bw)
  : champsim::operable(cxl_io_period), queues(std::move(ul)), channel_width(chan_width),
{
  channel = CXL_CHANNEL(cxl_io_period, t_cxl, chan_width, rq_size, wq_size, rx_bw, tx_bw);
}

CXL_CHANNEL::CXL_CHANNEL(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl, champsim::data::bytes width,
                         std::size_t rq_size, std::size_t wq_size, double rx_bw, double tx_bw)
  : champsim::operable(cxl_io_period), channel_width(width), WQ{wq_size}, RQ{rq_size},
    tCXL(t_cxl), tRD(static_cast<champsim::chrono::picoseconds>((1000 * (BLOCK_SIZE * 8/ (rx_bw * 8 * width))))),
    tWR(static_cast<champsim::chrono::picoseconds>((1000 * (BLOCK_SIZE * 8/ (tx_bw * 8 * width))))),
    // rx_bw and tx_bw is based on GB/s, width is based on bytes, tWR and tRD (clock::duration) is based on picoseconds
{

}

long CXL_CONTROLLER::operate() {
  long progress{0};
  initiate_requests();
  progress += channel._operate();
  return progress;
}

long CXL_CHANNEL::operate() {
  long progress{0};

  if (warmup) {
    for (auto& entry : RQ) {
      if (entry.has_value()) {
        response_type response{entry->address, entry->v_address, entry->data, entry->pf_metadata, entry->instr_depend_on_me};
        for (auto* ret : entry.value().to_return) {
          ret->push_back(response);
        }

        ++progress;
        entry.reset();
      }
    }

    for (auto& entry : WQ) {
      if (entry.has_value()) {
        ++progress;
      }
      entry.reset();
    }
  }

  check_collision();
  progress += finish_pcie_transfer();

  return progress;
}

void CXL_CONTROLLER::initiate_requests() {
  for (auto* ul : queues) {
    for (auto q : {std::ref(ul->RQ), std::ref(ul->PQ)}) {
      auto [begin, end] = champsim::get_span_p(std::cbegin(q.get()), std::cend(q.get()), [ul, this](const auto& pkt) { return this->add_rq(pkt, ul); });
      q.get().erase(begin, end);
    }

    // Initiate write requests
    auto [wq_begin, wq_end] = champsim::get_span_p(std::cbegin(ul->WQ), std::cend(ul->WQ), [this](const auto& pkt) { return this->add_wq(pkt); });
    ul->WQ.erase(wq_begin, wq_end);
  }
}

bool CXL_CONTROLLER::add_rq(const auto& pkt, channel_type* ul) {
  // TODO: In future we may have more than one cxl channel,
  // so here leaves room for addressing corresponding cxl channel
  if (auto rq_it = std::find_if_not(std::begin(channel.RQ), std::end(channel.RQ), [this](const auto &pkt){ return pkt.has_value(); });
      rq_it != std::end(channel.RQ)) {
    *rq_it = CXL_CHANNEL::request_type{pkt}; 
    rq_it->value().forward_checked = false;
    rq_it->value().finished = false;
    rq_it->value().ready_time = current_time + tCXL;
    if (packet.response_requested)
      rq_it->value().to_return = {&ul->returned};

    return true; 
  }

  return false;
}

bool CXL_CONTROLLER::add_wq(const auto& pkt) {
  if (auto wq_it = std::find_if_not(std::begin(channel.WQ), std::end(channel.WQ), [this](const auto &pkt){ return pkt.has_value(); });
      wq_it != std::end(channel.WQ)) {
    *wq_it = CXL_CHANNEL::request_type{pkt};
    wq_it->value().forward_checked = false;
    wq_it->value().finished = false;
    wq_it->value().ready_time = current_time + tCXL;

    return true; 
  }

  return false;
}

void CXL_CHANNEL::check_collision()
{
  auto is_collision = [](champsim::address a, champsim::address b) {
    // collision if everything but offset matches
    // TODO: Offset is 6 bits?
    champsim::data::bits offset_bits = champsim::data::bits{6};
    return a.slice_upper(offset_bits) == b.slice_upper(offset_bits);
  };
  auto checker = [addr_map = address_mapping, check_val = wq_it->value().address](const auto& pkt) {
        return pkt.has_value() && is_collision(check_val, pkt.value().address);
  };

  // Write Collision
  for (auto wq_it = std::begin(WQ); wq_it != std::end(WQ); ++wq_it) {
    if (wq_it->has_value() && !wq_it->value().forward_checked) {
      auto found = std::find_if(std::begin(WQ), wq_it, checker); // Forward check
      if (found == wq_it) {
        found = std::find_if(std::next(wq_it), std::end(WQ), checker); // Backward check
      }

      if (found != std::end(WQ)) {
        wq_it->reset();
      } else {
        wq_it->value().forward_checked = true;
      }
    }
  }

  // Read Collision
  for (auto rq_it = std::begin(RQ); rq_it != std::end(RQ); ++rq_it) {
    if (rq_it->has_value() && !rq_it->value().forward_checked) {
      // write forwarding
      if (auto wq_it = std::find_if(std::begin(WQ), std::end(WQ), checker); wq_it != std::end(WQ)) {
        response_type response{rq_it->value().address, rq_it->value().v_address, wq_it->value().data, rq_it->value().pf_metadata,
                               rq_it->value().instr_depend_on_me};
        for (auto* ret : rq_it->value().to_return) {
          ret->push_back(response);
        }

        rq_it->reset();
      }
      // backwards check
      else if (auto found = std::find_if(std::begin(RQ), rq_it, checker); found != rq_it) {
        auto instr_copy = std::move(found->value().instr_depend_on_me);
        auto ret_copy = std::move(found->value().to_return);

        std::set_union(std::begin(instr_copy), std::end(instr_copy), std::begin(rq_it->value().instr_depend_on_me), std::end(rq_it->value().instr_depend_on_me),
                       std::back_inserter(found->value().instr_depend_on_me));
        std::set_union(std::begin(ret_copy), std::end(ret_copy), std::begin(rq_it->value().to_return), std::end(rq_it->value().to_return),
                       std::back_inserter(found->value().to_return));

        rq_it->reset();

      }
      // forwards check
      else if (found = std::find_if(std::next(rq_it), std::end(RQ), checker); found != std::end(RQ)) {
        auto instr_copy = std::move(found->value().instr_depend_on_me);
        auto ret_copy = std::move(found->value().to_return);

        std::set_union(std::begin(instr_copy), std::end(instr_copy), std::begin(rq_it->value().instr_depend_on_me), std::end(rq_it->value().instr_depend_on_me),
                       std::back_inserter(found->value().instr_depend_on_me));
        std::set_union(std::begin(ret_copy), std::end(ret_copy), std::begin(rq_it->value().to_return), std::end(rq_it->value().to_return),
                       std::back_inserter(found->value().to_return));

        rq_it->reset();
      } else {
        rq_it->value().forward_checked = true;
      }
    }
  }
}

// Operate bus for CXL/PCIe interface, dual direction
long CXL_CHANNEL::finish_pcie_transfer() {
  long progress{0};
  // finish read response transfer  
  if (active_rd_resp_on_bus != std::end(RespQ) && active_rd_resp_on_bus->value().ready_time <= current_time) {
    response_type response{active_rd_resp_on_bus->value().address, active_rd_resp_on_bus->value().v_address, active_rd_resp_on_bus->value().data,
                           active_rd_resp_on_bus->value().pf_metadata, active_rd_resp_on_bus->value().instr_depend_on_me};

    for (auto* ret : active_rd_resp_on_bus->value().to_return) {
      ret->push_back(response);
    }
    
    active_rd_resp_on_bus->reset();
    active_rd_resp_on_bus = std::end(RespQ);
    ++progress;
  }
      
  // finish write request transfer 
  handle_writes(); 

  return progress;
}

// 1. If a request is on write bus and satisfies ready_time, we move it from dbus into channel between cxl controller and dram 
// 2. Then we choose another write request (which is already in WQ and goes through tCXL latency) and set its ready_time as current_time + tWR
void CXL_CHANNEL::handle_writes() {
  if (active_wr_req_on_bus != std::end(WQ) && active_wr_req_on_bus->value().ready_time <= current_time) {
    auto pkt = champsim::request_type{*active_wr_req_on_bus};
    ll->add_wq(pkt);
    active_wr_req_on_bus->reset();
    active_wr_req_on_bus = std::end(WQ);
  }

  if (active_wr_req_on_bus == std::end(WQ)) {
    // find a suitable write request in WQ and put it on the bus
    // rule: has_value(), FCFS, tCXL satisfied
    auto next_to_write = [this](const auto& lhs, const auto& rhs) {
      if (!lhs->has_value() || current_time > wr_bus_cycle_available)
        return false;
      if (!rhs->has_value() || current_time > wr_bus_cycle_available)
        return true;

      return lhs->value().ready_time < rhs->value().ready_time;
    }

    if (queue_type::iterator iter_next_to_write = std::min_element(std::begin(WQ), std::end(WQ), next_to_write);
        iter_next_to_write != std::end(WQ)) {
      wr_bus_cycle_available = current_time + tWR;
      active_wr_req_on_bus = std::move(iter_next_to_write);
    }
  }
}

void CXL_CHANNEL::handle_reads() {
  auto next_to_read = [this](const auto& lhs, const auto& rhs) {
    if (!lhs->has_value() || lhs->value().ready_time > current_time)
      return false;
    if (!rhs->has_value() || rhs->value().ready_time > current_time)
      return true;

    return lhs->value().ready_time < rhs->value().ready_time;
  }

  if (queue_type::iterator iter_next_to_read = std::min_element(std::begin(RQ), std::end(RQ), next_to_read);
    iter_next_to_read != std::end(RQ)) {
      auto pkt = champsim::request_type{*iter_next_to_read};
      ll->add_rq(pkt);
      iter_next_to_read->reset();
  }
}


// Inherit from operable
void CXL_CONTROLLER::initialize() {
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

void CXL_CHANNEL::initialize() {}

void CXL_CONTROLLER::begin_phase()
{
  std::size_t chan_idx = 0;

  DRAM_CHANNEL::stats_type new_stats;
  new_stats.name = "Channel " + std::to_string(chan_idx);
  channel.sim_stats = new_stats;
  channel.warmup = warmup;

  for (auto* ul : queues) {
    channel_type::stats_type ul_new_roi_stats;
    channel_type::stats_type ul_new_sim_stats;
    ul->roi_stats = ul_new_roi_stats;
    ul->sim_stats = ul_new_sim_stats;
  }
}

void CXL_CHANNEL::begin_phase() {}

void CXL_CONTROLLER::end_phase(unsigned cpu)
{
  channel.end_phase(cpu);
}

void DRAM_CHANNEL::end_phase(unsigned /*cpu*/) { roi_stats = sim_stats; }

void CXL_CONTROLLER::print_deadlock() {
  int j = 0;
  fmt::print("DRAM Channel {}\n", j);
  channel.print_deadlock();
}

void DRAM_CHANNEL::print_deadlock() {
  std::string_view q_writer{"address: {} forward_checked: {} finished: {}"};
  auto q_entry_pack = [](const auto& entry) {
    return std::tuple{entry->address, entry->forward_checked, entry->finished};
  };

  champsim::range_print_deadlock(RQ, "RQ", q_writer, q_entry_pack);
  champsim::range_print_deadlock(WQ, "WQ", q_writer, q_entry_pack);
}

CXL_CHANNEL::request_type::request_type(const typename champsim::channel::request_type& req)
  : pf_metadata(req.pf_metadata), address(req.address), v_address(req.address),
    data(req.data), instr_depend_on_me(req.instr_depend_on_me) {
  asid[0] = req.asid[0];
  asid[1] = req.asid[1];
}
