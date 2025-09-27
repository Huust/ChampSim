#ifndef CXL_H
#define CXL_H

#include <cstddef>  // for size_t
#include <cstdint>  // for uint64_t, uint32_t, uint8_t
#include <deque>    // for deque
#include <limits>
#include <optional>

#include "address.h"
#include "channel.h"
#include "chrono.h"
#include "operable.h"

// CXL_CHANNEL simulates PCIe BUS between Host and Device CXL/PCIe interface
struct CXL_CHANNEL final: public champsim::operable {
  using response_type = typename champsim::channel::response_type;

  struct request_type {
    // finished: WQ only, if a write request has been
    // transferred from LLC to CXL controller, tag it as
    // finished so we can propagate it into dram
    bool finished = false;
    bool forward_checked = false;

    uint8_t asid[2] = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};

    uint32_t pf_metadata = 0;

    champsim::address address{};
    champsim::address v_address{};
    champsim::address data{};

    champsim::chrono::clock::time_point ready_time = champsim::chrono::clock::time_point::max();

    std::vector<uint64_t> instr_depend_on_me{};
    std::vector<std::deque<response_type>*> to_return{};
    
    // read/write requests from upper level (LLC) needs to convert from
    // champsim::channel::request_type -> CXL_CHANNEL::request_type -> champsim::channel::request_type
    // so reserve request from upper level
    champsim::channel::request_type raw_req;

    request_type() = default;  // Add default constructor
    explicit request_type(const typename champsim::channel::request_type& req);
  };
  
  using value_type = request_type;
  using queue_type = std::vector<std::optional<value_type>>;
  
  queue_type WQ;
  queue_type RQ;
  queue_type RespQ;

  queue_type::iterator active_rd_resp_on_bus{std::end(RespQ)};
  queue_type::iterator active_wr_req_on_bus{std::end(WQ)};

  // champsim::chrono::clock::time_point rd_bus_cycle_available{};
  // champsim::chrono::clock::time_point wr_bus_cycle_available{};

  champsim::channel *lower_level; // lower level points to channel between cxl controller and dram

  champsim::data::bytes channel_width;
  
  const champsim::chrono::clock::duration tCXL, tRD, tWR;

  std::vector<std::deque<response_type>*> upper_returns;  // 保存所有上层返回队列

  // CXL channel statistics
  struct cxl_channel_stats_type {
    std::string name{};
    uint64_t bus_cycles_rd_busy = 0;       // Read bus busy cycles
    uint64_t bus_cycles_wr_busy = 0;       // Write bus busy cycles
    uint64_t total_operating_cycles = 0;   // Total operating cycles
  };
  using stats_type = cxl_channel_stats_type;
  stats_type roi_stats, sim_stats;

  void check_collision();
  long finish_pcie_transfer();  // handle responses + handle_writes()
  long handle_writes();
  long handle_reads();
  long handle_responses();
  long populate_responses();

  void initialize();
  long operate();
  void begin_phase();
  void end_phase(unsigned cpu);
  void print_deadlock();

public:
  CXL_CHANNEL(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl,
              std::size_t rq_size, std::size_t wq_size, std::size_t respq_size,
              double rx_bw, double tx_bw, champsim::channel *ll, champsim::data::bytes width, std::vector<champsim::channel*>&& ul);
};

class CXL_CONTROLLER final: public champsim::operable {
  using channel_type = champsim::channel;
  using request_type = champsim::channel::request_type;
  using response_type = champsim::channel::response_type;

  std::vector<channel_type*> queues;  // upper level (more than one in some case)
  const champsim::data::bytes channel_width;  // at least pciex8, which is 8bits(1byte) width for uni-direction

  champsim::chrono::picoseconds cxl_io_period{};  // CXL_IO_FREQ
  
  double rx_bw, tx_bw;  // bandwidth from json config (GT/s), USED FOR PRINTING ONLY

public:
  struct cxl_stats_type {
    std::string name{};
    
    // CXL-specific statistics
    uint64_t bus_cycles_rd_busy = 0;       // Read bus busy cycles
    uint64_t bus_cycles_wr_busy = 0;       // Write bus busy cycles
    uint64_t total_operating_cycles = 0;   // Total operating cycles for utilization calculation
  };
  using stats_type = cxl_stats_type;
  stats_type roi_stats;
  stats_type sim_stats;
  
public:
  CXL_CHANNEL channel;
  
  CXL_CONTROLLER(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl,
                 std::vector<channel_type*>&& ul, std::size_t rq_size, std::size_t wq_size, std::size_t respq_size,
                 champsim::data::bytes chan_width, double rx_bw, double tx_bw, champsim::channel *ll);

  // inherit from operable
  void initialize();
  long operate();
  void begin_phase();
  void end_phase(unsigned cpu);
  void print_deadlock();

  // own function
  void initiate_requests();
  bool add_rq(const request_type& pkt, channel_type* ul);
  bool add_wq(const request_type& pkt);
};

#endif
