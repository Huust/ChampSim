# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Configuration Commands

### Initial Setup
```bash
git submodule update --init
vcpkg/bootstrap-vcpkg.sh
vcpkg/vcpkg install
```

### Building ChampSim
```bash
# Configure with JSON file, then build
./config.sh champsim_config.json   # Python3 script that generates C++ instantiation code + _configuration.mk
make                                # or make -j for parallel

# Clean
make clean        # Remove object/dependency files
make configclean  # Full clean including generated config in .csconfig/
```

### Building All Hardware Configurations
```bash
# Build all four binaries at once (recommended)
./scripts/make_all_binaries.sh
```

This produces:
- `bin/champsim_tiered_memory` — CXL + DRAM hybrid (from `hardware_configs/tiered_memory.json`)
- `bin/champsim_tiered_memory_skip` — hybrid + skip translation (`SKIP_TRANSLATION=1`)
- `bin/champsim_dram_only` — DRAM only (from `hardware_configs/dram_only.json`)
- `bin/champsim_cxl_only` — CXL only (from `hardware_configs/cxl_only.json`)

To build individually:
```bash
./config.sh hardware_configs/tiered_memory.json && make
./config.sh hardware_configs/tiered_memory.json && make SKIP_TRANSLATION=1  # adds -DENABLE_SKIP_CXL_TRANSLATION
./config.sh hardware_configs/dram_only.json && make
./config.sh hardware_configs/cxl_only.json && make
```

### Testing
```bash
make test                # All tests (Catch2 C++ + Python)
make pytest              # Python tests only (test/python/)
make maketest            # C++ tests only
make test TEST_NUM=044   # Specific test by number
```

### CLI Flags (src/main.cc, uses CLI11)
```
-w, --warmup-instructions N      Warmup instruction count
-i, --simulation-instructions N  Simulation instruction count
-c, --cloudsuite                 Read traces in CloudSuite format
--hide-heartbeat                 Disable heartbeat output
--json <path>                    JSON output file

# Heatmap two-phase simulation
--generate-heatmap <path>        Phase 1: generate heatmap file from LLC misses
--use-heatmap <path>             Phase 2: load heatmap for hot/cold page allocation
--sort-by-criticality            Sort pages by criticality instead of access count
--ratio N:M                      DRAM:CXL allocation ratio (default "1:3")

# Interleaving allocation
--generate-interleaving <path>   Track page allocations in interleaving mode
--use-interleaving <path>        Load precomputed allocation mapping (CSV: vpage,device)

# Perforated pages (fragmented 2MB large pages)
--pmap <path>                    Page map file (CSV: vpn_hex,page_size 0=4K 1=2M 2=PERF)
--perf-frag-ratio <0.0-1.0>     Auto-perforate: fraction of holes in 2MB pages
--perf-frag-dist <dist>          Hole distribution: clustered (default), dispersed, random

# Diagnostics
--save-bandwidth <path>          Save bandwidth statistics
```

### Running Simulations
```bash
# Two-phase heatmap workflow (most common):
# 1. Generate heatmap with dram_only binary
bin/champsim_dram_only -w 200000000 -i 500000000 \
    --generate-heatmap ./heatmaps/test.heatmap path/to/trace.champsimtrace.xz

# 2. Use heatmap with tiered memory binary
bin/champsim_tiered_memory -w 200000000 -i 500000000 \
    --use-heatmap ./heatmaps/test.heatmap --ratio 1:1 path/to/trace.champsimtrace.xz

# 3. Compare with skip-translation version
bin/champsim_tiered_memory_skip -w 200000000 -i 500000000 \
    --use-heatmap ./heatmaps/test.heatmap --ratio 1:1 path/to/trace.champsimtrace.xz
```

**Common mistake**: Using the wrong binary for each phase. Phase 1 (heatmap generation) must use `champsim_dram_only`. Phase 2 (heatmap usage) must use `champsim_tiered_memory` or `champsim_tiered_memory_skip`.

