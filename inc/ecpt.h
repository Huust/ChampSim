#ifndef ECPT_H
#define ECPT_H

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "address.h"
#include "bandwidth.h"
#include "channel.h"
#include "operable.h"
#include "ecpt_builder.h"

class VirtualMemory;

// ============================================================================
// Hash table descriptor for one (page_size, way) combination.
// Physical address of a probe = base_paddr + hash(cluster_key) * 64.
// ============================================================================
struct ECPTTableInfo {
  uint64_t base_paddr = 0;  // physical start address
  uint64_t num_entries = 0;  // number of 64-byte slots
};

// ============================================================================
// CWT (Cuckoo Walk Table) — software table, built once at init, zero-cost lookup.
// Per-2MB-aligned VPN: records whether a 2MB page exists and in which PMD way.
// ============================================================================
struct CWTEntry {
  bool has_2mb = false;
  uint8_t pmd_way = 0;  // valid only when has_2mb == true
};

// ============================================================================
// CWC (Cuckoo Walk Cache) — hardware cache, models hit/miss latency.
// Caches section headers (16MB section = 8 contiguous 2MB pages).
// ============================================================================
struct CWCSectionHeader {
  uint64_t section_id = 0;  // 4KB VPN >> 12  (16MB granularity)
  bool has_4kb = false;
  bool has_2mb = false;
  bool valid = false;
};

class CWC {
public:
  static constexpr unsigned DEFAULT_ENTRIES = 16;
  static constexpr unsigned DEFAULT_LATENCY = 4;

  void init(unsigned num_entries, unsigned latency_cycles);
  bool lookup(uint64_t section_id, CWCSectionHeader& out);
  void fill(uint64_t section_id, bool has_4kb, bool has_2mb);

  // Stats
  uint64_t stat_hits = 0;
  uint64_t stat_misses = 0;

private:
  void touch(std::size_t idx);
  std::size_t find_lru() const;

  std::vector<CWCSectionHeader> entries_;
  std::vector<uint64_t> lru_counter_;  // higher = more recently used
  uint64_t lru_clock_ = 0;
  unsigned latency_cycles_ = DEFAULT_LATENCY;
};

// ============================================================================
// ECPTWalker — Elastic Cuckoo Page Table walker.
// Drop-in replacement for PageTableWalker (same channel interface).
// ============================================================================
class ECPTWalker : public champsim::operable
{
  using channel_type = champsim::channel;
  using request_type = typename channel_type::request_type;
  using response_type = typename channel_type::response_type;

  // Walk types determine how many probes are issued
  enum class WalkType : uint8_t {
    COMPLETE   = 0,  // CWC miss: 4 probes (PTE-H1, PTE-H2, PMD-H1, PMD-H2)
    SIZE       = 1,  // Only 4KB in section: 2 probes (PTE-H1, PTE-H2)
    DIRECT     = 2,  // Only 2MB in section: 1 probe (PMD-Hx, way from CWT)
    PARTIAL    = 3,  // Mixed section: 3 probes (PMD-Hx + PTE-H1 + PTE-H2)
    PERF_PROBE = 4   // Perforated page mini walk: 2 PTE probes (replaces bitmap fetch)
  };

  // MSHR states
  enum class WalkState : uint8_t {
    CWC_PENDING  = 0,  // Waiting for CWC lookup latency
    HASH_PENDING = 1,  // Waiting for hash computation latency
    PROBING      = 2,  // Probes issued, waiting for responses
    DRAINING     = 3,  // Critical probe done (response sent), waiting for rest
    DONE         = 4   // All probes complete, ready to free
  };

  struct Probe {
    champsim::address paddr{};
    bool issued = false;
    bool done = false;
  };

  struct mshr_type {
    champsim::address req_address{};   // original request address (for response matching)
    champsim::address v_address{};
    uint32_t cpu = std::numeric_limits<uint32_t>::max();
    uint8_t asid[2] = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};
    uint32_t pf_metadata = 0;
    std::vector<uint64_t> instr_depend_on_me{};
    std::vector<std::deque<response_type>*> to_return{};

