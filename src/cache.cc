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

#include "cache.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <unordered_set>
#include <fmt/core.h>
#include <iostream>
#include <sstream>

#include "access_type.h"
#include "bandwidth.h"
#include "champsim.h"
#include "chrono.h"
#include "deadlock.h"
#include "heatmap.h"
#include "instruction.h"
#include "vmem.h"
#include "util/algorithm.h"
#include "util/bits.h"
#include "util/span.h"

namespace {
// Compute STLB address for 2MB-aligned VPN.
// 2MB-aligned VPNs have lower 9 bits = 0, so they all collide in STLB set 0.
// Shift right by 9 to place the region index in the set-index bit range.
// Set a high bit (bit 40 of page_number) to prevent tag collisions with 4KB entries
// whose VPN might equal region_index. Normal VPNs use at most ~36 bits, so bit 40
// is safely outside the real VPN range.
constexpr uint64_t STLB_2M_TAG_MARKER = 1ULL << 40;
inline champsim::address stlb_addr_2m(uint64_t base_2m) {
  return champsim::address{champsim::page_number{(base_2m >> 9) | STLB_2M_TAG_MARKER}};
}
} // namespace

CACHE::CACHE(CACHE&& other)
    : operable(other),

      upper_levels(std::move(other.upper_levels)), lower_level(std::move(other.lower_level)), lower_translate(std::move(other.lower_translate)),
      lower_translate_2m(std::move(other.lower_translate_2m)),

      cpu(other.cpu), NAME(std::move(other.NAME)), NUM_SET(other.NUM_SET), NUM_WAY(other.NUM_WAY), MSHR_SIZE(other.MSHR_SIZE), PQ_SIZE(other.PQ_SIZE),
      HIT_LATENCY(other.HIT_LATENCY), FILL_LATENCY(other.FILL_LATENCY), OFFSET_BITS(other.OFFSET_BITS), block(std::move(other.block)), MAX_TAG(other.MAX_TAG),
      MAX_FILL(other.MAX_FILL), prefetch_as_load(other.prefetch_as_load), match_offset_bits(other.match_offset_bits), virtual_prefetch(other.virtual_prefetch),
      pref_activate_mask(std::move(other.pref_activate_mask)),

      sim_stats(std::move(other.sim_stats)), roi_stats(std::move(other.roi_stats)),

      pref_module_pimpl(std::move(other.pref_module_pimpl)), repl_module_pimpl(std::move(other.repl_module_pimpl))
{
  pref_module_pimpl->bind(this);
  repl_module_pimpl->bind(this);
}

auto CACHE::operator=(CACHE&& other) -> CACHE&
{
  this->clock_period = other.clock_period;
  this->current_time = other.current_time;
  this->warmup = other.warmup;

  this->upper_levels = std::move(other.upper_levels);
  this->lower_level = std::move(other.lower_level);
  this->lower_translate = std::move(other.lower_translate);
  this->lower_translate_2m = std::move(other.lower_translate_2m);

  this->cpu = other.cpu;
  this->NAME = std::move(other.NAME);
  this->NUM_SET = other.NUM_SET;
  this->NUM_WAY = other.NUM_WAY;
  ;
  this->MSHR_SIZE = other.MSHR_SIZE;
  ;
  this->PQ_SIZE = other.PQ_SIZE;
  this->HIT_LATENCY = other.HIT_LATENCY;
  this->FILL_LATENCY = other.FILL_LATENCY;
  this->OFFSET_BITS = other.OFFSET_BITS;
  ;
  this->block = std::move(other.block);
  this->MAX_TAG = other.MAX_TAG;
  this->MAX_FILL = other.MAX_FILL;
  this->prefetch_as_load = other.prefetch_as_load;
  this->match_offset_bits = other.match_offset_bits;
  this->virtual_prefetch = other.virtual_prefetch;
  this->pref_activate_mask = std::move(other.pref_activate_mask);

  this->sim_stats = std::move(other.sim_stats);
  this->roi_stats = std::move(other.roi_stats);

  this->pref_module_pimpl = std::move(other.pref_module_pimpl);
  this->repl_module_pimpl = std::move(other.repl_module_pimpl);

  pref_module_pimpl->bind(this);
  repl_module_pimpl->bind(this);

  return *this;
}

CACHE::tag_lookup_type::tag_lookup_type(const request_type& req, bool local_pref, bool skip)
    : address(req.address), v_address(req.v_address), data(req.data), ip(req.ip), instr_id(req.instr_id), pf_metadata(req.pf_metadata), cpu(req.cpu),
      type(req.type), prefetch_from_this(local_pref), skip_fill(skip), is_translated(req.is_translated), page_size(req.page_size),
      entry_type(req.entry_type), instr_depend_on_me(req.instr_depend_on_me)
{
}

CACHE::mshr_type::mshr_type(const tag_lookup_type& req, champsim::chrono::clock::time_point _time_enqueued)
    : address(req.address), v_address(req.v_address), ip(req.ip), instr_id(req.instr_id), page_size(req.page_size), entry_type(req.entry_type), cpu(req.cpu), type(req.type),
      prefetch_from_this(req.prefetch_from_this), time_enqueued(_time_enqueued), instr_depend_on_me(req.instr_depend_on_me), to_return(req.to_return)
{
  // Set is_cxl_memory flag for per-cache DRAM/CXL miss statistics.
  // Heatmap path (--use-heatmap)
  if (champsim::heatmap::is_hotness_allocation_enabled()) {
    is_cxl_memory = !champsim::heatmap::is_fast_memory(champsim::page_number{v_address});
  }
  // Policy path (--use-policy)
  else if (g_vmem && g_vmem->use_policy) {
    is_cxl_memory = g_vmem->is_cxl_page(champsim::page_number{v_address});
  }
}

CACHE::mshr_type CACHE::mshr_type::merge(mshr_type predecessor, mshr_type successor)
{
  std::vector<uint64_t> merged_instr{};
  std::vector<std::deque<response_type>*> merged_return{};

  std::set_union(std::begin(predecessor.instr_depend_on_me), std::end(predecessor.instr_depend_on_me), std::begin(successor.instr_depend_on_me),
                 std::end(successor.instr_depend_on_me), std::back_inserter(merged_instr));
  std::set_union(std::begin(predecessor.to_return), std::end(predecessor.to_return), std::begin(successor.to_return), std::end(successor.to_return),
                 std::back_inserter(merged_return));

  mshr_type retval{(successor.type == access_type::PREFETCH) ? predecessor : successor};

  // set the time enqueued to the predecessor unless its a demand into prefetch, in which case we use the successor
  retval.time_enqueued =
      ((successor.type != access_type::PREFETCH && predecessor.type == access_type::PREFETCH)) ? successor.time_enqueued : predecessor.time_enqueued;
  retval.instr_depend_on_me = merged_instr;
  retval.to_return = merged_return;
  retval.data_promise = predecessor.data_promise;

  if constexpr (champsim::debug_print) {
    if (successor.type == access_type::PREFETCH) {
      fmt::print("[MSHR] {} address {} type: {} into address {} type: {}\n", __func__, successor.address,
                 access_type_names.at(champsim::to_underlying(successor.type)), predecessor.address,
                 access_type_names.at(champsim::to_underlying(successor.type)));
    } else {
      fmt::print("[MSHR] {} address {} type: {} into address {} type: {}\n", __func__, predecessor.address,
                 access_type_names.at(champsim::to_underlying(predecessor.type)), successor.address,
                 access_type_names.at(champsim::to_underlying(successor.type)));
    }
  }

  return retval;
}

auto CACHE::fill_block(mshr_type mshr, uint32_t metadata) -> BLOCK
{
  CACHE::BLOCK to_fill;
  to_fill.valid = true;
  to_fill.prefetch = mshr.prefetch_from_this;
  to_fill.dirty = (mshr.type == access_type::WRITE);
  to_fill.address = mshr.address;
  to_fill.v_address = mshr.v_address;
  to_fill.data = mshr.data_promise->data;
  to_fill.pf_metadata = metadata;
  to_fill.page_size = mshr.page_size;
  to_fill.entry_type = mshr.entry_type;

  return to_fill;
}

auto CACHE::matches_address(champsim::address addr, uint8_t etype) const
{
  return [match = addr.slice_upper(OFFSET_BITS), shamt = OFFSET_BITS, etype](const auto& entry) {
    return entry.address.slice_upper(shamt) == match && entry.entry_type == etype;
  };
}

template <typename T>
champsim::address CACHE::module_address(const T& element) const
{
  auto address = virtual_prefetch ? element.v_address : element.address;
  return champsim::address{address.slice_upper(match_offset_bits ? champsim::data::bits{} : OFFSET_BITS)};
}

