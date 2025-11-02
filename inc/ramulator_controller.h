#ifndef RAMULATOR_CONTROLLER_H
#define RAMULATOR_CONTROLLER_H

#include <cstdint>
#include <deque>
#include <unordered_map>

#include "address.h"
#include "champsim.h"
#include "channel.h"
#include "dram_controller.h"

// Ramulator includes
#include "ChampSimWrapper.h"

class RAMULATOR_CONTROLLER : public MEMORY_CONTROLLER
{
  using channel_type = champsim::channel;
  using request_type = typename channel_type::request_type;
  using response_type = typename channel_type::response_type;

  struct pending_request {
    champsim::address address;
    champsim::address v_address;
    champsim::address data;
    uint32_t pf_metadata;
    std::vector<uint64_t> instr_depend_on_me;
    std::vector<std::deque<response_type>*> to_return;
    champsim::chrono::clock::time_point arrival_time;
    bool is_write;
  };

  std::vector<channel_type*> queues;  // upper level request queue from different upper level channels
  std::unique_ptr<ramulator::ChampSimWrapper> ramulator_wrapper;

  // Track pending requests by block address
  std::unordered_map<uint64_t, pending_request> pending_reads;
  std::unordered_map<uint64_t, pending_request> pending_writes;

  // Time accumulator for frequency adaptation
  // Implements a "gearbox" to convert between ChampSim and Ramulator clock domains
  double m_accumulator = 0.0;  // Accumulated time in Ramulator tCK units
  double m_ratio = 1.0;        // Ratio of ChampSim clock to Ramulator tCK (how many tCK per operate() call)

  void initiate_requests();
  bool add_rq(const request_type& pkt, champsim::channel* ul);
  bool add_wq(const request_type& pkt);

  // Callback functions for Ramulator
  void read_callback(uint64_t block_addr);
  void write_callback(uint64_t block_addr);

  // Convert ChampSim address to block address
  uint64_t get_block_address(champsim::address addr) const;

public:
  RAMULATOR_CONTROLLER(champsim::chrono::picoseconds clock_period,
                       std::vector<channel_type*>&& ul,
                       const std::string& ramulator_config_path,
                       const std::string& stats_output_dir);

  void initialize() final;
  long operate() final;
  void begin_phase() final;
  void end_phase(unsigned cpu) final;
  void print_deadlock() final;

  void finish();

  [[nodiscard]] champsim::data::bytes size() const;
};

#endif // RAMULATOR_CONTROLLER_H
