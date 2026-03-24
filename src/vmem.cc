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

#include "vmem.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <fmt/core.h>
#include <numeric>
#include <sys/types.h>
#include <variant>

#include "champsim.h"
#include "dram_controller.h"
#include "heatmap.h"
#include "util/bits.h"

using namespace champsim::data::data_literals;

namespace {
constexpr uint64_t HUGE_PAGE_SIZE_BYTES = 2ULL * 1024ULL * 1024ULL;
uint64_t hugepage_span_in_pages() { return HUGE_PAGE_SIZE_BYTES / PAGE_SIZE; }
}

// Global pointer to virtual memory for easy access
VirtualMemory* g_vmem = nullptr;

VirtualMemory::VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                             std::vector<MEMORY_CONTROLLER*> dram_, std::optional<uint64_t> randomization_seed_)
    // Here the fault penalty is set to 0 because:
    // We should assume the benchmark won't do any page allocation as they are done before the instructions of benchmark (benchmark record critical part, like a
    // for loop, and page allocation won't happen during this heavy and critical for loop as it's so slow)
    : randomization_seed(randomization_seed_), devices(dram_), minor_fault_penalty(minor_penalty), pt_levels(page_table_levels),
      pte_page_size(page_table_page_size),
      next_pte_page(
          champsim::dynamic_extent{champsim::data::bits{LOG2_PAGE_SIZE}, champsim::data::bits{champsim::lg2(champsim::data::bytes{pte_page_size}.count())}}, 0)
{
  assert(pte_page_size > 1_kiB);
  assert(champsim::is_power_of_2(pte_page_size.count()));

  // Use dram for interleaving mode at the beginning
  active_device = (dram_.size() == 2) ? Device{Dram{}} : Device{Single{}};

  champsim::page_number last_vpage{
      champsim::lowest_address_for_size(champsim::data::bytes{PAGE_SIZE + champsim::ipow(pte_page_size.count(), static_cast<unsigned>(pt_levels))})};
  champsim::data::bits required_bits{LOG2_PAGE_SIZE + champsim::lg2(last_vpage.to<uint64_t>())};
  if (required_bits > champsim::address::bits) {
    fmt::print("[VMEM] WARNING: virtual memory configuration would require {} bits of addressing.\n", required_bits); // LCOV_EXCL_LINE
  }
  if (required_bits > champsim::data::bits{champsim::lg2(std::accumulate(devices.begin(), devices.end(), 0, [](auto accumulator, auto device) {
    return accumulator + device->size().count();
  }))}) {
    fmt::print("[VMEM] WARNING: physical memory size is smaller than virtual memory size.\n"); // LCOV_EXCL_LINE
  }
  populate_pages();
  // shuffle_pages(); // We turn off randomization due to different page size granularity
}

VirtualMemory::VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                             std::vector<MEMORY_CONTROLLER*> dram_)
    : VirtualMemory(page_table_page_size, page_table_levels, minor_penalty, dram_, {})
{
}

void VirtualMemory::populate_pages()
{
  auto dram_size = std::accumulate(devices.begin(), devices.end(), champsim::data::bytes{0}, [](auto accumulator, auto& dev) {
    return accumulator + dev->size();
  });
  assert(dram_size > 2_MiB);

  ppage_free_list.resize(devices.size());

  std::for_each(ppage_free_list.begin(), ppage_free_list.end(), [this, dev = devices.begin()](auto& list) mutable {
    assert((*dev)->size().count() != 0);
    if (dev == devices.begin())  // If this is the first memory device, spare 1 MB address space
      list.resize((((*dev)->size() - 2_MiB) / PAGE_SIZE).count());
    else
      list.resize(((*dev)->size() / PAGE_SIZE).count());

    assert(dev != devices.cend());
    assert(list.size() != 0);
    dev++;
  });

  champsim::page_number base_address =
      champsim::page_number{champsim::lowest_address_for_size(std::max<champsim::data::mebibytes>(champsim::data::bytes{PAGE_SIZE}, 2_MiB))};
  auto initialize_free_list = [&base_address](auto& page) {
    page = base_address;
    base_address++;
  };

  std::for_each(ppage_free_list.begin(), ppage_free_list.end(), [initialize_free_list](auto& list){
    std::for_each(list.begin(), list.end(), initialize_free_list);
  });

  // Build 2MB free lists by carving out 2MB-aligned groups of 512 contiguous 4KB pages
  // Only carve out up to half of each device's pages (by count of 2MB groups), keeping
  // enough 4KB pages for page tables and small allocations.
  ppage_free_list_2m.resize(devices.size());
  for (std::size_t dev_idx = 0; dev_idx < ppage_free_list.size(); ++dev_idx) {
    auto& list_4k = ppage_free_list[dev_idx];
    auto& list_2m = ppage_free_list_2m[dev_idx];
    // Limit: at most half of total pages go to 2MB pool
    std::size_t max_2m_pages = list_4k.size() / (512 * 2);
    std::size_t i = 0;
    while (i + 512 <= list_4k.size() && list_2m.size() < max_2m_pages) {
      auto base_ppn = list_4k[i].to<uint64_t>();
      if ((base_ppn & 0x1FF) == 0) {
        bool contiguous = true;
        for (std::size_t j = 1; j < 512; ++j) {
          if (list_4k[i + j].to<uint64_t>() != base_ppn + j) {
            contiguous = false;
            break;
          }
        }
        if (contiguous) {
          list_2m.push_back(list_4k[i]);
          list_4k.erase(list_4k.begin() + static_cast<std::ptrdiff_t>(i),
                        list_4k.begin() + static_cast<std::ptrdiff_t>(i + 512));
          continue;
        }
      }
      ++i;
    }
  }
}

