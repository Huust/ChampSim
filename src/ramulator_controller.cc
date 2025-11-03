#include "ramulator_controller.h"

#include <cassert>
#include <cstdint>
#include <fmt/core.h>

#include "champsim.h"
#include "util/span.h"
#include "Request.h"

RAMULATOR_CONTROLLER::RAMULATOR_CONTROLLER(champsim::chrono::picoseconds cpu_clock_period,
                                           std::vector<channel_type*>&& ul,
                                           const std::string& ramulator_config_path,
                                           const std::string& stats_output_dir)
    : MEMORY_CONTROLLER(
        // Parameters below are placeholders and won't be used by Ramulator
        champsim::chrono::picoseconds{416}, champsim::chrono::picoseconds{833},
        std::size_t{24}, std::size_t{24}, std::size_t{24}, std::size_t{52},
        champsim::chrono::microseconds{32000}, {nullptr},
        64, 64, 1, champsim::data::bytes{8}, 65536, 1024, 1, 8, 4, 8192
      ), queues(std::move(ul))
{
  // Initialize Ramulator configuration
  ramulator::Config configs(ramulator_config_path.c_str());
  configs.set_core_num(NUM_CPUS);
  configs.add("stats_dir", stats_output_dir);

  // Initialize Ramulator wrapper with configuration
  // With wrapper, we don't need to care about what specifc memory type we are using,
  // instead we can directly use the universal interface provided in ChampSimWrapper (e.g. tick(), send(), finish()...)
  ramulator_wrapper = std::make_unique<ramulator::ChampSimWrapper>(configs, BLOCK_SIZE);

  // Get Ramulator's tCK (one memory clock cycle in nanoseconds)
  double ramulator_tck_ns = ramulator_wrapper->get_tCK();
  double ramulator_tck_ps = ramulator_tck_ns * 1000.0;  // Convert to picoseconds

  // Calculate the ratio: how many Ramulator tCKs occur per operate() call
  // m_ratio = cpu_clock_period (ps) / ramulator_tCK (ps)
  // Example: if cpu_clock_period = 250 ps (4000 MHz), ramulator_tCK = 833 ps (1200 MHz)
  //          m_ratio = 250 / 833 = 0.3 (means 0.3 DRAM cycles per CPU cycle)
  m_ratio = cpu_clock_period.count() / ramulator_tck_ps;

  // Print configuration info
  double champsim_freq_mhz = 1000000.0 / clock_period.count();  // ps to MHz
  double ramulator_freq_mhz = 1000.0 / ramulator_tck_ns;        // ns to MHz

  fmt::print("[RAMULATOR] Frequency: {:.1f} MHz\n", ramulator_freq_mhz);
}

void RAMULATOR_CONTROLLER::initialize()
{
  fmt::print("Using Ramulator for simulation\n");
  using namespace champsim::data::data_literals;
  using namespace std::literals::chrono_literals;
  auto sz = this->size();
  if (champsim::data::gibibytes gb_sz{sz}; gb_sz > 1_GiB) {
    fmt::print("Off-chip DRAM Size: {}\n", gb_sz);
  } else if (champsim::data::mebibytes mb_sz{sz}; mb_sz > 1_MiB) {
    fmt::print("Off-chip DRAM Size: {}\n", mb_sz);
  } else if (champsim::data::kibibytes kb_sz{sz}; kb_sz > 1_kiB) {
    fmt::print("Off-chip DRAM Size: {}\n", kb_sz);
  } else {
    fmt::print("Off-chip DRAM Size: {}\n", sz);
  }
}

long RAMULATOR_CONTROLLER::operate()
{
  long progress = 0;

  // Add the time slice for this operate() call
  m_accumulator += m_ratio;

  // Tick Ramulator for each full tCK that has accumulated
  // Here we assume that CPU frequency is higher than memory frequency (so m_ratio < 1)
  if (m_accumulator >= 1.0) {
    initiate_requests();
    ramulator_wrapper->tick();
    m_accumulator -= 1.0;
    assert(m_accumulator < 1.0);  // m_ratio shoule be lower than 1
    progress++;
  }

  return progress;
}

void RAMULATOR_CONTROLLER::initiate_requests()
{
  // Initiate read requests
  for (auto* ul : queues) {
    for (auto q : {std::ref(ul->RQ), std::ref(ul->PQ)}) {
      auto [begin, end] = champsim::get_span_p(std::cbegin(q.get()), std::cend(q.get()), [ul, this](const auto& pkt) { return this->add_rq(pkt, ul); });
      q.get().erase(begin, end);
    }

    // Initiate write requests
    auto [wq_begin, wq_end] = champsim::get_span_p(std::cbegin(ul->WQ), std::cend(ul->WQ), [this](const auto& pkt) { return this->add_wq(pkt); });
    ul->WQ.erase(wq_begin, wq_end);
  } 
}