### SLURM Cluster Workflow
```bash
# Submit all 11 configurations across all traces (with SLURM dependency management)
python3 scripts/runall_pelle.py

# Collect results into CSV/pickle
python3 scripts/collect_stats.py

# Visualize
python3 scripts/plot_by_suite.py      # Suite-level geomean analysis (dram-only baseline)
python3 scripts/skip_vs_no_skip.py    # Skip vs no-skip comparison (no_skip baseline)
python3 scripts/plot.py               # Per-trace detailed analysis
```

The 11 configurations have a dependency DAG: `dram_only` and `interleaving` run first (generating shared heatmap/allocation files), then all dependent configs run. See `scripts/SLURM_WORKFLOW.md` for the full dependency graph.

## Architecture Overview

### Build System
- **Language**: C++17 (`global.options`: `-std=c++17 -g -O3`)
- **Build tool**: Custom Makefile (not CMake)
- **Config pipeline**: `config.sh` (Python3) parses JSON → generates `.csconfig/` files + `_configuration.mk` → `make` compiles
- **Dependencies**: vcpkg (submodule), Ramulator (submodule, built as `ramulator/libramulator.a`)

### Memory Hierarchy
```
CPU (O3_CPU) → L1I/L1D → L2C → LLC → SHIM_LAYER → { DRAM_CONTROLLER, CXL_CONTROLLER }
                 ↓                                                         ↓
              ITLB/DTLB → STLB → PTW                               CXL_DRAM (internal)
```

### Key Architectural Classes
- **`champsim::operable`**: Base class providing clock and lifecycle management for all components
- **`champsim::channel`**: Inter-component communication via request/response queues
- **`O3_CPU`**: Out-of-order CPU core with pipeline modeling
- **`CACHE`**: Unified cache with configurable prefetchers and replacement policies
- **`SHIM_LAYER`** (`inc/shim_layer.h`, `src/shim_layer.cc`): Routes LLC misses to DRAM or CXL based on address. Has three modes: `DRAM_ONLY`, `CXL_ONLY`, `HYBRID`. Address boundary is dynamic (based on configured DRAM size, not a hardcoded constant). Tracks bandwidth congestion vs buffer fullness separately.
- **`MEMORY_CONTROLLER`**: Virtual base class for memory controllers
  - **`DRAM_CONTROLLER`**: Native ChampSim controller with bank/rank/row/column modeling
  - **`RAMULATOR_CONTROLLER`** (`src/ramulator_controller.cc`): Ramulator-based controller with clock domain adaptation (accumulator-based gearbox). Selected via `"use_ramulator": true` in JSON config.
- **`CXL_CONTROLLER`/`CXL_CHANNEL`** (`src/cxl_memory.cc`): CXL device modeling with PCIe protocol latencies (tCXL, tRD, tWR), collision detection, write forwarding, and internal DRAM controller.
- **`PageTableWalker`**: Virtual-to-physical translation with page table caches

### Request/Response Flow
- Requests flow down via `add_rq()`, `add_wq()`, `add_pq()` methods
- Responses flow up via `returned` deques in channel objects
- Each component's `operate()` processes one simulation cycle
- Warmup vs simulation phases controlled by global `warmup` flag

### Configuration System
- JSON configs in `hardware_configs/` define the memory system topology
- `config.sh` parses JSON → generates C++ instantiation code in `.csconfig/`
- Ramulator: set `"use_ramulator": true` in `physical_memory` or `cxl_memory.dram` sections, with `"ramulator_config_path"` pointing to a `.cfg` file in `ramulator/configs/`
- Swappable modules: `branch/`, `prefetcher/`, `replacement/`, `btb/` directories

### Skip Translation Optimization
- **Purpose**: Reduces CXL address translation overhead for cold pages
- **Compile flag**: `make SKIP_TRANSLATION=1` → defines `ENABLE_SKIP_CXL_TRANSLATION` preprocessor macro
- **Implementation**: In `src/cache.cc`, applied at **L1 caches only** (L1D and L1I). For pages known to be CXL-bound (via heatmap or interleaving allocation), translation is resolved directly at L1 without a full page table walk.
- **Two paths**: Heatmap-based (checks `champsim::heatmap::is_hotness_allocation_enabled()`) and interleaving-based (checks `g_vmem->use_allocation_map`)

