#include <catch.hpp>
#include <cstdint>
#include "mocks.hpp"
#include "shim_layer.h"

SCENARIO("SHIM_LAYER complete request-response cycle works correctly") {
    GIVEN("A HYBRID SHIM_LAYER with realistic memory behavior") {
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{5};    // 5 cycle latency
        do_nothing_MRC mock_cxl{10};    // 10 cycle latency (CXL typically slower)
        
        std::vector<champsim::channel*> lower_channels = {&mock_dram.queues, &mock_cxl.queues};
        SHIM_LAYER uut{&mock_upper_level.queues, std::move(lower_channels), 64, 64, 32, true, true};
        
        // Initialize all components following ChampSim pattern
        std::array<champsim::operable*, 4> elements{{&uut, &mock_upper_level, &mock_dram, &mock_cxl}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A stream of requests is sent through the system") {
            // Create diverse test requests
            std::vector<champsim::channel::request_type> test_requests;
            
            for (uint64_t i = 0; i < 6; ++i) {
                champsim::channel::request_type req;
                req.address = champsim::address{static_cast<uint64_t>(0x10000 + i * 0x1000)};
                req.v_address = req.address;
                req.type = access_type::LOAD;  // Focus on reads for response testing
                req.response_requested = true; // Request responses
                req.instr_id = i;
                req.cpu = 0;
                req.is_translated = true;
                test_requests.push_back(req);
            }
            
            // Issue all requests
            int successfully_issued = 0;
            for (const auto& req : test_requests) {
                if (mock_upper_level.issue(req)) {
                    successfully_issued++;
                }
            }
            
            REQUIRE(successfully_issued == static_cast<int>(test_requests.size()));
            
            // Run simulation for enough cycles to complete all operations
            int max_cycles = 100;
            for (int cycle = 0; cycle < max_cycles; ++cycle) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All requests are processed by the memory subsystem") {
                auto total_processed = mock_dram.packet_count() + mock_cxl.packet_count();
                REQUIRE(total_processed == test_requests.size());
                
                // Verify that both memory types received requests in HYBRID mode
                REQUIRE(mock_dram.packet_count() > 0);
                REQUIRE(mock_cxl.packet_count() > 0);
            }
            
            THEN("Responses are correctly forwarded back to upper level") {
                // All LOAD requests should have responses
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(test_requests.size()));
                
                // Verify response timing makes sense
                for (const auto& packet_result : mock_upper_level.packets) {
                    REQUIRE(packet_result.return_time > packet_result.issue_time);
                    REQUIRE(packet_result.return_time > 0);
                }
            }
        }
    }
}

SCENARIO("SHIM_LAYER handles extreme load conditions gracefully") {
    GIVEN("A SHIM_LAYER with limited queue sizes") {
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_slow_memory{20};  // Very slow memory (20 cycle latency)
        
        std::vector<champsim::channel*> lower_channels = {&mock_slow_memory.queues};
        
        // Small queue sizes to test overflow handling
        SHIM_LAYER uut{&mock_upper_level.queues, std::move(lower_channels), 8, 8, 8, true, false};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_slow_memory}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A burst of requests exceeds queue capacity") {
            // Generate more requests than queue can handle
            int issued_count = 0;
            for (uint64_t i = 0; i < 25; ++i) {  // Much more than queue size of 8
                champsim::channel::request_type req;
                req.address = champsim::address{static_cast<uint64_t>(0x10000 + i * 64)};
                req.type = access_type::LOAD;
                req.instr_id = i;
                req.cpu = 0;
                req.is_translated = true;
                req.response_requested = false;  // Reduce complexity for stress test
                
                if (mock_upper_level.issue(req)) {
                    issued_count++;
                }
            }
            
            // Process for limited cycles to create backpressure
            for (int cycle = 0; cycle < 30; ++cycle) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("System handles overflow gracefully without crashing") {
                // System should not crash, even with queue overflow
                // Some requests should be processed
                REQUIRE(mock_slow_memory.packet_count() > 0);
                REQUIRE(mock_slow_memory.packet_count() <= static_cast<std::size_t>(issued_count));
                
                // If we reach here, system remained stable under stress
                REQUIRE(true);
            }
        }
    }
}

SCENARIO("SHIM_LAYER different request types are handled correctly") {
    GIVEN("A SHIM_LAYER with multiple request type support") {
        to_rq_MRP mock_ul_read;
        to_wq_MRP mock_ul_write;
        to_pq_MRP mock_ul_prefetch;
        do_nothing_MRC mock_memory;
        
        std::vector<champsim::channel*> lower_channels = {&mock_memory.queues};
        SHIM_LAYER uut{&mock_ul_read.queues, std::move(lower_channels), 32, 32, 32, true, false};
        
        // Note: SHIM_LAYER only connects to one upper level in constructor
        // This test focuses on read requests through the connected channel
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul_read, &mock_memory}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Read requests are sent through the system") {
            champsim::channel::request_type read_req;
            read_req.address = champsim::address{0x10000};
            read_req.type = access_type::LOAD;
            read_req.instr_id = 1;
            read_req.cpu = 0;
            read_req.is_translated = true;
            read_req.response_requested = true;
            
            auto issue_result = mock_ul_read.issue(read_req);
            REQUIRE(issue_result);
            
            // Process the request
            for (int cycle = 0; cycle < 20; ++cycle) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Read request is processed correctly") {
                REQUIRE(mock_memory.packet_count() == 1);
                REQUIRE_THAT(mock_ul_read.packets, Catch::Matchers::SizeIs(1));
            }
        }
    }
}

TEST_CASE("SHIM_LAYER stress test with mixed workload") {
    to_rq_MRP mock_upper_level;
    do_nothing_MRC mock_memory1;
    do_nothing_MRC mock_memory2;
    
    std::vector<champsim::channel*> lower_channels = {&mock_memory1.queues, &mock_memory2.queues};
    SHIM_LAYER uut{&mock_upper_level.queues, std::move(lower_channels), 64, 64, 32, true, true};
    
    std::array<champsim::operable*, 4> elements{{&uut, &mock_upper_level, &mock_memory1, &mock_memory2}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    // Issue mixed requests with different addresses
    int total_issued = 0;
    for (uint64_t i = 0; i < 20; ++i) {
        champsim::channel::request_type req;
        req.address = champsim::address{static_cast<uint64_t>(0x10000 + i * 64)};
        req.type = access_type::LOAD;
        req.instr_id = i;
        req.cpu = 0;
        req.is_translated = true;
        
        if (mock_upper_level.issue(req)) {
            total_issued++;
        }
    }
    
    // Run stress test
    for (int cycle = 0; cycle < 50; ++cycle) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    // Verify system stability and load distribution
    auto total_processed = mock_memory1.packet_count() + mock_memory2.packet_count();
    REQUIRE(total_processed <= static_cast<std::size_t>(total_issued));
    REQUIRE(total_processed > 0);  // Some progress made
    
    // In hybrid mode, both memories should get some load
    REQUIRE(mock_memory1.packet_count() > 0);
    REQUIRE(mock_memory2.packet_count() > 0);
}
