#include "cxl_memory.h"

#ifndef CXL_H
#define CXL_H
#include <array>
#include <cmath>
#include <cstddef>  // for size_t
#include <cstdint>  // for uint64_t, uint32_t, uint8_t
#include <deque>    // for deque
#include <iterator> // for end
#include <limits>
#include <optional>
#include <string>

#include "address.h"
#include "channel.h"
#include "chrono.h"
#include "dram_stats.h"
#include "extent_set.h"
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

    explicit request_type(const typename champsim::channel::request_type& req);
  };
  using value_type = request_type;
  using queue_type = std::vector<std::optional<value_type>>;
  queue_type WQ;
  queue_type RQ;
  queue_type RespQ;

  queue_type::iterator active_rd_resp_on_bus;
  queue_type::iterator active_wr_req_on_bus;

  champsim::data::bytes channel_width;
  
  const champsim::chrono::clock::duration tCXL, tRD, tWR;
  
  CXL_CHANNEL(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl, champsim::data::bytes width,
              std::size_t rq_size, std::size_t wq_size, double rx_bw, double tx_bw);

  void check_collision();
  long finish_pcie_transfer();
  long schedule_refresh();
  void swap_write_mode();
  long populate_dbus();
  queue_type::iterator schedule_packet();
  long service_packet(queue_type::iterator pkt);

  void initialize();
  long operate();
  void begin_phase();
  void end_phase(unsigned cpu);
  void print_deadlock();
};

class CXL_CONTROLLER final: public champsim::operable {
  using channel_type = champsim::channel;
  using request_type = champsim::channel::request_type;
  using response_type = champsim::channel::response_type;

  std::vector<channel_type*> queues;  // upper level (more than one in some case)
  const champsim::data::bytes channel_width;  // at least pciex8, which is 8bits(1byte) width for uni-direction

  champsim::chrono::picoseconds cxl_io_period{};  // CXL_IO_FREQ

public:
  CXL_CHANNEL channel;
  
  CXL_CONTROLLER(champsim::chrono::picoseconds cxl_io_period, std::size_t t_cxl,
                 std::vector<channel_type*>&& ul, std::size_t rq_size, std::size_t wq_size,
                 champsim::data::bytes chan_width, double rx_bw, double tx_bw);

  // inherit from operable
  void initialize();
  long operate();
  void begin_phase();
  void end_phase(unsigned cpu);
  void print_deadlock();
  long initiate_requests();
};

#endif