bool CACHE::handle_fill(const mshr_type& fill_mshr)
{
  cpu = fill_mshr.cpu;

  // ── STLB 2MB/PERF: store at 2MB-aligned address for secondary matching ──
  const bool is_stlb = (g_vmem && NAME.find("STLB") != std::string::npos);
  const bool is_stlb_2m = (is_stlb && (fill_mshr.page_size == static_cast<uint8_t>(PageSize::PAGE_2M)
                                       || fill_mshr.page_size == static_cast<uint8_t>(PageSize::PAGE_IDEAL_PERF)));
  const bool is_stlb_perf = (is_stlb && fill_mshr.page_size == static_cast<uint8_t>(PageSize::PAGE_PERF));
  champsim::address fill_address = fill_mshr.address;
  if (is_stlb_2m || is_stlb_perf) {
    uint64_t vpn_4k = champsim::page_number{fill_mshr.v_address}.to<uint64_t>();
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    fill_address = stlb_addr_2m(base_2m);
  }

  // find victim
  auto [set_begin, set_end] = get_set_span(fill_address);
  auto way = std::find_if_not(set_begin, set_end, [](auto x) { return x.valid; });
  if (way == set_end) {
    way = std::next(set_begin, impl_find_victim(fill_mshr.cpu, fill_mshr.instr_id, get_set_index(fill_address), &*set_begin, fill_mshr.ip,
                                                fill_address, fill_mshr.type));
  }
  assert(set_begin <= way);
  assert(way <= set_end);
  assert(way != set_end || fill_mshr.type != access_type::WRITE); // Writes may not bypass
  const auto way_idx = std::distance(set_begin, way);             // cast protected by earlier assertion

  if constexpr (champsim::debug_print) {
    fmt::print("[{}] {} instr_id: {} address: {} v_address: {} set: {} way: {} type: {} prefetch_metadata: {} cycle_enqueued: {} cycle: {}\n", NAME, __func__,
               fill_mshr.instr_id, fill_address, fill_mshr.v_address, get_set_index(fill_address), way_idx,
               access_type_names.at(champsim::to_underlying(fill_mshr.type)), fill_mshr.data_promise->pf_metadata,
               (fill_mshr.time_enqueued.time_since_epoch()) / clock_period, (current_time.time_since_epoch()) / clock_period);
  }

  if (way != set_end && way->valid && way->dirty) {
    request_type writeback_packet;

    writeback_packet.cpu = fill_mshr.cpu;
    writeback_packet.address = way->address;
    writeback_packet.data = way->data;
    writeback_packet.instr_id = fill_mshr.instr_id;
    writeback_packet.ip = champsim::address{};
    writeback_packet.type = access_type::WRITE;
    writeback_packet.pf_metadata = way->pf_metadata;
    writeback_packet.response_requested = false;

    if constexpr (champsim::debug_print) {
      fmt::print("[{}] {} evict address: {:#x} v_address: {:#x} prefetch_metadata: {}\n", NAME, __func__, writeback_packet.address, writeback_packet.v_address,
                 fill_mshr.data_promise->pf_metadata);
    }

    auto success = lower_level->add_wq(writeback_packet);
    if (!success) {
      return false;
    }
  }

  champsim::address evicting_address{};
  if (way != set_end && way->valid) {
    evicting_address = module_address(*way);
  }

  auto metadata_thru = impl_prefetcher_cache_fill(module_address(fill_mshr), get_set_index(fill_address), way_idx,
                                                  (fill_mshr.type == access_type::PREFETCH), evicting_address, fill_mshr.data_promise->pf_metadata);
  impl_replacement_cache_fill(fill_mshr.cpu, get_set_index(fill_address), way_idx, module_address(fill_mshr), fill_mshr.ip, evicting_address,
                              fill_mshr.type);

  if (way != set_end) {
    if (way->valid && way->prefetch) {
      ++sim_stats.pf_useless;
    }

    if (fill_mshr.type == access_type::PREFETCH) {
      ++sim_stats.pf_fill;
    }

    if (is_stlb_2m || is_stlb_perf) {
      // Fill 2MB/PERF entry at 2MB-aligned address (for secondary matching)
      mshr_type fill_copy = fill_mshr;
      fill_copy.address = fill_address;
      *way = fill_block(fill_copy, metadata_thru);
    } else {
      // For non-STLB caches: PAGE_PERF → PAGE_4K, PAGE_IDEAL_PERF → PAGE_2M.
      if (fill_mshr.page_size == static_cast<uint8_t>(PageSize::PAGE_PERF)) {
        mshr_type fill_copy = fill_mshr;
        fill_copy.page_size = static_cast<uint8_t>(PageSize::PAGE_4K);
        *way = fill_block(fill_copy, metadata_thru);
      } else if (fill_mshr.page_size == static_cast<uint8_t>(PageSize::PAGE_IDEAL_PERF)) {
        mshr_type fill_copy = fill_mshr;
        fill_copy.page_size = static_cast<uint8_t>(PageSize::PAGE_2M);
        *way = fill_block(fill_copy, metadata_thru);
      } else {
        *way = fill_block(fill_mshr, metadata_thru);
      }
    }
  }

  // COLLECT STATS
  if (fill_mshr.type != access_type::PREFETCH) {
    long miss_latency = (current_time - (fill_mshr.time_enqueued + clock_period)) / clock_period;
    sim_stats.total_miss_latency_cycles += miss_latency;

    // Track CXL/DRAM split latency (count is already tracked in handle_miss/handle_write)
    if (fill_mshr.is_cxl_memory) {
      sim_stats.total_miss_latency_cycles_cxl += miss_latency;
    } else {
      sim_stats.total_miss_latency_cycles_dram += miss_latency;
    }
  }
  sim_stats.mshr_return.increment(std::pair{fill_mshr.type, fill_mshr.cpu});

  // ── STLB 2MB: synthesize 4KB response from 2MB fill ──
  if (is_stlb_2m) {
    champsim::page_number p_page_2m{fill_mshr.data_promise->data};
    champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
    champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
    auto synth_paddr = champsim::address{champsim::splice(
        champsim::address_slice{pn_2m, champsim::address{p_page_2m}},
        champsim::address_slice{off_2m, fill_mshr.v_address})};
    response_type response{fill_mshr.address, fill_mshr.v_address, synth_paddr, metadata_thru,
                           fill_mshr.instr_depend_on_me, static_cast<uint8_t>(PageSize::PAGE_2M), 0};
    response.is_llc_miss = fill_mshr.is_llc_miss;
    response.is_cxl_memory = fill_mshr.is_cxl_memory;
    for (auto* ret : fill_mshr.to_return) ret->push_back(response);
    return true;
  }

  // ── STLB PERF: handle template fill and bitmap fill ──
  if (is_stlb_perf) {
    uint64_t vpn_4k = champsim::page_number{fill_mshr.v_address}.to<uint64_t>();
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    champsim::page_number p_page_2m{fill_mshr.data_promise->data};

    if (fill_mshr.entry_type == 1) {
      // ── Bitmap fill: classify all waiting subpages for this 2MB region ──
      // Track bitmap fetch latency (time from MSHR enqueue to fill)
      if (!this->warmup) {
        long bm_latency = (current_time - (fill_mshr.time_enqueued + clock_period)) / clock_period;
        sim_stats.perf_bitmap_miss_latency_cycles += bm_latency;
      }
      constexpr uint64_t mask_2m = ~uint64_t{(1ULL << 9) - 1};
      champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
      champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};

      for (auto it = perf_bitmap_waiting.begin(); it != perf_bitmap_waiting.end(); ) {
        uint64_t wait_vpn = champsim::page_number{it->v_address}.to<uint64_t>();
        uint64_t wait_base = wait_vpn & mask_2m;
        if (wait_base != base_2m) { ++it; continue; }

        if (!g_vmem->is_hole(wait_vpn)) {
          // Not hole → synthesize 4KB from saved 2MB PPN, respond
          auto synth_paddr = champsim::address{champsim::splice(
              champsim::address_slice{pn_2m, champsim::address{it->saved_2m_ppage}},
              champsim::address_slice{off_2m, it->v_address})};
          if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_non_hole++; }
          response_type response{it->address, it->v_address, synth_paddr, metadata_thru,
                                 it->instr_depend_on_me, static_cast<uint8_t>(PageSize::PAGE_4K), 0};
          for (auto* ret : it->to_return) ret->push_back(response);
        } else {
          // Hole → queue for 4KB PTW
          if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_hole++; }
          perf_hole_pending.push_back(std::move(*it));
        }
        it = perf_bitmap_waiting.erase(it);
      }
      return true;
    }

    // ── PERF template fill (entry_type=0): classify requesting subpage ──
    if (!g_vmem->coarse_filter_pass(vpn_4k)) {
      // Coarse filter = 0: not hole → synthesize 4KB, respond immediately
      champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
      champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
      auto synth_paddr = champsim::address{champsim::splice(
          champsim::address_slice{pn_2m, champsim::address{p_page_2m}},
          champsim::address_slice{off_2m, fill_mshr.v_address})};
      if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_coarse_filtered++; }
      response_type response{fill_mshr.address, fill_mshr.v_address, synth_paddr, metadata_thru,
                             fill_mshr.instr_depend_on_me, static_cast<uint8_t>(PageSize::PAGE_4K), 0};
      response.is_llc_miss = fill_mshr.is_llc_miss;
      response.is_cxl_memory = fill_mshr.is_cxl_memory;
      for (auto* ret : fill_mshr.to_return) ret->push_back(response);
    } else {
      // Coarse filter = 1: need bitmap → queue for bitmap fetch (no response yet)
      mshr_type bm_waiting = fill_mshr;
      bm_waiting.saved_2m_ppage = p_page_2m;
      bm_waiting.data_promise = {}; // reset: this entry will be resolved later, not via this fill
      perf_bitmap_waiting.push_back(std::move(bm_waiting));
    }
    return true;
  }

  // For non-STLB caches, convert: PAGE_PERF → PAGE_4K, PAGE_IDEAL_PERF → PAGE_2M
  auto resp_ps = fill_mshr.page_size;
  if (resp_ps == static_cast<uint8_t>(PageSize::PAGE_PERF) && !is_stlb_perf)
    resp_ps = static_cast<uint8_t>(PageSize::PAGE_4K);
  if (resp_ps == static_cast<uint8_t>(PageSize::PAGE_IDEAL_PERF) && !is_stlb_2m)
    resp_ps = static_cast<uint8_t>(PageSize::PAGE_2M);

  response_type response{fill_mshr.address, fill_mshr.v_address, fill_mshr.data_promise->data, metadata_thru, fill_mshr.instr_depend_on_me, resp_ps, fill_mshr.entry_type};
  response.is_llc_miss = fill_mshr.is_llc_miss;
  response.is_cxl_memory = fill_mshr.is_cxl_memory;
  for (auto* ret : fill_mshr.to_return) {
    ret->push_back(response);
  }

  return true;
}

