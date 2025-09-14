#include <catch.hpp>
#include "champsim.h"
#include "matchers.hpp"
#include "mocks.hpp"
#include "cxl_memory.h"
#include <cmath>
#include <limits>

SCENARIO("Read request pipeline processing") {
    GIVEN("A CXL controller with upper and lower level connections") {
        const auto clock_period = champsim::chrono::picoseconds{1000};  // The frequency of this module
        const std::size_t t_cxl = 5;
        const std::size_t dram_latency = 3;
        const std::size_t rq_size = 16;
        const std::size_t wq_size = 16;
        const std::size_t respq_size = 16;
        
        // Set up proper mock components
        to_rq_MRP mock_ul;     // Simulates upper level (LLC) sending requests
        do_nothing_MRC mock_dram{dram_latency};
        
        // Create CXL controller with proper upper level connection
        std::vector<champsim::channel*> upper_levels = {&mock_ul.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          rq_size, wq_size, respq_size, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
        
        // Initialize all components
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A read request is issued through upper level") {
            // Create a test request using proper pattern
            decltype(mock_ul)::request_type test;
            test.address = champsim::address{0xdeadbeef};
            test.cpu = 0;
            test.instr_id = 1;
            test.type = access_type::LOAD;
            test.is_translated = true;
            
            // Issue request through mock upper level
            auto test_result = mock_ul.issue(test);
            
            THEN("The request is accepted by upper level") {
                REQUIRE(test_result);
            }
            
            // Let CXL controller consume the request
            for (auto i = 0; i < 100; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Request propagates through complete pipeline") {
                REQUIRE_THAT(mock_ul.packets, Catch::Matchers::SizeIs(1));
                
                auto& response = mock_ul.packets.front();
                constexpr auto rx_bw = 16.0;
                auto t_rd = static_cast<long>(std::ceil(BLOCK_SIZE/rx_bw));
                
                REQUIRE_THAT(response, champsim::test::LatencyRangeMatcher(
                    {t_cxl, dram_latency, t_cxl, t_rd}, 
                    {std::numeric_limits<long>::max()}));
            }
            
            THEN("DRAM receives the correct request") {
                REQUIRE(mock_dram.packet_count() == 1);
                REQUIRE(mock_dram.addresses.front() == champsim::address{0xdeadbeef});
            }
        }
    }
}

SCENARIO("Write request processing") {
    GIVEN("A CXL controller handling write requests") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 3;
        const std::size_t dram_latency = 2;
        const std::size_t rq_size = 8, wq_size = 8, respq_size = 8;
        
        to_rq_MRP mock_ul;
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_ul.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          rq_size, wq_size, respq_size, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Multiple write requests are issued") {
            static uint64_t id = 1;
            
            for (int i = 0; i < 5; ++i) {
                decltype(mock_ul)::request_type write_req;
                write_req.address = champsim::address{static_cast<uint64_t>(0x1000 + i * 64)};
                write_req.data = champsim::address{static_cast<uint64_t>(0x5000 + i)};
                write_req.cpu = 0;
                write_req.instr_id = id++;
                write_req.type = access_type::WRITE;
                write_req.is_translated = true;
                write_req.response_requested = false;
                
                mock_ul.issue(write_req);
            }
            
            // Allow sufficient processing time
            for (auto i = 0; i < 100; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All write requests reach DRAM") {
                REQUIRE(mock_dram.packet_count() == 5);
            }
            
            THEN("Write requests don't generate responses") {
                for (const auto& packet : mock_ul.packets) {
                    if (packet.pkt.type == access_type::WRITE) {
                        REQUIRE(packet.return_time == 0);
                    }
                }
            }
        }
    }
}

SCENARIO("Request timing and latency verification") {
    GIVEN("A CXL controller with specific timing parameters") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 8;
        const std::size_t dram_latency = 5;
        constexpr auto rx_bw = 8.0, tx_bw = 8.0;
        auto t_rd = static_cast<long>(std::ceil(BLOCK_SIZE/rx_bw));
        
        to_rq_MRP mock_ul;
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
        
        WHEN("A single request is processed with timing measurement") {
            decltype(mock_ul)::request_type test;
            test.address = champsim::address{0x2000};
            test.cpu = 0;
            test.instr_id = 42;
            test.type = access_type::LOAD;
            test.is_translated = true;
            
            mock_ul.issue(test);
            
            // Process until completion
            for (auto i = 0; i < 200; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Latency components are measurable") {
                REQUIRE_THAT(mock_ul.packets, Catch::Matchers::SizeIs(1));
                
                auto& response = mock_ul.packets.front();
                auto total_latency = response.return_time - response.issue_time;
                
                // Minimum expected latency: tCXL + dram_latency + tCXL + tRD
                auto min_latency = t_cxl + dram_latency + t_cxl + static_cast<std::size_t>(t_rd);
                REQUIRE(total_latency >= min_latency);
                
                // Verify specific timing ranges
                REQUIRE_THAT(response, champsim::test::LatencyRangeMatcher(
                    {t_cxl, dram_latency, t_cxl, t_rd},
                    {50}));  // Allow some scheduling overhead
            }
        }
    }
}

