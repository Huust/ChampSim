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

#ifndef VMEM_H
#define VMEM_H

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "address.h"
#include "champsim.h"
#include "chrono.h"

enum class PageSize : uint8_t { PAGE_4K = 0, PAGE_2M = 1, PAGE_PERF = 2 };
constexpr unsigned LOG2_PAGE_SIZE_4K = 12;
constexpr unsigned LOG2_PAGE_SIZE_2M = 21;

class MEMORY_CONTROLLER;
class VirtualMemory;  // Forward declaration

using pte_entry = champsim::data::size<long long, std::ratio<8>>;

// Global pointer to virtual memory for easy access
extern VirtualMemory* g_vmem;

struct Single {int id = 0;};  // single mode
struct Dram {int id = 0;};    // hybrid-dram
struct Cxl {int id = 1;};     // hybrid-cxl
using Device = std::variant<Single, Dram, Cxl>;

class VirtualMemory
{
private:
  std::map<std::pair<uint32_t, champsim::page_number>, champsim::page_number> vpage_to_ppage_map;
  std::map<std::tuple<uint32_t, uint32_t, champsim::address_slice<champsim::dynamic_extent>>, champsim::address> page_table;
  std::optional<uint64_t> randomization_seed; // 如果有random seed，代表vm管理的物理内存页不是连续的；可能这一次物理页号是0x100，下一次就是0x108

  std::vector<MEMORY_CONTROLLER*> devices; // may have both dram and cxl memory device
  Device active_device; // current active device for physical page allocation (this variable is only valid in interleaving mode)

  // Page allocation tracking/replay (vpage -> is_cxl: 0=DRAM, 1=CXL)
  std::map<champsim::page_number, bool> allocation_map;
  std::string allocation_file;

  // Multi-page-size support: pmap (4KB-VPN -> PageSize)
  std::unordered_map<uint64_t, PageSize> pmap; // key is 4KB-granularity VPN

  // Perforated page support: hole bitmaps and coarse filters
  std::unordered_map<uint64_t, std::array<uint64_t, 8>> hole_bitmaps; // 2MB-base VPN → 512-bit bitmap
  std::unordered_map<uint64_t, uint8_t> coarse_filters;               // 2MB-base VPN → 8-bit filter

  // Latency-parity: physical page mapping from 2MB dram-only golden run
  // Key: 2MB VPN (in hugepage units), Value: (2MB PPN in hugepage units, is_dram)
  std::map<uint64_t, std::pair<uint64_t, bool>> physical_page_map;
  std::unordered_set<uint64_t> protected_ppages;  // 4KB PPNs reserved for replay/mirror
  std::string physical_mapping_file;

  // Policy file: per-subpage tier bitmaps (bit=0 → DRAM, bit=1 → CXL)
  std::unordered_map<uint64_t, std::array<uint64_t, 8>> tier_bitmaps; // 2MB-base VPN → 512-bit tier map

private:
  // Helper function to get device index for array access
  [[nodiscard]] std::size_t get_device_index(const Device& device) const;
  [[nodiscard]] champsim::page_number allocate_ppage(const Device&);
  void reserve_replayed_region(uint64_t hugepage_ppn);

public:
  const champsim::chrono::clock::duration minor_fault_penalty;
  const std::size_t pt_levels;
  const pte_entry pte_page_size; // Size of a PTE page (存放pte的页，大小通常和一般页一致，例如4KB；每个pte大小是8字节，所以一个页表页可以存储512个entries；正好是9bits)
  bool track_allocations = false;  // If true, record allocations
  bool use_allocation_map = false; // If true, use preloaded map for allocation
  bool use_physical_mapping = false;          // If true, replay PPNs from physical_page_map
  bool track_physical_mappings = false;       // If true, save VPN→PPN at end of simulation
  bool use_policy = false;                    // If true, tier decisions from policy file
  bool generating_physical_mapping = false;   // If true, force all allocations to DRAM (Phase 1)
  PageSize default_page_size = PageSize::PAGE_4K;

private:
  std::vector<std::deque<champsim::page_number>> ppage_free_list;
  std::vector<std::deque<champsim::page_number>> ppage_free_list_2m; // 2MB page pool
  champsim::page_number active_pte_page{};
  champsim::address_slice<champsim::dynamic_extent> next_pte_page;

  [[nodiscard]] champsim::page_number ppage_front_2m(const Device&) const;
  void ppage_pop_2m(const Device&);