bool CACHE::try_hit(const tag_lookup_type& handle_pkt)
{
  cpu = handle_pkt.cpu;

  // access cache
  auto [set_begin, set_end] = get_set_span(handle_pkt.address);
  auto way = std::find_if(set_begin, set_end, [matcher = matches_address(handle_pkt.address, handle_pkt.entry_type)](const auto& x) { return x.valid && matcher(x); });
  const auto hit = (way != set_end);
  const auto useful_prefetch = (hit && way->prefetch && !handle_pkt.prefetch_from_this);

  if constexpr (champsim::debug_print) {
    fmt::print("[{}] {} instr_id: {} address: {} v_address: {} data: {} set: {} way: {} ({}) type: {} cycle: {}\n", NAME, __func__, handle_pkt.instr_id,
               handle_pkt.address, handle_pkt.v_address, handle_pkt.data, get_set_index(handle_pkt.address), std::distance(set_begin, way),
               hit ? "HIT" : "MISS", access_type_names.at(champsim::to_underlying(handle_pkt.type)), current_time.time_since_epoch() / clock_period);
  }

  auto metadata_thru = handle_pkt.pf_metadata;

  if (should_activate_prefetcher(handle_pkt)) {
    metadata_thru = impl_prefetcher_cache_operate(module_address(handle_pkt), 
                                                handle_pkt.ip, 
                                                hit, 
                                                useful_prefetch, 
                                                handle_pkt.type, 
                                                metadata_thru);
  }

  const auto way_idx = std::distance(set_begin, way);
  impl_update_replacement_state(handle_pkt.cpu, get_set_index(handle_pkt.address), way_idx, module_address(handle_pkt), handle_pkt.ip, {}, handle_pkt.type,
                                hit);

  if (hit) {
    sim_stats.hits.increment(std::pair{handle_pkt.type, handle_pkt.cpu});

    // ── STLB: if we hit a PERF template directly (base VPN == request VPN), classify ──
    if (way->page_size == static_cast<uint8_t>(PageSize::PAGE_PERF)
        && g_vmem && NAME.find("STLB") != std::string::npos) {
      uint64_t vpn_4k = champsim::page_number{handle_pkt.v_address}.to<uint64_t>();
      uint64_t base_2m = (vpn_4k >> 9) << 9;
      champsim::page_number p_page_2m{way->data};

      if (!g_vmem->coarse_filter_pass(vpn_4k)) {
        // Coarse filter = 0: definitely not hole → synthesize 4KB
        champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
        champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
        auto synth_paddr = champsim::address{champsim::splice(
            champsim::address_slice{pn_2m, champsim::address{p_page_2m}},
            champsim::address_slice{off_2m, handle_pkt.v_address})};
        if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_coarse_filtered++; }
        response_type response{handle_pkt.address, handle_pkt.v_address, synth_paddr, metadata_thru,
                               handle_pkt.instr_depend_on_me, static_cast<uint8_t>(PageSize::PAGE_4K), 0};
        for (auto* ret : handle_pkt.to_return) ret->push_back(response);
      } else {
        // Coarse filter = 1: need bitmap check. Look for cached bitmap in STLB.
        champsim::address bm_addr = stlb_addr_2m(base_2m);
        auto [bm_set_begin, bm_set_end] = get_set_span(bm_addr);
        auto bm_way = std::find_if(bm_set_begin, bm_set_end, [matcher = matches_address(bm_addr, 1)](const auto& x) {
          return x.valid && matcher(x);
        });
        if (bm_way != bm_set_end) {
          // Bitmap cached in STLB
          if (!this->warmup) sim_stats.perf_bitmap_stlb_hit++;
          if (!g_vmem->is_hole(vpn_4k)) {
            // Not hole → synthesize 4KB
            champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
            champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
            auto synth_paddr = champsim::address{champsim::splice(
                champsim::address_slice{pn_2m, champsim::address{p_page_2m}},
                champsim::address_slice{off_2m, handle_pkt.v_address})};
            if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_non_hole++; }
            response_type response{handle_pkt.address, handle_pkt.v_address, synth_paddr, metadata_thru,
                                   handle_pkt.instr_depend_on_me, static_cast<uint8_t>(PageSize::PAGE_4K), 0};
            for (auto* ret : handle_pkt.to_return) ret->push_back(response);
          } else {
            // Hole → return false so handle_miss routes to 4KB PTW
            if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_hole++; }
            return false;
          }
        } else {
          // Bitmap not cached → return false so handle_miss fetches bitmap
          if (!this->warmup) sim_stats.perf_bitmap_stlb_miss++;
          return false;
        }
      }
    } else {
      // Use stored entry's page_size (not request's) — handle normal TLB entries
      response_type response{handle_pkt.address, handle_pkt.v_address, way->data, metadata_thru, handle_pkt.instr_depend_on_me, way->page_size, way->entry_type};
      for (auto* ret : handle_pkt.to_return) {
        ret->push_back(response);
      }
    }

    way->dirty |= (handle_pkt.type == access_type::WRITE);

    // update prefetch stats and reset prefetch bit
    if (useful_prefetch) {
      ++sim_stats.pf_useful;
      way->prefetch = false;
    }
  }

  // ── STLB secondary 2MB matching for 2MB pages ──
  // 2MB entries are stored at 2MB-aligned VPN. Sub-page requests miss on the exact
  // 4KB tag, so we do a secondary lookup at the 2MB-aligned address.
  if (!hit && g_vmem
      && (handle_pkt.page_size == static_cast<uint8_t>(PageSize::PAGE_2M)
          || handle_pkt.page_size == static_cast<uint8_t>(PageSize::PAGE_IDEAL_PERF))
      && NAME.find("STLB") != std::string::npos) {
    uint64_t vpn_4k = champsim::page_number{handle_pkt.v_address}.to<uint64_t>();
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    champsim::address addr_2m = stlb_addr_2m(base_2m);

    auto [set_begin_2m, set_end_2m] = get_set_span(addr_2m);
    auto way_2m = std::find_if(set_begin_2m, set_end_2m, [matcher = matches_address(addr_2m)](const auto& x) {
      return x.valid && (x.page_size == static_cast<uint8_t>(PageSize::PAGE_2M)
                         || x.page_size == static_cast<uint8_t>(PageSize::PAGE_IDEAL_PERF)) && matcher(x);
    });

    if (way_2m != set_end_2m) {
      // Secondary hit: synthesize 4KB physical address from 2MB base PPN
      champsim::page_number p_page_2m{way_2m->data};
      champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
      champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
      auto synth_paddr = champsim::address{champsim::splice(
          champsim::address_slice{pn_2m, champsim::address{p_page_2m}},
          champsim::address_slice{off_2m, handle_pkt.v_address})};

      sim_stats.hits.increment(std::pair{handle_pkt.type, handle_pkt.cpu});
      // Preserve actual page_size so finish_translation can route IDEAL_PERF subpages
      response_type response{handle_pkt.address, handle_pkt.v_address, synth_paddr,
                             metadata_thru, handle_pkt.instr_depend_on_me,
                             way_2m->page_size, 0};
      for (auto* ret : handle_pkt.to_return) ret->push_back(response);
      return true;
    }
    // 2MB template not in STLB → fall through as regular miss
  }

  // ── STLB secondary 2MB matching for perforated pages ──
  // When a 4KB request for a PERF subpage misses in STLB on the exact 4KB tag,
  // try matching against the 2MB PERF template stored at the 2MB-aligned address.
  if (!hit && g_vmem
      && handle_pkt.page_size == static_cast<uint8_t>(PageSize::PAGE_PERF)
      && NAME.find("STLB") != std::string::npos) {
    uint64_t vpn_4k = champsim::page_number{handle_pkt.v_address}.to<uint64_t>();
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    champsim::address addr_2m = stlb_addr_2m(base_2m);

    auto [set_begin_2m, set_end_2m] = get_set_span(addr_2m);
    auto way_2m = std::find_if(set_begin_2m, set_end_2m, [matcher = matches_address(addr_2m)](const auto& x) {
      return x.valid && x.page_size == static_cast<uint8_t>(PageSize::PAGE_PERF) && matcher(x);
    });

    if (way_2m != set_end_2m) {
      // Secondary hit on PERF template.
      champsim::page_number p_page_2m{way_2m->data};

      if (!g_vmem->coarse_filter_pass(vpn_4k)) {
        // Coarse filter = 0: definitely not hole → synthesize 4KB
        champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
        champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
        auto synth_paddr = champsim::address{champsim::splice(
            champsim::address_slice{pn_2m, champsim::address{p_page_2m}},
            champsim::address_slice{off_2m, handle_pkt.v_address})};
        if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_coarse_filtered++; }

        sim_stats.hits.increment(std::pair{handle_pkt.type, handle_pkt.cpu});
        response_type response{handle_pkt.address, handle_pkt.v_address, synth_paddr,
                               metadata_thru, handle_pkt.instr_depend_on_me,
                               static_cast<uint8_t>(PageSize::PAGE_4K), 0};
        for (auto* ret : handle_pkt.to_return) ret->push_back(response);
        return true;
      }

      // Coarse filter = 1: check bitmap cache in STLB
      auto [bm_set_begin, bm_set_end] = get_set_span(addr_2m);
      auto bm_way = std::find_if(bm_set_begin, bm_set_end, [matcher = matches_address(addr_2m, 1)](const auto& x) {
        return x.valid && matcher(x);
      });

      if (bm_way != bm_set_end) {
        // Bitmap cached
        if (!this->warmup) sim_stats.perf_bitmap_stlb_hit++;
        if (!g_vmem->is_hole(vpn_4k)) {
          // Not hole → synthesize 4KB
          champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
          champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
          auto synth_paddr = champsim::address{champsim::splice(
              champsim::address_slice{pn_2m, champsim::address{p_page_2m}},
              champsim::address_slice{off_2m, handle_pkt.v_address})};
          if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_non_hole++; }

          sim_stats.hits.increment(std::pair{handle_pkt.type, handle_pkt.cpu});
          response_type response{handle_pkt.address, handle_pkt.v_address, synth_paddr,
                                 metadata_thru, handle_pkt.instr_depend_on_me,
                                 static_cast<uint8_t>(PageSize::PAGE_4K), 0};
          for (auto* ret : handle_pkt.to_return) ret->push_back(response);
          return true;
        } else {
          // Hole → return false so handle_miss routes to 4KB PTW
          if (!this->warmup) { sim_stats.perf_total++; sim_stats.perf_hole++; }
          return false;
        }
      } else {
        // Bitmap not cached → return false so handle_miss fetches bitmap
        if (!this->warmup) sim_stats.perf_bitmap_stlb_miss++;
        return false;
      }
    }
    // Template not in STLB → fall through as regular miss
  }

  return hit;
}

