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
  bool heatmap_generation_enabled = false;
  bool use_heatmap = false;
  std::string heatmap_file_path;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> llc_miss_heatmap; // VPN -> (total_access_count, critical_access_count)
  std::unordered_set<uint64_t> fast_vpns; // VPNs allocated to fast memory
  std::unordered_set<uint64_t> slow_vpns; // VPNs allocated to slow memory

  // Member functions for heatmap
public:
  void enable_heatmap_generation(const std::string& file_path);
  void track_llc_miss(champsim::address v_address);
  void track_critical_miss(champsim::address v_address);
  void save_heatmap();
  void load_heatmap();
  bool is_heatmap_enabled();  // Used during count
  void launch_heatmap();      // Use heatmap, if command line is set
  void allocate_vpns_by_heatmap(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second);
  bool is_fast_memory(uint64_t vpn) const;
};

// Global heatmap access interface
namespace champsim {
  namespace heatmap {
    void enable(const std::string& file_path);
    void track_llc_miss(champsim::address v_address);
    void track_critical_miss(champsim::address v_address);
    void save();
    void load();
    bool is_heatmap_enabled();
    bool is_heatmap_used();
    void allocate_vpns(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second);
    bool is_fast_memory(uint64_t vpn);
    void launch_heatmap();
  }
}

#endif