void VirtualMemory::shuffle_pages()
{
  if (randomization_seed.has_value())
    std::for_each(ppage_free_list.begin(), ppage_free_list.end(), [this](auto& list){
      std::shuffle(list.begin(), list.end(), std::mt19937_64{randomization_seed.value()});
    });
}

std::size_t VirtualMemory::get_device_index(const Device& device) const
{
  return std::visit([](const auto& dev) { return dev.id; }, device);
}

const Device VirtualMemory::select_device(champsim::page_number vpn) {
  // Phase 1 golden run: force all allocations to DRAM
  if (generating_physical_mapping)
    return (devices.size() == 1) ? Device{Single{}} : Device{Dram{}};

  // Physical mapping replay without policy/heatmap → dram-only replay (Phase 2)
  if (use_physical_mapping && !use_policy && !champsim::heatmap::is_hotness_allocation_enabled())
    return (devices.size() == 1) ? Device{Single{}} : Device{Dram{}};

  // Policy-based tier selection: tier_bitmaps provide per-subpage DRAM/CXL decision
  if (use_policy && devices.size() == 2) {
    uint64_t vpn_val = vpn.to<uint64_t>();
    uint64_t base_2m = (vpn_val >> 9) << 9;
    auto it = tier_bitmaps.find(base_2m);
    if (it != tier_bitmaps.end()) {
      uint64_t sub_idx = vpn_val & 0x1FF;
      unsigned word = sub_idx / 64;
      unsigned bit = sub_idx % 64;
      bool is_slow = (it->second[word] >> bit) & 1;
      return is_slow ? Device{Cxl{}} : Device{Dram{}};
    }
    // Region not in policy → default CXL
    return Device{Cxl{}};
  }

  // Pmap-based tier selection (legacy): entries in pmap → DRAM, absent → CXL.
  if (!pmap.empty() && devices.size() == 2) {
    uint64_t vpn_val = vpn.to<uint64_t>();

    // Check exact 4KB VPN match
    auto it = pmap.find(vpn_val);
    if (it != pmap.end())
      return Device{Dram{}}; // explicitly in pmap → fast tier

    // Check if within a 2MB or perforated region
    uint64_t base_2m = (vpn_val >> 9) << 9;
    it = pmap.find(base_2m);
    if (it != pmap.end()) {
      if (it->second == PageSize::PAGE_2M)
        return Device{Dram{}}; // entire 2MB region in DRAM
      if (it->second == PageSize::PAGE_PERF) {
        // Perforated: non-hole → DRAM, hole → CXL
        return is_hole(vpn_val) ? Device{Cxl{}} : Device{Dram{}};
      }
    }

    // Not in pmap → slow tier
    return Device{Cxl{}};
  }

  if (champsim::heatmap::is_hotness_allocation_enabled()) {
    assert(devices.size() == 2);
    return champsim::heatmap::is_fast_memory(vpn) ? Device{Dram{}} : Device{Cxl{}};
  }

  return active_device;
}

champsim::dynamic_extent VirtualMemory::extent(std::size_t level) const
{
  const champsim::data::bits lower{LOG2_PAGE_SIZE + champsim::lg2(pte_page_size.count()) * (level - 1)};
  const auto size = static_cast<std::size_t>(champsim::lg2(pte_page_size.count()));
  return champsim::dynamic_extent{lower, size};
}

champsim::data::bits VirtualMemory::shamt(std::size_t level) const { return extent(level).lower; }

uint64_t VirtualMemory::get_offset(champsim::address vaddr, std::size_t level) const { return champsim::address_slice{extent(level), vaddr}.to<uint64_t>(); }