auto CACHE::mshr_and_forward_packet(const tag_lookup_type& handle_pkt) -> std::pair<mshr_type, request_type>
{
  mshr_type to_allocate{handle_pkt, current_time};

  request_type fwd_pkt;

  fwd_pkt.asid[0] = handle_pkt.asid[0];
  fwd_pkt.asid[1] = handle_pkt.asid[1];
  fwd_pkt.type = (handle_pkt.type == access_type::WRITE) ? access_type::RFO : handle_pkt.type;
  fwd_pkt.pf_metadata = handle_pkt.pf_metadata;
  fwd_pkt.cpu = handle_pkt.cpu;

  fwd_pkt.address = handle_pkt.address;
  fwd_pkt.v_address = handle_pkt.v_address;
  fwd_pkt.data = handle_pkt.data;
  fwd_pkt.instr_id = handle_pkt.instr_id;
  fwd_pkt.ip = handle_pkt.ip;

  fwd_pkt.instr_depend_on_me = handle_pkt.instr_depend_on_me;
  fwd_pkt.response_requested = (!handle_pkt.prefetch_from_this || !handle_pkt.skip_fill);
  fwd_pkt.page_size = handle_pkt.page_size;
  fwd_pkt.entry_type = handle_pkt.entry_type;

  return std::pair{std::move(to_allocate), std::move(fwd_pkt)};
}

bool CACHE::handle_miss(const tag_lookup_type& handle_pkt)
{
  if constexpr (champsim::debug_print) {
    fmt::print("[{}] {} instr_id: {} address: {} v_address: {} type: {} local_prefetch: {} cycle: {}\n", NAME, __func__, handle_pkt.instr_id,
               handle_pkt.address, handle_pkt.v_address, access_type_names.at(champsim::to_underlying(handle_pkt.type)), handle_pkt.prefetch_from_this,
               current_time.time_since_epoch() / clock_period);
  }

  // ── STLB PERF routing: bitmap fetch or hole 4KB PTW ──
  // try_hit returned false for a PERF page. Determine why and route accordingly.
  if (g_vmem && handle_pkt.page_size == static_cast<uint8_t>(PageSize::PAGE_PERF)
      && NAME.find("STLB") != std::string::npos) {
    uint64_t vpn_4k = champsim::page_number{handle_pkt.v_address}.to<uint64_t>();
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    champsim::address addr_2m = stlb_addr_2m(base_2m);

    // Check if PERF template exists in STLB
    auto [set_begin_2m, set_end_2m] = get_set_span(addr_2m);
    auto way_2m = std::find_if(set_begin_2m, set_end_2m, [matcher = matches_address(addr_2m)](const auto& x) {
      return x.valid && x.page_size == static_cast<uint8_t>(PageSize::PAGE_PERF) && matcher(x);
    });

    if (way_2m != set_end_2m) {
      // Template exists — try_hit failed because of bitmap miss or hole
      champsim::page_number p_page_2m{way_2m->data};

      // Check bitmap entry in STLB
      auto bm_way = std::find_if(set_begin_2m, set_end_2m, [matcher = matches_address(addr_2m, 1)](const auto& x) {
        return x.valid && matcher(x);
      });

      if (bm_way != set_end_2m && g_vmem->is_hole(vpn_4k)) {
        // Bitmap cached + hole → queue for 4KB PTW
        mshr_type hole_entry{handle_pkt, current_time};
        hole_entry.saved_2m_ppage = p_page_2m;
        perf_hole_pending.push_back(std::move(hole_entry));
        sim_stats.misses.increment(std::pair{handle_pkt.type, handle_pkt.cpu});
        return true;
      }

      if (bm_way == set_end_2m) {
        // Bitmap not cached → queue for bitmap fetch
        mshr_type bm_waiting{handle_pkt, current_time};
        bm_waiting.saved_2m_ppage = p_page_2m;
        bm_waiting.to_return = handle_pkt.to_return;
        perf_bitmap_waiting.push_back(std::move(bm_waiting));

        // Check if bitmap MSHR is already inflight
        auto existing_bm = std::find_if(std::begin(MSHR), std::end(MSHR), matches_address(addr_2m, 1));
        if (existing_bm != MSHR.end()) {
          // Bitmap fetch already inflight → just queue, don't issue again
          sim_stats.misses.increment(std::pair{handle_pkt.type, handle_pkt.cpu});
          return true;
        }

        // Issue bitmap fetch to PTW
        bool mshr_full = (MSHR.size() == MSHR_SIZE);
        if (mshr_full) {
          sim_stats.mshr_congestion_cycles++;
          perf_bitmap_waiting.pop_back(); // undo the queue add
          return false;
        }

        request_type bm_req;
        bm_req.asid[0] = handle_pkt.asid[0];
        bm_req.asid[1] = handle_pkt.asid[1];
        bm_req.type = access_type::LOAD;
        bm_req.cpu = handle_pkt.cpu;
        bm_req.address = addr_2m; // 2MB-aligned VPN
        bm_req.v_address = champsim::address{champsim::page_number{base_2m}}; // for PTW to get bitmap paddr
        bm_req.instr_id = handle_pkt.instr_id;
        bm_req.ip = handle_pkt.ip;
        bm_req.entry_type = 1;
        bm_req.page_size = static_cast<uint8_t>(PageSize::PAGE_PERF);
        bm_req.response_requested = true;
        bm_req.is_translated = true;

        if (!lower_level->add_rq(bm_req)) {
          perf_bitmap_waiting.pop_back();
          return false;
        }

        // Create bitmap MSHR entry
        mshr_type bm_mshr{handle_pkt, current_time};
        bm_mshr.address = addr_2m;
        bm_mshr.entry_type = 1;
        bm_mshr.page_size = static_cast<uint8_t>(PageSize::PAGE_PERF);
        bm_mshr.saved_2m_ppage = p_page_2m;
        bm_mshr.to_return = {}; // no direct return; responses go through perf_bitmap_waiting
        MSHR.push_back(std::move(bm_mshr));

        sim_stats.misses.increment(std::pair{handle_pkt.type, handle_pkt.cpu});
        return true;
      }
    }
    // Template not in STLB → fall through to normal PTW path
  }

  mshr_type to_allocate{handle_pkt, current_time};

  cpu = handle_pkt.cpu;

  auto mshr_pkt = mshr_and_forward_packet(handle_pkt);

  // check mshr
  auto mshr_entry = std::find_if(std::begin(MSHR), std::end(MSHR), matches_address(handle_pkt.address, handle_pkt.entry_type));
  bool mshr_full = (MSHR.size() == MSHR_SIZE);

  if (mshr_entry != MSHR.end()) // miss already inflight
  {
    if (mshr_entry->type == access_type::PREFETCH && handle_pkt.type != access_type::PREFETCH) {
      // Mark the prefetch as useful
      if (mshr_entry->prefetch_from_this) {
        ++sim_stats.pf_useful;
      }
    }

    // COLLECT STATS
    sim_stats.mshr_merge.increment(std::pair{to_allocate.type, to_allocate.cpu});

    // Track CXL/DRAM split MSHR merge statistics
    if (to_allocate.is_cxl_memory) {
      sim_stats.mshr_merge_to_cxl++;
    } else {
      sim_stats.mshr_merge_to_dram++;
    }

    *mshr_entry = mshr_type::merge(*mshr_entry, to_allocate);
  } else {
    if (mshr_full) { // not enough MSHR resource
      // Track MSHR congestion cycles
      sim_stats.mshr_congestion_cycles++;
      return false;  // TODO should we allow prefetches anyway if they will not be filled to this level?
    }

    const bool send_to_rq = (prefetch_as_load || handle_pkt.type != access_type::PREFETCH);
    bool success = send_to_rq ? lower_level->add_rq(mshr_pkt.second) : lower_level->add_pq(mshr_pkt.second);

    if (!success) {
      return false;
    }

    // Allocate an MSHR
    if (mshr_pkt.second.response_requested) {
      MSHR.emplace_back(std::move(mshr_pkt.first));
    }
  }

  // Track CXL/DRAM split statistics (count all misses by memory type, including MSHR merges)
  if (to_allocate.is_cxl_memory) {
    sim_stats.misses_to_cxl++;
  } else {
    sim_stats.misses_to_dram++;
  }

  sim_stats.misses.increment(std::pair{handle_pkt.type, handle_pkt.cpu});

  return true;
}

bool CACHE::handle_write(const tag_lookup_type& handle_pkt)
{
  if constexpr (champsim::debug_print) {
    fmt::print("[{}] {} instr_id: {} address: {} v_address: {} type: {} local_prefetch: {} cycle: {}\n", NAME, __func__, handle_pkt.instr_id,
               handle_pkt.address, handle_pkt.v_address, access_type_names.at(champsim::to_underlying(handle_pkt.type)), handle_pkt.prefetch_from_this,
               current_time.time_since_epoch() / clock_period);
  }

  // Note: handle_write processes writebacks (evicted dirty lines), not user store instructions
  // User stores are handled by handle_miss, so we don't track page access here

  mshr_type to_allocate{handle_pkt, current_time};
  to_allocate.data_promise.ready_at(current_time + (warmup ? champsim::chrono::clock::duration{} : FILL_LATENCY));
  inflight_writes.push_back(to_allocate);

  sim_stats.misses.increment(std::pair{handle_pkt.type, handle_pkt.cpu});

  // Track CXL/DRAM split statistics (count misses by memory type)
  if (to_allocate.is_cxl_memory) {
    sim_stats.misses_to_cxl++;
  } else {
    sim_stats.misses_to_dram++;
  }

  return true;
}

template <bool UpdateRequest>
auto CACHE::initiate_tag_check(champsim::channel* ul)
{
  return [time = current_time + (warmup ? champsim::chrono::clock::duration{} : HIT_LATENCY), ul](const auto& entry) {
    CACHE::tag_lookup_type retval{entry};
    retval.event_cycle = time;

    if constexpr (UpdateRequest) {
      if (entry.response_requested) {
        retval.to_return = {&ul->returned};
      }
    } else {
      (void)ul; // supress warning about ul being unused
    }

    if constexpr (champsim::debug_print) {
      fmt::print("[TAG] initiate_tag_check instr_id: {} address: {} v_address: {} type: {} response_requested: {}\n", retval.instr_id, retval.address,
                 retval.v_address, access_type_names.at(champsim::to_underlying(retval.type)), !std::empty(retval.to_return));
    }

    return retval;
  };
}

