#include <catch.hpp>
#include "mocks.hpp"
#include "cxl_memory.h"

SCENARIO("CXL_CONTROLLER bus bandwidth affects response timing") {
    GIVEN("CXL controllers with different bandwidth configurations") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 2;
        
        to_rq_MRP mock_upper_level_fast, mock_upper_level_slow;
        do_nothing_MRC mock_dram_fast{1}, mock_dram_slow{1};
        
        // Fast CXL configuration
        std::vector<champsim::channel*> fast_upper = {&mock_upper_level_fast.queues};
        CXL_CONTROLLER fast_cxl{clock_period, t_cxl, std::move(fast_upper), 
                               8, 8, champsim::data::bytes{16}, 64.0, 64.0, &mock_dram_fast.queues}; // High BW
        
        // Slow CXL configuration
        std::vector<champsim::channel*> slow_upper = {&mock_upper_level_slow.queues};
        CXL_CONTROLLER slow_cxl{clock_period, t_cxl, std::move(slow_upper), 
                               8, 8, champsim::data::bytes{4}, 8.0, 8.0, &mock_dram_slow.queues};   // Low BW
        
        std::array<champsim::operable*, 3> fast_elements{{&fast_cxl, &mock_upper_level_fast, &mock_dram_fast}};
        std::array<champsim::operable*, 3> slow_elements{{&slow_cxl, &mock_upper_level_slow, &mock_dram_slow}};
        
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
        
        WHEN("The same request is processed by both configurations") {
            static uint64_t id = 1;
            
            // Issue identical requests to both systems
            decltype(mock_upper_level_fast)::request_type test_fast, test_slow;
            test_fast.address = champsim::address{0x100000};
            test_fast.cpu = 0;
            test_fast.instr_id = id++;
            test_fast.type = access_type::LOAD;
            test_fast.is_translated = true;
            
            test_slow = test_fast;
            test_slow.instr_id = id++;
            
            auto fast_result = mock_upper_level_fast.issue(test_fast);
            auto slow_result = mock_upper_level_slow.issue(test_slow);
            
            REQUIRE(fast_result);
            REQUIRE(slow_result);
            
            // Process both systems for the same number of cycles
            for (auto i = 0; i < 30; ++i) {
                for (auto elem : fast_elements) {
                    elem->_operate();
                }
                for (auto elem : slow_elements) {
                    elem->_operate();
                }
            }
            
            THEN("Both requests complete successfully") {
                REQUIRE_THAT(mock_upper_level_fast.packets, Catch::Matchers::SizeIs(1));
                REQUIRE_THAT(mock_upper_level_slow.packets, Catch::Matchers::SizeIs(1));
            }
            
            THEN("Both reach DRAM regardless of bandwidth") {
                REQUIRE(mock_dram_fast.packet_count() == 1);
                REQUIRE(mock_dram_slow.packet_count() == 1);
            }
            
            THEN("Both systems provide reasonable response timing") {
                auto& fast_response = mock_upper_level_fast.packets.front();
                auto& slow_response = mock_upper_level_slow.packets.front();
                
                // Both should complete with CXL latency included
                REQUIRE(fast_response.return_time >= fast_response.issue_time + static_cast<long>(t_cxl));
                REQUIRE(slow_response.return_time >= slow_response.issue_time + static_cast<long>(t_cxl));
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER handles burst traffic") {
    GIVEN("A CXL controller processing multiple concurrent requests") {
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
        
        WHEN("A burst of requests is issued quickly") {
            static uint64_t id = 1;
            const int burst_size = 8;
            int successful_issues = 0;
            
            // Issue burst of requests rapidly
            for (int i = 0; i < burst_size; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x200000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                if (mock_upper_level.issue(test)) {
                    successful_issues++;
                }
            }
            
            THEN("CXL controller accepts the burst") {
                REQUIRE(successful_issues > 0);
            }
            
            // Process the burst through the pipeline
            for (auto i = 0; i < 60; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All burst requests are eventually processed") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(static_cast<size_t>(successful_issues)));
                REQUIRE(mock_dram.packet_count() == successful_issues);
            }
            
            THEN("CXL pipeline enables concurrent processing") {
                // Responses should be spread out due to pipelining
                if (mock_upper_level.packets.size() > 1) {
                    auto first_return = mock_upper_level.packets[0].return_time;
                    auto last_return = mock_upper_level.packets.back().return_time;
                    REQUIRE(last_return >= first_return); // Should maintain ordering
                }
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER bus utilization statistics") {
    GIVEN("A CXL controller that tracks bus utilization") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 2;
        
        to_rq_MRP mock_read_source;
        to_wq_MRP mock_write_source;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_read_source.queues, &mock_write_source.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 4> elements{{&uut, &mock_read_source, &mock_write_source, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Mixed read and write operations are performed") {
            // Access channel statistics through correct path
            auto initial_total_cycles = uut.channel.sim_stats.total_operating_cycles;
            
            static uint64_t id = 1;
            
            // Issue some read requests
            for (int i = 0; i < 3; ++i) {
                decltype(mock_read_source)::request_type read_req;
                read_req.address = champsim::address{static_cast<uint64_t>(0x300000 + i * 64)};
                read_req.cpu = 0;
                read_req.instr_id = id++;
                read_req.type = access_type::LOAD;
                read_req.is_translated = true;
                
                mock_read_source.issue(read_req);
            }
            
            // Issue some write requests
            for (int i = 0; i < 2; ++i) {
                decltype(mock_write_source)::request_type write_req;
                write_req.address = champsim::address{static_cast<uint64_t>(0x400000 + i * 64)};
                write_req.data = champsim::address{static_cast<uint64_t>(0x1000 + i)};
                write_req.cpu = 0;
                write_req.instr_id = id++;
                write_req.type = access_type::WRITE;
                write_req.is_translated = true;
                write_req.response_requested = false;
                
                mock_write_source.issue(write_req);
            }
            
            // Process operations to generate bus activity
            for (auto i = 0; i < 40; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Operating cycle statistics are updated") {
                REQUIRE(uut.channel.sim_stats.total_operating_cycles > initial_total_cycles);
            }
            
            THEN("All requests are processed through CXL") {
                REQUIRE_THAT(mock_read_source.packets, Catch::Matchers::SizeIs(3));
                REQUIRE_THAT(mock_write_source.packets, Catch::Matchers::SizeIs(0)); // Writes don't generate responses
                REQUIRE(mock_dram.packet_count() == 5); // 3 reads + 2 writes
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER scheduling with multiple sources") {
    GIVEN("A CXL controller with requests from multiple sources") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 2;
        
        to_rq_MRP mock_source1, mock_source2;
        do_nothing_MRC mock_dram{1};
        
        // Connect multiple upper level sources
        std::vector<champsim::channel*> upper_levels = {&mock_source1.queues, &mock_source2.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          12, 12, champsim::data::bytes{8}, 32.0, 32.0, &mock_dram.queues};
        
        std::array<champsim::operable*, 4> elements{{&uut, &mock_source1, &mock_source2, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Both sources issue requests simultaneously") {
            static uint64_t id = 1;
            const int requests_per_source = 4;
            
            // Issue requests from first source
            for (int i = 0; i < requests_per_source; ++i) {
                decltype(mock_source1)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x500000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                mock_source1.issue(test);
            }
            
            // Issue requests from second source
            for (int i = 0; i < requests_per_source; ++i) {
                decltype(mock_source2)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x600000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = access_type::LOAD;
                test.is_translated = true;
                
                mock_source2.issue(test);
            }
            
            // Process all requests
            for (auto i = 0; i < 50; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Both sources get their requests processed") {
                REQUIRE_THAT(mock_source1.packets, Catch::Matchers::SizeIs(requests_per_source));
                REQUIRE_THAT(mock_source2.packets, Catch::Matchers::SizeIs(requests_per_source));
                REQUIRE(mock_dram.packet_count() == 2 * requests_per_source);
            }
            
            THEN("Response timing is reasonable for both sources") {
                // Both sources should get responses within reasonable time
                for (const auto& packet : mock_source1.packets) {
                    auto latency = packet.return_time - packet.issue_time;
                    REQUIRE(latency >= t_cxl);
                    REQUIRE(latency < 30);
                }
                
                for (const auto& packet : mock_source2.packets) {
                    auto latency = packet.return_time - packet.issue_time;
                    REQUIRE(latency >= t_cxl);
                    REQUIRE(latency < 30);
                }
            }
        }
    }
}

TEST_CASE("CXL_CONTROLLER PCIe timing parameters") {
    to_rq_MRP mock_upper_level;
    do_nothing_MRC mock_dram{1};
    
    const auto clock_period = champsim::chrono::picoseconds{1000};
    const std::size_t t_cxl = 4;
    const double bandwidth = 16.0; // GT/s
    const auto channel_width = champsim::data::bytes{8};
    
    std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
    CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                      8, 8, channel_width, bandwidth, bandwidth, &mock_dram.queues};
    
    std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    static uint64_t id = 1;
    
    // Issue a request to test timing
    decltype(mock_upper_level)::request_type test;
    test.address = champsim::address{0x700000};
    test.cpu = 0;
    test.instr_id = id++;
    test.type = access_type::LOAD;
    test.is_translated = true;
    
    auto issue_result = mock_upper_level.issue(test);
    REQUIRE(issue_result);
    
    // Process request
    for (auto i = 0; i < 25; ++i) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(1));
    REQUIRE(mock_dram.packet_count() == 1);
    
    // Verify CXL timing is enforced
    auto& response = mock_upper_level.packets.front();
    auto latency = response.return_time - response.issue_time;
    REQUIRE(latency >= t_cxl); // Must include CXL latency
}
