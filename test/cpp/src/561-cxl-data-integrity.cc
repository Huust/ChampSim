#include <catch.hpp>
#include "access_type.h"
#include "matchers.hpp"
#include "mocks.hpp"
#include "cxl_memory.h"

SCENARIO("Request forwarding to lower level") {
    GIVEN("A CXL controller connecting upper level to DRAM") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 4;
        const auto dram_latency = 2;
        constexpr auto rx_bw = 16.0, tx_bw = 16.0;
        auto t_rd = static_cast<long>(std::ceil(BLOCK_SIZE/rx_bw));
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, 8, champsim::data::bytes{8}, rx_bw, tx_bw, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Multiple read requests are sent through CXL") {
            static uint64_t id = 1;
            const int num_requests = 40;
            
            // Issue multiple read / prefetch requests
            for (int i = 0; i < num_requests; ++i) {
                decltype(mock_upper_level)::request_type test;
                test.address = champsim::address{static_cast<uint64_t>(0x100000 + i * 64)};
                test.cpu = 0;
                test.instr_id = id++;
                test.type = (i % 2 == 0) ? access_type::LOAD : access_type::PREFETCH;
                test.is_translated = true;
                mock_upper_level.issue(test);
            }
            
            // All requests are issued
            REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(num_requests));
            
            // Process through CXL pipeline
            for (auto i = 0; i < 5000; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All requests are forwarded to DRAM") {
                REQUIRE(mock_dram.packet_count() == num_requests);
            }
            
            THEN("All requests get responses through CXL") {
                // All responses should have reasonable timing
                for (const auto& pkt : mock_upper_level.packets)
                  REQUIRE_THAT(pkt, champsim::test::LatencyRangeMatcher(
                    {t_cxl, dram_latency, t_cxl, t_rd},
                    {500}));
                
                auto first_return = mock_upper_level.packets.front().return_time;
                auto last_return = mock_upper_level.packets.back().return_time;
                // With proper pipelining, responses should span multiple cycles
                REQUIRE(last_return > first_return);
            }
        }
    }
}

SCENARIO("Write request transmission") {
    GIVEN("A CXL controller handling write requests") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 3;
        const auto dram_latency = 1;
        constexpr auto rx_bw = 32.0, tx_bw = 32.0;
        
        to_wq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, 8, champsim::data::bytes{8}, rx_bw, tx_bw, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Write requests are processed through CXL") {
            static uint64_t id = 1;
            const int num_writes = 20;
            
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
                
                mock_upper_level.issue(test);
            }
            
            // All requests are issued
            REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(num_writes));
            
            // Process through pipeline
            for (auto i = 0; i < 2000; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("All writes are forwarded to DRAM") {
                REQUIRE(mock_dram.packet_count() == num_writes);
            }
            
            THEN("No responses are generated for writes") {
              for (auto& pkt: mock_upper_level.packets)
                REQUIRE(pkt.return_time == 0);
            }
        }
    }
}

SCENARIO("Request attribute preservation") {
    GIVEN("A CXL controller that preserves request information") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 2;
        
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{1};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          8, 8, 8, champsim::data::bytes{8}, 64.0, 64.0, &mock_dram.queues};
        
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

TEST_CASE("Request ordering guarantees") {
    to_rq_MRP mock_upper_level;
    do_nothing_MRC mock_dram{1};
    
    std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
    CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 2, std::move(upper_levels), 
                      8, 8, 8, champsim::data::bytes{8}, 64.0, 64.0, &mock_dram.queues};
    
    std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
    for (auto elem : elements) {
        elem->initialize();
        elem->warmup = false;
        elem->begin_phase();
    }
    
    static uint64_t id = 100;
    std::vector<champsim::address> issued_addresses;
    
    // Issue requests in sequence
    for (int i = 0; i < 5; ++i) {
        decltype(mock_upper_level)::request_type test;
        test.address = champsim::address{static_cast<uint64_t>(0x500000 + i * 64)};
        test.cpu = 0;
        test.instr_id = id++;
        test.type = access_type::LOAD;
        test.is_translated = true;
        
        issued_addresses.push_back(test.address);
        mock_upper_level.issue(test);
    }
    
    // Process all requests
    for (auto i = 0; i < 50; ++i) {
        for (auto elem : elements) {
            elem->_operate();
        }
    }
    
    REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(5));
    
    // Check DRAM receives requests in correct order
    REQUIRE_THAT(mock_dram.addresses, Catch::Matchers::RangeEquals(issued_addresses));

    // Check return times are monotonically increasing (first issued should return first)
    std::vector<long> return_times;
    // Double loop needed because std::partition scrambled packets array order
    // We reconstruct return_times in original issue order by matching addresses
    for (auto address : issued_addresses)
        for (auto &pkt : mock_upper_level.packets)
            if (address == pkt.pkt.address)
                return_times.push_back(pkt.return_time);

    REQUIRE_THAT(return_times, champsim::test::MonotonicallyIncreasingMatcher{});
}