uint64_t VirtualMemory::get_offset(champsim::page_number vaddr, std::size_t level) const { return get_offset(champsim::address{vaddr}, level); }

std::size_t VirtualMemory::available_ppages(const Device& dev) const { return (ppage_free_list[get_device_index(dev)].size()); }

champsim::page_number VirtualMemory::allocate_ppage(const Device& dev)
{
  auto& free_list = ppage_free_list[get_device_index(dev)];

  while (true) {
    while (!free_list.empty()) {
      auto candidate = free_list.front();
      free_list.pop_front();
      // Skip pages reserved for physical mapping replay
      if (protected_ppages.find(candidate.to<uint64_t>()) == protected_ppages.end())
        return candidate;
    }
    fmt::print("[VMEM] WARNING: Out of physical memory for device {}, repopulating\n", get_device_index(dev));
    populate_pages();
    shuffle_pages();
  }
}

void VirtualMemory::reserve_replayed_region(uint64_t hugepage_ppn)
{
  const uint64_t pages_per_hugepage = hugepage_span_in_pages();
  const uint64_t first_small_ppn = hugepage_ppn * pages_per_hugepage;
  for (uint64_t i = 0; i < pages_per_hugepage; ++i)
    protected_ppages.insert(first_small_ppn + i);
}

champsim::page_number VirtualMemory::ppage_front_2m(const Device& dev) const
{
  auto idx = get_device_index(dev);
  assert(idx < ppage_free_list_2m.size());
  assert(!ppage_free_list_2m[idx].empty());
  return ppage_free_list_2m[idx].front();
}

void VirtualMemory::ppage_pop_2m(const Device& dev)
{
  auto idx = get_device_index(dev);
  ppage_free_list_2m[idx].pop_front();
  if (ppage_free_list_2m[idx].empty()) {
    fmt::print("[VMEM] WARNING: Out of 2MB physical pages for device {}\n", idx);
  }
}

bool VirtualMemory::coarse_filter_pass(uint64_t vpn_4k) const
{
  uint64_t base_2m = (vpn_4k >> 9) << 9;
  auto it = coarse_filters.find(base_2m);
  if (it == coarse_filters.end())
    return false;
  uint64_t sub_idx = vpn_4k & 0x1FF;
  unsigned region = sub_idx / 64;
  return (it->second >> region) & 1;
}

bool VirtualMemory::is_hole(uint64_t vpn_4k) const
{
  uint64_t base_2m = (vpn_4k >> 9) << 9;
  auto it = hole_bitmaps.find(base_2m);
  if (it == hole_bitmaps.end())
    return false;
  uint64_t sub_idx = vpn_4k & 0x1FF;
  unsigned word = sub_idx / 64;
  unsigned bit = sub_idx % 64;
  return (it->second[word] >> bit) & 1;
}

void VirtualMemory::load_pmap(const std::string& path)
{
  std::ifstream infile(path);
  if (!infile.is_open()) {
    fmt::print("[VMEM] ERROR: Failed to open pmap file: {}\n", path);
    return;
  }

  pmap.clear();
  hole_bitmaps.clear();
  coarse_filters.clear();
  std::string line;
  uint64_t count_4k = 0, count_2m = 0, count_perf = 0;

  while (std::getline(infile, line)) {
    // Skip comments and empty lines
    if (line.empty() || line[0] == '#')
      continue;

    auto comma_pos = line.find(',');
    if (comma_pos == std::string::npos)
      continue;

    try {
      uint64_t vpn = std::stoull(line.substr(0, comma_pos), nullptr, 16);
      auto rest = line.substr(comma_pos + 1);

      // Check for second comma (bitmap field)
      auto comma2 = rest.find(',');
      int ps = std::stoi(rest.substr(0, comma2));

      if (ps == 2 && comma2 != std::string::npos) {
        // Perforated page with bitmap: vpn,2,bitmap_hex (128 hex chars = 512 bits)
        auto bitmap_hex = rest.substr(comma2 + 1);
        pmap[vpn] = PageSize::PAGE_PERF;

        // Parse 128-char hex string into 8 uint64_t words
        std::array<uint64_t, 8> bitmap{};
        for (int w = 0; w < 8 && w * 16 < static_cast<int>(bitmap_hex.size()); ++w) {
          auto chunk = bitmap_hex.substr(w * 16, 16);
          bitmap[w] = std::stoull(chunk, nullptr, 16);
        }
        hole_bitmaps[vpn] = bitmap;

        // Compute coarse filter: 1 bit per 64-sub-page region
        uint8_t cf = 0;
        for (int r = 0; r < 8; ++r) {
          if (bitmap[r] != 0)
            cf |= (1u << r);
        }
        coarse_filters[vpn] = cf;
        ++count_perf;
      } else if (ps == 2) {
        // page_size=2 without bitmap → treat as perforated with no holes (same as 2MB)
        pmap[vpn] = PageSize::PAGE_PERF;
        hole_bitmaps[vpn] = {};
        coarse_filters[vpn] = 0;
        ++count_perf;
      } else {
        PageSize page_size = (ps == 1) ? PageSize::PAGE_2M : PageSize::PAGE_4K;
        pmap[vpn] = page_size;
        if (page_size == PageSize::PAGE_2M)
          ++count_2m;
        else
          ++count_4k;
      }
    } catch (const std::exception& e) {
      fmt::print("[VMEM] WARNING: Failed to parse pmap line: {}\n", line);
      continue;
    }
  }

  infile.close();
  fmt::print("[VMEM] Loaded pmap from {}: {} entries (4K: {}, 2M: {}, PERF: {})\n", path, pmap.size(), count_4k, count_2m, count_perf);
}

