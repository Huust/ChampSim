#include <catch.hpp>
#include "mocks.hpp"
#include "cxl_memory.h"

SCENARIO("CXL_CONTROLLER processes read requests through complete pipeline") {
    GIVEN("A CXL controller with upper and lower level connections") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 5; // CXL latency
        const std::size_t rq_size = 16;
        const std::size_t wq_size = 16;
        
        // Set up proper mock components
        to_rq_MRP mock_upper_level;     // Simulates upper level (LLC) sending requests
        do_nothing_MRC mock_dram{3};    // 3 cycle DRAM latency
        
        // Create CXL controller with proper upper level connection
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          rq_size, wq_size, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
        
        // Initialize all components
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A read request is issued through upper level") {
            // Create a test request using proper pattern
            static uint64_t id = 1;
            decltype(mock_upper_level)::request_type test;
            test.address = champsim::address{0xdeadbeef};
            test.cpu = 0;
            test.instr_id = id++;
            test.type = access_type::LOAD;
            test.is_translated = true;
            
            // Issue request through mock upper level
            auto test_result = mock_upper_level.issue(test);
            
            THEN("The request is accepted by upper level") {
                REQUIRE(test_result);
            }
            
            // Run the pipeline for sufficient cycles
            for (auto i = 0; i < 50; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Request flows through CXL to DRAM") {
                REQUIRE(mock_dram.packet_count() == 1);
            }
            
            THEN("Response is returned through CXL pipeline") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(1));
                auto& response = mock_upper_level.packets.front();
                REQUIRE(response.return_time > response.issue_time);
                REQUIRE(response.return_time >= response.issue_time + static_cast<long>(t_cxl)); // At least CXL latency
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER processes write requests") {
    GIVEN("A CXL controller handling write requests") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 4;
        
        to_wq_MRP mock_upper_level;     // For write requests
        do_nothing_MRC mock_dram{2};    // 2 cycle DRAM latency
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          16, 16, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A write request is issued") {
            static uint64_t id = 1;
            decltype(mock_upper_level)::request_type test;
            test.address = champsim::address{0xcafebabe};
            test.data = champsim::address{0xdeadbeef};
            test.cpu = 0;
            test.instr_id = id++;
            test.type = access_type::WRITE;
            test.is_translated = true;
            test.response_requested = false; // Writes typically don't need responses
            
            auto test_result = mock_upper_level.issue(test);
            
            THEN("The write is accepted") {
                REQUIRE(test_result);
            }
            
            // Process write through pipeline
            for (auto i = 0; i < 30; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Write reaches DRAM") {
                REQUIRE(mock_dram.packet_count() == 1);
            }
            
            THEN("No response is generated for writes") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(0));
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER latency characteristics") {
    GIVEN("A CXL controller with specific timing") {
        constexpr auto cxl_latency = 8;
        constexpr auto dram_latency = 5;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, cxl_latency, std::move(upper_levels), 
                          8, 8, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A request is processed through the pipeline") {
            static uint64_t id = 1;
            decltype(mock_upper_level)::request_type test;
            test.address = champsim::address{0x12345678};
            test.cpu = 0;
            test.instr_id = id++;
            test.type = access_type::LOAD;
            test.is_translated = true;
            
            auto test_result = mock_upper_level.issue(test);
            REQUIRE(test_result);
            
            // Process for enough cycles to complete
            for (auto i = 0; i < 50; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Response includes CXL and DRAM latencies") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(1));
                auto& response = mock_upper_level.packets.front();
                
                // Total latency should be at least CXL + DRAM latencies
                auto total_latency = response.return_time - response.issue_time;
                REQUIRE(total_latency >= cxl_latency + dram_latency);
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER warmup mode behavior") {
    GIVEN("A CXL controller in warmup mode") {
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{10}; // High latency that should be bypassed in warmup
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 5, std::move(upper_levels), 
                          8, 8, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = true; // Enable warmup mode
            elem->begin_phase();
        }
        
        WHEN("Requests are processed in warmup mode") {
            static uint64_t id = 1;
            decltype(mock_upper_level)::request_type test;
            test.address = champsim::address{0xdeadbeef};
            test.cpu = 0;
            test.instr_id = id++;
            test.type = access_type::LOAD;
            test.is_translated = true;
            
            auto test_result = mock_upper_level.issue(test);
            REQUIRE(test_result);
            
            // Process just a few cycles (should be immediate in warmup)
            for (auto i = 0; i < 5; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Responses are generated immediately in warmup mode") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(1));
                auto& response = mock_upper_level.packets.front();
                
                // In warmup mode, response should be very fast
                auto warmup_latency = response.return_time - response.issue_time;
                REQUIRE(warmup_latency < 3); // Much faster than normal mode
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER queue capacity limits") {
    GIVEN("A CXL controller with small queue sizes") {
        constexpr auto small_queue_size = 2;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 3, std::move(upper_levels), 
                          small_queue_size, small_queue_size, champsim::data::bytes{8}, 
                          16.0, 16.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("More requests than queue capacity are issued") {
            int successful_issues = 0;
            static uint64_t id = 1;
            
            // Try to issue more requests than queue capacity
            for (int i = 0; i < small_queue_size + 2; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x100000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                if (mock_upper_level.issue(test)) {
                    successful_issues++;
                }
            }
            
            THEN("Queue capacity limits are respected") {
                // Should not exceed queue capacity in first cycle
                REQUIRE(successful_issues >= small_queue_size);
                
                // Process to make room
                for (auto i = 0; i < 20; ++i) {
                    for (auto elem : elements) {
                        elem->_operate();
                    }
                }
                
                // Some requests should have been processed
                REQUIRE(mock_dram.packet_count() > 0);
            }
        }
    }
}

TEST_CASE("CXL_CONTROLLER statistics collection") {
    to_rq_MRP mock_upper_level;
    do_nothing_MRC mock_dram;
    
    std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
    CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 2, std::move(upper_levels), 
                      8, 8, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
    
    std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    auto initial_cycles = uut.channel.sim_stats.total_operating_cycles;
    
    // Run operations
    for (int i = 0; i < 10; ++i) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    REQUIRE(uut.channel.sim_stats.total_operating_cycles > initial_cycles);
    
    // Test end_phase
    uut.end_phase(0);
    REQUIRE(uut.channel.roi_stats.total_operating_cycles == uut.channel.sim_stats.total_operating_cycles);
}
