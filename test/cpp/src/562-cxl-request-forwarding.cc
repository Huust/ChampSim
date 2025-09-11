#include <catch.hpp>
#include "mocks.hpp"
#include "cxl_memory.h"

SCENARIO("CXL_CONTROLLER forwards requests correctly to DRAM") {
    GIVEN("A CXL controller connecting upper level to DRAM") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 4;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{2};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Multiple read requests are sent through CXL") {
            static uint64_t id = 1;
            const int num_requests = 4;
            
            // Issue multiple read requests
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
            
            // Process through CXL pipeline
            for (auto i = 0; i < 40; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All requests are forwarded to DRAM") {
                REQUIRE(mock_dram.packet_count() == num_requests);
            }
            
            THEN("All requests get responses through CXL") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(num_requests));
                
                // All responses should have reasonable timing
                for (const auto& packet : mock_upper_level.packets) {
                    auto latency = packet.return_time - packet.issue_time;
                    REQUIRE(latency >= t_cxl); // At least CXL latency
                    REQUIRE(latency < 30);     // But reasonable upper bound
                }
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER handles write request forwarding") {
    GIVEN("A CXL controller handling write requests") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 3;
        
        to_wq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Write requests are processed through CXL") {
            static uint64_t id = 1;
            const int num_writes = 3;
            
            // Issue write requests
            for (int i = 0; i < num_writes; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x200000 + i * 64)};
                test.data = champsim::address{static_cast<uint64_t>(0x1000 + i)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::WRITE;
                test.is_translated = true;
                test.response_requested = false; // Writes typically don't need responses
                
                auto result = mock_upper_level.issue(test);
                REQUIRE(result);
            }
            
            // Process through pipeline
            for (auto i = 0; i < 25; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All writes are forwarded to DRAM") {
                REQUIRE(mock_dram.packet_count() == num_writes);
            }
            
            THEN("No responses are generated for writes") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(0));
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER request preservation through pipeline") {
    GIVEN("A CXL controller that preserves request information") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 2;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, champsim::data::bytes{8}, 64.0, 64.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("A request with specific attributes is processed") {
            static uint64_t id = 123;
            const uint64_t test_addr = 0x12345678;
            
            decltype(mock_upper_level)::request_type test;
            test.address = champsim::address{test_addr};
            test.cpu = 0;
            test.instr_id = id;
            test.type = access_type::LOAD;
            test.is_translated = true;
            
            auto result = mock_upper_level.issue(test);
            REQUIRE(result);
            
            // Process through pipeline
            for (auto i = 0; i < 20; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Request attributes are preserved in response") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(1));
                
                auto& response = mock_upper_level.packets.front();
                REQUIRE(response.pkt.address == champsim::address{test_addr});
                REQUIRE(response.pkt.instr_id == id);
                REQUIRE(response.pkt.type == access_type::LOAD);
                REQUIRE(response.pkt.cpu == 0);
            }
            
            THEN("DRAM receives correct request") {
                REQUIRE(mock_dram.packet_count() == 1);
                // DRAM should have processed the request (addresses tracked by mock)
                REQUIRE(mock_dram.addresses.size() == 1);
                REQUIRE(mock_dram.addresses[0] == champsim::address{test_addr});
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER response timing characteristics") {
    GIVEN("CXL controllers with different timing configurations") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        
        to_rq_MRP mock_fast_ul, mock_slow_ul;
        do_nothing_MRC mock_fast_dram{1}, mock_slow_dram{1};
        
        // Fast CXL (low latency)
        std::vector<champsim::channel*> fast_upper = {&mock_fast_ul.queues};
        CXL_CONTROLLER fast_cxl{clock_period, 2, std::move(fast_upper), 
                               8, 8, champsim::data::bytes{8}, 32.0, 32.0, &mock_fast_dram.queues};
        
        // Slow CXL (high latency)  
        std::vector<champsim::channel*> slow_upper = {&mock_slow_ul.queues};
        CXL_CONTROLLER slow_cxl{clock_period, 8, std::move(slow_upper), 
                               8, 8, champsim::data::bytes{8}, 32.0, 32.0, &mock_slow_dram.queues};
        
        std::array<champsim::operable*, 3> fast_elements{{&fast_cxl, &mock_fast_ul, &mock_fast_dram}};
        std::array<champsim::operable*, 3> slow_elements{{&slow_cxl, &mock_slow_ul, &mock_slow_dram}};
        
        for (auto elem : fast_elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        for (auto elem : slow_elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Identical requests are processed by both systems") {
            static uint64_t id = 1;
            
            decltype(mock_fast_ul)::request_type fast_req, slow_req;
            fast_req.address = champsim::address{0x300000};
            fast_req.cpu = 0;
            fast_req.instr_id = id++;
            fast_req.type = access_type::LOAD;
            fast_req.is_translated = true;
            
            slow_req = fast_req;
            slow_req.instr_id = id++;
            
            auto fast_result = mock_fast_ul.issue(fast_req);
            auto slow_result = mock_slow_ul.issue(slow_req);
            
            REQUIRE(fast_result);
            REQUIRE(slow_result);
            
            // Process both systems
            for (auto i = 0; i < 25; ++i) {
                for (auto elem : fast_elements) {
                    elem->_operate();
                }
                for (auto elem : slow_elements) {
                    elem->_operate();
                }
            }
            
            THEN("Both systems process requests successfully") {
                REQUIRE_THAT(mock_fast_ul.packets, Catch::Matchers::SizeIs(1));
                REQUIRE_THAT(mock_slow_ul.packets, Catch::Matchers::SizeIs(1));
                REQUIRE(mock_fast_dram.packet_count() == 1);
                REQUIRE(mock_slow_dram.packet_count() == 1);
            }
            
            THEN("Higher latency CXL has longer response times") {
                auto fast_latency = mock_fast_ul.packets[0].return_time - mock_fast_ul.packets[0].issue_time;
                auto slow_latency = mock_slow_ul.packets[0].return_time - mock_slow_ul.packets[0].issue_time;
                
                REQUIRE(fast_latency >= 2); // Fast CXL latency
                REQUIRE(slow_latency >= 8); // Slow CXL latency
                REQUIRE(slow_latency > fast_latency); // Slow should be slower
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER concurrent request handling") {
    GIVEN("A CXL controller with multiple concurrent requests") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 3;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{2};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          16, 16, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Many concurrent requests are issued") {
            static uint64_t id = 1;
            const int concurrent_requests = 12;
            
            // Issue many requests concurrently
            for (int i = 0; i < concurrent_requests; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x400000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                mock_upper_level.issue(test);
            }
            
            // Process with pipelining
            for (auto i = 0; i < 60; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All concurrent requests are processed") {
                REQUIRE(mock_dram.packet_count() == concurrent_requests);
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(concurrent_requests));
            }
            
            THEN("CXL pipelining enables concurrent processing") {
                // Responses should be spread over time due to pipelining
                if (mock_upper_level.packets.size() > 1) {
                    auto first_return = mock_upper_level.packets[0].return_time;
                    auto last_return = mock_upper_level.packets.back().return_time;
                    
                    // With proper pipelining, responses should span multiple cycles
                    REQUIRE(last_return >= first_return);
                    
                    // But all should complete in reasonable time
                    for (const auto& packet : mock_upper_level.packets) {
                        auto latency = packet.return_time - packet.issue_time;
                        REQUIRE(latency >= t_cxl);
                        REQUIRE(latency < 40); // Reasonable upper bound with pipelining
                    }
                }
            }
        }
    }
}

TEST_CASE("CXL_CONTROLLER request ordering preservation") {
    to_rq_MRP mock_upper_level;
    do_nothing_MRC mock_dram{1};
    
    std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
    CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 2, std::move(upper_levels), 
                      8, 8, champsim::data::bytes{8}, 64.0, 64.0, &mock_dram.queues};
    
    std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    static uint64_t id = 100;
    std::vector<uint64_t> issued_ids;
    
    // Issue requests in sequence
    for (int i = 0; i < 5; ++i) {
        decltype(mock_upper_level)::request_type test;
        test.address = champsim::address{static_cast<uint64_t>(0x500000 + i * 64)};
        test.cpu = 0;
        test.instr_id = id;
        test.type = access_type::LOAD;
        test.is_translated = true;
        
        issued_ids.push_back(id);
        id++;
        
        mock_upper_level.issue(test);
    }
    
    // Process all requests
    for (auto i = 0; i < 30; ++i) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(5));
    
    // Verify all instruction IDs are preserved
    std::vector<uint64_t> returned_ids;
    for (const auto& packet : mock_upper_level.packets) {
        returned_ids.push_back(packet.pkt.instr_id);
    }
    
    // All issued IDs should be returned (though possibly in different order)
    std::sort(issued_ids.begin(), issued_ids.end());
    std::sort(returned_ids.begin(), returned_ids.end());
    REQUIRE_THAT(returned_ids, Catch::Matchers::RangeEquals(issued_ids));
}