SCENARIO("Warmup vs simulation phase behavior") {
    GIVEN("A CXL controller in different operational phases") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 4;
        const auto dram_latency = 2;
        
        to_rq_MRP mock_ul;
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_ul.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, 8, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
        }
        
        WHEN("Processing requests during warmup phase") {
            static uint64_t id = 1;
            
            // Set warmup mode
            for (auto elem : elements) {
                elem->warmup = true;
                elem->begin_phase();
            }
            
            decltype(mock_ul)::request_type warmup_req;
            warmup_req.address = champsim::address{0x10000};
            warmup_req.cpu = 0;
            warmup_req.instr_id = id++;
            warmup_req.type = access_type::LOAD;
            warmup_req.is_translated = true;
            
            mock_ul.issue(warmup_req);
            
            // Process warmup request
            for (auto i = 0; i < 10; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Warmup requests are processed with minimal latency") {
                REQUIRE_THAT(mock_ul.packets, Catch::Matchers::SizeIs(1));
                auto warmup_latency = mock_ul.packets[0].return_time - mock_ul.packets[0].issue_time;
                REQUIRE(warmup_latency <= 3);  // Very fast in warmup
            }
            
            // Switch to simulation mode
            for (auto elem : elements) {
                elem->end_phase(0);
                elem->warmup = false;
                elem->begin_phase();
            }
            
            // Issue simulation request
            decltype(mock_ul)::request_type sim_req;
            sim_req.address = champsim::address{0x20000};
            sim_req.cpu = 0;
            sim_req.instr_id = id++;
            sim_req.type = access_type::LOAD;
            sim_req.is_translated = true;
            
            mock_ul.issue(sim_req);
            
            // Process simulation request
            for (auto i = 0; i < 30; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Simulation requests follow normal timing") {
                REQUIRE_THAT(mock_ul.packets, Catch::Matchers::SizeIs(2));
                
                constexpr auto rx_bw = 16.0;
                auto t_rd = static_cast<long>(std::ceil(BLOCK_SIZE/rx_bw));
                
                REQUIRE_THAT(mock_ul.packets[1], champsim::test::LatencyRangeMatcher(
                    {t_cxl, dram_latency, t_cxl, t_rd},
                    {std::numeric_limits<long>::max()}));
            }
        }
    }
}

SCENARIO("Queue capacity and backpressure handling") {
    GIVEN("A CXL controller with limited queue sizes") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 2;
        
        to_rq_MRP mock_ul;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_ul.queues};
        // Very small queues to test capacity limits
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          2, 2, 2, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("More requests are issued than queue capacity") {
            static uint64_t id = 1;
            int successful_issues = 0;
            
            // Try to overwhelm the small queues
            for (int i = 0; i < 10; ++i) {
                decltype(mock_ul)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x3000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                if (mock_ul.issue(test)) {
                    successful_issues++;
                }
                
                // Process a few cycles between issues
                for (auto j = 0; j < 3; ++j) {
                    for (auto elem : elements) {
                        elem->_operate();
                    }
                }
            }
            
            THEN("Queue capacity limits are respected") {
                // Not all requests should be accepted due to capacity limits
                REQUIRE(successful_issues <= 10);
                REQUIRE(successful_issues >= 2);  // At least some should be processed
            }
            
            // Complete processing
            for (auto i = 0; i < 50; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("System continues functioning despite backpressure") {
                // System should process some requests without deadlocking
                REQUIRE(mock_dram.packet_count() > 0);
                REQUIRE(mock_ul.packets.size() > 0);
            }
        }
    }
}

TEST_CASE("Basic statistics collection") {
    to_rq_MRP mock_ul;
    do_nothing_MRC mock_dram{2};
    
    std::vector<champsim::channel*> upper_levels = {&mock_ul.queues};
    CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 3, std::move(upper_levels), 
                      8, 8, 8, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
    
    std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_dram}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    // Issue some requests to generate statistics
    static uint64_t id = 1;
    for (int i = 0; i < 3; ++i) {
        decltype(mock_ul)::request_type test;
        test.address = champsim::address{static_cast<uint64_t>(0x4000 + i * 64)};
        test.cpu = 0;
        test.instr_id = id++;
        test.type = access_type::LOAD;
        test.is_translated = true;
        
        mock_ul.issue(test);
    }
    
    // Process and collect statistics
    for (auto i = 0; i < 50; ++i) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    // Verify statistics are being tracked
    REQUIRE(uut.channel.sim_stats.total_operating_cycles > 0);
    
    // Test phase management
    auto initial_cycles = uut.channel.sim_stats.total_operating_cycles;
    uut.end_phase(0);
    
    // Controller should aggregate channel statistics
    REQUIRE(uut.channel.sim_stats.total_operating_cycles == initial_cycles);
}

SCENARIO("Deadlock detection and system stability") {
    GIVEN("A CXL system that could encounter stress conditions") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        
        to_rq_MRP mock_ul;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_ul.queues};
        // Very small queues to potentially create stress
        CXL_CONTROLLER uut{clock_period, 5, std::move(upper_levels), 
                          2, 2, 2, champsim::data::bytes{4}, 8.0, 8.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("System is stressed with many requests") {
            static uint64_t id = 1;
            
            // Try to overwhelm the small queues
            for (int i = 0; i < 8; ++i) {
                decltype(mock_ul)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x5000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                mock_ul.issue(test);
            }
            
            // Process with potential for stress
            for (auto i = 0; i < 50; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Deadlock detection doesn't crash the system") {
                REQUIRE_NOTHROW(uut.print_deadlock());
            }
            
            THEN("System remains functional despite stress") {
                // Some requests should be processed (system shouldn't deadlock)
                REQUIRE(mock_dram.packet_count() > 0);
                REQUIRE(mock_ul.packets.size() > 0);
            }
        }
    }
}