bool RAMULATOR_CONTROLLER::add_rq(const request_type& packet, champsim::channel* ul)
{
  uint64_t block_addr = champsim::block_number{packet.address}.to<uint64_t>();

  // Create Ramulator request with callback
  ramulator::Request req(packet.address.to<uint64_t>(), ramulator::Request::Type::READ,
                         [this, block_addr](ramulator::Request& req) {
                           this->read_callback(block_addr);
                         },
                         packet.cpu);

  // Try to send request to Ramulator
  if (ramulator_wrapper->send(req)) {
    // Store pending request info for callback
    pending_request pending_read;
    pending_read.address = packet.address;
    pending_read.v_address = packet.v_address;
    pending_read.data = packet.data;
    pending_read.pf_metadata = packet.pf_metadata;
    pending_read.instr_depend_on_me = packet.instr_depend_on_me;
    pending_read.arrival_time = current_time;
    pending_read.is_write = false;

    if (packet.response_requested) {
      pending_read.to_return = {&ul->returned};
    }

    pending_reads[block_addr] = std::move(pending_read);

    return true;
  }

  return false;
}

bool RAMULATOR_CONTROLLER::add_wq(const request_type& packet)
{
  uint64_t block_addr = champsim::block_number{packet.address}.to<uint64_t>();

  // Create Ramulator write request with callback
  ramulator::Request req(packet.address.to<uint64_t>(), ramulator::Request::Type::WRITE,
                         [this, block_addr](ramulator::Request& req) {
                           this->write_callback(block_addr);
                         },
                         packet.cpu);

  // Try to send request to Ramulator
  if (ramulator_wrapper->send(req)) {
    // Store pending request info for callback
    pending_request pending_write;
    pending_write.address = packet.address;
    pending_write.v_address = packet.v_address;
    pending_write.data = packet.data;
    pending_write.pf_metadata = packet.pf_metadata;
    pending_write.instr_depend_on_me = packet.instr_depend_on_me;
    pending_write.arrival_time = current_time;
    pending_write.is_write = true;

    pending_writes[block_addr] = std::move(pending_write);

    return true;
  }

  return false;
}

void RAMULATOR_CONTROLLER::read_callback(uint64_t block_addr)
{
  auto it = pending_reads.find(block_addr);
  if (it != pending_reads.end()) {
    const auto& pending = it->second;

    // Create response
    response_type response{pending.address, pending.v_address, pending.data,
                          pending.pf_metadata, pending.instr_depend_on_me};

    // Send response to all requesters
    for (auto* ret : pending.to_return) {
      ret->push_back(response);
    }

    // Remove from pending requests
    pending_reads.erase(it);
  } else {
    fmt::print("[RAMULATOR PANIC] Read request not found in pending_reads! Address: {:#x}\n", block_addr);
    assert(false);
  }
}

void RAMULATOR_CONTROLLER::write_callback(uint64_t block_addr)
{
  auto it = pending_writes.find(block_addr);
  if (it != pending_writes.end()) {
    // Write completed, just remove from pending requests
    // No response needed for writes in ChampSim
    pending_writes.erase(it);
  } else {
    fmt::print("[RAMULATOR PANIC] Write request not found in pending_writes! Address: {:#x}\n", block_addr);
    assert(false);
  }
}

void RAMULATOR_CONTROLLER::begin_phase()
{
  ramulator_wrapper->resetStats();
}

void RAMULATOR_CONTROLLER::end_phase(unsigned /*cpu*/)
{
  // Note: finish() is no longer called here to control output order.
  // Statistics will be printed manually in main.cc after ChampSim stats.
  // The warmup check is preserved for future reference.
}

void RAMULATOR_CONTROLLER::print_ramulator_stats()
{
  // Manually trigger Ramulator statistics output
  // This is called from main.cc after ChampSim statistics are printed
  if (!warmup) {
    ramulator_wrapper->finish();
  }
}

champsim::data::bytes RAMULATOR_CONTROLLER::size() const
{
  // Get actual memory size from Ramulator
  // max_address is calculated from DRAM organization (channels * ranks * banks * rows * columns * width)
  return champsim::data::bytes{ramulator_wrapper->get_capacity()};
}

void RAMULATOR_CONTROLLER::print_deadlock()
{
  fmt::print("Ramulator Memory Controller Deadlock Info:\n");
  fmt::print("Pending reads: {}\n", pending_reads.size());
  fmt::print("Pending writes: {}\n", pending_writes.size());

  // Print some pending request details
  int count = 0;
  for (const auto& [addr, req] : pending_reads) {
    fmt::print("  Pending read {}: addr={:#x}, time={}\n", count++, req.address.to<uint64_t>(), req.arrival_time.time_since_epoch().count());
    if (count >= 10)
      break; // Limit output
  }

  count = 0;
  for (const auto& [addr, req] : pending_writes) {
    fmt::print("  Pending write {}: addr={:#x}, time={}\n", count++, req.address.to<uint64_t>(), req.arrival_time.time_since_epoch().count());
    if (count >= 10)
      break; // Limit output
  }
}
