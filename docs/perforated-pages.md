# Perforated Pages Implementation

Based on the ISCA 2020 paper: *"Perforated Page: Supporting Fragmented Memory Allocation for Large Pages"* (Park et al.)

## Overview

Perforated pages are 2MB large pages where some 4KB sub-pages ("holes") are mapped to non-contiguous physical addresses. This allows the OS to allocate large pages even when physical memory is fragmented — most sub-pages share the contiguous 2MB physical frame, but a few "holes" are individually mapped to scattered 4KB frames.

### Why This Matters

- **2MB pages** reduce TLB pressure but require 2MB of *contiguous* physical memory — hard to find under fragmentation
- **4KB pages** always work but cause high TLB miss rates
- **Perforated pages** get most of the TLB benefit of 2MB pages while tolerating fragmentation

## Architecture

### Data Structures

Each perforated 2MB page has:

1. **512-bit hole bitmap** — 1 bit per 4KB sub-page (512 sub-pages in a 2MB page). Bit=1 means that sub-page is a "hole" mapped to a separate 4KB physical frame.
2. **8-bit coarse filter** — 1 bit per 64-sub-page region (8 regions). If the bit is 0, no holes exist in that region (fast rejection).

```
2MB page (512 sub-pages):
┌─────────────────────────────────────────────────────┐
│ region0 (64) │ region1 (64) │ ... │ region7 (64)    │
│ coarse[0]    │ coarse[1]    │     │ coarse[7]       │
└─────────────────────────────────────────────────────┘

Hole bitmap: 8 × uint64_t = 512 bits
Coarse filter: 1 × uint8_t = 8 bits
```

### Translation Timing Model

#### Path A: STLB_2M Miss → PTW (full walk)

```
Normal 2MB:     L4 → L3 → L2 PDE → done (return 2MB PPN)
Perforated:     L4 → L3 → L2 PDE → bitmap read → done (return 2MB PPN + PAGE_PERF)
```

The bitmap read is a **real memory access** through the cache/memory hierarchy, adding ~1 memory access of latency. PTW returns `page_size = PAGE_PERF` with the 2MB base PPN. STLB_2M caches this normally.

#### Path B: STLB_2M Hit → L1 finish_translation()

When STLB_2M hits on a cached perforated PTE, L1 resolves per-sub-page:

| Step | Condition | Action | Extra Latency |
|------|-----------|--------|---------------|
| 1 | Coarse filter = 0 | **Fast path**: translate with 2MB PPN | +1 cycle |
| 2 | Coarse filter = 1, bitmap = 0 | **Medium path**: not a hole | +1 + 10 cycles |
| 3 | Coarse filter = 1, bitmap = 1 | **Slow path**: hole detected | +10 + 10 cycles, then 4KB TLB pipeline |

For holes (step 3): the entry is reset to `PAGE_4K` with `translate_issued = false`, re-entering the 4KB DTLB → STLB → PTW pipeline. The 4KB walk benefits from PSCLs cached during the original 2MB walk.

### Latency Constants

| Constant | Default | Meaning |
|----------|---------|---------|
| `PERF_COARSE_FILTER_CYCLES` | 1 | Coarse filter check (register lookup) |
| `PERF_BITMAP_LATENCY_CYCLES` | 10 | Bitmap memory access (approximates LLC hit) |
| `PERF_HOLE_LATENCY_CYCLES` | 10 | Shadow PTE access for hole resolution |

Defined in `inc/vmem.h` as `VirtualMemory::PERF_*` constants.

## Usage

### Pmap File Format

Extended pmap format with perforated page support:

```
# vpn_hex,page_size[,hole_bitmap_hex]
# page_size: 0=4KB, 1=2MB, 2=perforated

# Regular 4KB page
100,0

# Regular 2MB page (base VPN must be 2MB-aligned, i.e., lower 9 bits = 0)
200,1

# Perforated 2MB page with hole bitmap (128 hex chars = 512 bits)
400,2,00000000000000FF0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
```

The bitmap is stored as 8 words of 16 hex chars each (little-endian within each word). In the example above, sub-pages 0-7 of region 0 are holes.

### CLI Options

```bash
# Use pmap with explicit perforated entries
bin/champsim --pmap path/to/file.pmap -w 200000000 -i 500000000 trace.xz

# Auto-perforate: convert all 2MB pages to perforated with 25% holes
bin/champsim --pmap base.pmap --perf-frag-ratio 0.25 --perf-frag-dist random \
    -w 200000000 -i 500000000 trace.xz
```

#### Auto-perforation Options

| Option | Values | Description |
|--------|--------|-------------|
| `--perf-frag-ratio` | 0.0 - 1.0 | Fraction of sub-pages that become holes |
| `--perf-frag-dist` | `clustered` (default), `dispersed`, `random` | How holes are distributed within each 2MB page |

Distribution patterns:
- **clustered**: holes placed in contiguous runs at the start of each 64-sub-page region
- **dispersed**: holes spread evenly (every Nth sub-page)
- **random**: each sub-page independently becomes a hole with the given probability

### Statistics Output

When perforated pages are used, L1D/L1I stats include:

```
cpu0->cpu0_L1D PERFORATED PAGE TRANSLATIONS: 1234
  Coarse filter fast-path: 800 (64.8%)
  Bitmap non-hole: 300 (24.3%)
  Hole (re-routed to 4KB): 134 (10.9%)
```

## Files Modified

| File | Changes |
|------|---------|
| `inc/vmem.h` | `PAGE_PERF` enum, bitmap/filter maps, `is_hole()`, `coarse_filter_pass()`, latency constants |
| `src/vmem.cc` | Implement bitmap queries, extend `load_pmap()` and `get_page_size()`, `generate_perforated_pages()` |
| `inc/ptw.h` | `PerfState` enum in PTW `mshr_type` |
| `src/ptw.cc` | Bitmap step in PTW walk, `handle_fill()` state propagation, `is_last_step` logic |
| `src/cache.cc` | 3-path resolution in `finish_translation()`, routing in `issue_translation()`, skip-translation support |
| `src/main.cc` | `--perf-frag-ratio`, `--perf-frag-dist` CLI options |
| `inc/cache_stats.h` | `perf_total`, `perf_non_hole`, `perf_hole`, `perf_coarse_filtered` counters |
| `src/cache_stats.cc` | Operator- for stat differencing |
| `src/plain_printer.cc` | Perforated stats output |

## Verification Checklist

1. **No pmap**: identical behavior to before (regression safe)
2. **All 2MB pmap**: no perforated stats, same as before
3. **Mixed pmap** (4KB + 2MB + PERF): correct routing, stats show all three paths
4. **Auto-perforation sweep**: `--perf-frag-ratio` 0.0 → 1.0 shows gradual IPC degradation
5. **Skip-translation**: build with `SKIP_TRANSLATION=1`, perforated holes get 4KB physical addresses