PageSize VirtualMemory::get_page_size(champsim::page_number vpn_4k) const
{
  if (pmap.empty())
    return default_page_size;

  uint64_t vpn = vpn_4k.to<uint64_t>();

  // Check exact match first (4KB entry)
  auto it = pmap.find(vpn);
  if (it != pmap.end())
    return it->second;

  // Check if this VPN falls within a 2MB or perforated page
  uint64_t base_2m = (vpn >> 9) << 9;
  it = pmap.find(base_2m);
  if (it != pmap.end() && (it->second == PageSize::PAGE_2M || it->second == PageSize::PAGE_PERF))
    return it->second;

  return PageSize::PAGE_4K;
}

std::pair<champsim::page_number, champsim::chrono::clock::duration> VirtualMemory::va_to_pa_2m(uint32_t cpu_num, champsim::page_number vaddr)
{
  // For 2MB pages, align the VPN to 2MB boundary (mask off lower 9 bits of 4KB VPN)
  uint64_t vpn = vaddr.to<uint64_t>();
  uint64_t aligned_vpn = (vpn >> 9) << 9;
  champsim::page_number aligned_vaddr{aligned_vpn};

  const auto key = std::pair{cpu_num, aligned_vaddr};

  // Check existing mapping
  auto existing = vpage_to_ppage_map.find(key);
  if (existing != vpage_to_ppage_map.end()) {
    return {existing->second, champsim::chrono::clock::duration::zero()};
  }

  // Physical mapping replay: use recorded PPN with cross-device mirror
  if (use_physical_mapping) {
    const uint64_t hugepage_vpn = aligned_vpn / hugepage_span_in_pages();
    auto pm_it = physical_page_map.find(hugepage_vpn);
    if (pm_it != physical_page_map.end()) {
      const auto [parent_ppn, parent_is_dram] = pm_it->second;
      uint64_t ppn = parent_ppn * hugepage_span_in_pages(); // base 4KB PPN

      if (devices.size() > 1) {
        const Device target = select_device(vaddr);
        const bool target_is_dram = std::holds_alternative<Dram>(target);
        if (target_is_dram != parent_is_dram) {
          const uint64_t dram_cap_pages = devices[0]->size().count() / PAGE_SIZE;
          ppn = parent_is_dram ? ppn + dram_cap_pages : ppn - dram_cap_pages;
        }
      }

      auto [ppage, inserted] = vpage_to_ppage_map.emplace(key, champsim::page_number{ppn});

      if constexpr (champsim::debug_print) {
        fmt::print("[VMEM] {} (replay) paddr: {} vpage: {}\n", __func__, ppage->second, aligned_vaddr);
      }
      return {ppage->second, inserted ? minor_fault_penalty : champsim::chrono::clock::duration::zero()};
    }
  }

  // Normal allocation from 2MB free list
  Device selected_device = select_device(vaddr);
  auto [ppage, fault] = vpage_to_ppage_map.try_emplace(key, ppage_front_2m(selected_device));

  if (fault) {
    ppage_pop_2m(selected_device);

    if (!std::holds_alternative<Single>(active_device)) {
      active_device = std::holds_alternative<Dram>(active_device) ? Device{Cxl{}} : Device{Dram{}};
    }
  }

  auto penalty = fault ? minor_fault_penalty : champsim::chrono::clock::duration::zero();

  if constexpr (champsim::debug_print) {
    fmt::print("[VMEM] {} paddr: {} vpage: {} (2MB aligned: {}) fault: {}\n", __func__, ppage->second, vaddr, aligned_vaddr, fault);
  }

  return std::pair{ppage->second, penalty};
}

