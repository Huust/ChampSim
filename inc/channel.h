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

#ifndef CHANNEL_H
#define CHANNEL_H

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <string_view>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "champsim.h"

namespace champsim
{

struct cache_queue_stats {
  uint64_t RQ_ACCESS = 0;
  uint64_t RQ_MERGED = 0;
  uint64_t RQ_FULL = 0;
  uint64_t RQ_TO_CACHE = 0;
  uint64_t PQ_ACCESS = 0;
  uint64_t PQ_MERGED = 0;
  uint64_t PQ_FULL = 0;
  uint64_t PQ_TO_CACHE = 0;
  uint64_t WQ_ACCESS = 0;
  uint64_t WQ_MERGED = 0;
  uint64_t WQ_FULL = 0;
  uint64_t WQ_TO_CACHE = 0;
  uint64_t WQ_FORWARD = 0;
};

class channel
{
  // 在channel中定义这种结构体的作用是：在channel的成员变量中使用这些临时定义的类型
  struct request {
    bool forward_checked = false; // forward means check SQ before LOAD data from memory
    bool is_translated = true;
    bool response_requested = true; // whether this request needs response (need by some LOAD inst)

    uint8_t asid[2] = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};
    access_type type{access_type::LOAD};

    uint32_t pf_metadata = 0; // 通过位掩码标记prefetch的各种metadata
    uint32_t cpu = std::numeric_limits<uint32_t>::max();

    champsim::address address{};
    champsim::address v_address{};
    champsim::address data{};
    uint64_t instr_id = 0;
    champsim::address ip{};

    uint8_t page_size = 0; // 0=4K, 1=2M (matches PageSize enum)
    uint8_t entry_type = 0; // 0=TLB translation, 1=bitmap entry

    std::vector<uint64_t> instr_depend_on_me{};
  };

  struct response {
    champsim::address address{};
    champsim::address v_address{};
    champsim::address data{};
    uint32_t pf_metadata = 0;
    std::vector<uint64_t> instr_depend_on_me{};
    bool is_llc_miss = false; // the request response is caused by llc miss
                              // this flag is only set when returning responses from shim layer to upper level LLC
    bool is_cxl_memory = false; // the request was served by CXL memory (vs DRAM)
                                // this flag is only set when returning responses from shim layer to upper level LLC
    uint8_t page_size = 0; // 0=4K, 1=2M (matches PageSize enum)
    uint8_t entry_type = 0; // 0=TLB translation, 1=bitmap entry
    champsim::page_number perf_2m_ppage{}; // PERF cold path: 2MB PPN for STLB template install

    response(champsim::address addr, champsim::address v_addr, champsim::address data_, uint32_t pf_meta, std::vector<uint64_t> deps)
        : address(addr), v_address(v_addr), data(data_), pf_metadata(pf_meta), instr_depend_on_me(deps)
    {
    }
    response(champsim::address addr, champsim::address v_addr, champsim::address data_, uint32_t pf_meta, std::vector<uint64_t> deps, uint8_t ps)
        : address(addr), v_address(v_addr), data(data_), pf_metadata(pf_meta), instr_depend_on_me(deps), page_size(ps)
    {
    }
    response(champsim::address addr, champsim::address v_addr, champsim::address data_, uint32_t pf_meta, std::vector<uint64_t> deps, uint8_t ps, uint8_t et)
        : address(addr), v_address(v_addr), data(data_), pf_metadata(pf_meta), instr_depend_on_me(deps), page_size(ps), entry_type(et)
    {
    }
    explicit response(request req) : response(req.address, req.v_address, req.data, req.pf_metadata, req.instr_depend_on_me, req.page_size) { entry_type = req.entry_type; }
  };

  template <typename R>
  bool do_add_queue(R& queue, std::size_t queue_size, const typename R::value_type& packet);

  std::size_t RQ_SIZE = std::numeric_limits<std::size_t>::max();
  std::size_t PQ_SIZE = std::numeric_limits<std::size_t>::max();
  std::size_t WQ_SIZE = std::numeric_limits<std::size_t>::max();
  champsim::data::bits OFFSET_BITS{};
  bool match_offset_bits = false;

public:
  using response_type = response;
  using request_type = request;
  using stats_type = cache_queue_stats;

  std::deque<request_type> RQ{}, PQ{}, WQ{};
  std::deque<response_type> returned{};

  stats_type sim_stats{}, roi_stats{};

  channel() = default;
  // offset_bits表示寻址时offset占用地址的多少位
  channel(std::size_t rq_size, std::size_t pq_size, std::size_t wq_size, champsim::data::bits offset_bits, bool match_offset);

  bool add_rq(const request_type& packet);  // read request / queue
  bool add_wq(const request_type& packet);  // write request / queue
  bool add_pq(const request_type& packet);  // prefetch request / queue

  [[nodiscard]] std::size_t rq_occupancy() const;
  [[nodiscard]] std::size_t wq_occupancy() const;
  [[nodiscard]] std::size_t pq_occupancy() const;

  [[nodiscard]] std::size_t rq_size() const;
  [[nodiscard]] std::size_t wq_size() const;
  [[nodiscard]] std::size_t pq_size() const;

  void check_collision();
};
} // namespace champsim

#endif