  void populate_pages();
  void shuffle_pages();
  const Device select_device(champsim::page_number vpn);

public:
  /**
   * Initialize the virtual memory.
   * The size of the virtual memory space is determined from the size of a page table page and the number of levels in the hierarchy.
   *
   * :param page_table_page_size: The size of one page table page. This value must be less than the size of a physical page.
   * :param page_table_levels: The number of levels in the virtual memory table hierarchy.
   * :param minor_penalty: The latency of a minor page fault.
   * :param dram: The physical memory of the system.
   *   This is currently only used to issue a warning if the physical memory is smaller than the virtual memory.
   *   Future versions may perform major page faults through this reference.
   */
  VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                std::vector<MEMORY_CONTROLLER*> dram_);
  VirtualMemory(champsim::data::bytes page_table_page_size, std::size_t page_table_levels, champsim::chrono::clock::duration minor_penalty,
                std::vector<MEMORY_CONTROLLER*> dram_, std::optional<uint64_t> randomization_seed_);

  /**
   * Find the bit location of the lowest bit for the given page table level.
   */
  [[nodiscard]] champsim::data::bits shamt(std::size_t level) const;

  /**
   * Find the extent for the given page table level.
   */
  [[nodiscard]] champsim::dynamic_extent extent(std::size_t level) const;

  /**
   * Get the page table page offset of the address for the given level.
   */
  [[nodiscard]] uint64_t get_offset(champsim::address vaddr, std::size_t level) const;
  [[nodiscard]] uint64_t get_offset(champsim::page_number vaddr, std::size_t level) const;

  /**
   * The count of unallocated physical pages.
   */
  [[nodiscard]] std::size_t available_ppages(const Device&) const;

  /**
   * Enable tracking of page allocations in interleaving mode.
   * When enabled, records which device (DRAM/CXL) each virtual page is mapped to.
   */
  void enable_interleaving_allocation_tracking(const std::string& output_file);

  /**
   * Save the allocation tracking records to file.
   * Should be called at the end of simulation.
   */
  void save_allocation_tracking();

  /**
   * Load precomputed allocation mapping from file.
   * When loaded, page allocations will follow this mapping instead of interleaving.
   * File format: CSV with "vpage,device" where device is 0 (DRAM) or 1 (CXL).
   */
  void load_allocation_mapping(const std::string& input_file);

  /**
   * Load a pmap file specifying which virtual pages use 4KB vs 2MB pages.
   * File format: CSV with "vpn_hex,page_size" where page_size is 0 (4K) or 1 (2M).
   * For 2MB pages, use the base 4KB-VPN (2MB-aligned, lower 9 bits of VPN = 0).
   */
  void load_pmap(const std::string& path);

  /**
   * Save physical page mapping (2MB VPN → PPN) at end of 2MB dram-only run.
   * Format: CSV "vpage,ppage" (both hex, 4KB-granularity 2MB-aligned values).
   */
  void save_physical_mapping(const std::string& path);

  /**
   * Load physical page mapping for address replay in downstream configs.
   * Populates physical_page_map and reserves mirror PPNs in protected_ppages.
   */
  void load_physical_mapping(const std::string& path);

  /**
   * Load policy file specifying page types and per-subpage tier decisions.
   * Format: "base_2mb_vpn_hex, page_type, tier_bitmap_hex"
   *   page_type: 0=4KB, 1=2MB, 2=PERF
   *   tier_bitmap: 128 hex chars (512 bits), bit=0 → DRAM, bit=1 → CXL
   */
  void load_policy(const std::string& path);

  void set_default_page_size(PageSize ps) { default_page_size = ps; }

  // Perforated page support
  static constexpr unsigned PERF_COARSE_FILTER_CYCLES = 1;
  static constexpr unsigned PERF_BITMAP_LATENCY_CYCLES = 10;
  static constexpr unsigned PERF_HOLE_LATENCY_CYCLES = 10;

  [[nodiscard]] bool is_hole(uint64_t vpn_4k) const;
  [[nodiscard]] bool coarse_filter_pass(uint64_t vpn_4k) const;
  void generate_perforated_pages(double frag_ratio, const std::string& distribution);

  /**
   * Get the page size for the given virtual page number (4KB granularity).
   * If a pmap is loaded, looks up the VPN. Default is PAGE_4K.
   */
  [[nodiscard]] PageSize get_page_size(champsim::page_number vpn_4k) const;

  /**
   * Translate the given address for a 2MB page.
   */
  std::pair<champsim::page_number, champsim::chrono::clock::duration> va_to_pa_2m(uint32_t cpu_num, champsim::page_number vaddr);

  /**
   * Translate the given address from the virtual space to the physical space.
   * If a page translation does not already exist, one will be created and the minor fault penalty will be applied.
   *
   * :param cpu_num: The cpu index of the core making the request. This is currently used as an address space ID.
   * :param vaddr: The address to translate.
   *
   * :returns: A pair of the physical address and the latency to be applied to the translation.
   */
  std::pair<champsim::page_number, champsim::chrono::clock::duration> va_to_pa(uint32_t cpu_num, champsim::page_number vaddr);

  /**
   * Allocate physical page using preloaded allocation mapping and return device type.
   * This function uses the allocation_map loaded from file to determine device placement.
   *
   * :param cpu_num: The cpu index of the core making the request.
   * :param vaddr: The virtual address to translate and allocate.
   *
   * :returns: A pair of (physical page number, is_cxl). is_cxl is true if allocated to CXL, false if allocated to DRAM.
   *
   * Requirements:
   * - use_allocation_map must be true (allocation mapping must be loaded)
   * - vaddr must exist in allocation_map
   * - System must have 2 devices (tiered memory)
   */
  std::pair<champsim::page_number, bool> va_to_pa_using_map(uint32_t cpu_num, champsim::page_number vaddr);

  /**
   * Find the address for the page table page for the given virtual address (under translation), and the given level.
   * If a page table page does not already exist, one will be created and the minor fault penalty will be applied.
   *
   * :param cpu_num: The cpu index of the core making the request. This is currently used as an address space ID.
   * :param vaddr: The address to translate.
   * :param level: The current level being translated.
   *
   * :returns: A pair of the page table page address and the latency to be applied to the operation.
   */
  std::pair<champsim::address, champsim::chrono::clock::duration> get_pte_pa(uint32_t cpu_num, champsim::page_number vaddr, std::size_t level);
};

#endif