std::pair<champsim::page_number, champsim::chrono::clock::duration> VirtualMemory::va_to_pa(uint32_t cpu_num, champsim::page_number vaddr)
{
  const auto key = std::pair{cpu_num, champsim::page_number{vaddr}};
  auto existing = vpage_to_ppage_map.find(key);
  if (existing != vpage_to_ppage_map.end()) {
    return {existing->second, champsim::chrono::clock::duration::zero()};
  }

  champsim::page_number new_ppage{};
  bool replayed = false;

  // Physical mapping replay: compute PPN from 2MB parent + offset + cross-device mirror
  if (use_physical_mapping) {
    const uint64_t pages_per_hugepage = hugepage_span_in_pages();
    const uint64_t vpn = vaddr.to<uint64_t>();
    const uint64_t hugepage_vpn = vpn / pages_per_hugepage;
    const uint64_t offset = vpn % pages_per_hugepage;

    auto it = physical_page_map.find(hugepage_vpn);
    if (it != physical_page_map.end()) {
      const auto [parent_ppn, parent_is_dram] = it->second;
      const uint64_t same_device_ppn = parent_ppn * pages_per_hugepage + offset;

      if (devices.size() == 1) {
        new_ppage = champsim::page_number{same_device_ppn};
      } else {
        const Device target = select_device(vaddr);
        const bool target_is_dram = std::holds_alternative<Dram>(target);
        if (target_is_dram == parent_is_dram) {
          new_ppage = champsim::page_number{same_device_ppn};
        } else {
          // Cross-device mirror at the same physical offset
          const uint64_t dram_cap_pages = devices[0]->size().count() / PAGE_SIZE;
          const uint64_t cross_ppn = parent_is_dram
              ? same_device_ppn + dram_cap_pages
              : same_device_ppn - dram_cap_pages;
          new_ppage = champsim::page_number{cross_ppn};
        }
      }
      replayed = true;
    }
  }

  if (!replayed) {
    Device selected_device = select_device(vaddr);
    new_ppage = allocate_ppage(selected_device);

    if (track_allocations) {
      assert(devices.size() == 2);
      allocation_map[champsim::page_number{vaddr}] = std::holds_alternative<Cxl>(selected_device);
    }

    if (!std::holds_alternative<Single>(active_device)) {
      active_device = std::holds_alternative<Dram>(active_device) ? Device{Cxl{}} : Device{Dram{}};
    }
  }

  auto [ppage, inserted] = vpage_to_ppage_map.emplace(key, new_ppage);
  assert(inserted);

  if constexpr (champsim::debug_print) {
    fmt::print("[VMEM] {} paddr: {} vpage: {} replayed: {}\n", __func__, ppage->second, champsim::page_number{vaddr}, replayed);
  }

  return {ppage->second, minor_fault_penalty};
}

std::pair<champsim::page_number, bool> VirtualMemory::va_to_pa_using_map(uint32_t cpu_num, champsim::page_number vaddr)
{
  assert(use_allocation_map);
  assert(devices.size() == 2);
  assert(!champsim::heatmap::is_heatmap_generation_enabled());
  assert(!champsim::heatmap::is_hotness_allocation_enabled());

  // vpage must exist in allocation_map
  auto it = allocation_map.find(champsim::page_number{vaddr});
  assert(it != allocation_map.end());

  bool is_cxl = it->second;
  Device selected_device = is_cxl ? Device{Cxl{}} : Device{Dram{}};

  const auto key = std::pair{cpu_num, champsim::page_number{vaddr}};
  auto existing = vpage_to_ppage_map.find(key);
  if (existing != vpage_to_ppage_map.end()) {
    return {existing->second, is_cxl};
  }

  auto [ppage, inserted] = vpage_to_ppage_map.emplace(key, allocate_ppage(selected_device));
  assert(inserted);

  if constexpr (champsim::debug_print) {
    fmt::print("[VMEM] {} vpage: {} ppage: {} device: {}\n", __func__, champsim::page_number{vaddr}, ppage->second, is_cxl ? "CXL" : "DRAM");
  }

  return {ppage->second, is_cxl};
}

