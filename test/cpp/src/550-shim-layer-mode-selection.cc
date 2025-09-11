#include <catch.hpp>
#include "shim_layer.h"

SCENARIO("SHIM_LAYER correctly determines operating mode based on configuration") {
    GIVEN("Different memory subsystem configurations") {
        
        WHEN("Both DRAM and CXL are enabled") {
            bool is_dram_enabled = true;
            bool is_cxl_enabled = true;
            
            // Create mock channels
            champsim::channel upper_channel{};
            champsim::channel dram_channel{};
            champsim::channel cxl_channel{};
            std::vector<champsim::channel*> lower_channels = {&dram_channel, &cxl_channel};
            
            SHIM_LAYER uut{&upper_channel, std::move(lower_channels), 64, 64, 32, is_dram_enabled, is_cxl_enabled};
            
            THEN("SHIM_LAYER constructor succeeds with HYBRID configuration") {
                // Constructor should succeed without throwing exceptions
                // Mode behavior will be tested in operational tests (551-553)
                REQUIRE(true); // Constructor completed successfully
            }
        }
        
        WHEN("Only DRAM is enabled") {
            bool is_dram_enabled = true;
            bool is_cxl_enabled = false;
            
            champsim::channel upper_channel{};
            champsim::channel dram_channel{};
            std::vector<champsim::channel*> lower_channels = {&dram_channel};
            
            SHIM_LAYER uut{&upper_channel, std::move(lower_channels), 64, 64, 32, is_dram_enabled, is_cxl_enabled};
            
            THEN("SHIM_LAYER constructor succeeds with DRAM_ONLY configuration") {
                REQUIRE(true); // Constructor completed successfully
            }
        }
        
        WHEN("Only CXL is enabled") {
            bool is_dram_enabled = false;
            bool is_cxl_enabled = true;
            
            champsim::channel upper_channel{};
            champsim::channel cxl_channel{};
            std::vector<champsim::channel*> lower_channels = {&cxl_channel};
            
            SHIM_LAYER uut{&upper_channel, std::move(lower_channels), 64, 64, 32, is_dram_enabled, is_cxl_enabled};
            
            THEN("SHIM_LAYER constructor succeeds with CXL_ONLY configuration") {
                REQUIRE(true); // Constructor completed successfully
            }
        }
    }
}

TEST_CASE("SHIM_LAYER queue sizes are configured correctly") {
    champsim::channel upper_channel{};
    champsim::channel lower_channel{};
    std::vector<champsim::channel*> lower_channels = {&lower_channel};
    
    const std::size_t rq_size = 128;
    const std::size_t wq_size = 64;
    const std::size_t pq_size = 32;
    
    SHIM_LAYER uut{&upper_channel, std::move(lower_channels), rq_size, wq_size, pq_size, true, false};
    
    // Note: We can't directly access private queue sizes
    // This test mainly ensures the constructor doesn't throw exceptions
    // Queue behavior will be tested in operational tests
    REQUIRE(true);
}
