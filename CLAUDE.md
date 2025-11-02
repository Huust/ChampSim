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

# Two-phase heatmap-based simulation (for hybrid memory systems)
# Phase 1: Generate heatmap
bin/champsim_hybrid --warmup_instructions 200M --simulation_instructions 500M --generate-heatmap trace.xz

# Phase 2: Use heatmap for allocation (access-count or criticality-based)
bin/champsim_hybrid --warmup_instructions 200M --simulation_instructions 500M --use-heatmap trace.xz
bin/champsim_hybrid --warmup_instructions 200M --simulation_instructions 500M --use-heatmap --sort-by-criticality trace.xz
```

### Building Multiple Hardware Configurations (Repository-Specific)

This repository uses hardware configuration JSON files to build different memory system variants:

```bash
# Build all three binary configurations at once
./scripts/make_all_binaries.sh

# Or build individually with specific config files:
./config.sh hardware_configs/hybrid.json && make
./config.sh hardware_configs/dram_only.json && make
./config.sh hardware_configs/cxl_only.json && make
```

**Available Hardware Configurations:**
- `hardware_configs/hybrid.json` → `bin/champsim_hybrid` (CXL + DRAM)
- `hardware_configs/dram_only.json` → `bin/champsim_dram_only` (DRAM only)
- `hardware_configs/cxl_only.json` → `bin/champsim_cxl_only` (CXL only)

### Cluster Job Submission and Analysis Workflow

**Running experiments on SLURM cluster:**
```bash
# Submit all configurations across all traces
python3 scripts/runall_pelle.py

# Or manually submit individual jobs
sbatch --output=results/config_trace.out --error=results/config_trace.err \
    scripts/run_pelle.sh <configuration> <results_dir> <trace_path>
```

**Supported configurations in run_pelle.sh:**
- `cxl_only`: CXL-only memory system
- `dram_only`: DRAM-only memory system
- `hybrid-roundrobin`: Hybrid with round-robin allocation
- `hybrid-hotness-access`: Two-phase with access-count-based heatmap
- `hybrid-hotness-criticality`: Two-phase with criticality-based heatmap

**Data collection and visualization:**
```bash
# Collect statistics from all .out files into CSV/pickle
python3 scripts/collect_stats.py

# Generate various plots from collected data
python3 scripts/plot.py              # Basic IPC and MPKI plots
python3 scripts/advanced_plot.py     # Detailed comparisons
python3 scripts/heatmap_plot.py      # Memory access heatmaps
python3 scripts/tlb_plot.py          # TLB miss rate and latency analysis
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
- `MEMORY_CONTROLLER`: Base class for memory controllers (virtual interface)
- `DRAM_CONTROLLER`: Native ChampSim memory controller with bank/rank/row/column modeling
- `RAMULATOR_CONTROLLER`: Ramulator-based memory controller for detailed DRAM simulation
- `CXL_CONTROLLER`/`CXL_CHANNEL`: Models CXL memory devices with protocol overhead and internal DRAM
- `PageTableWalker`: Handles virtual-to-physical address translation with page table caches

### Configuration System

**JSON-Driven Configuration:**
- `champsim_config.json` contains the system configuration
- `config.sh` (Python script) parses JSON and generates C++ instantiation code
- Modular design allows swapping branch predictors, prefetchers, and replacement policies
- Configuration generates build-specific files in `.csconfig/` directory

**Ramulator Configuration:**
- Set `"use_ramulator": true` in `physical_memory` or `cxl_memory.dram` sections
- Specify Ramulator config file: `"ramulator_config_path": "./ramulator/configs/DDR4-config.cfg"`
- Specify stats output directory: `"stats_output_dir": "./results/ramulator/stats.dram"`
- Available Ramulator configs: DDR3, DDR4, LPDDR3, LPDDR4, GDDR5, HBM, WideIO, WideIO2, and more
- When `use_ramulator` is false or unset, ChampSim uses native `DRAM_CONTROLLER`

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

