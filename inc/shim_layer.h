#ifndef SHIM_H
#define SHIM_H

#include <cstddef>  // for size_t
#include <deque>    // for deque
#include <optional> // for gcc to compile

#include "address.h"
#include "channel.h"
#include "operable.h"
#include "bandwidth.h"

// Forward declarations
class MEMORY_CONTROLLER;
class CXL_CONTROLLER;

class SHIM_LAYER final: public champsim::operable {
  using channel_type = champsim::channel;
  using request_type = champsim::channel::request_type;
  using response_type = champsim::channel::response_type;

  channel_type* ul; // upper level, points to the channel between LLC and SHIM_LAYER
  std::vector<channel_type*> ll_queues;  // lower level (possible cxl + dram)

  using queue_type = std::vector<std::optional<request_type>>;
  queue_type WQ;
  queue_type RQ;
  queue_type PQ;
  std::deque<response_type> RespQ;  // No limited sizd

  // Memory controller pointers for size access
  MEMORY_CONTROLLER* dram_ptr = nullptr;        // pointer to DRAM controller, nullptr if not enabled
  MEMORY_CONTROLLER* cxl_ptr = nullptr;         // pointer to CXL DRAM controller, nullptr if not enabled

  enum class MODE {
    DRAM_ONLY,
    CXL_ONLY,
    HYBRID
  };
  MODE mode;

public:
  struct shim_stats {
    std::string name;
    
    uint64_t dram_requests_read = 0;
    uint64_t dram_requests_write = 0;
    uint64_t dram_requests_prefetch = 0;
    uint64_t dram_requests_total = 0;

    uint64_t cxl_requests_read = 0;
    uint64_t cxl_requests_write = 0;
    uint64_t cxl_requests_prefetch = 0;
    uint64_t cxl_requests_total = 0;

    uint64_t rq_full = 0;
    uint64_t wq_full = 0;
    uint64_t pq_full = 0;

    uint64_t upper_bw_congestion_cycles = 0;
    uint64_t lower_bw_congestion_cycles = 0;
  };
  using stats_type = shim_stats;
  shim_stats roi_stats{}, sim_stats{};

  // Set default port bandwidth
  champsim::bandwidth::maximum_type UPPER_STREAM_MAX_BW; // equal to LLC's bandwidth limit
  champsim::bandwidth::maximum_type LOWER_STREAM_MAX_BW;

public:
  SHIM_LAYER(champsim::chrono::picoseconds clock_period, champsim::channel *ul, std::vector<channel_type*>&& ll,
             std::size_t rq_size, std::size_t wq_size, std::size_t pq_size,
             long int max_upper_bw, long int max_lower_bw,
             MEMORY_CONTROLLER* dram_ptr, MEMORY_CONTROLLER* cxl_ptr
             );

  // own function
  long handle_responses();
  long route();
  long populate_requests();
  long warmup_fast_forward();
  MODE get_operate_mode(bool is_dram_enabled, bool is_cxl_enabled);

  // inherit from operable
  void initialize();
  long operate();
  void begin_phase();
  void end_phase(unsigned cpu);
  void print_deadlock();
};

#endif