    WalkState state = WalkState::CWC_PENDING;
    champsim::chrono::clock::time_point ready_time{};

    WalkType walk_type = WalkType::COMPLETE;
    bool cwc_hit = false;

    uint8_t total_probes = 0;
    uint8_t completed_probes = 0;
    uint8_t critical_probe_idx = 0;  // index of the probe that yields the answer
    std::array<Probe, 4> probes{};

    uint8_t request_page_size = 0;  // page_size from the original request
    uint8_t result_page_size = 0;  // 0=4K, 1=2M, 2=PERF (actual page size from vmem)
    uint8_t entry_type = 0;        // 0=normal, 1=bitmap/perf_probe (from STLB)

    mshr_type() = default;
    explicit mshr_type(const request_type& req);
  };

  std::deque<mshr_type> MSHR;

  std::vector<channel_type*> upper_levels;
  channel_type* lower_level;

  // ECPT hash tables: [page_size_idx][way]
  //   page_size_idx: 0=PTE(4KB), 1=PMD(2MB)
  //   way: 0 or 1
  std::array<std::array<ECPTTableInfo, 2>, 2> tables_{};

  // CWT: 2MB-aligned VPN → {has_2mb, pmd_way}
  std::unordered_map<uint64_t, CWTEntry> cwt_;

  // CWT section headers: section_id → {has_4kb, has_2mb}
  // section_id = 4KB VPN >> 12  (16MB section)
  std::unordered_map<uint64_t, CWCSectionHeader> cwt_sections_;

  CWC cwc_;

  // Way assignments from cuckoo construction: cluster_key → way (0 or 1)
  // [0] = PTE way assignments, [1] = PMD way assignments
  std::array<std::unordered_map<uint64_t, uint8_t>, 2> way_assignments_;

  // Hash function magic constants: [page_size_idx][way]
  static constexpr uint64_t HASH_MAGIC[2][2] = {
    {0x9E3779B97F4A7C15ULL, 0x517CC1B727220A95ULL},  // PTE H1, H2
    {0x6C62272E07BB0142ULL, 0xBF58476D1CE4E5B9ULL},  // PMD H1, H2
  };
  static constexpr unsigned CLUSTER_FACTOR = 8;
  static constexpr unsigned SECTION_SHIFT = 12;  // 4KB VPN >> 12 = 16MB section

  // Compute hash for a cluster key
  static uint64_t ecpt_hash(uint64_t cluster_key, int ps_idx, int way, uint64_t table_size);

  // Compute probe physical address
  champsim::address probe_address(uint64_t vpn_4k, int ps_idx, int way) const;

  // Determine which probe is critical (will find the actual translation)
  uint8_t find_critical_probe(uint64_t vpn_4k, WalkType wt, const std::array<Probe, 4>& probes, uint8_t total, uint8_t req_page_size = 0) const;

  // Build hash tables and CWT from vmem mappings
  void build_tables();
  void build_cwt();

  // Setup probes based on walk type
  void setup_probes(mshr_type& mshr);

public:
  const std::string NAME;
  const uint32_t MSHR_SIZE;
  champsim::bandwidth::maximum_type MAX_READ, MAX_FILL;
  const champsim::chrono::clock::duration HIT_LATENCY;
  const uint32_t CWC_LATENCY_CYCLES;
  const uint32_t HASH_LATENCY_CYCLES;
  const uint32_t PTE_TABLE_ENTRIES;
  const uint32_t PMD_TABLE_ENTRIES;

  VirtualMemory* vmem;

  // Statistics
  uint64_t stat_total_walks = 0;
  uint64_t stat_walk_type[5] = {};  // indexed by WalkType (0-4)
  uint64_t stat_total_probes = 0;
  uint64_t stat_walk_latency_sum = 0;

  explicit ECPTWalker(champsim::ecpt_builder builder);

  long operate() final;
  void initialize() final;
  void begin_phase() final;
  void end_phase(unsigned cpu_index) final;
  void print_deadlock() final;
};

#endif
