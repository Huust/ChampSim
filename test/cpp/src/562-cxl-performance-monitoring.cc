#include <catch.hpp>
#include <catch2/catch_test_macros.hpp>
#include "mocks.hpp"
#include "cxl_memory.h"

/*
 * IMPORTANT: This test requires modified mock code to pass correctly.
 * 
 * Standard to_rq_MRP always routes to RQ regardless of access_type.
 * For mixed read/write testing, to_rq_MRP::issue() must route based on access_type:
 * - WRITE requests → WQ  
 * - READ/PREFETCH requests → RQ
 * 
 * Without this modification, write requests incorrectly enter RQ, causing 
 * wrong pipeline behavior and statistics.
 */
SCENARIO("Bus bandwidth utilization tracking") {
    GIVEN("A CXL controller that tracks bus utilization") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 5;
        const std::size_t dram_latency = 3;
        constexpr auto rx_bw = 16.0, tx_bw = 16.0;
        auto t_rd = static_cast<long>(std::ceil(BLOCK_SIZE/rx_bw));
        
        to_rq_MRP mock_ul;  // Unified source for both read and write requests
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_ul.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, 8, champsim::data::bytes{8}, rx_bw, tx_bw, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }

        WHEN("Mixed read and write operations are performed") {
            // Access channel statistics through correct path
            auto initial_total_cycles = uut.channel.sim_stats.total_operating_cycles;
            auto initial_rd_busy_cycles = uut.channel.sim_stats.bus_cycles_rd_busy;
            auto initial_wr_busy_cycles = uut.channel.sim_stats.bus_cycles_wr_busy;
            
            static uint64_t id = 1;
            
            // Issue some read requests
            for (int i = 0; i < 5; ++i) {
                decltype(mock_ul)::request_type read_req;
                read_req.address = champsim::address{static_cast<uint64_t>(0x300000 + i * 64)};
                read_req.cpu = 0;
                read_req.instr_id = id++;
                read_req.type = access_type::LOAD;
                read_req.is_translated = true;
                read_req.response_requested = true;

                mock_ul.issue(read_req);
            }

            // Issue some prefetch requests
            for (int i = 0; i < 6; ++i) {
                decltype(mock_ul)::request_type read_req;
                read_req.address = champsim::address{static_cast<uint64_t>(0x400000 + i * 64)};
                read_req.cpu = 0;
                read_req.instr_id = id++;
                read_req.type = access_type::PREFETCH;
                read_req.is_translated = true;
                read_req.response_requested = true;

                mock_ul.issue(read_req);
            }
            
            // Issue some write requests
            for (int i = 0; i < 7; ++i) {
                decltype(mock_ul)::request_type write_req;
                write_req.address = champsim::address{static_cast<uint64_t>(0x500000 + i * 64)};
                write_req.data = champsim::address{static_cast<uint64_t>(0x1000 + i)};
                write_req.cpu = 0;
                write_req.instr_id = id++;
                write_req.type = access_type::WRITE;
                write_req.is_translated = true;
                write_req.response_requested = false;
                
                mock_ul.issue(write_req);
            }
            
            // Process operations to generate bus activity
            for (auto i = 0; i < 200; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Statistics are updated") {
                REQUIRE(uut.channel.sim_stats.total_operating_cycles > initial_total_cycles);
                REQUIRE(uut.channel.sim_stats.bus_cycles_rd_busy > initial_rd_busy_cycles);
                REQUIRE(uut.channel.sim_stats.bus_cycles_wr_busy > initial_wr_busy_cycles);
            }
            
            THEN("All requests are processed through CXL") {
                REQUIRE(mock_dram.packet_count() == 18);  // 5 reads + 6 prefetches + 7 writes

                // Check read and prefetch responses (writes don't generate responses)
                int read_responses = 0;
                for (const auto& pkt : mock_ul.packets) {
                    if (pkt.return_time > 0) {  // Only count actual responses
                        read_responses++;
                        REQUIRE_THAT(pkt, champsim::test::LatencyRangeMatcher(
                            {t_cxl, dram_latency, t_cxl, t_rd},
                            {std::numeric_limits<long>::max()}));
                    }
                }
                REQUIRE(read_responses == 11);  // 5 reads + 6 prefetches
            }
        }
    }
}

SCENARIO("Phase transition and statistics management") {
    GIVEN("A CXL system with phase management") {
        constexpr auto t_cxl = 2;
        constexpr auto dram_latency = 2;
        constexpr auto rx_bw = 32.0, tx_bw = 32.0;
        auto t_rd = static_cast<long>(std::ceil(BLOCK_SIZE/rx_bw));

        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, t_cxl, std::move(upper_levels), 
                          8, 8, 8, champsim::data::bytes{8}, rx_bw, tx_bw, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
        }
        
        WHEN("System operates through warmup and simulation phases") {
            static uint64_t id = 1;
            
            // Start in warmup phase
            for (auto elem : elements) {
                elem->warmup = true;
                elem->begin_phase();
            }
            
            // Issue request during warmup
            decltype(mock_upper_level)::request_type warmup_req;
            warmup_req.address = champsim::address{0xa00000};
            warmup_req.cpu = 0;
            warmup_req.instr_id = id++;
            warmup_req.type = access_type::LOAD;
            warmup_req.is_translated = true;
            
            mock_upper_level.issue(warmup_req);
            
            // Process during warmup
            for (auto i = 0; i < 10; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Warmup phase processes requests immediately") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(1));
                auto warmup_latency = mock_upper_level.packets[0].return_time - mock_upper_level.packets[0].issue_time;
                REQUIRE(warmup_latency < 5); // Very fast in warmup
            }
            
            // Switch to simulation phase
            for (auto elem : elements) {
                elem->end_phase(0); // cpu 0
                elem->warmup = false;
                elem->begin_phase();
            }
            
            // Access channel statistics through correct path
            auto initial_sim_cycles = uut.channel.sim_stats.total_operating_cycles;
            auto initial_sim_rd_busy_cycles = uut.channel.sim_stats.bus_cycles_rd_busy;
            auto initial_sim_wr_busy_cycles = uut.channel.sim_stats.bus_cycles_wr_busy;
            REQUIRE(initial_sim_cycles == 0); // Should reset on new phase
            REQUIRE(initial_sim_rd_busy_cycles == 0);
            REQUIRE(initial_sim_wr_busy_cycles == 0);
            
            // Issue request during simulation
            decltype(mock_upper_level)::request_type sim_req;
            sim_req.address = champsim::address{0xb00000};
            sim_req.cpu = 0;
            sim_req.instr_id = id++;
            sim_req.type = access_type::LOAD;
            sim_req.is_translated = true;
            
            mock_upper_level.issue(sim_req);
            
            // Process during simulation
            for (auto i = 0; i < 20; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Simulation phase has normal timing") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(2)); // warmup + sim
                REQUIRE_THAT(mock_upper_level.packets[1], champsim::test::LatencyRangeMatcher(
                    {t_cxl, dram_latency, t_cxl, t_rd},
                    {std::numeric_limits<long>::max()}));
            }
            
            THEN("Statistics are properly managed across phases") {
                REQUIRE(uut.channel.sim_stats.total_operating_cycles > initial_sim_cycles);
                REQUIRE(uut.channel.sim_stats.bus_cycles_rd_busy > initial_sim_rd_busy_cycles);
                
                // Test end_phase - controller aggregates channel stats
                uut.end_phase(0);
                // Note: CXL_CONTROLLER aggregates channel stats into its own stats in end_phase
            }
        }
    }
}
