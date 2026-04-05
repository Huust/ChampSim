#ifndef ECPT_BUILDER_H
#define ECPT_BUILDER_H

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "bandwidth.h"
#include "chrono.h"

class VirtualMemory;
class ECPTWalker;
namespace champsim
{
class channel;
class ecpt_builder
{
  std::string_view m_name{};
  chrono::picoseconds m_clock_period{250};
  uint32_t m_cpu{0};
  std::optional<uint32_t> m_mshr_size{};
  double m_mshr_factor{1};
  std::optional<champsim::bandwidth::maximum_type> m_max_tag_check{};
  std::optional<champsim::bandwidth::maximum_type> m_max_fill{};
  double m_bandwidth_factor{1};
  unsigned m_latency{};
  std::vector<champsim::channel*> m_uls{};
  champsim::channel* m_ll{};
  VirtualMemory* m_vmem{};

  // ECPT-specific parameters
  uint32_t m_cwc_entries{16};
  uint32_t m_cwc_latency{4};     // cycles
  uint32_t m_hash_latency{2};    // cycles
  uint32_t m_pte_table_entries{131072}; // per way (128K)
  uint32_t m_pmd_table_entries{512};    // per way

  friend class ::ECPTWalker;

  uint32_t scaled_by_ul_size(double factor) const;

public:
  ecpt_builder& name(std::string_view name_);
  ecpt_builder& clock_period(champsim::chrono::picoseconds clock_period_);
  ecpt_builder& cpu(uint32_t cpu_);
  ecpt_builder& mshr_size(uint32_t mshr_size_);
  ecpt_builder& mshr_factor(double mshr_factor_);
  ecpt_builder& tag_bandwidth(champsim::bandwidth::maximum_type max_read_);
  ecpt_builder& fill_bandwidth(champsim::bandwidth::maximum_type max_fill_);
  ecpt_builder& bandwidth_factor(double bandwidth_factor_);
  ecpt_builder& latency(unsigned latency_);
  ecpt_builder& upper_levels(std::vector<champsim::channel*>&& uls_);
  ecpt_builder& lower_level(champsim::channel* ll_);
  ecpt_builder& virtual_memory(VirtualMemory* vmem_);

  // ECPT-specific setters
  ecpt_builder& cwc_entries(uint32_t n);
  ecpt_builder& cwc_latency(uint32_t cycles);
  ecpt_builder& hash_latency(uint32_t cycles);
  ecpt_builder& pte_table_entries(uint32_t n);
  ecpt_builder& pmd_table_entries(uint32_t n);
};
} // namespace champsim

#endif
