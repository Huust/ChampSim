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
#include <cmath>
#include "dram_controller.h"

class CXL_CONTROLLER : public MEMORY_CONTROLLER {
  using channel_type = champsim::channel;
  using request_type = typename channel_type::request_type;
  using response_type = typename channel_type::response_type;

  public:
    const std::string NAME;
    const static uint64_t tCXL = static_cast<uint64_t>(std::ceil(1.0 * tCXL_DRAM_NANOSECONDS * CXL_IO_FREQ / 1000));
    
    const static uint64_t tRD = static_cast<uint64_t>(std::ceil(1.0 * BLOCK_SIZE*8.0/(CXL_RD_BW*CXL_RD_CH_WIDTH_BITS) 
                                                    * CXL_IO_FREQ / 1000));
    const static uint64_t tWR = static_cast<uint64_t>(std::ceil(1.0 * BLOCK_SIZE*8.0/(CXL_WR_BW*CXL_WR_CH_WIDTH_BITS) 
                                                    * CXL_IO_FREQ / 1000));
    struct CXL_BUS {
      void* active_rd_resp_on_bus = NULL;
      void* active_wr_req_on_bus = NULL;
      uint64_t rd_bus_cycle_available = 0, wr_bus_cycle_available = 0;
      bool rd_pkt_valid = false, wr_pkt_valid = false;
    };
    struct CXL_CHANNEL {
      channel_type RQ{CXL_RQ_SIZE, 0, CXL_WQ_SIZE, 16, false}
      
      struct CXL_BUS bus = {};
      uint64_t s_reads = 0, s_writes = 0, s_resps = 0, s_read_dup_merged = 0, s_rd_to_wr_forward = 0,
              s_rq_full = 0, s_wr_dup_merged = 0, s_wq_full = 0, s_rdbus_cycles_congested = 0, 
              s_rdbus_cycles_occ = 0, s_wr_dram_wq_full_retry = 0, s_wrbus_cycles_occ = 0,
              s_wrbus_cycles_congested = 0, s_rd_dram_rq_full_retry = 0, s_rd_tot_qdelay = 0,
              s_wr_tot_qdelay = 0;
    };

    uint64_t s_cycles = 0;
    uint64_t RQ_ACCESS=0;

    std::array<struct CXL_CHANNEL, CXL_CHANNELS> channels;

    int add_rq(void* packet);
    int add_wq(void* packet);
    int add_pq(void* packet);
    void return_data(void* packet);

    void operate_bus();
    void operate_channel();

    void handle_responses();
    void handle_writes();
    void handle_reads();

    uint32_t get_channel(uint64_t address);

    void PrintStats();
    void ResetStats();
};

#endif
