#include "cxl_memory.h"

#include <algorithm>
#include <cfenv>
#include <fmt/core.h>
#include <iterator>

#include "chrono.h"
#include "operable.h"
#include "deadlock.h"
#include "util/bits.h"
#include "util/span.h"

CXL_CONTROLLER::CXL_CONTROLLER(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl,
                               std::vector<channel_type*>&& ul, std::size_t rq_size, std::size_t wq_size, std::size_t respq_size,
                               champsim::data::bytes chan_width, double rx_bw, double tx_bw, champsim::channel *ll)
  : champsim::operable(cxl_io_period), queues(ul), channel_width(chan_width), rx_bw(rx_bw), tx_bw(tx_bw),
    channel(cxl_io_period, t_cxl, rq_size, wq_size, respq_size, rx_bw, tx_bw, ll, chan_width, std::move(ul))
{
}

CXL_CHANNEL::CXL_CHANNEL(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl, std::size_t rq_size, std::size_t wq_size, std::size_t respq_size,
                         double rx_bw, double tx_bw, champsim::channel *ll, champsim::data::bytes width, std::vector<champsim::channel*>&& ul)
  : champsim::operable(cxl_io_period), WQ(wq_size), RQ(rq_size), RespQ(respq_size), lower_level(ll), channel_width(width),
    tCXL(cxl_io_period * t_cxl), tRD((int)(1000 * BLOCK_SIZE / (rx_bw * (int)width.count() / 8))),
    tWR((int)(1000 * BLOCK_SIZE / (tx_bw * (int)width.count() / 8)))
    // rx_bw and tx_bw is based on GT/s, width is based on bytes, tWR and tRD (clock::duration) is based on picoseconds
{
  // Backup return queue for CXL
  for (const auto& item: ul) {
    upper_returns.push_back(&(item->returned));
  }
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
    return progress;
  }

  // Collision detection is handled by lower-level MEMORY_CONTROLLER
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

bool CXL_CONTROLLER::add_rq(const request_type& pkt, channel_type* ul) {
  // TODO: In future we may have more than one cxl channel,
  // so here leaves room for addressing corresponding cxl channel
  if (auto rq_it = std::find_if_not(std::begin(channel.RQ), std::end(channel.RQ), [](const auto &pkt){ return pkt.has_value(); });
      rq_it != std::end(channel.RQ)) {
    *rq_it = CXL_CHANNEL::request_type{pkt}; 
    rq_it->value().forward_checked = false;
    rq_it->value().finished = false;
    rq_it->value().ready_time = current_time + channel.tCXL;
    if (pkt.response_requested)
      rq_it->value().to_return = {&ul->returned};

    return true; 
  }

  return false;
}

bool CXL_CONTROLLER::add_wq(const request_type& pkt) {
  if (auto wq_it = std::find_if_not(std::begin(channel.WQ), std::end(channel.WQ), [](const auto &pkt){ return pkt.has_value(); });
      wq_it != std::end(channel.WQ)) {
    *wq_it = CXL_CHANNEL::request_type{pkt};
    wq_it->value().forward_checked = false;
    wq_it->value().finished = false;
    wq_it->value().ready_time = current_time + channel.tCXL;

    return true;
  }

  return false;
}

