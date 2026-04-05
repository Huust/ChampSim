#include "ecpt_builder.h"

#include <cmath>
#include <utility>

auto champsim::ecpt_builder::name(std::string_view name_) -> ecpt_builder& { m_name = name_; return *this; }
auto champsim::ecpt_builder::clock_period(champsim::chrono::picoseconds clock_period_) -> ecpt_builder& { m_clock_period = clock_period_; return *this; }
auto champsim::ecpt_builder::cpu(uint32_t cpu_) -> ecpt_builder& { m_cpu = cpu_; return *this; }
auto champsim::ecpt_builder::mshr_size(uint32_t mshr_size_) -> ecpt_builder& { m_mshr_size = mshr_size_; return *this; }
auto champsim::ecpt_builder::mshr_factor(double mshr_factor_) -> ecpt_builder& { m_mshr_factor = mshr_factor_; return *this; }
auto champsim::ecpt_builder::tag_bandwidth(champsim::bandwidth::maximum_type max_read_) -> ecpt_builder& { m_max_tag_check = max_read_; return *this; }
auto champsim::ecpt_builder::fill_bandwidth(champsim::bandwidth::maximum_type max_fill_) -> ecpt_builder& { m_max_fill = max_fill_; return *this; }
auto champsim::ecpt_builder::bandwidth_factor(double bandwidth_factor_) -> ecpt_builder& { m_bandwidth_factor = bandwidth_factor_; return *this; }
auto champsim::ecpt_builder::latency(unsigned latency_) -> ecpt_builder& { m_latency = latency_; return *this; }
auto champsim::ecpt_builder::upper_levels(std::vector<champsim::channel*>&& uls_) -> ecpt_builder& { m_uls = std::move(uls_); return *this; }
auto champsim::ecpt_builder::lower_level(champsim::channel* ll_) -> ecpt_builder& { m_ll = ll_; return *this; }
auto champsim::ecpt_builder::virtual_memory(VirtualMemory* vmem_) -> ecpt_builder& { m_vmem = vmem_; return *this; }

auto champsim::ecpt_builder::cwc_entries(uint32_t n) -> ecpt_builder& { m_cwc_entries = n; return *this; }
auto champsim::ecpt_builder::cwc_latency(uint32_t cycles) -> ecpt_builder& { m_cwc_latency = cycles; return *this; }
auto champsim::ecpt_builder::hash_latency(uint32_t cycles) -> ecpt_builder& { m_hash_latency = cycles; return *this; }
auto champsim::ecpt_builder::pte_table_entries(uint32_t n) -> ecpt_builder& { m_pte_table_entries = n; return *this; }
auto champsim::ecpt_builder::pmd_table_entries(uint32_t n) -> ecpt_builder& { m_pmd_table_entries = n; return *this; }

auto champsim::ecpt_builder::scaled_by_ul_size(double factor) const -> uint32_t
{
  return factor < 0 ? 0 : static_cast<uint32_t>(std::lround(factor * std::floor(std::size(m_uls))));
}
