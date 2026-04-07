#include "ecpt.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <fmt/core.h>

#include "champsim.h"
#include "deadlock.h"
#include "instruction.h"
#include "util/span.h"
#include "vmem.h"

// ============================================================================
// CWC implementation
// ============================================================================

void CWC::init(unsigned num_entries, unsigned latency_cycles)
{
  entries_.resize(num_entries);
  lru_counter_.resize(num_entries, 0);
  latency_cycles_ = latency_cycles;
  lru_clock_ = 0;
}

bool CWC::lookup(uint64_t section_id, CWCSectionHeader& out)
{
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].valid && entries_[i].section_id == section_id) {
      touch(i);
      out = entries_[i];
      ++stat_hits;
      return true;
    }
  }
  ++stat_misses;
  return false;
}

void CWC::fill(uint64_t section_id, bool has_4kb, bool has_2mb)
{
  // Check if already present
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].valid && entries_[i].section_id == section_id) {
      entries_[i].has_4kb = has_4kb;
      entries_[i].has_2mb = has_2mb;
      touch(i);
      return;
    }
  }
  // Find LRU victim
  auto idx = find_lru();
  entries_[idx] = {section_id, has_4kb, has_2mb, true};
  touch(idx);
}

void CWC::touch(std::size_t idx)
{
  lru_counter_[idx] = ++lru_clock_;
}

std::size_t CWC::find_lru() const
{
  // Find invalid entry first
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].valid)
      return i;
  }
  // Find entry with smallest counter
  return static_cast<std::size_t>(
      std::distance(lru_counter_.begin(),
                    std::min_element(lru_counter_.begin(), lru_counter_.end())));
}

// ============================================================================
// ECPTWalker construction
// ============================================================================

ECPTWalker::ECPTWalker(champsim::ecpt_builder b)
    : champsim::operable(b.m_clock_period),
      upper_levels(b.m_uls),
      lower_level(b.m_ll),
      NAME(b.m_name),
      MSHR_SIZE(b.m_mshr_size.value_or(std::lround(b.m_mshr_factor * std::floor(std::size(upper_levels))))),
      MAX_READ(b.m_max_tag_check.value_or(champsim::bandwidth::maximum_type{b.scaled_by_ul_size(b.m_bandwidth_factor)})),
      MAX_FILL(b.m_max_fill.value_or(champsim::bandwidth::maximum_type{b.scaled_by_ul_size(b.m_bandwidth_factor)})),
      HIT_LATENCY(b.m_clock_period * b.m_latency),
      CWC_LATENCY_CYCLES(b.m_cwc_latency),
      HASH_LATENCY_CYCLES(b.m_hash_latency),
      PTE_TABLE_ENTRIES(b.m_pte_table_entries),
      PMD_TABLE_ENTRIES(b.m_pmd_table_entries),
      vmem(b.m_vmem)
{
  cwc_.init(b.m_cwc_entries, b.m_cwc_latency);
}

ECPTWalker::mshr_type::mshr_type(const request_type& req)
    : req_address(req.address), v_address(req.v_address), pf_metadata(req.pf_metadata), cpu(req.cpu),
      instr_depend_on_me(req.instr_depend_on_me)
{
  asid[0] = req.asid[0];
  asid[1] = req.asid[1];
}

// ============================================================================
// Hash function
// ============================================================================

uint64_t ECPTWalker::ecpt_hash(uint64_t cluster_key, int ps_idx, int way, uint64_t table_size)
{
  uint64_t h = cluster_key;
  h ^= h >> 30;
  h *= HASH_MAGIC[ps_idx][way];
  h ^= h >> 27;
  h *= 0x94D049BB133111EBULL;
  h ^= h >> 31;
  return h % table_size;
}

champsim::address ECPTWalker::probe_address(uint64_t vpn_4k, int ps_idx, int way) const
{
  uint64_t cluster_key;
  if (ps_idx == 0) {
    // PTE: cluster by 4KB VPN / 8
    cluster_key = vpn_4k / CLUSTER_FACTOR;
  } else {
    // PMD: cluster by 2MB VPN / 8, where 2MB VPN = 4KB VPN >> 9
    cluster_key = (vpn_4k >> 9) / CLUSTER_FACTOR;
  }
  uint64_t slot = ecpt_hash(cluster_key, ps_idx, way, tables_[ps_idx][way].num_entries);
  return champsim::address{tables_[ps_idx][way].base_paddr + slot * 64};
}