std::pair<champsim::address, champsim::chrono::clock::duration> VirtualMemory::get_pte_pa(uint32_t cpu_num, champsim::page_number vaddr, std::size_t level)
{
  if (champsim::page_offset{next_pte_page} == champsim::page_offset{0}) {
    Device selected_device;
    if (devices.size() == 2)
      selected_device = Device{Dram{}};
    else
      selected_device = active_device;

    active_pte_page = allocate_ppage(selected_device);
    
    // if (!std::holds_alternative<Single>(active_device)) {
    //   active_device = std::holds_alternative<Dram>(active_device) ? Device{Cxl{}} : Device{Dram{}};
    // }
  }

  champsim::dynamic_extent pte_table_entry_extent{champsim::address::bits, shamt(level)};
  auto [ppage, fault] =
      page_table.try_emplace({cpu_num, level, champsim::address_slice{pte_table_entry_extent, vaddr}}, champsim::splice(active_pte_page, next_pte_page));

  // this PTE doesn't yet have a mapping
  if (fault) {
    next_pte_page++;
  }

  auto offset = get_offset(vaddr, level);
  champsim::address paddr{
      champsim::splice(ppage->second, champsim::address_slice{champsim::dynamic_extent{champsim::data::bits{champsim::lg2(pte_entry::byte_multiple)},
                                                                                       static_cast<std::size_t>(champsim::lg2(pte_page_size.count()))},
                                                              offset})};
  if constexpr (champsim::debug_print) {
    fmt::print("[VMEM] {} paddr: {} vaddr: {} pt_page_offset: {} translation_level: {} fault: {}\n", __func__, paddr, vaddr, offset, level, fault);
  }

  auto penalty = minor_fault_penalty;
  if (!fault) {
    penalty = champsim::chrono::clock::duration::zero();
  }

  return {paddr, penalty};
}

void VirtualMemory::enable_interleaving_allocation_tracking(const std::string& output_file)
{
  track_allocations = true;
  allocation_file = output_file;
  allocation_map.clear();
  fmt::print("[VMEM] Interleaving allocation tracking enabled, output: {}\n", output_file);
}

void VirtualMemory::save_allocation_tracking()
{
  if (!track_allocations || allocation_map.empty()) {
    return;
  }

  std::ofstream outfile(allocation_file);
  if (!outfile.is_open()) {
    fmt::print("[VMEM] ERROR: Failed to open allocation tracking file: {}\n", allocation_file);
    return;
  }

  // Write header: vpage,device (0=DRAM, 1=CXL)
  outfile << "vpage,device\n";

  // Write records
  uint64_t dram_count = 0, cxl_count = 0;
  for (const auto& [vpage, is_cxl] : allocation_map) {
    outfile << std::hex << vpage.to<uint64_t>() << ","
            << (is_cxl ? 1 : 0) << std::dec << "\n";
    if (is_cxl) {
      cxl_count++;
    } else {
      dram_count++;
    }
  }

  outfile.close();

  // Print summary statistics
  fmt::print("\n[VMEM] Allocation Tracking Summary:\n");
  fmt::print("  Total allocations: {}\n", allocation_map.size());
  fmt::print("  DRAM allocations: {} ({:.2f}%)\n", dram_count,
             100.0 * dram_count / allocation_map.size());
  fmt::print("  CXL allocations:  {} ({:.2f}%)\n", cxl_count,
             100.0 * cxl_count / allocation_map.size());
  fmt::print("  Saved to: {}\n", allocation_file);
}

void VirtualMemory::generate_perforated_pages(double frag_ratio, const std::string& distribution)
{
  if (pmap.empty()) {
    fmt::print("[VMEM] WARNING: No pmap loaded, cannot generate perforated pages\n");
    return;
  }

  uint64_t converted = 0;
  std::mt19937_64 rng(42); // deterministic seed
  std::uniform_real_distribution<double> dist(0.0, 1.0);

  // Collect all 2MB entries to convert
  std::vector<uint64_t> pages_2m;
  for (auto& [vpn, ps] : pmap) {
    if (ps == PageSize::PAGE_2M)
      pages_2m.push_back(vpn);
  }

  for (auto vpn : pages_2m) {
    pmap[vpn] = PageSize::PAGE_PERF;
    std::array<uint64_t, 8> bitmap{};
    int num_holes = static_cast<int>(512 * frag_ratio);

    if (distribution == "random") {
      for (int s = 0; s < 512; ++s) {
        if (dist(rng) < frag_ratio) {
          unsigned w = s / 64;
          unsigned b = s % 64;
          bitmap[w] |= (1ULL << b);
        }
      }
    } else if (distribution == "dispersed") {
      if (num_holes > 0) {
        int stride = 512 / num_holes;
        if (stride < 1) stride = 1;
        for (int s = 0, count = 0; s < 512 && count < num_holes; s += stride, ++count) {
          unsigned w = s / 64;
          unsigned b = s % 64;
          bitmap[w] |= (1ULL << b);
        }
      }
    } else { // clustered (default)
      // Place holes in contiguous runs within each 64-sub-page region
      int holes_per_region = num_holes / 8;
      int extra = num_holes % 8;
      for (int r = 0; r < 8; ++r) {
        int region_holes = holes_per_region + (r < extra ? 1 : 0);
        for (int b = 0; b < region_holes && b < 64; ++b) {
          bitmap[r] |= (1ULL << b);
        }
      }
    }

    hole_bitmaps[vpn] = bitmap;

    // Compute coarse filter
    uint8_t cf = 0;
    for (int r = 0; r < 8; ++r) {
      if (bitmap[r] != 0)
        cf |= (1u << r);
    }
    coarse_filters[vpn] = cf;
    ++converted;
  }

  fmt::print("[VMEM] Generated perforated pages: {} pages converted, frag_ratio: {}, distribution: {}\n",
             converted, frag_ratio, distribution);
}