long CACHE::operate()
{
  long progress{0};

  auto is_ready = [time = current_time](const auto& entry) {
    return entry.event_cycle <= time;
  };
  auto is_translated = [](const auto& entry) {
    return entry.is_translated;
  };

  for (auto* ul : upper_levels) {
    ul->check_collision();
  }

  // Finish returns
  std::for_each(std::cbegin(lower_level->returned), std::cend(lower_level->returned), [this](const auto& pkt) { this->finish_packet(pkt); });
  progress += std::distance(std::cbegin(lower_level->returned), std::cend(lower_level->returned));
  lower_level->returned.clear();

  // Finish translations
  if (lower_translate != nullptr) {
    std::for_each(std::cbegin(lower_translate->returned), std::cend(lower_translate->returned), [this](const auto& pkt) { this->finish_translation(pkt); });
    progress += std::distance(std::cbegin(lower_translate->returned), std::cend(lower_translate->returned));
    lower_translate->returned.clear();
  }
  if (lower_translate_2m != nullptr) {
    std::for_each(std::cbegin(lower_translate_2m->returned), std::cend(lower_translate_2m->returned), [this](const auto& pkt) { this->finish_translation(pkt); });
    progress += std::distance(std::cbegin(lower_translate_2m->returned), std::cend(lower_translate_2m->returned));
    lower_translate_2m->returned.clear();
  }

  // Perform fills
  champsim::bandwidth fill_bw{MAX_FILL};
  for (auto q : {std::ref(MSHR), std::ref(inflight_writes)}) {
    auto [fill_begin, fill_end] = champsim::get_span_p(std::cbegin(q.get()), std::cend(q.get()), fill_bw,
                                                       [time = current_time](const auto& x) { return x.data_promise.is_ready_at(time); });
    auto complete_end = std::find_if_not(fill_begin, fill_end, [this](const auto& x) { return this->handle_fill(x); });
    fill_bw.consume(std::distance(fill_begin, complete_end));
    q.get().erase(fill_begin, complete_end);
  }

  // hardware limited bw -> - inflight_tag_check -> - translation_stash -> divided between upper levels -> - internal_PQ
  const champsim::bandwidth::maximum_type bandwidth_from_tag_checks{champsim::to_underlying(MAX_TAG) * (long)(HIT_LATENCY / clock_period)
                                                                    - (long)std::size(inflight_tag_check)};
  champsim::bandwidth initiate_tag_bw{std::clamp(bandwidth_from_tag_checks, champsim::bandwidth::maximum_type{0}, MAX_TAG)};
  auto can_translate = [avail = (std::size(translation_stash) < static_cast<std::size_t>(MSHR_SIZE))](const auto& entry) {
    return avail || entry.is_translated;
  };
  auto stash_bandwidth_consumed =
      champsim::transform_while_n(translation_stash, std::back_inserter(inflight_tag_check), initiate_tag_bw, is_translated, initiate_tag_check<false>());
  initiate_tag_bw.consume(stash_bandwidth_consumed);
  std::vector<long long> channels_bandwidth_consumed{};

  if (std::size(upper_levels) > 1) {
    std::rotate(upper_levels.begin(), upper_levels.begin() + 1, upper_levels.end());
  }

  // upper levels get an equal portion of the remaining bandwidth
  champsim::bandwidth::maximum_type per_upper_bandwidth =
      std::size(upper_levels) >= 1
          ? (champsim::bandwidth::maximum_type)std::max((size_t)initiate_tag_bw.amount_remaining() / std::size(upper_levels), size_t{1})
          : champsim::bandwidth::maximum_type{};

  for (auto* ul : upper_levels) {
    for (auto q : {std::ref(ul->WQ), std::ref(ul->RQ), std::ref(ul->PQ)}) {
#ifdef ENABLE_SKIP_CXL_TRANSLATION
      auto is_L1_cache = NAME.size() >= 3 && (NAME.compare(NAME.size() - 3, 3, "L1D") == 0 || NAME.compare(NAME.size() - 3, 3, "L1I") == 0);
      // auto skip_translation_with_heatmap = champsim::heatmap::is_hotness_allocation_enabled() && is_L1_cache && g_vmem;
      auto skip_translation_with_heatmap = is_L1_cache && g_vmem;
      auto skip_translation_with_interleaving = is_L1_cache && g_vmem && g_vmem->use_allocation_map;

      if (skip_translation_with_heatmap && !warmup) {
        for (auto& q_entry : q.get()) {
          // if (!q_entry.is_translated && !champsim::heatmap::is_fast_memory(champsim::page_number{q_entry.v_address})) {
          if (!q_entry.is_translated) {
            // Determine page size and use appropriate translation
            auto ps = g_vmem->get_page_size(champsim::page_number{q_entry.v_address});
            q_entry.page_size = static_cast<uint8_t>(ps);
            if (ps == PageSize::PAGE_PERF) {
              // Perforated page: check if hole
              uint64_t vpn_4k = champsim::page_number{q_entry.v_address}.to<uint64_t>();
              if (!g_vmem->is_hole(vpn_4k)) {
                // Non-hole: use 2MB base PPN + 21-bit splice
                auto [ppage, penalty] = g_vmem->va_to_pa_2m(q_entry.cpu, champsim::page_number{q_entry.v_address});
                champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
                champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
                q_entry.address = champsim::address{champsim::splice(champsim::address_slice{pn_2m, champsim::address{ppage}}, champsim::address_slice{off_2m, q_entry.v_address})};
              } else {
                // Hole: use 4KB allocation + 12-bit splice
                auto [ppage, penalty] = g_vmem->va_to_pa(q_entry.cpu, champsim::page_number{q_entry.v_address});
                q_entry.address = champsim::address{champsim::splice(ppage, champsim::page_offset{q_entry.v_address})};
              }
            } else if (ps == PageSize::PAGE_2M) {
              auto [ppage, penalty] = g_vmem->va_to_pa_2m(q_entry.cpu, champsim::page_number{q_entry.v_address});
              champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
              champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
              q_entry.address = champsim::address{champsim::splice(champsim::address_slice{pn_2m, champsim::address{ppage}}, champsim::address_slice{off_2m, q_entry.v_address})};
            } else {
              auto [ppage, penalty] = g_vmem->va_to_pa(q_entry.cpu, champsim::page_number{q_entry.v_address});
              q_entry.address = champsim::address{champsim::splice(ppage, champsim::page_offset{q_entry.v_address})};
            }
            q_entry.is_translated = true;
          }
        }
      } else if (skip_translation_with_interleaving) {
        assert(1==0);
        for (auto& q_entry : q.get()) {
          if (auto [ppage, is_cxl] = g_vmem->va_to_pa_using_map(q_entry.cpu, champsim::page_number{q_entry.v_address});
              !q_entry.is_translated) {
            q_entry.address = champsim::address{champsim::splice(ppage, champsim::page_offset{q_entry.v_address})};
            q_entry.is_translated = true;
          }
        }
      }
#endif
      // this needs to be in this loop, we need to ensure that for cases where bandwidth doesn't divide nicely across upstreams,
      // we don't accidentally consume more bandwidth than expected
      champsim::bandwidth per_upper_tag_bw{std::min(per_upper_bandwidth, champsim::bandwidth::maximum_type{initiate_tag_bw.amount_remaining()})};
      auto bandwidth_consumed =
          champsim::transform_while_n(q.get(), std::back_inserter(inflight_tag_check), per_upper_tag_bw, can_translate, initiate_tag_check<true>(ul));
      channels_bandwidth_consumed.push_back(bandwidth_consumed);
      initiate_tag_bw.consume(bandwidth_consumed);
    }
  }

  auto pq_bandwidth_consumed =
      champsim::transform_while_n(internal_PQ, std::back_inserter(inflight_tag_check), initiate_tag_bw, can_translate, initiate_tag_check<false>());
  initiate_tag_bw.consume(pq_bandwidth_consumed);

  // Issue translations
  std::for_each(std::begin(inflight_tag_check), std::end(inflight_tag_check), [this](auto& x) { this->issue_translation(x); });
  std::for_each(std::begin(translation_stash), std::end(translation_stash), [this](auto& x) { this->issue_translation(x); });

  // Find entries that would be ready except that they have not finished translation, move them to the stash
  auto [last_not_missed, stash_end] = champsim::extract_if(std::begin(inflight_tag_check), std::end(inflight_tag_check), std::back_inserter(translation_stash),
                                                           [is_ready, is_translated](const auto& x) { return is_ready(x) && !is_translated(x); });
  progress += std::distance(last_not_missed, std::end(inflight_tag_check));
  inflight_tag_check.erase(last_not_missed, std::end(inflight_tag_check));

  auto do_handle_miss = [this](const auto& pkt) {
    if (pkt.type == access_type::WRITE && !this->match_offset_bits) {
      return this->handle_write(pkt); // Treat writes (that is, writebacks) like fills
    }
    return this->handle_miss(pkt); // Treat writes (that is, stores) like reads
  };
  champsim::bandwidth tag_check_bw{MAX_TAG};
  auto [tag_check_ready_begin, tag_check_ready_end] =
      champsim::get_span_p(std::begin(inflight_tag_check), std::end(inflight_tag_check), tag_check_bw,
                           [is_ready, is_translated](const auto& pkt) { return is_ready(pkt) && is_translated(pkt); });
  auto hits_end = std::stable_partition(tag_check_ready_begin, tag_check_ready_end, [this](const auto& pkt) { return this->try_hit(pkt); });
  auto finish_tag_check_end = std::stable_partition(hits_end, tag_check_ready_end, do_handle_miss);
  tag_check_bw.consume(std::distance(tag_check_ready_begin, finish_tag_check_end));
  inflight_tag_check.erase(tag_check_ready_begin, finish_tag_check_end);

  impl_prefetcher_cycle_operate();

  if constexpr (champsim::debug_print) {
    fmt::print("[{}] {} cycle completed: {} tags checked: {} remaining: {} stash consumed: {} remaining: {} channel consumed: {} pq consumed {} unused consume "
               "bw {}\n",
               NAME, __func__, current_time.time_since_epoch() / clock_period, tag_check_bw.amount_consumed(), std::size(inflight_tag_check),
               stash_bandwidth_consumed, std::size(translation_stash), channels_bandwidth_consumed, pq_bandwidth_consumed, initiate_tag_bw.amount_remaining());
  }

  // ── STLB: process perforated page pending queues ──
  if (g_vmem && NAME.find("STLB") != std::string::npos) {
    // Process perf_bitmap_waiting entries that need bitmap fetch
    // (entries added by handle_fill for PERF template with coarse_filter=1)
    // Group by 2MB region — issue one bitmap fetch per region
    {
      constexpr uint64_t mask_2m = ~uint64_t{(1ULL << 9) - 1};
      std::unordered_set<uint64_t> regions_needing_fetch;
      for (const auto& entry : perf_bitmap_waiting) {
        uint64_t base = champsim::page_number{entry.v_address}.to<uint64_t>() & mask_2m;
        // Check if bitmap MSHR already exists
        champsim::address addr_2m = stlb_addr_2m(base);
        auto existing = std::find_if(std::begin(MSHR), std::end(MSHR), matches_address(addr_2m, 1));
        if (existing == MSHR.end()) {
          regions_needing_fetch.insert(base);
        }
      }
      for (uint64_t base_2m : regions_needing_fetch) {
        if (MSHR.size() >= MSHR_SIZE) break;
        champsim::address addr_2m = stlb_addr_2m(base_2m);

        request_type bm_req;
        bm_req.type = access_type::LOAD;
        bm_req.cpu = cpu;
        bm_req.address = addr_2m;
        bm_req.v_address = champsim::address{champsim::page_number{base_2m}};
        bm_req.entry_type = 1;
        bm_req.page_size = static_cast<uint8_t>(PageSize::PAGE_PERF);
        bm_req.response_requested = true;
        bm_req.is_translated = true;

        if (lower_level->add_rq(bm_req)) {
          // Construct mshr_type from the bm_req via tag_lookup_type
          tag_lookup_type bm_tag{bm_req};
          mshr_type bm_mshr_entry{bm_tag, current_time};
          bm_mshr_entry.address = addr_2m;
          bm_mshr_entry.entry_type = 1;
          bm_mshr_entry.page_size = static_cast<uint8_t>(PageSize::PAGE_PERF);
          bm_mshr_entry.to_return = {}; // no direct return
          // Find a waiting entry for saved_2m_ppage
          for (const auto& w : perf_bitmap_waiting) {
            uint64_t w_base = champsim::page_number{w.v_address}.to<uint64_t>() & mask_2m;
            if (w_base == base_2m) { bm_mshr_entry.saved_2m_ppage = w.saved_2m_ppage; break; }
          }
          MSHR.push_back(std::move(bm_mshr_entry));
        }
      }
    }

    // Process perf_hole_pending: issue real 4KB PTW for confirmed hole subpages.
    // The result will be cached in STLB as a 4KB hole PTE entry.
    for (auto it = perf_hole_pending.begin(); it != perf_hole_pending.end(); ) {
      // Check if 4KB MSHR already exists for this VPN (from concurrent request)
      auto existing_4k = std::find_if(std::begin(MSHR), std::end(MSHR), matches_address(it->address, 0));
      if (existing_4k != MSHR.end()) {
        // Merge to_return into existing MSHR
        *existing_4k = mshr_type::merge(*existing_4k, *it);
        it = perf_hole_pending.erase(it);
        continue;
      }

      if (MSHR.size() >= MSHR_SIZE) break;

      request_type hole_req;
      hole_req.asid[0] = it->asid[0];
      hole_req.asid[1] = it->asid[1];
      hole_req.type = access_type::LOAD;
      hole_req.cpu = it->cpu;
      hole_req.address = it->address;
      hole_req.v_address = it->v_address;
      hole_req.instr_id = it->instr_id;
      hole_req.ip = it->ip;
      hole_req.page_size = static_cast<uint8_t>(PageSize::PAGE_4K);
      hole_req.entry_type = 0;
      hole_req.response_requested = true;
      hole_req.is_translated = true;

      if (lower_level->add_rq(hole_req)) {
        mshr_type hole_mshr = *it;
        hole_mshr.page_size = static_cast<uint8_t>(PageSize::PAGE_4K);
        hole_mshr.entry_type = 0;
        hole_mshr.data_promise = {}; // CRITICAL: reset to unknown readiness
        MSHR.push_back(std::move(hole_mshr));
        it = perf_hole_pending.erase(it);
      } else {
        ++it;
      }
    }
  }

  return progress + fill_bw.amount_consumed() + initiate_tag_bw.amount_consumed() + tag_check_bw.amount_consumed();
}

