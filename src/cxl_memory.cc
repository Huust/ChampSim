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
  progress += finish_dbus_request();
  progress += schedule_refresh();
  progress += populate_dbus();
  progress += service_packet(schedule_packet());

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
    *rq_it = DRAM_CHANNEL::request_type{pkt}; 
    rq_it->value().forward_checked = false;
    rq_it->value().finished = false;
    rq_it->value().ready_time = current_time;
    if (packet.response_requested)
      rq_it->value().to_return = {&ul->returned};

    return true; 
  }

  return false;
}

bool CXL_CONTROLLER::add_wq(const auto& pkt) {
  if (auto wq_it = std::find_if_not(std::begin(channel.WQ), std::end(channel.WQ), [this](const auto &pkt){ return pkt.has_value(); });
      wq_it != std::end(channel.WQ)) {
    *wq_it = DRAM_CHANNEL::request_type{pkt}; 
    wq_it->value().forward_checked = false;
    wq_it->value().finished = false;
    wq_it->value().ready_time = current_time;

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
  if (active_wr_req_on_bus != std::end(WQ) && active_wr_req_on_bus->value().ready_time <= current_time) {
    active_wr_req_on_bus->value().finished = true;
    active_wr_req_on_bus = std::end(RespQ);
    ++progress;
  } 

  return progress;
}