void VirtualMemory::save_physical_mapping(const std::string& path)
{
  // Collect unique VPN→PPN mappings in hugepage units (VPN/512, PPN/512).
  // vpage_to_ppage_map stores 4KB-granularity values; convert to hugepage units
  // so the file format matches latency-parity conventions and the replay code.
  const uint64_t pages_per_hugepage = hugepage_span_in_pages();
  std::map<uint64_t, uint64_t> unique_mappings; // hugepage VPN → hugepage PPN
  for (const auto& [key, ppage] : vpage_to_ppage_map) {
    const auto& [cpu_num, vpage] = key;
    (void)cpu_num;
    uint64_t hp_vpn = vpage.to<uint64_t>() / pages_per_hugepage;
    uint64_t hp_ppn = ppage.to<uint64_t>() / pages_per_hugepage;
    auto [it, inserted] = unique_mappings.emplace(hp_vpn, hp_ppn);
    if (!inserted && it->second != hp_ppn) {
      fmt::print("[VMEM] ERROR: Conflicting physical mapping for hugepage vpage={:x}\n", hp_vpn);
      std::abort();
    }
  }

  std::ofstream outfile(path);
  if (!outfile.is_open()) {
    fmt::print("[VMEM] ERROR: Failed to open physical mapping file for writing: {}\n", path);
    return;
  }

  const uint64_t dram_cap_hugepages = devices[0]->size().count() / HUGE_PAGE_SIZE_BYTES;
  outfile << "vpage,ppage,device\n";
  for (const auto& [vpage, ppage] : unique_mappings) {
    int device = (ppage < dram_cap_hugepages) ? 0 : 1;
    outfile << std::hex << vpage << "," << ppage << "," << std::dec << device << "\n";
  }
  outfile.close();

  fmt::print("[VMEM] Saved physical mapping: {} hugepage entries to {}\n", unique_mappings.size(), path);
}

void VirtualMemory::load_physical_mapping(const std::string& path)
{
  std::ifstream infile(path);
  if (!infile.is_open()) {
    fmt::print("[VMEM] ERROR: Failed to open physical mapping file: {}\n", path);
    std::abort();
  }

  physical_page_map.clear();
  protected_ppages.clear();

  std::string line;
  std::getline(infile, line); // skip header

  const uint64_t dram_cap_hugepages = devices.size() >= 2
      ? devices[0]->size().count() / HUGE_PAGE_SIZE_BYTES : 0;

  std::size_t loaded = 0;
  while (std::getline(infile, line)) {
    if (line.empty())
      continue;

    // Parse "vpage,ppage,device" (3 columns)
    auto c1 = line.find(',');
    auto c2 = (c1 != std::string::npos) ? line.find(',', c1 + 1) : std::string::npos;
    if (c2 == std::string::npos)
      continue;

    try {
      uint64_t vpage = std::stoull(line.substr(0, c1), nullptr, 16);
      uint64_t ppage = std::stoull(line.substr(c1 + 1, c2 - c1 - 1), nullptr, 16);
      bool is_dram = (std::stoi(line.substr(c2 + 1)) == 0);

      auto [it, inserted] = physical_page_map.emplace(vpage, std::pair{ppage, is_dram});
      if (!inserted) {
        if (it->second.first != ppage) {
          fmt::print("[VMEM] ERROR: Conflicting physical mapping for vpage={:x}\n", vpage);
          std::abort();
        }
        continue;
      }

      // Reserve the replayed region so allocate_ppage skips it
      reserve_replayed_region(ppage);
      // For tiered memory, also reserve the mirror region in the other device
      if (devices.size() == 2) {
        const uint64_t mirror_ppn = is_dram ? (ppage + dram_cap_hugepages) : (ppage - dram_cap_hugepages);
        reserve_replayed_region(mirror_ppn);
      }
      loaded++;
    } catch (const std::exception&) {
      fmt::print("[VMEM] WARNING: Failed to parse physical mapping line: {}\n", line);
    }
  }
  infile.close();

  use_physical_mapping = true;
  fmt::print("[VMEM] Loaded {} physical mappings, {} protected pages from {}\n",
             loaded, protected_ppages.size(), path);
}