### Perforated Pages (multi-page-size branch)
- **Based on**: ISCA 2020 paper "Perforated Page: Supporting Fragmented Memory Allocation for Large Pages" (Park et al.)
- **Concept**: 2MB pages where some 4KB sub-pages ("holes") are mapped to non-contiguous physical addresses
- **PageSize enum**: `PAGE_4K=0`, `PAGE_2M=1`, `PAGE_PERF=2` (in `inc/vmem.h`)
- **Data structures** (`vmem.h/cc`): 512-bit hole bitmap + 8-bit coarse filter per perforated page
- **PTW** (`ptw.cc`): Adds one extra memory access (bitmap read) for perforated pages via `PerfState::BITMAP_PENDING`
- **L1 resolution** (`cache.cc` `finish_translation()`): 3-path — coarse filter fast-path (+1 cycle), bitmap non-hole (+11 cycles), hole re-route to 4KB pipeline (+20 cycles + 4KB TLB walk)
- **Pmap format**: `vpn_hex,2,bitmap_hex` (128 hex chars = 512-bit bitmap)
- **Auto-perforation**: `--perf-frag-ratio 0.25 --perf-frag-dist random` converts 2MB→perforated with synthetic holes
- **Stats**: `perf_total`, `perf_coarse_filtered`, `perf_non_hole`, `perf_hole` in L1D/L1I output
- **Key files**: `inc/vmem.h`, `src/vmem.cc`, `inc/ptw.h`, `src/ptw.cc`, `src/cache.cc`, `src/main.cc`
- **Detailed docs**: `docs/perforated-pages.md`

### Heatmap-Based Memory Allocation
- **Phase 1** (`--generate-heatmap`): During simulation, records per-page access counts and criticality scores from LLC misses
- **Phase 2** (`--use-heatmap`): Loads heatmap, sorts pages by access count (default) or criticality (`--sort-by-criticality`), allocates hot pages to DRAM and cold to CXL per the `--ratio`
- Criticality tracking: Instructions mark LLC miss sources at retirement time

### Statistics
- Cache: hits, misses, MSHR merges, miss latency, congestion cycles
- SHIM layer: separate DRAM/CXL read/write/prefetch counts
- TLB: DTLB/STLB access counts, miss counts, average miss latencies
- All stats separated between warmup and simulation phases
- Collected by `scripts/collect_stats.py` into CSV/pickle for analysis

### Development Workflow
When adding new memory components:
1. Update JSON config with new parameters
2. Modify `config/instantiation_file.py` if adding new component types
3. Inherit from `champsim::operable`, use `champsim::channel` for communication
4. Add statistics collection (distinguish warmup vs simulation phases)
5. If extending memory controllers, implement for both `DRAM_CONTROLLER` and `RAMULATOR_CONTROLLER`

### Key Source Files
- `src/main.cc`: Entry point, CLI parsing, main simulation loop
- `src/cache.cc`: Cache implementation, MSHR management, skip-translation logic
- `src/ooo_cpu.cc`: CPU pipeline stages
- `src/shim_layer.cc`: DRAM/CXL routing (custom to this repo)
- `src/cxl_memory.cc`: CXL device modeling (custom to this repo)
- `src/ramulator_controller.cc`: Ramulator integration (custom to this repo)
- `config/instantiation_file.py`: Main configuration parser and code generator

### Analysis Scripts
- `scripts/collect_stats.py`: Extract stats from `.out` files → CSV/pickle
- `scripts/plot.py`: Per-trace IPC, memory breakdown, MPKI (prototype validation)
- `scripts/plot_by_suite.py`: Geomean by benchmark suite (final analysis, dram-only baseline)
- `scripts/skip_vs_no_skip.py`: Skip-translation comparison (no_skip baseline)
- `scripts/compare_ipc.py`: IPC speedup comparison between skip and non-skip
- `scripts/trace_footprint.py`: Memory footprint of individual traces (`--skip`, `--instructions`)
- `scripts/gapbs/`: GAPBS benchmark-specific collection and plotting (run `collect_stats.py` first, supports `--ignore`)
- `scripts/runall_pelle.py`: SLURM batch submission with dependency management
- `scripts/run_pelle.sh`: Individual SLURM job runner (called by runall_pelle.py)
- `scripts/make_all_binaries.sh`: Builds all four binary configurations
