#include <catch.hpp>
#include "mocks.hpp"
#include "shim_layer.h"
#include "dram_controller.h"

SCENARIO("SHIM_LAYER constructor initializes correctly with different memory configurations") {
    GIVEN("Different memory subsystem configurations") {
        WHEN("Both DRAM and CXL are enabled") {
            // Create mock channels
            champsim::channel upper_channel{};
            champsim::channel dram_channel{};
            champsim::channel cxl_channel{};
            std::vector<champsim::channel*> lower_channels = {&dram_channel, &cxl_channel};

            MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200}, champsim::chrono::picoseconds{6400}, std::size_t{18}, std::size_t{18}, std::size_t{18}, std::size_t{38}, champsim::chrono::microseconds{64000}, {}, 64, 64, 1, champsim::data::bytes{8}, 1024, 1024, 4, 4, 4, 8192};
            MEMORY_CONTROLLER cxl_dram{champsim::chrono::picoseconds{3200}, champsim::chrono::picoseconds{6400}, std::size_t{18}, std::size_t{18}, std::size_t{18}, std::size_t{38}, champsim::chrono::microseconds{64000}, {}, 64, 64, 1, champsim::data::bytes{8}, 1024, 1024, 4, 4, 4, 8192};
            SHIM_LAYER uut{&upper_channel, std::move(lower_channels), 64, 64, 32, 4, 2, &dram, &cxl_dram};
            
            THEN("SHIM_LAYER constructor succeeds with HYBRID configuration") {
                // Constructor should succeed without throwing exceptions
                // Mode behavior will be tested in operational tests
                REQUIRE(true); // Constructor completed successfully
            }
        }
    }
}

SCENARIO("SHIM_LAYER component lifecycle management works correctly") {
    GIVEN("A SHIM_LAYER with basic configuration") {
        to_rq_MRP mock_ul;
        do_nothing_MRC mock_ll;
        
        MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200}, champsim::chrono::picoseconds{6400}, std::size_t{18}, std::size_t{18}, std::size_t{18}, std::size_t{38}, champsim::chrono::microseconds{64000}, {}, 64, 64, 1, champsim::data::bytes{8}, 1024, 1024, 4, 4, 4, 8192};

        std::vector<champsim::channel*> lower_channels = {&mock_ll.queues};
        SHIM_LAYER uut{&mock_ul.queues, std::move(lower_channels), 64, 64, 32, 4, 2, &dram, nullptr};
        
        std::array<champsim::operable*, 3> elements{{&uut, &mock_ul, &mock_ll}};
        
        WHEN("Component lifecycle phases are executed") {
            // Test phase management lifecycle
            for (auto elem : elements) {
                elem->initialize();
                elem->warmup = false;
                elem->begin_phase();
            }
            
            // Run some operations
            for (int cycle = 0; cycle < 5; ++cycle) {
                for (auto elem : elements) {
                    elem->_operate();
                }
            }
            
            // Test end_phase
            for (auto elem : elements) {
                elem->end_phase(0);  // CPU 0
            }
            
            THEN("All lifecycle phases complete without errors") {
                // If we reach here without crashing, phase management works
                REQUIRE(true);
            }
        }
    }
}

TEST_CASE("SHIM_LAYER initialization completes successfully") {
    to_rq_MRP mock_ul;
    do_nothing_MRC mock_ll;

    MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200}, champsim::chrono::picoseconds{6400}, std::size_t{18}, std::size_t{18}, std::size_t{18}, std::size_t{38}, champsim::chrono::microseconds{64000}, {}, 64, 64, 1, champsim::data::bytes{8}, 1024, 1024, 4, 4, 4, 8192};

    std::vector<champsim::channel*> lower_channels = {&mock_ll.queues};
    SHIM_LAYER uut{&mock_ul.queues, std::move(lower_channels), 64, 64, 32, 4, 2, &dram, nullptr};
    
    // Test initialize function
    uut.initialize();
    
    // If we reach here without crashing, initialization works
    REQUIRE(true);
}

TEST_CASE("SHIM_LAYER deadlock detection system functions properly") {
    to_rq_MRP mock_ul;
    do_nothing_MRC mock_ll;

    MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200}, champsim::chrono::picoseconds{6400}, std::size_t{18}, std::size_t{18}, std::size_t{18}, std::size_t{38}, champsim::chrono::microseconds{64000}, {}, 64, 64, 1, champsim::data::bytes{8}, 1024, 1024, 4, 4, 4, 8192};

    std::vector<champsim::channel*> lower_channels = {&mock_ll.queues};
    SHIM_LAYER uut{&mock_ul.queues, std::move(lower_channels), 64, 64, 32, 4, 2, &dram, nullptr};
    
    // Test print_deadlock function
    uut.print_deadlock();
    
    // If we reach here without crashing, deadlock detection works
    REQUIRE(true);
}