void VirtualMemory::load_policy(const std::string& path)
{
  std::ifstream infile(path);
  if (!infile.is_open()) {
    fmt::print("[VMEM] ERROR: Failed to open policy file: {}\n", path);
    return;
  }

  pmap.clear();
  hole_bitmaps.clear();
  coarse_filters.clear();
  tier_bitmaps.clear();

  std::string line;
  uint64_t count_4k = 0, count_2m = 0, count_perf = 0;

  while (std::getline(infile, line)) {
    if (line.empty() || line[0] == '#')
      continue;

    // Format: base_2mb_vpn_hex, page_type, tier_bitmap_hex
    auto comma1 = line.find(',');
    if (comma1 == std::string::npos)
      continue;
    auto comma2 = line.find(',', comma1 + 1);
    if (comma2 == std::string::npos)
      continue;

    try {
      uint64_t base_vpn = std::stoull(line.substr(0, comma1), nullptr, 16);
      int page_type = std::stoi(line.substr(comma1 + 1, comma2 - comma1 - 1));
      auto bitmap_hex = line.substr(comma2 + 1);

      // Parse 128-char hex string into 8 uint64_t words
      std::array<uint64_t, 8> bitmap{};
      for (int w = 0; w < 8 && w * 16 < static_cast<int>(bitmap_hex.size()); ++w) {
        auto chunk = bitmap_hex.substr(w * 16, 16);
        bitmap[w] = std::stoull(chunk, nullptr, 16);
      }
      tier_bitmaps[base_vpn] = bitmap;

      PageSize ps;
      if (page_type == 2) {
        ps = PageSize::PAGE_PERF;
        // For PERF pages, tier_bitmap == hole_bitmap
        hole_bitmaps[base_vpn] = bitmap;
        uint8_t cf = 0;
        for (int r = 0; r < 8; ++r) {
          if (bitmap[r] != 0)
            cf |= (1u << r);
        }
        coarse_filters[base_vpn] = cf;
        ++count_perf;
      } else if (page_type == 1) {
        ps = PageSize::PAGE_2M;
        ++count_2m;
      } else {
        ps = PageSize::PAGE_4K;
        ++count_4k;
      }
      pmap[base_vpn] = ps;
    } catch (const std::exception& e) {
      fmt::print("[VMEM] WARNING: Failed to parse policy line: {}\n", line);
    }
  }
  infile.close();

  use_policy = true;
  fmt::print("[VMEM] Loaded policy: {} regions (4K: {}, 2M: {}, PERF: {}) from {}\n",
             pmap.size(), count_4k, count_2m, count_perf, path);
}

void VirtualMemory::load_allocation_mapping(const std::string& input_file)
{
  std::ifstream infile(input_file);
  if (!infile.is_open()) {
    fmt::print("[VMEM] ERROR: Failed to open allocation mapping file: {}\n", input_file);
    return;
  }

  allocation_map.clear();

  std::string line;
  // Skip header line
  std::getline(infile, line);

  uint64_t dram_count = 0, cxl_count = 0;
  while (std::getline(infile, line)) {
    // Parse CSV: vpage,device
    size_t comma_pos = line.find(',');
    if (comma_pos == std::string::npos) {
      continue;
    }

    try {
      // Parse vpage (hex)
      uint64_t vpage_val = std::stoull(line.substr(0, comma_pos), nullptr, 16);
      champsim::page_number vpage{vpage_val};

      // Parse device (0 or 1)
      int device = std::stoi(line.substr(comma_pos + 1));
      bool is_cxl = (device == 1);

      allocation_map[vpage] = is_cxl;
      if (is_cxl) {
        cxl_count++;
      } else {
        dram_count++;
      }
    } catch (const std::exception& e) {
      fmt::print("[VMEM] WARNING: Failed to parse line: {}\n", line);
      continue;
    }
  }

  infile.close();
  use_allocation_map = true;

  fmt::print("[VMEM] Loaded {} allocation mappings from: {}\n", allocation_map.size(), input_file);
  fmt::print("  DRAM: {} ({:.2f}%), CXL: {} ({:.2f}%)\n",
             dram_count, 100.0 * dram_count / allocation_map.size(),
             cxl_count, 100.0 * cxl_count / allocation_map.size());
}
