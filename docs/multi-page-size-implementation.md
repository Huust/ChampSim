# Multi-Page-Size Support (4KB + 2MB) Implementation

**Commit**: `94dbac2` on branch `multi-page-size`
**Date**: 2026-03-16
**14 files changed, 356 insertions(+), 34 deletions(-)**

## Overview

Adds runtime support for mixed 4KB/2MB pages to ChampSim via a `--pmap` CLI option. A split TLB hierarchy (ITLB_2M/DTLB_2M/STLB_2M with 21-bit offset) runs alongside the existing 4KB TLBs. L1 caches route translation requests to the correct TLB chain based on a pmap lookup. The Page Table Walker performs variable-depth walks (stop at level 1 for 2MB, level 0 for 4KB).

## Architecture

```
L1I ──lower_translate───> ITLB (4K, offset=12) ──> STLB (4K) ──> PTW
    ──lower_translate_2m─> ITLB_2M (2M, offset=21) ──> STLB_2M (2M) ──> PTW

L1D ──lower_translate───> DTLB (4K, offset=12) ──> STLB (4K) ──> PTW
    ──lower_translate_2m─> DTLB_2M (2M, offset=21) ──> STLB_2M (2M) ──> PTW
```

L1 caches query `g_vmem->get_page_size()` to determine whether a virtual address maps to a 4KB or 2MB page, then route the translation request to the appropriate TLB chain. A 2MB page's VPN in 4KB granularity has its lower 9 bits zeroed (since 2MB = 512 x 4KB).

## Key Design Decision

**Keep `PAGE_SIZE = 4096` as the global baseline.** The existing `champsim::page_number` / `champsim::page_offset` types (used in 30+ locations) stay at 4KB granularity. 2MB pages are handled as a special case via a per-request `page_size` metadata field. This avoids touching the entire codebase.

## pmap File Format

```csv
# vpn (hex, 4KB-granularity), page_size (0=4K, 1=2M)
# For 2MB pages, use the base 4KB-VPN (2MB-aligned, i.e., lower 9 bits = 0)
400,1       # 2MB page at VPN 0x400 (covers VPN range 0x400-0x5FF)
800,0       # 4KB page at VPN 0x800
```

## Changes by File

### Headers

#### `inc/vmem.h`
- Added `PageSize` enum: `{ PAGE_4K = 0, PAGE_2M = 1 }`
- Added constants: `LOG2_PAGE_SIZE_4K = 12`, `LOG2_PAGE_SIZE_2M = 21`
- Added `extern VirtualMemory* g_vmem` global pointer
- Added to `VirtualMemory` private members:
  - `std::unordered_map<uint64_t, PageSize> pmap` — maps 4KB-VPN to PageSize
  - `std::vector<std::deque<champsim::page_number>> ppage_free_list_2m` — 2MB physical page pool
- Added public methods: `load_pmap()`, `get_page_size()`, `va_to_pa_2m()`
- Added private helpers: `ppage_front_2m()`, `ppage_pop_2m()`

#### `inc/channel.h`
- Added `uint8_t page_size = 0` to both `request` (line 70) and `response` (line 85) structs
- Added 6-argument `response` constructor that accepts `page_size`
- Updated `explicit response(request)` to propagate `page_size`

#### `inc/cache.h`
- Added `uint8_t page_size = 0` to `tag_lookup_type` and `mshr_type`
- Added `channel_type* lower_translate_2m = nullptr` member to CACHE
- Updated CACHE template constructor to initialize `lower_translate_2m` from builder

#### `inc/block.h`
- Added `uint8_t page_size = 0` to `cache_block` struct

#### `inc/ptw.h`
- Added `uint8_t page_size = 0` to PTW's `mshr_type`

#### `inc/cache_builder.h`
- Added `champsim::channel* m_lt_2m{nullptr}` to `cache_builder_base`
- Added `lower_translate_2m(champsim::channel*)` builder method

#### `inc/defaults.hpp`
- Added default configurations for 2MB TLBs:
  - `default_itlb_2m`: 4 sets, 4 ways, offset_bits=21
  - `default_dtlb_2m`: 4 sets, 4 ways, MSHR=8, offset_bits=21
  - `default_stlb_2m`: 16 sets, 12 ways, offset_bits=21

### Source Files

#### `src/vmem.cc`
- **`populate_pages()`**: After building 4KB free lists, carves out 2MB-aligned groups of 512 contiguous 4KB pages into `ppage_free_list_2m`. Limited to at most half of total pages per device.
- **`ppage_front_2m()` / `ppage_pop_2m()`**: 2MB pool access helpers.
- **`load_pmap()`**: Parses CSV file (`vpn_hex,page_size`), stores entries in `pmap` unordered_map. Prints summary of loaded entries.
- **`get_page_size()`**: Returns page size for a 4KB VPN. Checks exact VPN match first, then checks if VPN falls within a 2MB page by looking up the 2MB-aligned base VPN `(vpn >> 9) << 9`.
- **`va_to_pa_2m()`**: Translates a 2MB page. Aligns VPN to 2MB boundary, allocates from `ppage_free_list_2m`, tracks in `vpage_to_ppage_map`.