// LCOV_EXCL_START exclude deprecated function
uint64_t CACHE::get_set(uint64_t address) const { return static_cast<uint64_t>(get_set_index(champsim::address{address})); }
// LCOV_EXCL_STOP

long CACHE::get_set_index(champsim::address address) const { return address.slice(champsim::dynamic_extent{OFFSET_BITS, champsim::lg2(NUM_SET)}).to<long>(); }

template <typename It>
std::pair<It, It> get_span(It anchor, typename std::iterator_traits<It>::difference_type set_idx, typename std::iterator_traits<It>::difference_type num_way)
{
  auto begin = std::next(anchor, set_idx * num_way);
  return {std::move(begin), std::next(begin, num_way)};
}

auto CACHE::get_set_span(champsim::address address) -> std::pair<set_type::iterator, set_type::iterator>
{
  const auto set_idx = get_set_index(address);
  assert(set_idx < NUM_SET);
  return get_span(std::begin(block), static_cast<set_type::difference_type>(set_idx), NUM_WAY); // safe cast because of prior assert
}

auto CACHE::get_set_span(champsim::address address) const -> std::pair<set_type::const_iterator, set_type::const_iterator>
{
  const auto set_idx = get_set_index(address);
  assert(set_idx < NUM_SET);
  return get_span(std::cbegin(block), static_cast<set_type::difference_type>(set_idx), NUM_WAY); // safe cast because of prior assert
}

// LCOV_EXCL_START exclude deprecated function
uint64_t CACHE::get_way(uint64_t address, uint64_t /*unused set index*/) const
{
  champsim::address intern_addr{address};
  auto [begin, end] = get_set_span(intern_addr);
  return static_cast<uint64_t>(std::distance(begin, std::find_if(begin, end, matches_address(champsim::address{address}))));
}
// LCOV_EXCL_STOP

long CACHE::invalidate_entry(champsim::address inval_addr)
{
  auto [begin, end] = get_set_span(inval_addr);
  auto inv_way = std::find_if(begin, end, matches_address(inval_addr));

  if (inv_way != end) {
    inv_way->valid = false;
  }

  return std::distance(begin, inv_way);
}

bool CACHE::prefetch_line(champsim::address pf_addr, bool fill_this_level, uint32_t prefetch_metadata)
{
  ++sim_stats.pf_requested;

  if (std::size(internal_PQ) >= PQ_SIZE) {
    return false;
  }

  request_type pf_packet;
  pf_packet.type = access_type::PREFETCH;
  pf_packet.pf_metadata = prefetch_metadata;
  pf_packet.cpu = cpu;
  pf_packet.address = pf_addr;
  pf_packet.v_address = virtual_prefetch ? pf_addr : champsim::address{};
  pf_packet.is_translated = !virtual_prefetch;

  internal_PQ.emplace_back(pf_packet, true, !fill_this_level);
  ++sim_stats.pf_issued;

  return true;
}

// LCOV_EXCL_START exclude deprecated function
bool CACHE::prefetch_line(uint64_t pf_addr, bool fill_this_level, uint32_t prefetch_metadata)
{
  return prefetch_line(champsim::address{pf_addr}, fill_this_level, prefetch_metadata);
}

bool CACHE::prefetch_line(uint64_t /*deprecated*/, uint64_t /*deprecated*/, uint64_t pf_addr, bool fill_this_level, uint32_t prefetch_metadata)
{
  return prefetch_line(champsim::address{pf_addr}, fill_this_level, prefetch_metadata);
}
// LCOV_EXCL_STOP

void CACHE::finish_packet(const response_type& packet)
{
  // check MSHR information
  auto mshr_entry = std::find_if(std::begin(MSHR), std::end(MSHR), matches_address(packet.address, packet.entry_type));
  auto first_unreturned = std::find_if(MSHR.begin(), MSHR.end(), [](auto x) { return x.data_promise.has_unknown_readiness(); });

  // sanity check
  if (mshr_entry == MSHR.end()) {
    fmt::print(stderr, "[{}_MSHR] {} cannot find a matching entry! address: {} v_address: {} entry_type: {}\n", NAME, __func__, packet.address, packet.v_address, packet.entry_type);
    assert(0);
  }

  // MSHR holds the most updated information about this request
  mshr_type::returned_value finished_value{packet.data, packet.pf_metadata};
  mshr_entry->is_llc_miss = packet.is_llc_miss;
  // Propagate page_size from response (STLB may transform PAGE_PERF → PAGE_4K)
  mshr_entry->page_size = packet.page_size;
  mshr_entry->entry_type = packet.entry_type;
  // is_cxl_memory is already set in MSHR constructor based on heatmap (forward marking)
  // No need to update from response packet
  mshr_entry->data_promise = champsim::waitable{finished_value, current_time + (warmup ? champsim::chrono::clock::duration{} : FILL_LATENCY)};
  if constexpr (champsim::debug_print) {
    fmt::print("[{}_MSHR] finish_packet instr_id: {} address: {} data: {} type: {} current: {} llc_miss {}\n", this->NAME, mshr_entry->instr_id, mshr_entry->address,
               mshr_entry->data_promise->data, access_type_names.at(champsim::to_underlying(mshr_entry->type)), current_time.time_since_epoch() / clock_period, mshr_entry->is_llc_miss);
  }

  // Order this entry after previously-returned entries, but before non-returned
  // entries (put the newly finished entry at the end of all finished entry, but before unreturned entry)
  std::iter_swap(mshr_entry, first_unreturned);
}