**Ramulator Integration (Repository-Specific):**
- `RAMULATOR_CONTROLLER`: Alternative memory controller using Ramulator for detailed DRAM simulation
- Inherits from `MEMORY_CONTROLLER` base class alongside `DRAM_CONTROLLER`
- Supports multiple DRAM standards (DDR3/4, LPDDR3/4, GDDR5, HBM, WideIO, etc.)
- Clock domain adaptation via accumulator-based "gearbox" (m_ratio and m_accumulator)
- Configuration via `.cfg` files in `ramulator/configs/` directory
- Enable by setting `"use_ramulator": true` in JSON config's `physical_memory` or `cxl_memory.dram` sections
- Ramulator config path and stats output directory are configurable per memory controller
- Callbacks handle read/write completion and return responses to upper-level caches
- Located in `inc/ramulator_controller.h` and `src/ramulator_controller.cc`
- Ramulator wrapper interface in `ramulator/src/ChampSimWrapper.{h,cpp}`

**Heatmap-Based Memory Allocation (Repository-Specific):**
- Two-phase simulation for intelligent memory placement
- Phase 1: Generates heatmap of page access patterns (`--generate-heatmap`)
- Phase 2: Uses heatmap to allocate hot pages to DRAM, cold to CXL (`--use-heatmap`)
- Two allocation strategies:
  - Access-count based: Pages sorted by total access count
  - Criticality-based: Pages sorted by criticality (`--sort-by-criticality`)
- Heatmap files stored per-trace for reproducible allocation decisions
- Criticality tracking: Instructions track LLC miss sources and mark retirement-time critical misses

**Statistics Collection (Repository-Specific):**
- Cache statistics: Hits, misses, MSHR merges, miss latency, congestion cycles
- SHIM layer statistics: Separate tracking for DRAM and CXL read/write requests
- TLB statistics: DTLB/STLB access counts, miss counts, and average miss latencies
- All statistics separated between warmup and simulation phases

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

**Dual Memory Controller Architecture:**
- Both `DRAM_CONTROLLER` and `RAMULATOR_CONTROLLER` inherit from virtual base `MEMORY_CONTROLLER`
- Selection controlled by `"use_ramulator"` flag in JSON configuration
- Allows transparent switching between native ChampSim and Ramulator simulation
- Each CXL device and physical memory can independently choose controller type
- When extending memory controller functionality, consider both implementations

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
- `src/dram_controller.cc`: Native ChampSim DDR memory controller with bank modeling
- `src/ramulator_controller.cc`: Ramulator-based memory controller (custom to this repository)
- `src/shim_layer.cc`: Memory routing layer (custom to this repository)
- `src/cxl_memory.cc`: CXL memory device modeling (custom to this repository)

**Configuration System:**
- `config/`: Python scripts for parsing JSON and generating build files
- `config/instantiation_file.py`: Main configuration parser and code generator
- `champsim_config.json`: Example configuration with all available options

**Ramulator Files:**
- `ramulator/`: Ramulator submodule for detailed DRAM simulation
- `ramulator/configs/`: Ramulator configuration files for different DRAM standards
- `ramulator/src/ChampSimWrapper.{h,cpp}`: ChampSim-Ramulator integration interface
- Statistics output to directories specified in JSON config (e.g., `results/ramulator/`)

**Analysis and Visualization Scripts:**
- `scripts/runall_pelle.py`: Batch job submission for SLURM cluster
- `scripts/run_pelle.sh`: Individual job runner with configuration handling
- `scripts/collect_stats.py`: Extracts IPC, LLC stats, SHIM statistics, and TLB metrics from .out files
- `scripts/plot.py`: Generates MPKI distribution and IPC comparison plots
- `scripts/advanced_plot.py`: Additional detailed performance analysis plots
- `scripts/heatmap_plot.py`: Visualizes memory access patterns and allocation
- `scripts/tlb_plot.py`: Analyzes TLB miss rates, MPKI, and average miss latencies across configurations

**Expected Results Directory Structure:**
```
results/
├── dram_only/          # DRAM-only configuration outputs
├── cxl_only/           # CXL-only configuration outputs
├── hybrid-roundrobin/  # Hybrid round-robin allocation
├── hybrid-hotness-access/       # Access-count based allocation
├── hybrid-hotness-criticality/  # Criticality-based allocation
├── heatmaps/           # Generated heatmap files (per-trace)
├── collected_stats.csv # Aggregated statistics
└── collected_stats.pkl # Pandas pickle for analysis
```