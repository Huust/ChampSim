#include <catch.hpp>
#include <catch2/catch_test_macros.hpp>
#include "mocks.hpp"
#include "cxl_memory.h"

SCENARIO("Memory access optimization patterns") {
    GIVEN("A CXL system that optimizes memory requests") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 10;
        const auto dram_latency = 10;
        constexpr auto rx_bw = 32.0, tx_bw = 32.0;
        to_rq_MRP mock_upper_level;
        do_nothing_MRC mock_dram{dram_latency};
        
        std::vector<champsim::channel*> upper_levels = {&mock_upper_level.queues};
        CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                          16, 16, 16, champsim::data::bytes{8}, rx_bw, tx_bw, &mock_dram.queues};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_upper_level, &mock_dram}};
        for (auto elem : elements) {
            elem->initialize();
            elem->warmup = false;
            elem->begin_phase();
        }
        
        WHEN("Write-to-Read forwarding opportunity exists") {
            static uint64_t id = 1;
            const auto target_addr = static_cast<uint64_t>(0x100000);
            
            // First issue a write request
            decltype(mock_upper_level)::request_type write_req;
            write_req.address = champsim::address{target_addr};
            write_req.data = champsim::address{0xDEADBEEF};
            write_req.cpu = 0;
            write_req.instr_id = id++;
            write_req.type = access_type::WRITE;
            write_req.is_translated = true;
            write_req.response_requested = false;
            
            mock_upper_level.issue(write_req);
            
            // Process a few cycles to get write into CXL pipeline
            for (auto i = 0; i < 5; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            // Now issue a read to the same address - should get forwarded data
            decltype(mock_upper_level)::request_type read_req;
            read_req.address = champsim::address{target_addr};
            read_req.cpu = 0;
            read_req.instr_id = id++;
            read_req.type = access_type::LOAD;
            read_req.is_translated = true;
            
            mock_upper_level.issue(read_req);
            
            // Process to completion
            for (auto i = 0; i < 100; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Read gets forwarded data with short latency") {
                // Should have both write and read in packets
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(2));
                
                // Find the read response
                auto read_response = std::find_if(mock_upper_level.packets.begin(), 
                                                  mock_upper_level.packets.end(),
                                                  [](const auto& pkt) { 
                                                      return pkt.pkt.type == access_type::LOAD && pkt.return_time > 0; 
                                                  });
                
                if (read_response != mock_upper_level.packets.end()) {
                    REQUIRE_THAT(*read_response, champsim::test::LatencyRangeMatcher(
                        {0},
                        {t_cxl + 1}));  // a short latency, compared to 2*t_cxl + dram_latency + t_rd
                }
            }
        }
        
        WHEN("Read merge opportunity exists") {
            REQUIRE(mock_dram.packet_count() == 0);
            static uint64_t id = 10;
            const auto target_addr = static_cast<uint64_t>(0x200000);
            
            // Issue two reads to the same address quickly
            for (int i = 0; i < 2; ++i) {
                decltype(mock_upper_level)::request_type read_req;
                read_req.address = champsim::address{target_addr};
                read_req.cpu = 0;
                read_req.instr_id = id++;
                read_req.type = access_type::LOAD;
                read_req.is_translated = true;
                
                mock_upper_level.issue(read_req);
            }
            
            // Process to completion
            for (auto i = 0; i < 100; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Reads are merged efficiently") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(2));
                
                // Read merging: CXL sends only 1 request to DRAM for 2 read requests
                REQUIRE(mock_dram.packet_count() == 1);
            }
        }
        
        WHEN("Write merge opportunity exists") {
            static uint64_t id = 20;
            const auto target_addr = static_cast<uint64_t>(0x300000);
            
            // Issue multiple writes to the same address
            for (int i = 0; i < 3; ++i) {
                decltype(mock_upper_level)::request_type write_req;
                write_req.address = champsim::address{target_addr};
                write_req.data = champsim::address{static_cast<uint64_t>(0x1000 + i)}; // Different data
                write_req.cpu = 0;
                write_req.instr_id = id++;
                write_req.type = access_type::WRITE;
                write_req.is_translated = true;
                write_req.response_requested = false;
                
                mock_upper_level.issue(write_req);
            }
            
            // Process to completion
            for (auto i = 0; i < 100; ++i) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            THEN("Writes are merged to reduce DRAM traffic") {
                REQUIRE_THAT(mock_upper_level.packets, Catch::Matchers::SizeIs(3));
                
                // Write merging: CXL sends only 1 request to DRAM for 3 write requests
                // (typically the latest write with the most recent data)
                REQUIRE(mock_dram.packet_count() == 1);
                
                // Writes don't generate responses
                for (const auto& packet : mock_upper_level.packets) {
                    if (packet.pkt.type == access_type::WRITE) {
                        REQUIRE(packet.return_time == 0);
                    }
                }
            }
        }
    }
}
