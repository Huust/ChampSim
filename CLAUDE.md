# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Configuration Commands

### Initial Setup
```bash
# Download dependencies using vcpkg
git submodule update --init
vcpkg/bootstrap-vcpkg.sh
vcpkg/vcpkg install
```

### Building ChampSim
```bash
# Configure with JSON file (or empty for defaults)
./config.sh champsim_config.json
# Or for custom configurations
./config.sh <custom_config.json>

# Build the simulator
make

# Clean build artifacts
make clean
make configclean
```

### Testing
```bash
# Run all tests
make test

# Run Python tests specifically
make pytest

# Run C++ tests specifically
make maketest

# Run specific test by number (e.g., test 044)
make test TEST_NUM=044
```

### Running Simulations
```bash
# Basic simulation with trace file
bin/champsim --warmup_instructions 200000000 --simulation_instructions 500000000 path/to/trace.champsimtrace.xz

# Using different executables (if multiple configurations built)
bin/<executable_name> --warmup_instructions <warmup> --simulation_instructions <sim> <trace_file>
```

## Architecture Overview

### Core Components

**ChampSim Environment Structure:**
- `environment.h` defines the main system interface with views into all major components
- Components communicate through `champsim::channel` objects that handle request/response queues
- All major components inherit from `champsim::operable` providing common clock and lifecycle management

**Memory Hierarchy:**
```
CPU (O3_CPU) → L1I/L1D Caches → L2C Cache → LLC Cache → SHIM_LAYER (Router) → {DRAM_CONTROLLER, CXL_CONTROLLER}
                    ↓                                                                              ↓
                   ITLB/DTLB → STLB → PTW                                                   CXL_DRAM (internal)
```

**Key Architectural Classes:**
- `O3_CPU`: Out-of-order CPU core with detailed pipeline modeling
- `CACHE`: Unified cache implementation with configurable prefetchers and replacement policies
- `SHIM_LAYER`: Address-based router that directs memory requests between DRAM and CXL memory
- `MEMORY_CONTROLLER`/`DRAM_CONTROLLER`: Models DDR memory with banks, timing, and refresh
- `CXL_CONTROLLER`/`CXL_CHANNEL`: Models CXL memory devices with protocol overhead and internal DRAM
- `PageTableWalker`: Handles virtual-to-physical address translation with page table caches

### Configuration System

**JSON-Driven Configuration:**
- `champsim_config.json` contains the system configuration
- `config.sh` (Python script) parses JSON and generates C++ instantiation code
- Modular design allows swapping branch predictors, prefetchers, and replacement policies
- Configuration generates build-specific files in `.csconfig/` directory

**Module Directories:**
- `branch/`: Branch predictor implementations (bimodal, gshare, perceptron, etc.)
- `prefetcher/`: Data prefetcher modules for different cache levels
- `replacement/`: Cache replacement policy implementations
- `btb/`: Branch target buffer implementations

### Custom Extensions (This Repository)

**SHIM_LAYER Router:**
- Routes memory requests based on address ranges (>=4GB → CXL, <4GB → DRAM)
- Implements bandwidth modeling with separate upper/lower stream limits
- Provides detailed statistics for bandwidth congestion vs buffer fullness
- Located in `inc/shim_layer.h` and `src/shim_layer.cc`

**CXL Memory Modeling:**
- `CXL_CONTROLLER`: Manages interface with upper-level caches
- `CXL_CHANNEL`: Models PCIe/CXL bus with protocol latencies (tCXL, tRD, tWR)
- Connected to internal DRAM controller for CXL device memory
- Implements collision detection and write forwarding
- Located in `inc/cxl_memory.h` and `src/cxl_memory.cc`

### Important Implementation Details

**Request/Response Flow:**
- Requests flow down the hierarchy via `add_rq()`, `add_wq()`, `add_pq()` methods
- Responses flow back up via `returned` deques in channel objects
- Each component's `operate()` method processes one cycle of simulation
- Warmup vs simulation phases controlled by global `warmup` flag

**Bandwidth Modeling:**
- Uses `champsim::bandwidth` class to model per-cycle operation limits
- Components track bandwidth consumption and congestion statistics
- SHIM_LAYER distinguishes between port bandwidth limits vs buffer capacity limits

**Address Mapping:**
- Physical addresses mapped to memory controllers via configurable schemes
- DRAM uses detailed bank/rank/row/column addressing with timing constraints
- CXL implements simpler mapping focused on protocol and bus modeling

### Development Workflow

When adding new memory components or modifying existing ones:
1. Update JSON configuration to include new parameters
2. Modify `config/instantiation_file.py` if new component types added
3. Follow the `champsim::operable` interface for lifecycle management
4. Use `champsim::channel` for inter-component communication
5. Add appropriate statistics collection (distinguish warmup vs simulation phases)
6. Test with both single and multi-level memory hierarchies

### Module Development

**Creating Custom Modules:**
```bash
# Create a new prefetcher module
mkdir prefetcher/my_prefetcher
cp prefetcher/no_l2c/no.cc prefetcher/my_prefetcher/my_prefetcher.cc

# Add to configuration file:
{
    "L2C": {
        "prefetcher": "my_prefetcher"
    }
}
```

**Module Interface Requirements:**
- Branch predictors: Implement prediction and update functions
- Prefetchers: Implement `prefetcher_operate()` and `prefetcher_cache_fill()`
- Replacement policies: Implement `find_victim()` and `update_replacement_state()`

### Key File Locations

**Core Source Files:**
- `src/main.cc`: Simulation entry point and main loop
- `src/cache.cc`: Cache implementation with MSHR management
- `src/ooo_cpu.cc`: Out-of-order CPU core with pipeline stages
- `src/dram_controller.cc`: DDR memory controller with bank modeling
- `src/shim_layer.cc`: Memory routing layer (custom to this repository)
- `src/cxl_memory.cc`: CXL memory device modeling (custom to this repository)

**Configuration System:**
- `config/`: Python scripts for parsing JSON and generating build files
- `config/instantiation_file.py`: Main configuration parser and code generator
- `champsim_config.json`: Example configuration with all available options