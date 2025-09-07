#ifndef SHIM_H
#define SHIM_H

#include <cstddef>  // for size_t
#include <deque>    // for deque

#include "address.h"
#include "channel.h"
#include "operable.h"

struct SHIM_LAYER final: public champsim::operable {
  enum class MODE {
    DRAM_ONLY,
    CXL_ONLY,
    HYBRID
  };
  MODE mode;

  using channel_type = champsim::channel;
  using request_type = champsim::channel::request_type;
  using response_type = champsim::channel::response_type;
  
  using queue_type = std::deque<request_type>;
  queue_type WQ;
  queue_type RQ;
  queue_type PQ;
  std::deque<response_type> RespQ;

  channel_type* ul; // upper level, points to the channel between LLC and SHIM_LAYER
  std::vector<channel_type*> ll_queues;  // lower level (possible cxl + dram)

public:
  SHIM_LAYER(std::vector<channel_type*>&& ll, std::size_t rq_size,
             std::size_t wq_size, champsim::channel *ul,
             std::size_t enable_dram, std::size_t enable_cxl);

  // own function
  long handle_responses();
  long route();
  long populate_requests();
  MODE get_operate_mode(std::size_t enable_dram, std::size_t enable_cxl);

  // inherit from operable
  void initialize();
  long operate();
  void begin_phase();
  void end_phase(unsigned cpu);
  void print_deadlock();
};

#endif
