#ifndef HEATMAP_H
#define HEATMAP_H

#include <cstddef> // for size_t
#include <cstdint> // for uint64_t, uint32_t, uint8_t
#include <string>
#include "address.h"
#include "champsim.h"
#include <unordered_map>
#include <unordered_set>

class HeatMapTracker {
  bool heatmap_generation_enabled = false;  // Enable LLC miss data collection mode
  bool hotness_allocation_enabled = false;  // Enable hotness-based memory allocation mode
  std::unordered_map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>> page_heatmap; // VPN -> (llc_miss_count, critical_count, total_access_count)
  std::unordered_map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>> subpage_heatmap; // 4KB sub-page VPN -> (llc_miss_count, critical_count, total_access_count)
  std::unordered_set<uint64_t> fast_vpns; // VPNs allocated to fast memory
  std::unordered_set<uint64_t> slow_vpns; // VPNs allocated to slow memory

public:
  void enable_heatmap_generation();
  bool is_heatmap_generation_enabled();
  void track_llc_miss(champsim::page_number vpn);
  void track_critical_miss(champsim::page_number vpn);
  void track_page_access(champsim::page_number vpn);  // Track all page accesses at L1
  void save_heatmap(const std::string& file_path);
  void load_heatmap(const std::string& file_path);
  void track_llc_miss_subpage(uint64_t subpage_vpn);  // Subpage: How the 4KB statistics look like under 2MB-only hardware config
  void track_critical_miss_subpage(uint64_t subpage_vpn);
  void track_page_access_subpage(uint64_t subpage_vpn);
  void save_subpage_heatmap(const std::string& file_path);
  void enable_hotness_allocation();
  bool is_hotness_allocation_enabled();
  void allocate_vpns_by_heatmap(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second);
  bool is_fast_memory(champsim::page_number vpn) const;
};

// Global heatmap access interface
namespace champsim {
  namespace heatmap {
    void enable_heatmap_generation();
    void enable_hotness_allocation();
    void track_llc_miss(champsim::address v_address);
    void track_critical_miss(champsim::address v_address);
    void track_page_access(champsim::address v_address);  // Track all page accesses at L1
    void save(const std::string& file_path);
    void load(const std::string& file_path);
    bool is_heatmap_generation_enabled();
    bool is_hotness_allocation_enabled();
    void allocate_vpns(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second);
    bool is_fast_memory(champsim::page_number vpn);
    void enable_hotness_allocation();
  }
}

#endif
