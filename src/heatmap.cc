#include "heatmap.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <vector>
#include <fmt/core.h>

void HeatMapTracker::enable_heatmap_generation(const std::string& file_path) {
  heatmap_generation_enabled = true;
  heatmap_file_path = file_path;
  llc_miss_heatmap.clear();
  fmt::print("Heatmap generation enabled, output: {}\n", file_path);
}

void HeatMapTracker::track_llc_miss(champsim::address v_address) {
  if (!heatmap_generation_enabled)
    return;

  // pick the VPN
  // auto vpn = champsim::address_slice{champsim::dynamic_extent{champsim::data::bits{LOG2_PAGE_SIZE}, champsim::address::bits}, v_address}.to<uint64_t>();
  auto vpn = v_address.to<uint64_t>() >> LOG2_PAGE_SIZE;
  llc_miss_heatmap[vpn].first++;
}

void HeatMapTracker::track_critical_miss(champsim::address v_address) {
  if (!heatmap_generation_enabled)
    return;

  // pick the VPN
  auto vpn = v_address.to<uint64_t>() >> LOG2_PAGE_SIZE;
  llc_miss_heatmap[vpn].second++;
}

void HeatMapTracker::save_heatmap() {
  if (!heatmap_generation_enabled || llc_miss_heatmap.empty())
    return;
  
  std::ofstream file(heatmap_file_path);
  if (!file.is_open()) {
    std::cerr << " Error: Cannot open heatmap file for writing: " << heatmap_file_path << std::endl;
    return;
  }

  // Write into it!
  for (const auto& entry : llc_miss_heatmap) {
    file << std::hex << entry.first << ": " << std::dec << entry.second.first << " " << entry.second.second << std::endl;
  }
  file.close();
  fmt::print("Heatmap saved to: {} with {} virtual pages\n", heatmap_file_path, llc_miss_heatmap.size());
}

void HeatMapTracker::load_heatmap() {
  std::ifstream file(heatmap_file_path);
  if (!file.is_open()) {
    std::cerr << "Error: Cannot open heatmap file for reading: " << heatmap_file_path << std::endl;
    return;
  }

  llc_miss_heatmap.clear();
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;

    std::istringstream iss(line);
    std::string vpn_str;

    if (std::getline(iss, vpn_str, ':')) {
      uint64_t vpn = std::stoull(vpn_str, nullptr, 16);

      uint64_t total_count = 0, critical_count = 0;
      if (iss >> total_count >> critical_count) {
        llc_miss_heatmap[vpn] = std::make_pair(total_count, critical_count);
      }
    }
  }

  file.close();
  fmt::print("Heatmap loaded from: {} with {} virtual pages\n", heatmap_file_path, llc_miss_heatmap.size());
}

bool HeatMapTracker::is_heatmap_enabled() {
  return heatmap_generation_enabled;
}

void HeatMapTracker::launch_heatmap() {
  use_heatmap = true;
}

bool HeatMapTracker::is_fast_memory(uint64_t vpn) const {
  return fast_vpns.find(vpn) != fast_vpns.end();
}

void HeatMapTracker::allocate_vpns_by_heatmap(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second) {
  if (llc_miss_heatmap.empty()) {
    fmt::print("Warning: No heatmap data loaded, cannot allocate VPNs\n");
    return;
  }

  // Convert to vector for sorting
  std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> vpn_data;
  for (const auto& entry : llc_miss_heatmap) {
    vpn_data.push_back({entry.first, entry.second});
  }

  // Sort by total access count or criticality
  if (sort_by_criticality) {
    std::sort(vpn_data.begin(), vpn_data.end(),
              [](const auto& a, const auto& b) {
                return a.second.second > b.second.second; // Sort by critical_count descending
              });
  } else {
    std::sort(vpn_data.begin(), vpn_data.end(),
              [](const auto& a, const auto& b) {
                return a.second.first > b.second.first; // Sort by total_count descending
              });
  }

  // Calculate allocation sizes
  size_t total_vpns = vpn_data.size();
  size_t fast_vpns_count = (total_vpns * ratio_first) / (ratio_first + ratio_second);

  fast_vpns.clear();
  slow_vpns.clear();

  // Allocate top VPNs to fast memory
  for (size_t i = 0; i < vpn_data.size(); ++i) {
    uint64_t vpn = vpn_data[i].first;
    if (i < fast_vpns_count) {
      fast_vpns.insert(vpn);
    } else {
      slow_vpns.insert(vpn);
    }
  }

  fmt::print("VPN allocation complete: {} fast, {} slow (ratio {}:{})\n",
             fast_vpns.size(), slow_vpns.size(), ratio_first, ratio_second);
  if (sort_by_criticality) {
    fmt::print("Allocation based on criticality\n");
  } else {
    fmt::print("Allocation based on total access count\n");
  }
}

// Global heatmap instance and interface implementation
namespace {
  HeatMapTracker global_heatmap_instance;
}

namespace champsim {
  namespace heatmap {
    void enable(const std::string& file_path) {
      global_heatmap_instance.enable_heatmap_generation(file_path);
    }

    void track_llc_miss(champsim::address v_address) {
      global_heatmap_instance.track_llc_miss(v_address);
    }

    void track_critical_miss(champsim::address v_address) {
      global_heatmap_instance.track_critical_miss(v_address);
    }

    void save() {
      global_heatmap_instance.save_heatmap();
    }

    void load() {
      global_heatmap_instance.load_heatmap();
    }

    bool is_heatmap_enabled() {
      return global_heatmap_instance.is_heatmap_enabled();
    }

    bool is_heatmap_used() {
      return global_heatmap_instance.use_heatmap;
    }

    void allocate_vpns(bool sort_by_criticality, uint32_t ratio_first, uint32_t ratio_second) {
      global_heatmap_instance.allocate_vpns_by_heatmap(sort_by_criticality, ratio_first, ratio_second);
    }

    bool is_fast_memory(uint64_t vpn) {
      return global_heatmap_instance.is_fast_memory(vpn);
    }

    void launch_heatmap() {
      global_heatmap_instance.launch_heatmap();
    }
  }
}
