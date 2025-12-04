#include "heatmap.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <vector>
#include <fmt/core.h>
#include "champsim.h"

void HeatMapTracker::enable_heatmap_generation() {
  heatmap_generation_enabled = true;
  page_heatmap.clear();
  fmt::print("Heatmap generation enabled\n");
}

bool HeatMapTracker::is_heatmap_generation_enabled() {
  return heatmap_generation_enabled;
}

void HeatMapTracker::track_llc_miss(champsim::page_number vpn) {
  std::get<0>(page_heatmap[vpn.to<uint64_t>()])++;
}

void HeatMapTracker::track_critical_miss(champsim::page_number vpn) {
  std::get<1>(page_heatmap[vpn.to<uint64_t>()])++;
}

void HeatMapTracker::track_page_access(champsim::page_number vpn) {
  std::get<2>(page_heatmap[vpn.to<uint64_t>()])++;
}


void HeatMapTracker::save_heatmap(const std::string& file_path) {
  if (page_heatmap.empty()) {
    fmt::print("ERROR: Cannot save heatmap file due to empty page_heatmap\n");
    exit(1);
  }

  std::ofstream file(file_path);
  if (!file.is_open()) {
    fmt::print("ERROR: Cannot open heatmap file for writing: {}\n", file_path);
    exit(1);
  }

  // Write into it!
  for (const auto& entry : page_heatmap) {
    file << std::hex << entry.first << ": " << std::dec
         << std::get<0>(entry.second) << " "
         << std::get<1>(entry.second) << " "
         << std::get<2>(entry.second) << std::endl;
  }
  file.close();
  fmt::print("Heatmap saved to: {} with {} virtual pages\n", file_path, page_heatmap.size());
}

void HeatMapTracker::load_heatmap(const std::string& file_path) {
  std::ifstream file(file_path);
  if (!file.is_open()) {
    fmt::print("Error: Cannot open heatmap file for reading: {}\n", file_path);
    exit(1);
  }

  page_heatmap.clear();
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;

    std::istringstream iss(line);
    std::string vpn_str;

    if (std::getline(iss, vpn_str, ':')) {
      uint64_t vpn = std::stoull(vpn_str, nullptr, 16);

      uint64_t llc_miss_count = 0, critical_count = 0, total_access_count = 0;
      if (iss >> llc_miss_count >> critical_count >> total_access_count) {
        page_heatmap[vpn] = std::make_tuple(llc_miss_count, critical_count, total_access_count);
      }
    }
  }

  file.close();
  fmt::print("Heatmap loaded from: {} with {} virtual pages\n", file_path, page_heatmap.size());
}

void HeatMapTracker::enable_hotness_allocation() {
  hotness_allocation_enabled = true;
}

bool HeatMapTracker::is_hotness_allocation_enabled() {
  return hotness_allocation_enabled;
}

bool HeatMapTracker::is_fast_memory(champsim::page_number vpn) const {
  return (fast_vpns.find(vpn.to<uint64_t>()) != fast_vpns.end());
}

void HeatMapTracker::allocate_vpns_by_heatmap(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second) {
  if (page_heatmap.empty()) {
    fmt::print("Warning: No heatmap data loaded, cannot allocate VPNs\n");
    return;
  }

  // Convert to vector for sorting
  std::vector<std::pair<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>>> vpn_data;
  for (const auto& entry : page_heatmap) {
    vpn_data.push_back({entry.first, entry.second});
  }

  // Sort by LLC miss count or criticality
  // Use VPN as tie-breaker for deterministic allocation
  // Example: if 1000 pages all have count=0, and ratio splits at 600/400,
  //          the 600 smallest VPNs always go to fast memory consistently
  if (sort_by_criticality) {
    std::sort(vpn_data.begin(), vpn_data.end(),
              [](const auto& a, const auto& b) {
                if (std::get<1>(a.second) != std::get<1>(b.second))
                  return std::get<1>(a.second) > std::get<1>(b.second); // Sort by critical_count descending
                return a.first < b.first; // Tie-break by VPN ascending
              });
  } else {
    std::sort(vpn_data.begin(), vpn_data.end(),
              [](const auto& a, const auto& b) {
                if (std::get<0>(a.second) != std::get<0>(b.second))
                  return std::get<0>(a.second) > std::get<0>(b.second); // Sort by llc_miss_count descending
                return a.first < b.first; // Tie-break by VPN ascending
              });
  }

  // Calculate allocation sizes
  size_t total_vpns = vpn_data.size();
  size_t fast_vpns_count = (total_vpns * ratio_first) / (ratio_first + ratio_second);

  fast_vpns.clear();
  slow_vpns.clear();

  // Statistics for allocated pages
  uint64_t fast_total_access = 0;
  uint64_t slow_total_access = 0;

  // Allocate top VPNs to fast memory
  for (size_t i = 0; i < vpn_data.size(); ++i) {
    uint64_t vpn = vpn_data[i].first;
    uint64_t total_access_count = std::get<2>(vpn_data[i].second);

    if (i < fast_vpns_count) {
      fast_vpns.insert(vpn);
      fast_total_access += total_access_count;
    } else {
      slow_vpns.insert(vpn);
      slow_total_access += total_access_count;
    }
  }

  auto format_ratio = [](uint64_t fast, uint64_t slow) -> std::string {
    if (slow == 0) return "inf:1";
    if (fast == 0) return "1:inf";
    double ratio = (double)fast / slow;
    return ratio >= 1 ? fmt::format("{:.1f}:1", ratio) : fmt::format("1:{:.1f}", 1.0/ratio);
  };

  fmt::print("VPN allocation complete: {} fast, {} slow (ratio {}:{})\n",
             fast_vpns.size(), slow_vpns.size(), ratio_first, ratio_second);
  fmt::print("Total access count: {} fast, {} slow (ratio {})\n",
             fast_total_access, slow_total_access, format_ratio(fast_total_access, slow_total_access));
  if (sort_by_criticality) {
    fmt::print("Allocation based on CRITICALITY count\n");
  } else {
    fmt::print("Allocation based on ACCESS count\n");
  }
}

// Global heatmap instance and interface implementation
namespace {
  HeatMapTracker global_heatmap_instance;
}

namespace champsim {
  namespace heatmap {
    void enable_heatmap_generation() {
      global_heatmap_instance.enable_heatmap_generation();
    }

    void enable_hotness_allocation() {
      global_heatmap_instance.enable_hotness_allocation();
    }

    void track_llc_miss(champsim::address v_address) {
      global_heatmap_instance.track_llc_miss(champsim::page_number{v_address});
    }

    void track_critical_miss(champsim::address v_address) {
      global_heatmap_instance.track_critical_miss(champsim::page_number{v_address});
    }

    void track_page_access(champsim::address v_address) {
      global_heatmap_instance.track_page_access(champsim::page_number{v_address});
    }


    void save(const std::string& file_path) {
      global_heatmap_instance.save_heatmap(file_path);
    }

    void load(const std::string& file_path) {
      global_heatmap_instance.load_heatmap(file_path);
    }

    bool is_heatmap_generation_enabled() {
      return global_heatmap_instance.is_heatmap_generation_enabled();
    }

    bool is_hotness_allocation_enabled() {
      return global_heatmap_instance.is_hotness_allocation_enabled();
    }

    void allocate_vpns(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second) {
      global_heatmap_instance.allocate_vpns_by_heatmap(sort_by_criticality, ratio_first, ratio_second);
    }

    bool is_fast_memory(champsim::page_number vpn) {
      return global_heatmap_instance.is_fast_memory(vpn);
    }
  }
}