// ============================================================================
// Hash table + CWT construction (called once at initialize())
// ============================================================================

void ECPTWalker::build_tables()
{
  // Reserve physical address space for ECPT tables at the END of DRAM.
  // Compute total table size first, then place them at (total_memory - table_size).
  uint64_t total_table_bytes = 0;
  for (int ps = 0; ps < 2; ++ps) {
    uint32_t entries = (ps == 0) ? PTE_TABLE_ENTRIES : PMD_TABLE_ENTRIES;
    total_table_bytes += static_cast<uint64_t>(entries) * 64 * 2; // 2 ways
  }
  uint64_t total_mem = vmem->total_memory_size();
  uint64_t ecpt_base = total_mem - total_table_bytes;

  for (int ps = 0; ps < 2; ++ps) {
    uint32_t entries = (ps == 0) ? PTE_TABLE_ENTRIES : PMD_TABLE_ENTRIES;
    for (int way = 0; way < 2; ++way) {
      tables_[ps][way].base_paddr = ecpt_base;
      tables_[ps][way].num_entries = entries;
      ecpt_base += static_cast<uint64_t>(entries) * 64;
    }
  }

  // Protect ECPT hash table pages from being allocated as data pages.
  // Convert byte range [total_mem - total_table_bytes, total_mem) to 4KB page numbers.
  uint64_t first_protected_ppn = (total_mem - total_table_bytes) / PAGE_SIZE;
  uint64_t protected_page_count = (total_table_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  vmem->protect_page_range(first_protected_ppn, protected_page_count);

  fmt::print("[ECPT] Hash table layout:\n");
  fmt::print("[ECPT]   PTE way0: base={:#x} entries={}\n", tables_[0][0].base_paddr, tables_[0][0].num_entries);
  fmt::print("[ECPT]   PTE way1: base={:#x} entries={}\n", tables_[0][1].base_paddr, tables_[0][1].num_entries);
  fmt::print("[ECPT]   PMD way0: base={:#x} entries={}\n", tables_[1][0].base_paddr, tables_[1][0].num_entries);
  fmt::print("[ECPT]   PMD way1: base={:#x} entries={}\n", tables_[1][1].base_paddr, tables_[1][1].num_entries);
  fmt::print("[ECPT]   Total size: {:.2f} MB (at end of {:.0f} MB DRAM), {} pages protected\n",
             static_cast<double>(total_table_bytes) / (1024.0 * 1024.0),
             static_cast<double>(total_mem) / (1024.0 * 1024.0),
             protected_page_count);

  // Do cuckoo insertion to determine way assignments.
  // We iterate all VPN→PPN mappings from vmem and assign each to a way.
  // For PTE (4KB): always probe both ways, so way assignment only affects cache access pattern.
  // For PMD (2MB): way assignment stored in CWT for Direct Walk.

  // We use a simple cuckoo strategy: try way 0 first, then way 1.
  // Collision → cuckoo eviction chain (max depth 500).
  static constexpr int MAX_EVICT_DEPTH = 500;

  // occupied[ps][way] maps slot_index → cluster_key
  std::array<std::array<std::unordered_map<uint64_t, uint64_t>, 2>, 2> occupied;

  auto do_insert = [&](uint64_t cluster_key, int ps_idx) -> bool {
    // Try inserting cluster_key. Returns false on failure.
    uint64_t current = cluster_key;
    for (int depth = 0; depth < MAX_EVICT_DEPTH; ++depth) {
      for (int try_way = 0; try_way < 2; ++try_way) {
        uint64_t slot = ecpt_hash(current, ps_idx, try_way, tables_[ps_idx][try_way].num_entries);
        auto& occ = occupied[ps_idx][try_way];
        if (occ.find(slot) == occ.end()) {
          occ[slot] = current;
          way_assignments_[ps_idx][current] = static_cast<uint8_t>(try_way);
          return true;
        }
      }
      // Both ways occupied. Evict from way 0.
      uint64_t slot0 = ecpt_hash(current, ps_idx, 0, tables_[ps_idx][0].num_entries);
      auto& occ0 = occupied[ps_idx][0];
      uint64_t evicted = occ0[slot0];
      occ0[slot0] = current;
      way_assignments_[ps_idx][current] = 0;
      current = evicted;
    }
    // Eviction chain too long — try way 1 for the remaining item
    uint64_t slot1 = ecpt_hash(current, ps_idx, 1, tables_[ps_idx][1].num_entries);
    auto& occ1 = occupied[ps_idx][1];
    if (occ1.find(slot1) == occ1.end()) {
      occ1[slot1] = current;
      way_assignments_[ps_idx][current] = 1;
      return true;
    }
    fmt::print("[ECPT] WARNING: Cuckoo insertion failed for cluster_key={:#x} ps_idx={}\n",
               current, ps_idx);
    return false;
  };

  // Collect all VPNs from vmem's page table
  // vmem->vpage_to_ppage_map is private, so we use get_page_size to classify.
  // We iterate the pmap if available, otherwise we work with default page sizes.
  // For ECPT, we need to know all page mappings that will be accessed.
  // Strategy: scan vmem's pmap (if loaded) to get VPN → page_size.
  // Since ECPT is built during initialize() (after vmem is fully set up),
  // we can access vmem's public interface.

  // We need the VPN→PPN mappings. Since vpage_to_ppage_map is private,
  // we'll pre-populate it by calling va_to_pa/va_to_pa_2m during build.
  // But actually, those are called lazily during simulation.
  //
  // For hash table construction, we only need the cluster_key set (which VPNs
  // map to which page sizes). The actual PPN isn't needed — the hash table
  // is just for cache access pattern modeling.
  //
  // We gather VPNs from the pmap. If no pmap: all pages are default_page_size.
  // Since we don't know which VPNs will be accessed at build time, we just
  // pre-size the tables and do insertion lazily during the walk.
  //
  // REVISED APPROACH: Tables are pre-sized. Way assignments are computed on
  // first access (lazy). The hash table slot → physical address mapping is
  // deterministic from the VPN, so cache behavior is correct regardless of
  // when the assignment happens. We just need to track way assignments for
  // CWT (PMD only, for Direct Walk).

  // For now, insert all pmap entries if available
  if (vmem) {
    // We'll rely on lazy way assignment during walks.
    // The tables_ are already sized. CWT is built from pmap data.
    fmt::print("[ECPT] Hash tables pre-sized. Way assignments will be computed lazily.\n");
  }
}

void ECPTWalker::build_cwt()
{
  if (!vmem)
    return;

  const auto& pmap = vmem->get_pmap();
  if (pmap.empty()) {
    fmt::print("[ECPT] No pmap loaded. CWT will be populated dynamically.\n");
    return;
  }

  uint64_t count_4k = 0, count_2m = 0, count_perf = 0, count_holes = 0;

  for (const auto& [vpn, ps] : pmap) {
    uint64_t section_id = vpn >> SECTION_SHIFT;
    auto& sec = cwt_sections_[section_id];
    sec.section_id = section_id;
    sec.valid = true;

    if (ps == PageSize::PAGE_2M) {
      sec.has_2mb = true;
      // Record CWT entry for Direct Walk
      uint64_t ck_2m = (vpn >> 9) / CLUSTER_FACTOR;
      if (way_assignments_[1].find(ck_2m) == way_assignments_[1].end())
        way_assignments_[1][ck_2m] = 0;
      cwt_[vpn] = {true, way_assignments_[1][ck_2m]};
      ++count_2m;
    } else if (ps == PageSize::PAGE_PERF) {
      // Perforated page: 2MB base in PMD + hole subpages in PTE
      sec.has_2mb = true;
      sec.has_4kb = true; // holes contribute 4KB entries
      // PMD way assignment for 2MB base
      uint64_t ck_2m = (vpn >> 9) / CLUSTER_FACTOR;
      if (way_assignments_[1].find(ck_2m) == way_assignments_[1].end())
        way_assignments_[1][ck_2m] = 0;
      cwt_[vpn] = {true, way_assignments_[1][ck_2m]};
      // PTE way assignments for hole subpages
      for (uint64_t sub = 0; sub < 512; ++sub) {
        uint64_t sub_vpn = vpn + sub;
        if (vmem->is_hole(sub_vpn)) {
          uint64_t ck = sub_vpn / CLUSTER_FACTOR;
          if (way_assignments_[0].find(ck) == way_assignments_[0].end())
            way_assignments_[0][ck] = 0;
          ++count_holes;
        }
      }
      ++count_perf;
    } else if (ps == PageSize::PAGE_4K) {
      sec.has_4kb = true;
      ++count_4k;
    }
  }

  fmt::print("[ECPT] CWT built from pmap: {} 4KB, {} 2MB, {} PERF ({} holes), {} sections\n",
             count_4k, count_2m, count_perf, count_holes, cwt_sections_.size());

  // Pre-fill CWC with the most common sections (optional warmup)
  for (const auto& [sid, hdr] : cwt_sections_) {
    cwc_.fill(sid, hdr.has_4kb, hdr.has_2mb);
  }
  // Reset CWC stats after prefill (don't count init fills)
  cwc_.stat_hits = 0;
  cwc_.stat_misses = 0;
}

// ============================================================================
// Probe setup
// ============================================================================

uint8_t ECPTWalker::find_critical_probe(uint64_t vpn_4k, WalkType wt,
                                         const std::array<Probe, 4>& probes,
                                         uint8_t total, uint8_t req_page_size) const
{
  // Determine the actual page size from vmem
  auto ps = vmem->get_page_size(champsim::page_number{vpn_4k});
  bool is_2mb = (ps == PageSize::PAGE_2M);
  bool is_perf = (ps == PageSize::PAGE_PERF);
  // Full walk (request=PAGE_PERF): PMD is critical (returns template)
  // Mini walk (request=PAGE_4K for PERF VPN): must wait for PTE (hole confirmation)
  bool perf_full_walk = is_perf && (req_page_size == static_cast<uint8_t>(PageSize::PAGE_PERF));

  if (wt == WalkType::DIRECT) {
    return 0; // Only 1 probe, it's the critical one
  }

  if (wt == WalkType::SIZE) {
    // 2 PTE probes. Find which way this VPN is in.
    uint64_t ck = vpn_4k / CLUSTER_FACTOR;
    auto it = way_assignments_[0].find(ck);
    uint8_t way = (it != way_assignments_[0].end()) ? it->second : 0;
    return way; // probe[0]=H1, probe[1]=H2
  }

  if (wt == WalkType::COMPLETE) {
    // 4 probes: PTE-H1(0), PTE-H2(1), PMD-H1(2), PMD-H2(3)
    if (is_perf) {
      if (perf_full_walk) {
        // Full walk: PMD is critical (returns PERF template for STLB caching)
        uint64_t ck_2m = (vpn_4k >> 9) / CLUSTER_FACTOR;
        auto it = way_assignments_[1].find(ck_2m);
        uint8_t way = (it != way_assignments_[1].end()) ? it->second : 0;
        return 2 + way;
      } else {
        // Mini walk: must wait for PTE to confirm hole/non-hole
        uint64_t ck = vpn_4k / CLUSTER_FACTOR;
        auto it = way_assignments_[0].find(ck);
        uint8_t way = (it != way_assignments_[0].end()) ? it->second : 0;
        return 1 - way; // non-matching PTE way
      }
    } else if (is_2mb) {
      uint64_t ck_2m = (vpn_4k >> 9) / CLUSTER_FACTOR;
      auto it = way_assignments_[1].find(ck_2m);
      uint8_t way = (it != way_assignments_[1].end()) ? it->second : 0;
      return 2 + way; // PMD-H1 or PMD-H2
    } else {
      uint64_t ck = vpn_4k / CLUSTER_FACTOR;
      auto it = way_assignments_[0].find(ck);
      uint8_t way = (it != way_assignments_[0].end()) ? it->second : 0;
      return way; // PTE-H1 or PTE-H2
    }
  }

  if (wt == WalkType::PARTIAL) {
    // 3 probes: PMD-Hx(0), PTE-H1(1), PTE-H2(2)
    if (is_perf) {
      if (perf_full_walk) {
        return 0; // Full walk: PMD is critical (returns PERF template)
      } else {
        // Mini walk: must wait for PTE confirmation
        uint64_t ck = vpn_4k / CLUSTER_FACTOR;
        auto it = way_assignments_[0].find(ck);
        uint8_t way = (it != way_assignments_[0].end()) ? it->second : 0;
        return 1 + (1 - way); // non-matching PTE way
      }
    } else if (is_2mb) {
      return 0; // PMD probe
    } else {
      uint64_t ck = vpn_4k / CLUSTER_FACTOR;
      auto it = way_assignments_[0].find(ck);
      uint8_t way = (it != way_assignments_[0].end()) ? it->second : 0;
      return 1 + way; // PTE-H1 or PTE-H2
    }
  }

  return 0;
}

void ECPTWalker::setup_probes(mshr_type& mshr)
{
  uint64_t vpn_4k = champsim::page_number{mshr.v_address}.to<uint64_t>();

  // Ensure way assignment exists for this VPN
  // PTE way assignment (4KB)
  {
    uint64_t ck = vpn_4k / CLUSTER_FACTOR;
    if (way_assignments_[0].find(ck) == way_assignments_[0].end()) {
      // Lazy assignment: try way 0, then way 1 based on hash
      uint64_t slot0 = ecpt_hash(ck, 0, 0, tables_[0][0].num_entries);
      uint64_t slot1 = ecpt_hash(ck, 0, 1, tables_[0][1].num_entries);
      // Simple heuristic: assign to the way with fewer collisions
      // For now, just use way 0
      way_assignments_[0][ck] = 0;
    }
  }
  // PMD way assignment (2MB)
  {
    uint64_t ck_2m = (vpn_4k >> 9) / CLUSTER_FACTOR;
    if (way_assignments_[1].find(ck_2m) == way_assignments_[1].end()) {
      way_assignments_[1][ck_2m] = 0;
    }
  }

  switch (mshr.walk_type) {
  case WalkType::COMPLETE:
    // 4 probes: PTE-H1, PTE-H2, PMD-H1, PMD-H2
    mshr.total_probes = 4;
    mshr.probes[0].paddr = probe_address(vpn_4k, 0, 0);
    mshr.probes[1].paddr = probe_address(vpn_4k, 0, 1);
    mshr.probes[2].paddr = probe_address(vpn_4k, 1, 0);
    mshr.probes[3].paddr = probe_address(vpn_4k, 1, 1);
    break;

  case WalkType::SIZE:
    // 2 probes: PTE-H1, PTE-H2
    mshr.total_probes = 2;
    mshr.probes[0].paddr = probe_address(vpn_4k, 0, 0);
    mshr.probes[1].paddr = probe_address(vpn_4k, 0, 1);
    break;

  case WalkType::DIRECT: {
    // 1 probe: PMD-Hx (way from CWT)
    mshr.total_probes = 1;
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    auto cwt_it = cwt_.find(base_2m);
    uint8_t pmd_way = (cwt_it != cwt_.end() && cwt_it->second.has_2mb) ? cwt_it->second.pmd_way : 0;
    mshr.probes[0].paddr = probe_address(vpn_4k, 1, pmd_way);
    break;
  }

  case WalkType::PARTIAL: {
    // 3 probes: PMD-Hx, PTE-H1, PTE-H2
    mshr.total_probes = 3;
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    auto cwt_it = cwt_.find(base_2m);
    uint8_t pmd_way = (cwt_it != cwt_.end() && cwt_it->second.has_2mb) ? cwt_it->second.pmd_way : 0;
    mshr.probes[0].paddr = probe_address(vpn_4k, 1, pmd_way);
    mshr.probes[1].paddr = probe_address(vpn_4k, 0, 0);
    mshr.probes[2].paddr = probe_address(vpn_4k, 0, 1);
    break;
  }

  case WalkType::PERF_PROBE: {
    // Single bitmap fetch (1 memory access) instead of 2 hash probes.
    // The bitmap physical address is pre-allocated by vmem at policy load time.
    mshr.total_probes = 1;
    uint64_t base_2m = (vpn_4k >> 9) << 9;
    mshr.probes[0].paddr = champsim::address{vmem->get_bitmap_paddr(base_2m)};
    break;
  }
  }

  mshr.critical_probe_idx = find_critical_probe(vpn_4k, mshr.walk_type, mshr.probes, mshr.total_probes, mshr.request_page_size);
}

// ============================================================================
// Main operate() loop
// ============================================================================

long ECPTWalker::operate()
{
  long progress = 0;

  // ── (1) Process responses from lower level ──
  for (const auto& resp : lower_level->returned) {
    auto block = champsim::block_number{resp.address};
    for (auto& mshr : MSHR) {
      if (mshr.state != WalkState::PROBING && mshr.state != WalkState::DRAINING)
        continue;
      for (int i = 0; i < mshr.total_probes; ++i) {
        if (!mshr.probes[i].done && champsim::block_number{mshr.probes[i].paddr} == block) {
          mshr.probes[i].done = true;
          mshr.completed_probes++;

          if (mshr.state == WalkState::PROBING && i == mshr.critical_probe_idx) {
            // Critical probe done — resolve translation and send response
            uint64_t vpn_4k = champsim::page_number{mshr.v_address}.to<uint64_t>();
            auto ps = vmem->get_page_size(champsim::page_number{vpn_4k});

            champsim::page_number ppage;
            champsim::chrono::clock::duration penalty;

            if (ps == PageSize::PAGE_PERF
                && mshr.request_page_size == static_cast<uint8_t>(PageSize::PAGE_PERF)) {
              // Full walk (STLB miss): return PERF template (2MB base PPN).
              // STLB will cache the template and use coarse filter for future hits.
              std::tie(ppage, penalty) = vmem->va_to_pa_2m(mshr.cpu, champsim::page_number{mshr.v_address});
              mshr.result_page_size = static_cast<uint8_t>(PageSize::PAGE_PERF);
            } else if (ps == PageSize::PAGE_PERF) {
              // Mini walk (STLB hit template, filter=1): classify internally, return PAGE_4K.
              // Request had page_size=PAGE_4K (set by STLB try_hit).
              if (vmem->is_hole(vpn_4k)) {
                std::tie(ppage, penalty) = vmem->va_to_pa(mshr.cpu, champsim::page_number{mshr.v_address});
              } else {
                auto [base_ppage, base_penalty] = vmem->va_to_pa_2m(mshr.cpu, champsim::page_number{mshr.v_address});
                champsim::dynamic_extent off_2m{champsim::data::bits{LOG2_PAGE_SIZE_2M}, champsim::data::bits{}};
                champsim::dynamic_extent pn_2m{champsim::address::bits, champsim::data::bits{LOG2_PAGE_SIZE_2M}};
                auto synth = champsim::address{champsim::splice(
                    champsim::address_slice{pn_2m, champsim::address{base_ppage}},
                    champsim::address_slice{off_2m, mshr.v_address})};
                ppage = champsim::page_number{synth};
                penalty = base_penalty;
              }
              mshr.result_page_size = static_cast<uint8_t>(PageSize::PAGE_4K);
            } else if (ps == PageSize::PAGE_2M) {
              std::tie(ppage, penalty) = vmem->va_to_pa_2m(mshr.cpu, champsim::page_number{mshr.v_address});
              mshr.result_page_size = static_cast<uint8_t>(PageSize::PAGE_2M);
            } else {
              std::tie(ppage, penalty) = vmem->va_to_pa(mshr.cpu, champsim::page_number{mshr.v_address});
              mshr.result_page_size = static_cast<uint8_t>(PageSize::PAGE_4K);
            }

            // Update CWT with actual page info
            {
              uint64_t base_2m = (vpn_4k >> 9) << 9;
              bool is_large = (ps == PageSize::PAGE_2M || ps == PageSize::PAGE_PERF);
              if (is_large) {
                uint64_t ck_2m = (vpn_4k >> 9) / CLUSTER_FACTOR;
                auto it = way_assignments_[1].find(ck_2m);
                uint8_t pmd_way = (it != way_assignments_[1].end()) ? it->second : 0;
                cwt_[base_2m] = {true, pmd_way};
              }
              uint64_t section_id = vpn_4k >> SECTION_SHIFT;
              auto& sec = cwt_sections_[section_id];
              sec.section_id = section_id;
              sec.valid = true;
              if (is_large)
                sec.has_2mb = true;
              else
                sec.has_4kb = true;
              if (ps == PageSize::PAGE_PERF)
                sec.has_4kb = true;
              if (!mshr.cwc_hit)
                cwc_.fill(section_id, sec.has_4kb, sec.has_2mb);
            }

            for (auto* ret : mshr.to_return) {
              ret->emplace_back(mshr.v_address, mshr.v_address,
                                champsim::address{ppage}, mshr.pf_metadata,
                                mshr.instr_depend_on_me, mshr.result_page_size, uint8_t{0});
            }

            ++stat_total_walks;
            ++stat_walk_type[static_cast<int>(mshr.walk_type)];
            stat_total_probes += mshr.total_probes;

            mshr.state = (mshr.completed_probes == mshr.total_probes)
                             ? WalkState::DONE
                             : WalkState::DRAINING;
          }

          if (mshr.completed_probes == mshr.total_probes) {
            mshr.state = WalkState::DONE;
          }

          break; // one response per probe
        }
      }
    }
    ++progress;
  }
  lower_level->returned.clear();

  // ── (2) Remove completed MSHR entries ──
  MSHR.erase(std::remove_if(MSHR.begin(), MSHR.end(),
                             [](const auto& m) { return m.state == WalkState::DONE; }),
             MSHR.end());

  // ── (3) CWC_PENDING → HASH_PENDING transitions ──
  for (auto& mshr : MSHR) {
    if (mshr.state == WalkState::CWC_PENDING && mshr.ready_time <= current_time) {
      uint64_t vpn_4k = champsim::page_number{mshr.v_address}.to<uint64_t>();
      uint64_t section_id = vpn_4k >> SECTION_SHIFT;

      CWCSectionHeader hdr;
      if (cwc_.lookup(section_id, hdr)) {
        mshr.cwc_hit = true;
        if (hdr.has_4kb && hdr.has_2mb) {
          mshr.walk_type = WalkType::PARTIAL;
        } else if (hdr.has_2mb) {
          mshr.walk_type = WalkType::DIRECT;
        } else {
          mshr.walk_type = WalkType::SIZE;
        }
      } else {
        mshr.cwc_hit = false;
        mshr.walk_type = WalkType::COMPLETE;
      }

      mshr.state = WalkState::HASH_PENDING;
      mshr.ready_time = current_time + clock_period * HASH_LATENCY_CYCLES;
    }
  }

  // ── (4) HASH_PENDING → PROBING transitions ──
  for (auto& mshr : MSHR) {
    if (mshr.state == WalkState::HASH_PENDING && mshr.ready_time <= current_time) {
      setup_probes(mshr);
      mshr.state = WalkState::PROBING;
    }
  }

  // ── (5) Issue pending probes (PROBING or DRAINING state) ──
  for (auto& mshr : MSHR) {
    if (mshr.state != WalkState::PROBING && mshr.state != WalkState::DRAINING)
      continue;
    for (int i = 0; i < mshr.total_probes; ++i) {
      if (mshr.probes[i].issued || mshr.probes[i].done)
        continue;

      request_type packet;
      packet.address = mshr.probes[i].paddr;
      packet.v_address = mshr.v_address;
      packet.pf_metadata = mshr.pf_metadata;
      packet.cpu = mshr.cpu;
      packet.asid[0] = mshr.asid[0];
      packet.asid[1] = mshr.asid[1];
      packet.is_translated = true;
      packet.type = access_type::TRANSLATION;

      if (lower_level->add_rq(packet)) {
        mshr.probes[i].issued = true;
      }
      // If add_rq fails (queue full), retry next cycle
    }
  }

  // ── (6) Accept new requests from upper levels ──
  champsim::bandwidth tag_bw{MAX_READ};
  for (auto* ul : upper_levels) {
    auto [rq_begin, rq_end] = champsim::get_span_p(std::cbegin(ul->RQ), std::cend(ul->RQ), tag_bw,
        [this, ul](const auto& pkt) {
          if (MSHR.size() >= MSHR_SIZE)
            return false;

          mshr_type new_mshr{pkt};
          new_mshr.request_page_size = pkt.page_size;
          if (pkt.response_requested)
            new_mshr.to_return = {&ul->returned};

          new_mshr.state = WalkState::CWC_PENDING;
          new_mshr.ready_time = current_time + clock_period * CWC_LATENCY_CYCLES;

          MSHR.push_back(std::move(new_mshr));
          return true;
        });
    tag_bw.consume(std::distance(rq_begin, rq_end));
    ul->RQ.erase(rq_begin, rq_end);
  }

  progress += tag_bw.amount_consumed();

  if constexpr (champsim::debug_print) {
    if (progress > 0) {
      fmt::print("[{}] operate() MSHR={}/{} cycle={}\n",
                 NAME, MSHR.size(), MSHR_SIZE,
                 current_time.time_since_epoch() / clock_period);
    }
  }

  return progress;
}

// ============================================================================
// Lifecycle
// ============================================================================

void ECPTWalker::initialize()
{
  fmt::print("[ECPT] Initializing {} with MSHR_SIZE={} CWC_latency={} hash_latency={}\n",
             NAME, MSHR_SIZE, CWC_LATENCY_CYCLES, HASH_LATENCY_CYCLES);
  fmt::print("[ECPT] PTE table: {} entries/way, PMD table: {} entries/way\n",
             PTE_TABLE_ENTRIES, PMD_TABLE_ENTRIES);
  build_tables();
  build_cwt();
}

void ECPTWalker::begin_phase()
{
  for (auto* ul : upper_levels) {
    channel_type::stats_type ul_new_roi_stats;
    channel_type::stats_type ul_new_sim_stats;
    ul->roi_stats = ul_new_roi_stats;
    ul->sim_stats = ul_new_sim_stats;
  }

  // Reset stats for new phase
  stat_total_walks = 0;
  std::fill(std::begin(stat_walk_type), std::end(stat_walk_type), 0);
  stat_total_probes = 0;
  stat_walk_latency_sum = 0;
  cwc_.stat_hits = 0;
  cwc_.stat_misses = 0;
}

void ECPTWalker::end_phase(unsigned /*cpu_index*/)
{
  fmt::print("\n[ECPT] {} Statistics:\n", NAME);
  fmt::print("[ECPT]   Total walks: {}\n", stat_total_walks);
  fmt::print("[ECPT]   CWC hits: {} misses: {} hit_rate: {:.2f}%\n",
             cwc_.stat_hits, cwc_.stat_misses,
             (cwc_.stat_hits + cwc_.stat_misses) > 0
                 ? 100.0 * cwc_.stat_hits / (cwc_.stat_hits + cwc_.stat_misses)
                 : 0.0);
  fmt::print("[ECPT]   Walk types: Complete={} Size={} Direct={} Partial={} PerfProbe={}\n",
             stat_walk_type[0], stat_walk_type[1], stat_walk_type[2], stat_walk_type[3], stat_walk_type[4]);
  if (stat_total_walks > 0) {
    fmt::print("[ECPT]   Avg probes/walk: {:.2f}\n",
               static_cast<double>(stat_total_probes) / stat_total_walks);
  }
}

void ECPTWalker::print_deadlock()
{
  fmt::print("[{}] DEADLOCK: MSHR size={}/{} current_time={}\n",
             NAME, MSHR.size(), MSHR_SIZE,
             current_time.time_since_epoch() / clock_period);
  for (const auto& m : MSHR) {
    fmt::print("[{}]   v_addr={} state={} walk={} et={} probes={}/{} ready_time={}\n",
               NAME, m.v_address, static_cast<int>(m.state),
               static_cast<int>(m.walk_type), m.entry_type,
               m.completed_probes, m.total_probes,
               m.ready_time.time_since_epoch() / clock_period);
  }
}
