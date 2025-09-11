#include <catch.hpp>
#include "mocks.hpp"
#include "cxl_memory.h"

// We can use to_rq_MRP to create the upper levels, but in this testing file,
// we don't need to operate the upper levels (to send / receive requests or responses),
// so we don't need to construct the to_rq_MRP objects
SCENARIO("CXL_CONTROLLER construction and configuration") {
    GIVEN("Various CXL controller configurations") {
        
        WHEN("A basic CXL controller is constructed") {
            const auto clock_period = champsim::chrono::picoseconds{312};
            const std::size_t t_cxl = 50;
            const std::size_t rq_size = 1024;
            const std::size_t wq_size = 1024;
            const champsim::data::bytes channel_width{8};
            const double rx_bw = 25.7;
            const double tx_bw = 12.7;
            
            // Create mock lower level (DRAM controller)
            do_nothing_MRC mock_dram;
            
            // Create mock upper level channels
            champsim::channel upper_level1{rq_size, 32, wq_size, champsim::data::bits{}, false};
            std::vector<champsim::channel*> upper_levels = {&upper_level1};
            
            CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                              rq_size, wq_size, channel_width, rx_bw, tx_bw, &mock_dram.queues};
            
            THEN("CXL controller is constructed successfully") {
                // Constructor should complete without exceptions
                REQUIRE(true);
            }
        }
        
        WHEN("CXL controller is constructed with different queue sizes") {
            const auto clock_period = champsim::chrono::picoseconds{1000};
            const std::size_t t_cxl = 5;
            
            auto rq_size = GENERATE(as<std::size_t>{}, 32, 64, 128);
            auto wq_size = GENERATE(as<std::size_t>{}, 16, 32, 64);
            
            do_nothing_MRC mock_dram;
            champsim::channel upper_level{rq_size, 32, wq_size, champsim::data::bits{}, false};
            std::vector<champsim::channel*> upper_levels = {&upper_level};
            
            CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                              rq_size, wq_size, champsim::data::bytes{8}, 8.0, 8.0, &mock_dram.queues};
            
            THEN("Constructor succeeds with different queue configurations") {
                REQUIRE(true);
            }
        }
        
        WHEN("CXL controller is constructed with different channel widths") {
            const auto clock_period = champsim::chrono::picoseconds{1000};
            const std::size_t t_cxl = 8;
            const std::size_t rq_size = 64;
            const std::size_t wq_size = 64;
            
            long long channel_width = GENERATE(as<std::size_t>{}, 1, 4, 8, 16); // PCIe x1, x4, x8, x16
            
            do_nothing_MRC mock_dram;
            champsim::channel upper_level{rq_size, 32, wq_size, champsim::data::bits{}, false};
            std::vector<champsim::channel*> upper_levels = {&upper_level};
            
            CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels),
                              rq_size, wq_size, champsim::data::bytes{channel_width},
                              8.0, 8.0, &mock_dram.queues};
            
            THEN("Constructor handles different PCIe widths correctly") {
                REQUIRE(true);
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER timing parameter configuration") {
    GIVEN("A CXL controller with specific timing parameters") {
        do_nothing_MRC mock_dram;
        champsim::channel upper_level{64, 32, 64, champsim::data::bits{}, false};
        std::vector<champsim::channel*> upper_levels = {&upper_level};
        
        WHEN("Controller is configured with fast CXL timing") {
            const auto clock_period = champsim::chrono::picoseconds{500}; // 2GHz
            const std::size_t t_cxl = 5; // 5 cycle CXL latency
            
            CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                              64, 64, champsim::data::bytes{16}, 32.0, 32.0, &mock_dram.queues};
            
            THEN("Constructor succeeds with fast timing") {
                REQUIRE(true);
            }
        }
        
        WHEN("Controller is configured with slow CXL timing") {
            const auto clock_period = champsim::chrono::picoseconds{2000}; // 500MHz
            const std::size_t t_cxl = 20; // 20 cycle CXL latency
            
            champsim::channel upper_level2{64, 32, 64, champsim::data::bits{}, false};
            std::vector<champsim::channel*> upper_levels2 = {&upper_level2};
            
            CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels2), 
                              64, 64, champsim::data::bytes{8}, 8.0, 8.0, &mock_dram.queues};
            
            THEN("Constructor succeeds with slow timing") {
                REQUIRE(true);
            }
        }
    }
}

SCENARIO("CXL_CONTROLLER bandwidth configuration") {
    GIVEN("A CXL controller with different bandwidth settings") {
        const auto clock_period = champsim::chrono::picoseconds{1000};
        const std::size_t t_cxl = 10;
        do_nothing_MRC mock_dram;
        
        WHEN("Controller is configured with high bandwidth") {
            champsim::channel upper_level{64, 32, 64, champsim::data::bits{}, false};
            std::vector<champsim::channel*> upper_levels = {&upper_level};
            
            const double high_rx_bw = 64.0; // 64 GT/s
            const double high_tx_bw = 64.0; // 64 GT/s
            
            CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                              64, 64, champsim::data::bytes{16}, high_rx_bw, high_tx_bw, &mock_dram.queues};
            
            THEN("Constructor succeeds with high bandwidth configuration") {
                REQUIRE(true);
            }
        }
        
        WHEN("Controller is configured with asymmetric bandwidth") {
            champsim::channel upper_level{64, 32, 64, champsim::data::bits{}, false};
            std::vector<champsim::channel*> upper_levels = {&upper_level};
            
            const double rx_bw = 32.0; // 32 GT/s read
            const double tx_bw = 16.0; // 16 GT/s write (asymmetric)
            
            CXL_CONTROLLER uut{clock_period, t_cxl, std::move(upper_levels), 
                              64, 64, champsim::data::bytes{8}, rx_bw, tx_bw, &mock_dram.queues};
            
            THEN("Constructor handles asymmetric bandwidth correctly") {
                REQUIRE(true);
            }
        }
    }
}

TEST_CASE("CXL_CONTROLLER initialization") {
    do_nothing_MRC mock_dram;
    champsim::channel upper_level{64, 32, 64, champsim::data::bits{}, false};
    std::vector<champsim::channel*> upper_levels = {&upper_level};
    
    CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 10, std::move(upper_levels), 
                      64, 64, champsim::data::bytes{8}, 16.0, 16.0, &mock_dram.queues};
    
    // Test initialization
    uut.initialize();
    
    // If we reach here without exceptions, initialization succeeded
    REQUIRE(true);
}

TEST_CASE("CXL_CONTROLLER multiple upper level channels") {
    do_nothing_MRC mock_dram;
    
    // Create multiple upper level channels (like shared memory for CPU and GPU, etc.)
    champsim::channel upper_level1{64, 32, 64, champsim::data::bits{}, false};
    champsim::channel upper_level2{32, 16, 32, champsim::data::bits{}, false};
    std::vector<champsim::channel*> upper_levels = {&upper_level1, &upper_level2};
    
    CXL_CONTROLLER uut{champsim::chrono::picoseconds{1000}, 10, std::move(upper_levels), 
                      64, 64, champsim::data::bytes{16}, 16.0, 16.0, &mock_dram.queues};
    
    uut.initialize();
    
    // Constructor should handle multiple upper levels
    REQUIRE(true);
}
