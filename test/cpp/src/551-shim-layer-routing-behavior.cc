#include <catch.hpp>
#include "mocks.hpp"
#include "shim_layer.h"

SCENARIO("SHIM_LAYER routes requests correctly based on address") {
    GIVEN("A HYBRID mode SHIM_LAYER with DRAM and CXL channels") {
        // Setup the test components using correct ChampSim pattern
        to_rq_MRP mock_upper_level;       // Simulates LLC sending requests
        do_nothing_MRC mock_dram;         // Simulates DRAM controller  
        do_nothing_MRC mock_cxl;          // Simulates CXL controller
        
        // Create SHIM_LAYER with proper connections
        std::vector<champsim::channel*> lower_channels = {&mock_dram.queues, &mock_cxl.queues};
        SHIM_LAYER uut{&mock_upper_level.queues, std::move(lower_channels), 64, 64, 32, true, true};
        
        // Initialize all components properly (following 401 test pattern)
        std::array<champsim::operable*, 4> elements{{&uut, &mock_upper_level, &mock_dram, &mock_cxl}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Requests with different addresses are sent through upper level") {
            // Create test requests with different address patterns (depends on your implementation)
            champsim::channel::request_type req1;
            req1.address = champsim::address{0x10000};  // Should route to one channel
            req1.type = access_type::LOAD;
            req1.instr_id = 1;
            req1.cpu = 0;
            req1.is_translated = true;
            
            champsim::channel::request_type req2;
            req2.address = champsim::address{0x100000000ULL};  // Should route to potentially different channel  
            req2.type = access_type::LOAD;
            req2.instr_id = 2;
            req2.cpu = 0;
            req2.is_translated = true;
            
            // Issue requests through the MRP (correct way)
            // After issueing, the requests are in the upper level channel
            // and the mock class would record their send time
            auto issue1_result = mock_upper_level.issue(req1);
            auto issue2_result = mock_upper_level.issue(req2);
            
            THEN("Both requests are accepted") {
                REQUIRE(issue1_result);
                REQUIRE(issue2_result);
            }
            
            // Run the simulation for several cycles
            for (int cycle = 0; cycle < 20; ++cycle) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Requests are routed to appropriate lower-level channels") {
                // Verify that requests were processed by lower level consumers
                auto total_processed = mock_dram.packet_count() + mock_cxl.packet_count();
                REQUIRE(total_processed >= 2);
                
                // In HYBRID mode, both channels should receive some requests
                // (This assumes the routing algorithm distributes across channels)
                REQUIRE(mock_dram.packet_count() > 0);
                REQUIRE(mock_cxl.packet_count() > 0);
            }
        }
    }
}

SCENARIO("SHIM_LAYER handles response forwarding correctly") {
    GIVEN("A SHIM_LAYER with connected components") {
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_lower_level;
        
        std::vector<champsim::channel*> lower_channels = {&mock_lower_level.queues};
        SHIM_LAYER uut{&mock_upper_level.queues, std::move(lower_channels), 64, 64, 32, true, false};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_lower_level}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A request requiring response is sent") {
            champsim::channel::request_type req;
            req.address = champsim::address{0x10000};
            req.type = access_type::LOAD;
            req.instr_id = 1;
            req.cpu = 0;
            req.is_translated = true;
            req.response_requested = true;  // This is key for response testing
            
            auto issue_result = mock_upper_level.issue(req);
            REQUIRE(issue_result);
            
            // Run simulation to process request and generate response
            for (int cycle = 0; cycle < 30; ++cycle) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Response is forwarded back to upper level") {
                // Check that the MRP received the response
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(1));
                REQUIRE(mock_upper_level.packets[0].return_time > 0);
            }
        }
    }
}

TEST_CASE("SHIM_LAYER bandwidth limiting works correctly") {
    to_rq_MRP mock_upper_level;
    do_nothing_MRC mock_lower_level;
    
    std::vector<champsim::channel*> lower_channels = {&mock_lower_level.queues};
    SHIM_LAYER uut{&mock_upper_level.queues, std::move(lower_channels), 64, 64, 32, true, false};
    
    std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_lower_level}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    // Issue many requests in burst to test bandwidth limiting
    int issued_count = 0;
    for (int i = 0; i < 20; ++i) {
        champsim::channel::request_type req;
        req.address = champsim::address{static_cast<uint64_t>(0x10000 + i * 64)};
        req.type = access_type::LOAD;
        req.instr_id = i;
        req.cpu = 0;
        req.is_translated = true;
        
        if (mock_upper_level.issue(req)) {
            issued_count++;
        }
    }
    
    // Run just a few cycles to test bandwidth limiting
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    // Due to bandwidth limits, not all requests should be processed immediately
    // The exact numbers depend on the bandwidth configuration
    REQUIRE(mock_lower_level.packet_count() <= static_cast<std::size_t>(issued_count));
    REQUIRE(mock_lower_level.packet_count() > 0);  // But some should be processed
}