void CACHE::finish_translation(const response_type& packet)
{
  auto pkt_page_size = packet.page_size;
  auto pkt_entry_type = packet.entry_type;

  // entry_type=1 bitmap responses are handled internally by STLB.
  // They should never reach L1. If they do, ignore them.
  if (pkt_entry_type == 1)
    return;

  // ── Normal translation response (4KB, 2MB, or first step of PERF) ──
  auto matches_vpage = [page_num = champsim::page_number{packet.v_address}, pkt_page_size](const auto& entry) {
    if (entry.is_translated || entry.bitmap_check_pending)
      return false;
    if (pkt_page_size == static_cast<uint8_t>(PageSize::PAGE_2M)
        || pkt_page_size == static_cast<uint8_t>(PageSize::PAGE_IDEAL_PERF)) {
      constexpr uint64_t mask_2m = ~uint64_t{(1ULL << 9) - 1};
      return (champsim::page_number{entry.v_address}.to<uint64_t>() & mask_2m) == (page_num.to<uint64_t>() & mask_2m);
    }
    return champsim::page_number{entry.v_address} == page_num;
  };
  auto mark_translated = [p_page = champsim::page_number{packet.data}, pkt_page_size, this](auto& entry) {
    [[maybe_unused]] auto old_address = entry.address;

    if (pkt_page_size == static_cast<uint8_t>(PageSize::PAGE_PERF)) {
      // PAGE_PERF can reach L1 on first access when STLB miss → PTW → STLB handle_fill.
      // This happens for concurrent requests before STLB template is installed.
      // Handle it: classify here as a fallback, same as STLB would.
      uint64_t vpn_4k = champsim::page_number{entry.v_address}.to<uint64_t>();

      if (g_vmem && !g_vmem->coarse_filter_pass(vpn_4k)) {
        champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
        champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
        entry.address = champsim::address{champsim::splice(
            champsim::address_slice{pn_2m, champsim::address{p_page}},
            champsim::address_slice{off_2m, entry.v_address})};
        entry.is_translated = true;
        if (!this->warmup) { this->sim_stats.perf_total++; this->sim_stats.perf_coarse_filtered++; }
      } else if (g_vmem && !g_vmem->is_hole(vpn_4k)) {
        champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
        champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
        entry.address = champsim::address{champsim::splice(
            champsim::address_slice{pn_2m, champsim::address{p_page}},
            champsim::address_slice{off_2m, entry.v_address})};
        entry.is_translated = true;
        if (!this->warmup) { this->sim_stats.perf_total++; this->sim_stats.perf_non_hole++; }
      } else {
        // Hole: re-route to 4KB pipeline
        entry.page_size = static_cast<uint8_t>(PageSize::PAGE_4K);
        entry.page_size_determined = true;
        entry.entry_type = 0;
        entry.translate_issued = false;
        if (!this->warmup) { this->sim_stats.perf_total++; this->sim_stats.perf_hole++; }
      }
      return;
    }

    if (pkt_page_size == static_cast<uint8_t>(PageSize::PAGE_IDEAL_PERF)) {
      // Ideal PERF: 2MB TLB, per-subpage tier routing, zero overhead.
      // Use tier_bitmap to determine DRAM vs CXL, then resolve address.
      // Non-CXL subpages: 2MB splice (stay in base DRAM frame).
      // CXL subpages: need 4KB allocation — re-route to 4KB pipeline.
      uint64_t vpn_4k = champsim::page_number{entry.v_address}.to<uint64_t>();
      if (g_vmem && !g_vmem->is_subpage_dram(vpn_4k)) {
        // CXL subpage: re-route to 4KB pipeline (needs separate physical frame)
        entry.page_size = static_cast<uint8_t>(PageSize::PAGE_4K);
        entry.page_size_determined = true;
        entry.entry_type = 0;
        entry.translate_issued = false;
      } else {
        // DRAM subpage: use 2MB base frame, zero extra cost
        champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
        champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
        entry.address = champsim::address{champsim::splice(
            champsim::address_slice{pn_2m, champsim::address{p_page}},
            champsim::address_slice{off_2m, entry.v_address})};
        entry.is_translated = true;
      }
      return;
    }

    if (pkt_page_size == static_cast<uint8_t>(PageSize::PAGE_2M)) {
      champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
      champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
      entry.address = champsim::address{champsim::splice(
          champsim::address_slice{pn_2m, champsim::address{p_page}},
          champsim::address_slice{off_2m, entry.v_address})};
    } else {
      entry.address = champsim::address{champsim::splice(p_page, champsim::page_offset{entry.v_address})};
    }
    entry.is_translated = true;

    if constexpr (champsim::debug_print) {
      fmt::print("[{}_TRANSLATE] finish_translation old: {} paddr: {} vaddr: {} type: {} page_size: {} cycle: {}\n",
                 this->NAME, old_address, entry.address, entry.v_address,
                 access_type_names.at(champsim::to_underlying(entry.type)), pkt_page_size,
                 this->current_time.time_since_epoch() / this->clock_period);
    }
  };

  // Restart stashed translations
  auto finish_begin = std::find_if_not(std::begin(translation_stash), std::end(translation_stash), [](const auto& x) { return x.is_translated; });
  auto finish_end = std::stable_partition(finish_begin, std::end(translation_stash), matches_vpage);
  std::for_each(finish_begin, finish_end, mark_translated);

  // Find all packets that match the page of the returned packet
  for (auto& entry : inflight_tag_check) {
    if (matches_vpage(entry)) {
      mark_translated(entry);
    }
  }
}

void CACHE::issue_translation(tag_lookup_type& q_entry) const
{
  if (!q_entry.translate_issued && !q_entry.is_translated) {
    // Determine page size for this request if pmap is available and not already resolved
    if (g_vmem && !q_entry.page_size_determined) {
      auto ps = g_vmem->get_page_size(champsim::page_number{q_entry.v_address});
      // All page types (4K, 2M, PERF) use their pmap-assigned type directly.
      // For PERF pages, hole/non-hole classification happens later in
      // finish_translation() via coarse filter + bitmap check.
      q_entry.page_size = static_cast<uint8_t>(ps);
      q_entry.page_size_determined = true;
    }

    request_type fwd_pkt;
    fwd_pkt.asid[0] = q_entry.asid[0];
    fwd_pkt.asid[1] = q_entry.asid[1];
    fwd_pkt.type = access_type::LOAD;
    fwd_pkt.cpu = q_entry.cpu;

    fwd_pkt.v_address = q_entry.v_address;
    fwd_pkt.data = q_entry.data;
    fwd_pkt.instr_id = q_entry.instr_id;
    fwd_pkt.ip = q_entry.ip;

    fwd_pkt.instr_depend_on_me = q_entry.instr_depend_on_me;
    fwd_pkt.is_translated = true;
    fwd_pkt.page_size = q_entry.page_size;
    fwd_pkt.entry_type = q_entry.entry_type;

    fwd_pkt.address = q_entry.address;

    // Route to 2MB TLB only for pure 2MB pages.
    // PERF pages go through DTLB (4KB chain): L1 DTLB stores 4KB entries per subpage,
    // STLB holds the 2MB PERF template and does classification via secondary matching.
    champsim::channel* target_tlb = lower_translate;
    if (q_entry.page_size == static_cast<uint8_t>(PageSize::PAGE_2M) &&
        lower_translate_2m != nullptr) {
      target_tlb = lower_translate_2m;
    }

    q_entry.translate_issued = target_tlb->add_rq(fwd_pkt);

    if constexpr (champsim::debug_print) {
      if (q_entry.translate_issued) {
        fmt::print("[TRANSLATE] do_issue_translation instr_id: {} paddr: {} vaddr: {} type: {} page_size: {} entry_type: {} bitmap_pending: {}\n",
                   q_entry.instr_id, q_entry.address, q_entry.v_address,
                   access_type_names.at(champsim::to_underlying(q_entry.type)),
                   q_entry.page_size, q_entry.entry_type, q_entry.bitmap_check_pending);
      }
    }
  }
}

std::size_t CACHE::get_mshr_occupancy() const { return std::size(MSHR); }

std::vector<std::size_t> CACHE::get_rq_occupancy() const
{
  std::vector<std::size_t> retval;
  std::transform(std::begin(upper_levels), std::end(upper_levels), std::back_inserter(retval), [](auto ulptr) { return ulptr->rq_occupancy(); });
  return retval;
}

std::vector<std::size_t> CACHE::get_wq_occupancy() const
{
  std::vector<std::size_t> retval;
  std::transform(std::begin(upper_levels), std::end(upper_levels), std::back_inserter(retval), [](auto ulptr) { return ulptr->wq_occupancy(); });
  return retval;
}

std::vector<std::size_t> CACHE::get_pq_occupancy() const
{
  std::vector<std::size_t> retval;
  std::transform(std::begin(upper_levels), std::end(upper_levels), std::back_inserter(retval), [](auto ulptr) { return ulptr->pq_occupancy(); });
  retval.push_back(std::size(internal_PQ));
  return retval;
}

// LCOV_EXCL_START exclude deprecated function
std::size_t CACHE::get_occupancy(uint8_t queue_type, uint64_t /*deprecated*/) const
{
  if (queue_type == 0) {
    return get_mshr_occupancy();
  }
  return 0;
}

std::size_t CACHE::get_occupancy(uint8_t queue_type, champsim::address /*deprecated*/) const
{
  if (queue_type == 0) {
    return get_mshr_occupancy();
  }
  return 0;
}
// LCOV_EXCL_STOP

std::size_t CACHE::get_mshr_size() const { return MSHR_SIZE; }
std::vector<std::size_t> CACHE::get_rq_size() const
{
  std::vector<std::size_t> retval;
  std::transform(std::begin(upper_levels), std::end(upper_levels), std::back_inserter(retval), [](auto ulptr) { return ulptr->rq_size(); });
  return retval;
}

std::vector<std::size_t> CACHE::get_wq_size() const
{
  std::vector<std::size_t> retval;
  std::transform(std::begin(upper_levels), std::end(upper_levels), std::back_inserter(retval), [](auto ulptr) { return ulptr->wq_size(); });
  return retval;
}

