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
  shuffle_pages();
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
  if (champsim::heatmap::is_hotness_allocation_enabled()) {
    assert(devices.size() == 2); // Should have 2 devices when using heatmap

    // Use heatmap-based allocation
    return champsim::heatmap::is_fast_memory(vpn) ? Device{Dram{}} : Device{Cxl{}};
  } else {
    // Update: flip is deprecated, we use it outside of this function
    //
    // Use traditional interleaving allocation
    // Flip the device, if we do have 2 devices
    // if (!std::holds_alternative<Single>(active_device)) {
    //   active_device = std::holds_alternative<Dram>(active_device) ? Device{Cxl{}} : Device{Dram{}};
    // }
    return active_device;
  }
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

champsim::page_number VirtualMemory::ppage_front(const Device& dev) const
{
  assert(available_ppages(dev) > 0);
  return ppage_free_list[get_device_index(dev)].front();
}

void VirtualMemory::ppage_pop(const Device& dev)
{
  ppage_free_list[get_device_index(dev)].pop_front();
  if (available_ppages(dev) == 0) {
    fmt::print("[VMEM] WARNING: Out of physical memory, freeing ppages\n");
    populate_pages();
    shuffle_pages();
  }
}

std::size_t VirtualMemory::available_ppages(const Device& dev) const { return (ppage_free_list[get_device_index(dev)].size()); }

std::pair<champsim::page_number, champsim::chrono::clock::duration> VirtualMemory::va_to_pa(uint32_t cpu_num, champsim::page_number vaddr)
{
  Device selected_device = select_device(vaddr);
  auto [ppage, fault] = vpage_to_ppage_map.try_emplace({cpu_num, champsim::page_number{vaddr}}, ppage_front(selected_device));

  // this vpage doesn't yet have a ppage mapping
  if (fault) {
    ppage_pop(selected_device);

    // Track allocation in interleaving mode
    if (track_allocations) {
      assert(devices.size() == 2); // Should have 2 devices when tracking
      assert(!champsim::heatmap::is_heatmap_generation_enabled()); // Should not be in generate-heatmap mode
      assert(!champsim::heatmap::is_hotness_allocation_enabled()); // Should not be in use-heatmap mode
      bool is_cxl_device = std::holds_alternative<Cxl>(selected_device);
      allocation_map[champsim::page_number{vaddr}] = is_cxl_device;
    }

    // If the ppage has existed, you don't need to flip the device for next time allocation
    if (!std::holds_alternative<Single>(active_device)) {
      active_device = std::holds_alternative<Dram>(active_device) ? Device{Cxl{}} : Device{Dram{}};
    }
  }

  auto penalty = fault ? minor_fault_penalty : champsim::chrono::clock::duration::zero();

  if constexpr (champsim::debug_print) {
    fmt::print("[VMEM] {} paddr: {} vpage: {} fault: {}\n", __func__, ppage->second, champsim::page_number{vaddr}, fault);
  }

  return std::pair{ppage->second, penalty};
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

  // Determine device based on allocation_map
  bool is_cxl = it->second;
  Device selected_device = is_cxl ? Device{Cxl{}} : Device{Dram{}};

  // Try to allocate physical page
  auto [ppage, fault] = vpage_to_ppage_map.try_emplace({cpu_num, champsim::page_number{vaddr}}, ppage_front(selected_device));

  // this vpage doesn't yet have a ppage mapping
  if (fault) {
    ppage_pop(selected_device);
    // Note: No device flipping in this mode, allocation is deterministic based on map
  }

  if constexpr (champsim::debug_print) {
    fmt::print("[VMEM] {} vpage: {} ppage: {} fault: {} device: {}\n", __func__, champsim::page_number{vaddr}, ppage->second, fault, is_cxl ? "CXL" : "DRAM");
  }

  return std::pair{ppage->second, is_cxl};
}

std::pair<champsim::address, champsim::chrono::clock::duration> VirtualMemory::get_pte_pa(uint32_t cpu_num, champsim::page_number vaddr, std::size_t level)
{
  if (champsim::page_offset{next_pte_page} == champsim::page_offset{0}) {
    Device selected_device;
    if (devices.size() == 2)
      selected_device = Device{Dram{}};  
    else
      selected_device = active_device;

    active_pte_page = ppage_front(selected_device);
    ppage_pop(selected_device);
    
    if (!std::holds_alternative<Single>(active_device)) {
      active_device = std::holds_alternative<Dram>(active_device) ? Device{Cxl{}} : Device{Dram{}};
    }
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
