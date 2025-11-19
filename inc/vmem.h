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

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <variant>

#include "address.h"
#include "champsim.h"
#include "chrono.h"

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

private:
  // Helper function to get device index for array access
  [[nodiscard]] std::size_t get_device_index(const Device& device) const;

public:
  const champsim::chrono::clock::duration minor_fault_penalty;
  const std::size_t pt_levels;
  const pte_entry pte_page_size; // Size of a PTE page (存放pte的页，大小通常和一般页一致，例如4KB；每个pte大小是8字节，所以一个页表页可以存储512个entries；正好是9bits)

private:
  std::vector<std::deque<champsim::page_number>> ppage_free_list;
  champsim::page_number active_pte_page{};
  champsim::address_slice<champsim::dynamic_extent> next_pte_page;

  [[nodiscard]] champsim::page_number ppage_front(const Device&) const;
  void ppage_pop(const Device&);

  void populate_pages();
  void shuffle_pages();
  Device select_device(champsim::page_number vpn);

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
