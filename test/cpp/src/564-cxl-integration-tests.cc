#include <catch.hpp>
#include "mocks.hpp"
#include "cxl_memory.h"

SCENARIO("CXL_CONTROLLER complete end-to-end functionality") {
    GIVEN("A complete CXL system with proper mock connections") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 6;
        const std::size_t rq_size = 16;
        const std::size_t wq_size = 16;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{4}; // 4 cycle DRAM latency
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          rq_size, wq_size, champsim::data::bytes{8}, 25.6, 25.6, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A diverse workload is processed through the complete system") {
            static uint64_t id = 1;
            const int num_requests = 6;
            
            // Issue diverse requests
            for (int i = 0; i < num_requests; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x100000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                auto result = mock_upper_level.issue(test);
                REQUIRE(result);
            }
            
            // Process through complete pipeline
            for (auto i = 0; i < 80; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Complete end-to-end processing succeeds") {
                // All requests should reach DRAM
                REQUIRE(mock_dram.packet_count() == num_requests);
                
                // All requests should get responses
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(num_requests));
                
                // Response timing should include CXL + DRAM latencies
                for (const auto& packet : mock_upper_level.packets) {
                    auto total_latency = packet.return_time - packet.issue_time;
                    REQUIRE(total_latency >= t_cxl + 4); // At least CXL + DRAM latency
                    REQUIRE(total_latency < 50); // But not excessively long
                }
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER system resilience under stress") {
    GIVEN("A CXL system under heavy load") {
        const auto clock_period = champsim::chrono::picoseconds{500};
        const std::size_t t_cxl = 3;
        
        to_rq_MRP mock_read_source;
        to_wq_MRP mock_write_source;
        do_nothing_MRC mock_dram{2};
        
        std::vector<champsim::channel*> upper_levels = {&mock_read_source.queues, &mock_write_source.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          32, 32, champsim::data::bytes{16}, 51.2, 51.2, &mock_dram.queues};
        
        std::array<champsim::operable*, 4> elements{{&uut, &mock_read_source, &mock_write_source, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Heavy mixed workload is applied") {
            static uint64_t id = 1;
            int total_reads_issued = 0;
            int total_writes_issued = 0;
            
            // Issue heavy mixed workload in bursts
            for (int burst = 0; burst < 4; ++burst) {
                // Issue read requests
                for (int i = 0; i < 6; ++i) {
                    decltype(mock_read_source)::request_type read_req;
                    read_req.address = champsim::address{static_cast<uint64_t>(0x800000 + burst * 1000 + i * 64)};
                    read_req.cpu = 0;
                    read_req.instr_id = id++;
                    read_req.type = access_type::LOAD;
                    read_req.is_translated = true;
                    
                    if (mock_read_source.issue(read_req)) {
                        total_reads_issued++;
                    }
                }
                
                // Issue write requests
                for (int i = 0; i < 4; ++i) {
                    decltype(mock_write_source)::request_type write_req;
                    write_req.address = champsim::address{static_cast<uint64_t>(0x900000 + burst * 1000 + i * 64)};
                    write_req.data = champsim::address{static_cast<uint64_t>(0x2000 + burst * 100 + i)};
                    write_req.cpu = 0;
                    write_req.instr_id = id++;
                    write_req.type = access_type::WRITE;
                    write_req.is_translated = true;
                    write_req.response_requested = false;
                    
                    if (mock_write_source.issue(write_req)) {
                        total_writes_issued++;
                    }
                }
                
                // Process some requests between bursts
                for (auto i = 0; i < 10; ++i) {
                    for (auto elem : elements) {
                        elem->_operate();
                    }
                }
            }
            
            // Complete processing of all requests
            for (auto i = 0; i < 120; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("System handles stress gracefully") {
                // All reads should get responses
                REQUIRE_THAT(mock_read_source.packets, Catch::Matchers::SizeIs(static_cast<size_t>(total_reads_issued)));
                
                // Writes should not generate responses
                REQUIRE_THAT(mock_write_source.packets, Catch::Matchers::SizeIs(0));
                
                // All requests should eventually reach DRAM
                REQUIRE(mock_dram.packet_count() == total_reads_issued + total_writes_issued);
                
                // Response timing should still be reasonable under stress
                for (const auto& packet : mock_read_source.packets) {
                    REQUIRE(packet.return_time > packet.issue_time);
                    REQUIRE(packet.return_time < packet.issue_time + 60); // Reasonable under stress
                }
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER phase management and statistics") {
    GIVEN("A CXL system with phase management") {
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 2, std::move(upper_levels), 
                          8, 8, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
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
                elem->warmup = false;
                elem->begin_phase();
            }
            
            // Access channel statistics through correct path
            auto initial_sim_cycles = uut.channel.sim_stats.total_operating_cycles;
            REQUIRE(initial_sim_cycles == 0); // Should reset on new phase
            
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
                auto sim_latency = mock_upper_level.packets[1].return_time - mock_upper_level.packets[1].issue_time;
                REQUIRE(sim_latency >= 2); // Should include CXL latency
            }
            
            THEN("Statistics are properly managed across phases") {
                REQUIRE(uut.channel.sim_stats.total_operating_cycles > initial_sim_cycles);
                
                // Test end_phase - controller aggregates channel stats
                uut.end_phase(0);
                // Note: CXL_CONTROLLER aggregates channel stats into its own stats in end_phase
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER deadlock detection and recovery") {
    GIVEN("A CXL system that could encounter deadlock conditions") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        // Very small queues to potentially create backpressure
        CXL_CONTROLLER uut{clock_period, 5, std::move(upper_levels), 
                          2, 2, champsim::data::bytes{4}, 8.0, 8.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("System is stressed with many requests") {
            static uint64_t id = 1;
            
            // Try to overwhelm the small queues
            for (int i = 0; i < 10; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0xc00000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
            }
            
            // Process with potential for backpressure
            for (auto i = 0; i < 50; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Deadlock detection doesn't crash the system") {
                REQUIRE_NOTHROW(uut.print_deadlock());
            }
            
            THEN("System remains functional despite small queues") {
                // Some requests should be processed (system shouldn't deadlock)
                REQUIRE(mock_dram.packet_count() > 0);
                REQUIRE(mock_upper_level.packets.size() > 0);
            }
        }
    }
}

TEST_CASE("CXL_CONTROLLER multi-upper-level integration") {
    // Test multiple upper level channels (like L2C + LLC both connecting to CXL)
    to_rq_MRP mock_l2c, mock_llc;
    do_nothing_MRC mock_dram{1};
    
    std::vector<champsim::channel*> upper_levels = {&mock_l2c.queues, &mock_llc.queues};
    CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 2, std::move(upper_levels), 
                      16, 16, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
    
    std::array<champsim::operable*, 4> elements{{&uut, &mock_l2c, &mock_llc, &mock_dram}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    static uint64_t id = 1;
    
    // Issue from L2C
    decltype(mock_l2c)::request_type l2c_req;
    l2c_req.address = champsim::address{0xd00000};
    l2c_req.cpu = 0;
    l2c_req.instr_id = id++;
    l2c_req.type = access_type::LOAD;
    l2c_req.is_translated = true;
    
    // Issue from LLC
    decltype(mock_llc)::request_type llc_req;
    llc_req.address = champsim::address{0xe00000};
    llc_req.cpu = 0;
    llc_req.instr_id = id++;
    llc_req.type = access_type::LOAD;
    llc_req.is_translated = true;
    
    mock_l2c.issue(l2c_req);
    mock_llc.issue(llc_req);
    
    // Process both channels
    for (auto i = 0; i < 30; ++i) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    // Both upper levels should get their responses
    REQUIRE_THAT(mock_l2c.packets, Catch::Matchers::SizeIs(1));
    REQUIRE_THAT(mock_llc.packets, Catch::Matchers::SizeIs(1));
    REQUIRE(mock_dram.packet_count() == 2);
    
    // Responses should go back to correct upper levels
    REQUIRE(mock_l2c.packets[0].pkt.address == champsim::address{0xd00000});
    REQUIRE(mock_llc.packets[0].pkt.address == champsim::address{0xe00000});
}

SCENARIO("CXL_CONTROLLER with DRAM memory collision handling") {
    GIVEN("A CXL system where DRAM handles address collisions") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 3;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{3}; // DRAM will handle collision detection
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          16, 16, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Multiple requests to similar addresses are sent through CXL") {
            static uint64_t id = 1;
            
            // Issue requests to addresses that might collide at DRAM level
            // CXL forwards them all, DRAM handles any collision detection needed
            for (int i = 0; i < 4; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x100000 + i * 16)}; // Close addresses
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                auto result = mock_upper_level.issue(test);
                REQUIRE(result);
            }
            
            // Process through CXL to DRAM
            for (auto i = 0; i < 50; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("CXL forwards all requests to DRAM for collision handling") {
                // CXL should forward all requests - collision detection is DRAM's responsibility
                REQUIRE(mock_dram.packet_count() >= 4); // All should reach DRAM
                
                // All should eventually get responses
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(4));
                
                // CXL timing should still be enforced
                for (const auto& packet : mock_upper_level.packets) {
                    auto latency = packet.return_time - packet.issue_time;
                    REQUIRE(latency >= t_cxl); // CXL latency preserved
                }
            }
        }
    }
}