std::vector<std::size_t> CACHE::get_pq_size() const
{
  std::vector<std::size_t> retval;
  std::transform(std::begin(upper_levels), std::end(upper_levels), std::back_inserter(retval), [](auto ulptr) { return ulptr->pq_size(); });
  retval.push_back(PQ_SIZE);
  return retval;
}

// LCOV_EXCL_START exclude deprecated function
std::size_t CACHE::get_size(uint8_t queue_type, champsim::address /*deprecated*/) const
{
  if (queue_type == 0) {
    return get_mshr_size();
  }
  return 0;
}

std::size_t CACHE::get_size(uint8_t queue_type, uint64_t /*deprecated*/) const
{
  if (queue_type == 0) {
    return get_mshr_size();
  }
  return 0;
}
// LCOV_EXCL_STOP

namespace
{
double occupancy_ratio(std::size_t occ, std::size_t sz) { return std::ceil(occ) / std::ceil(sz); }

std::vector<double> occupancy_ratio_vec(std::vector<std::size_t> occ, std::vector<std::size_t> sz)
{
  std::vector<double> retval;
  std::transform(std::begin(occ), std::end(occ), std::begin(sz), std::back_inserter(retval), occupancy_ratio);
  return retval;
}
} // namespace

double CACHE::get_mshr_occupancy_ratio() const { return ::occupancy_ratio(get_mshr_occupancy(), get_mshr_size()); }

std::vector<double> CACHE::get_rq_occupancy_ratio() const { return ::occupancy_ratio_vec(get_rq_occupancy(), get_rq_size()); }

std::vector<double> CACHE::get_wq_occupancy_ratio() const { return ::occupancy_ratio_vec(get_wq_occupancy(), get_wq_size()); }

std::vector<double> CACHE::get_pq_occupancy_ratio() const { return ::occupancy_ratio_vec(get_pq_occupancy(), get_pq_size()); }

void CACHE::impl_prefetcher_initialize() const { pref_module_pimpl->impl_prefetcher_initialize(); }

uint32_t CACHE::impl_prefetcher_cache_operate(champsim::address addr, champsim::address ip, bool cache_hit, bool useful_prefetch, access_type type,
                                              uint32_t metadata_in) const
{
  return pref_module_pimpl->impl_prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in);
}

uint32_t CACHE::impl_prefetcher_cache_fill(champsim::address addr, long set, long way, bool prefetch, champsim::address evicted_addr,
                                           uint32_t metadata_in) const
{
  return pref_module_pimpl->impl_prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
}

void CACHE::impl_prefetcher_cycle_operate() const { pref_module_pimpl->impl_prefetcher_cycle_operate(); }

void CACHE::impl_prefetcher_final_stats() const { pref_module_pimpl->impl_prefetcher_final_stats(); }

void CACHE::impl_prefetcher_branch_operate(champsim::address ip, uint8_t branch_type, champsim::address branch_target) const
{
  pref_module_pimpl->impl_prefetcher_branch_operate(ip, branch_type, branch_target);
}

void CACHE::impl_initialize_replacement() const { repl_module_pimpl->impl_initialize_replacement(); }

long CACHE::impl_find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const BLOCK* current_set, champsim::address ip, champsim::address full_addr,
                             access_type type) const
{
  return repl_module_pimpl->impl_find_victim(triggering_cpu, instr_id, set, current_set, ip, full_addr, type);
}

void CACHE::impl_update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                          champsim::address victim_addr, access_type type, bool hit) const
{
  repl_module_pimpl->impl_update_replacement_state(triggering_cpu, set, way, full_addr, ip, victim_addr, type, hit);
}

void CACHE::impl_replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                        champsim::address victim_addr, access_type type) const
{
  repl_module_pimpl->impl_replacement_cache_fill(triggering_cpu, set, way, full_addr, ip, victim_addr, type);
}

void CACHE::impl_replacement_final_stats() const { repl_module_pimpl->impl_replacement_final_stats(); }

void CACHE::initialize()
{
  impl_prefetcher_initialize();
  impl_initialize_replacement();
}

void CACHE::begin_phase()
{
  stats_type new_roi_stats;
  stats_type new_sim_stats;

  new_roi_stats.name = NAME;
  new_sim_stats.name = NAME;

  roi_stats = new_roi_stats;
  sim_stats = new_sim_stats;

  for (auto* ul : upper_levels) {
    channel_type::stats_type ul_new_roi_stats;
    channel_type::stats_type ul_new_sim_stats;
    ul->roi_stats = ul_new_roi_stats;
    ul->sim_stats = ul_new_sim_stats;
  }
}

void CACHE::end_phase(unsigned finished_cpu)
{
  finished_cpu = finished_cpu;
  roi_stats.total_miss_latency_cycles = sim_stats.total_miss_latency_cycles;

  // CXL/DRAM split statistics
  roi_stats.total_miss_latency_cycles_dram = sim_stats.total_miss_latency_cycles_dram;
  roi_stats.total_miss_latency_cycles_cxl = sim_stats.total_miss_latency_cycles_cxl;
  roi_stats.misses_to_dram = sim_stats.misses_to_dram;
  roi_stats.misses_to_cxl = sim_stats.misses_to_cxl;
  roi_stats.mshr_merge_to_dram = sim_stats.mshr_merge_to_dram;
  roi_stats.mshr_merge_to_cxl = sim_stats.mshr_merge_to_cxl;

  roi_stats.hits = sim_stats.hits;
  roi_stats.misses = sim_stats.misses;
  roi_stats.mshr_merge = sim_stats.mshr_merge;
  roi_stats.mshr_return = sim_stats.mshr_return;

  roi_stats.pf_requested = sim_stats.pf_requested;
  roi_stats.pf_issued = sim_stats.pf_issued;
  roi_stats.pf_useful = sim_stats.pf_useful;
  roi_stats.pf_useless = sim_stats.pf_useless;
  roi_stats.pf_fill = sim_stats.pf_fill;

  // Perforated page statistics
  roi_stats.perf_total = sim_stats.perf_total;
  roi_stats.perf_coarse_filtered = sim_stats.perf_coarse_filtered;
  roi_stats.perf_non_hole = sim_stats.perf_non_hole;
  roi_stats.perf_hole = sim_stats.perf_hole;
  roi_stats.perf_bitmap_stlb_hit = sim_stats.perf_bitmap_stlb_hit;
  roi_stats.perf_bitmap_stlb_miss = sim_stats.perf_bitmap_stlb_miss;
  roi_stats.perf_bitmap_miss_latency_cycles = sim_stats.perf_bitmap_miss_latency_cycles;

  for (auto* ul : upper_levels) {
    ul->roi_stats.RQ_ACCESS = ul->sim_stats.RQ_ACCESS;
    ul->roi_stats.RQ_MERGED = ul->sim_stats.RQ_MERGED;
    ul->roi_stats.RQ_FULL = ul->sim_stats.RQ_FULL;
    ul->roi_stats.RQ_TO_CACHE = ul->sim_stats.RQ_TO_CACHE;

    ul->roi_stats.PQ_ACCESS = ul->sim_stats.PQ_ACCESS;
    ul->roi_stats.PQ_MERGED = ul->sim_stats.PQ_MERGED;
    ul->roi_stats.PQ_FULL = ul->sim_stats.PQ_FULL;
    ul->roi_stats.PQ_TO_CACHE = ul->sim_stats.PQ_TO_CACHE;

    ul->roi_stats.WQ_ACCESS = ul->sim_stats.WQ_ACCESS;
    ul->roi_stats.WQ_MERGED = ul->sim_stats.WQ_MERGED;
    ul->roi_stats.WQ_FULL = ul->sim_stats.WQ_FULL;
    ul->roi_stats.WQ_TO_CACHE = ul->sim_stats.WQ_TO_CACHE;
    ul->roi_stats.WQ_FORWARD = ul->sim_stats.WQ_FORWARD;
  }
}

template <typename T>
bool CACHE::should_activate_prefetcher(const T& pkt) const
{
  return !pkt.prefetch_from_this && std::count(std::begin(pref_activate_mask), std::end(pref_activate_mask), pkt.type) > 0;
}

// LCOV_EXCL_START Exclude the following function from LCOV
void CACHE::print_deadlock()
{
  std::string_view mshr_write{"instr_id: {} address: {} v_addr: {} type: {} ready: {}"};
  auto mshr_pack = [time = current_time](const auto& entry) {
    return std::tuple{entry.instr_id, entry.address, entry.v_address, access_type_names.at(champsim::to_underlying(entry.type)),
                      entry.data_promise.is_ready_at(time)};
  };

  std::string_view tag_check_write{"instr_id: {} address: {} v_addr: {} is_translated: {} translate_issued: {} event_cycle: {}"};
  auto tag_check_pack = [period = clock_period](const auto& entry) {
    return std::tuple{entry.instr_id,      entry.address,          entry.v_address,
                      entry.is_translated, entry.translate_issued, entry.event_cycle.time_since_epoch() / period};
  };

  champsim::range_print_deadlock(MSHR, NAME + "_MSHR", mshr_write, mshr_pack);
  if (!perf_bitmap_waiting.empty() || !perf_hole_pending.empty())
    fmt::print("[{}] perf_bitmap_waiting={} perf_hole_pending={}\n", NAME, perf_bitmap_waiting.size(), perf_hole_pending.size());
  champsim::range_print_deadlock(inflight_tag_check, NAME + "_tags", tag_check_write, tag_check_pack);
  champsim::range_print_deadlock(translation_stash, NAME + "_translation", tag_check_write, tag_check_pack);

  std::string_view q_writer{"instr_id: {} address: {} v_addr: {} type: {} translated: {}"};
  auto q_entry_pack = [](const auto& entry) {
    return std::tuple{entry.instr_id, entry.address, entry.v_address, access_type_names.at(champsim::to_underlying(entry.type)), entry.is_translated};
  };

  for (auto* ul : upper_levels) {
    champsim::range_print_deadlock(ul->RQ, NAME + "_RQ", q_writer, q_entry_pack);
    champsim::range_print_deadlock(ul->WQ, NAME + "_WQ", q_writer, q_entry_pack);
    champsim::range_print_deadlock(ul->PQ, NAME + "_PQ", q_writer, q_entry_pack);
  }
}
// LCOV_EXCL_STOP