#### `src/cache.cc`
- **Move constructor/assignment**: Include `lower_translate_2m` in moves.
- **`tag_lookup_type` constructor**: Copies `page_size` from request.
- **`mshr_type` constructor**: Copies `page_size` from tag_lookup_type.
- **`fill_block()`**: Copies `page_size` to cache_block.
- **`operate()`**: Processes responses from both `lower_translate->returned` and `lower_translate_2m->returned`.
- **`finish_translation()`**: Page-size-aware matching and address splicing:
  - For 2MB responses: matches on 2MB-aligned VPN (mask lower 9 bits), splices with 21-bit dynamic_extent offset.
  - For 4KB responses: existing logic unchanged.
- **`issue_translation()`**: Queries `g_vmem->get_page_size()` to determine page size, routes to `lower_translate_2m` for 2MB pages if available.
- **Skip-translation path** (`#ifdef ENABLE_SKIP_CXL_TRANSLATION`): Updated to call `va_to_pa_2m()` for 2MB pages and use 21-bit address splicing.

#### `src/ptw.cc`
- **`handle_read()`**: Propagates `page_size` from request to PTW MSHR.
- **`finish_packet()`**:
  - `is_last_step`: 2MB pages stop at `translation_level <= 1` (skip level-1 PTE walk); 4KB pages stop at `translation_level <= 0`.
  - `finish_last_step`: Calls `va_to_pa_2m()` for 2MB pages, `va_to_pa()` for 4KB.
- **`operate()`**: Passes `page_size` in 6-arg response constructor via `emplace_back()`.

#### `src/main.cc`
- Added `--pmap` CLI option.
- After environment construction: loads pmap via `g_vmem->load_pmap(pmap_path)`.

### Config System

#### `config/defaults.py`
- Added 2MB TLB chain: `ITLB_2M→STLB_2M`, `DTLB_2M→STLB_2M`, `STLB_2M→PTW`.
- Added `connect_translator_2m()` function for L1→2M TLB connections.
- Updated `list_defaults_for_core()` to return 2MB paths and translation connections.

#### `config/instantiation_file.py`
- Added `'lower_translate_2m'` to `cache_builder_parts` dict.
- Updated `get_cache_builder()` to handle `lower_translate_2m` in local_params.
- Updated `get_upper_levels()` to include `lower_translate_2m` connections.

#### `config/parse.py`
- Added `'ITLB_2M', 'DTLB_2M', 'STLB_2M'` to `core_default_names()`.
- Added `'ITLB_2M', 'DTLB_2M'` to `path_root_names` and `default_frequencies()`.
- Split TLB paths: `tlb_4k_path` (ITLB, DTLB) uses `offset_bits = lg2(page_size)` = 12; `tlb_2m_path` (ITLB_2M, DTLB_2M) uses `offset_bits = 21`.
- Added `path_end_in` for ITLB_2M and DTLB_2M to PTW.

## Generated Code

After `./config.sh champsim_config.json`, the generated `.csconfig/core_inst.cc.inc` contains:
- 21 channels (including 6 new channels for 2MB TLB chains with `champsim::data::bits{21}`)
- 10 caches: LLC, cpu0_DTLB, **cpu0_DTLB_2M**, cpu0_ITLB, **cpu0_ITLB_2M**, cpu0_L1D, cpu0_L1I, cpu0_L2C, cpu0_STLB, **cpu0_STLB_2M**
- L1D and L1I have both `.lower_translate()` (4KB) and `.lower_translate_2m()` (2MB)

## `page_size` Field Propagation

```
request.page_size  →  tag_lookup_type.page_size  →  mshr_type.page_size  →  cache_block.page_size
                   →  PTW mshr_type.page_size     →  response.page_size
```

The `page_size` field (uint8_t: 0=4K, 1=2M) flows through the entire translation pipeline.

## Verification Results

Tested with `401.bzip2-7B.champsimtrace.xz` trace:

| Configuration | ITLB_2M accesses | DTLB_2M accesses | ITLB accesses | DTLB accesses | IPC |
|---|---|---|---|---|---|
| No pmap (all 4KB) | 0 | 0 | 1258 | 55430 | 1.250 |
| With pmap (VPN 0x400=2MB) | 1014 | 120 | 244 | 55304 | 1.252 |

The 2MB TLB chain correctly captures accesses to the 2MB-mapped region, with a slight IPC improvement from reduced TLB misses.

## Usage

```bash
# Build
./config.sh champsim_config.json && make

# Run with pmap
bin/champsim -w 200000000 -i 500000000 \
    --pmap path/to/pages.pmap \
    path/to/trace.champsimtrace.xz
```