void CXL_CHANNEL::check_collision()
{
  auto equal_address = [](champsim::address a, champsim::address b) {
    // collision if everything but offset matches
    // TODO: Offset is 6 bits?
    champsim::data::bits offset_bits = champsim::data::bits{6};
    return a.slice_upper(offset_bits) == b.slice_upper(offset_bits);
  };
  
  // Write Collision
  for (auto wq_it = std::begin(WQ); wq_it != std::end(WQ); ++wq_it) {
    auto checker = [wq_it, equal_address](const auto& pkt) {
          return pkt.has_value() && equal_address(wq_it->value().address, pkt.value().address);
    };

    if (wq_it->has_value() && !wq_it->value().forward_checked) {
      auto found = std::find_if(std::begin(WQ), wq_it, checker); // Forward check
      if (found == wq_it) {
        found = std::find_if(std::next(wq_it), std::end(WQ), checker); // Backward check
      }

      // TODO: What if the found is before the wq_it?
      // If so you need to reset the found one, not the wq_it
      if (found != std::end(WQ)) {
        wq_it->reset();
      } else {
        wq_it->value().forward_checked = true;
      }
    }
  }

  // Read Collision
  for (auto rq_it = std::begin(RQ); rq_it != std::end(RQ); ++rq_it) {
    auto checker = [rq_it, equal_address](const auto& pkt) {
          return pkt.has_value() && equal_address(rq_it->value().address, pkt.value().address);
    };

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
  progress += handle_responses();
  // populate new responses from dram into cxl controller
  progress += populate_responses();
  // finish write request transfer 
  progress += handle_writes();
  // put read requests into lower level
  progress += handle_reads();

  sim_stats.total_operating_cycles++;

  return progress;
}

long CXL_CHANNEL::handle_responses() {
  long progress{0};

  if (active_rd_resp_on_bus != std::end(RespQ)) {
    if (active_rd_resp_on_bus->value().ready_time <= current_time) {
      response_type response{active_rd_resp_on_bus->value().address, active_rd_resp_on_bus->value().v_address, active_rd_resp_on_bus->value().data,
                             active_rd_resp_on_bus->value().pf_metadata, active_rd_resp_on_bus->value().instr_depend_on_me};

      for (auto* ret : active_rd_resp_on_bus->value().to_return) {
        ret->push_back(response);
      }
      
      active_rd_resp_on_bus->reset();
      active_rd_resp_on_bus = std::end(RespQ);
      ++progress;
    } else {
      // Some response is transferring over the PCIe bus, but not yet finished
      return progress;
    }
  }

  auto schedule_next_response = [this](const auto &lhs, const auto &rhs) {
    if (!lhs.has_value() || lhs.value().ready_time > current_time)
      return false;
    if (!rhs.has_value() || rhs.value().ready_time > current_time)
      return true;

    return lhs.value().ready_time < rhs.value().ready_time;
  };

  if (active_rd_resp_on_bus = std::min_element(std::begin(RespQ), std::end(RespQ), schedule_next_response);
      active_rd_resp_on_bus != std::end(RespQ) && active_rd_resp_on_bus->has_value() &&
      active_rd_resp_on_bus->value().ready_time <= current_time) {
    active_rd_resp_on_bus->value().ready_time = current_time + tRD;
    ++progress;
  } else {
    active_rd_resp_on_bus = std::end(RespQ);
  }
  
  // Update read bus utilization statistics
  if (!warmup && active_rd_resp_on_bus != std::end(RespQ)) {
    sim_stats.bus_cycles_rd_busy++;
  }

  return progress;
}

// Populate responses from lower level channel into CXL_CHANNEL's internal RespQ
// In our implementation, we have limited buffer for RespQ
long CXL_CHANNEL::populate_responses() {
  long progress{0};
  
  auto populate = [this](const auto &resp) {
    auto slot = std::find_if_not(std::begin(this->RespQ), std::end(this->RespQ), [](const auto &resp){ return resp.has_value(); });
    
    if (slot != std::end(this->RespQ)) {
      // Common trap: direct field assignment on nullopt optional doesn't make it has_value
      // Correct approach: use emplace() to construct and assign
      slot->emplace();  // Make optional have value
      (*slot)->pf_metadata = resp.pf_metadata;
      (*slot)->address = resp.address;
      (*slot)->v_address = resp.v_address;
      (*slot)->data = resp.data;
      (*slot)->instr_depend_on_me = resp.instr_depend_on_me;
      (*slot)->ready_time = current_time + tCXL;
      (*slot)->to_return = upper_returns;

      return true;
    }
    
    return false;
  };

  auto complete_end = std::find_if_not(std::begin(lower_level->returned), std::end(lower_level->returned), populate);
  progress += std::distance(std::begin(lower_level->returned), complete_end);
  lower_level->returned.erase(std::begin(lower_level->returned), complete_end);

  return progress;
}

// 1. If a request is on write bus and satisfies ready_time, we move it from dbus into channel between cxl controller and dram 
// 2. Then we choose another write request (which is already in WQ and goes through tCXL latency) and set its ready_time as current_time + tWR
long CXL_CHANNEL::handle_writes() {
  long progress{0};

  if (active_wr_req_on_bus != std::end(WQ) && active_wr_req_on_bus->value().ready_time <= current_time) {
    lower_level->add_wq(active_wr_req_on_bus->value().raw_req);
    active_wr_req_on_bus->reset();
    active_wr_req_on_bus = std::end(WQ);
    ++progress;
  }

  if (active_wr_req_on_bus == std::end(WQ)) {
    // find a suitable write request in WQ and put it on the bus
    // rule: has_value(), FCFS, tCXL satisfied
    auto schedule_next_write = [this](auto const &lhs, auto const &rhs) {
      if (!lhs.has_value() || lhs.value().ready_time > current_time)
        return false;
      if (!rhs.has_value() || rhs.value().ready_time > current_time)
        return true;

      return lhs.value().ready_time < rhs.value().ready_time;
    }; 

    if (active_wr_req_on_bus = std::min_element(std::begin(WQ), std::end(WQ), schedule_next_write);
        active_wr_req_on_bus != std::end(WQ) && active_wr_req_on_bus->has_value() &&
        active_wr_req_on_bus->value().ready_time <= current_time) {
      active_wr_req_on_bus->value().ready_time = current_time + tWR;
      ++progress;
    } else {
      active_wr_req_on_bus = std::end(WQ);
    }
    
    // Update write bus utilization statistics
    if (!warmup && active_wr_req_on_bus != std::end(WQ)) {
      sim_stats.bus_cycles_wr_busy++;
    }
  }

  return progress;
}

// Each cycle we push all satisfied (meet tCXL latency) read requests into lower_level's champsim channel
// Note: No explicit bandwidth limit for read requests - this is by design because:
// 1. Read requests only carry address info (small payload), unlike write/response with full data blocks
// 2. PCIe/CXL bus can pipeline multiple read requests while waiting for responses  
// 3. Actual bandwidth limitation is handled by lower-level MEMORY_CONTROLLER through bank/timing constraints
// 4. This matches DRAM controller behavior which also sends reads without explicit bandwidth limits
long CXL_CHANNEL::handle_reads() {
  long progress{0};
  auto schedule_next_read = [this](const auto& lhs, const auto& rhs) {
    if (!lhs.has_value() || lhs.value().ready_time > current_time)
      return false;
    if (!rhs.has_value() || rhs.value().ready_time > current_time)
      return true;

    return lhs.value().ready_time < rhs.value().ready_time;
  };

  while (1) {
    if (queue_type::iterator iter_next_to_read = std::min_element(std::begin(RQ), std::end(RQ), schedule_next_read);
        iter_next_to_read != std::end(RQ) && iter_next_to_read->has_value() && iter_next_to_read->value().ready_time <= current_time) {
      lower_level->add_rq(iter_next_to_read->value().raw_req);
      iter_next_to_read->reset();
      iter_next_to_read = std::end(RQ);   // not necessary
    } else break;

    ++progress;
  }

  return progress == 0 ? 1 : 0; 
}


// Inherit from operable
// cxl controller needs to print its' dram size, like what memory controller do
void CXL_CONTROLLER::initialize() {
  fmt::print("CXL MEMORY: {} channel, {:.1f} GB/s RX bandwidth, {:.1f} GB/s TX bandwidth\n", 
             1, rx_bw, tx_bw);
}

void CXL_CHANNEL::initialize() {}

void CXL_CONTROLLER::begin_phase()
{
  CXL_CONTROLLER::stats_type new_roi_stats;
  CXL_CONTROLLER::stats_type new_sim_stats;
  new_roi_stats.name = "CXL_CONTROLLER";
  new_sim_stats.name = "CXL_CONTROLLER";
  this->roi_stats = new_roi_stats;
  this->sim_stats = new_sim_stats;

  for (auto* ul : queues) {
    channel_type::stats_type ul_new_roi_stats;
    channel_type::stats_type ul_new_sim_stats;
    ul->roi_stats = ul_new_roi_stats;
    ul->sim_stats = ul_new_sim_stats;
  }

  channel.warmup = warmup;
  channel.begin_phase();
}

void CXL_CHANNEL::begin_phase() {
  CXL_CHANNEL::channel_stats_type new_roi_stats;
  CXL_CHANNEL::channel_stats_type new_sim_stats;
  new_roi_stats.name = "CXL_CHANNEL";
  new_sim_stats.name = "CXL_CHANNEL";
  this->roi_stats = new_roi_stats;
  this->sim_stats = new_sim_stats;
}

void CXL_CONTROLLER::end_phase(unsigned cpu)
{
  // Aggregate channel statistics into controller statistics
  sim_stats.bus_cycles_rd_busy = channel.sim_stats.bus_cycles_rd_busy;
  sim_stats.bus_cycles_wr_busy = channel.sim_stats.bus_cycles_wr_busy;
  sim_stats.total_operating_cycles = channel.sim_stats.total_operating_cycles;
  
  roi_stats = sim_stats;
  channel.end_phase(cpu);
}

void CXL_CHANNEL::end_phase(unsigned /*cpu*/) { 
  roi_stats = sim_stats; 
}

void CXL_CONTROLLER::print_deadlock() {
  fmt::print("CXL Channel 0\n");
  channel.print_deadlock();
}

void CXL_CHANNEL::print_deadlock() {
  std::string_view q_writer{"address: {:#x} finished: {}"};

  auto q_entry_pack = [](const auto& entry) {
    return std::tuple{entry->address, entry->finished};
  };

  champsim::range_print_deadlock(RQ, "CHANNEL_RQ", q_writer, q_entry_pack);
  champsim::range_print_deadlock(WQ, "CHANNEL_WQ", q_writer, q_entry_pack);
}

CXL_CHANNEL::request_type::request_type(const typename champsim::channel::request_type& req)
  : pf_metadata(req.pf_metadata), address(req.address), v_address(req.address),
    data(req.data), instr_depend_on_me(req.instr_depend_on_me) {
  asid[0] = req.asid[0];
  asid[1] = req.asid[1];
  
  raw_req = req;
}
