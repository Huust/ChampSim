#!/usr/bin/env python3
"""Tier-aware perforation policy generator.

Phase 1 (Memtis): Split skewed huge pages, promote/demote 4KB pages.
Phase 2 (Perforation): Exchange cold fast-tier data with hot slow-tier data
    using two-level priority matching, without splitting any remaining huge pages.

Modes:
  --no-perforate       Run Phase 1 only (Memtis baseline).
  --skip-memtis        Skip Phase 1 entirely (perforation on top of pure 2MB tiering).
  --memtis-split-ratio Override Memtis Ns: force split this fraction of all HPs.
  default              Run Phase 1 (Ns from formula) + Phase 2 (perforation).

Output pmap:
  vpn_hex,0              -> 4KB page in fast tier
  vpn_hex,1              -> intact 2MB huge page in fast tier
  vpn_hex,2,bitmap_hex   -> fast-base perforated huge page (holes in slow tier)
  vpn_hex,3,bitmap_hex   -> slow-base perforated huge page (hot holes in fast tier)
  Absent pages are implicitly in the slow tier.
"""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Set, Tuple

PAGES_PER_2MB = 512


@dataclass
class PageStat:
    vpn: int
    llc_miss: int
    criticality: int
    total_access: int


@dataclass
class RegionInfo:
    region_base: int
    pages: List[int]
    total_hotness: float
    ideal_hot_pages: List[int]
    cold_pages: List[int]
    skewness: float


def parse_latency_from_tiered_output(path: str) -> Tuple[float, float]:
    fast_lat = slow_lat = None
    with open(path) as f:
        for line in f:
            if "LLC AVERAGE MISS LATENCY TO DRAM:" in line:
                val = line.split(":")[-1].strip().split()[0]
                if val != "-":
                    fast_lat = float(val)
            elif "LLC AVERAGE MISS LATENCY TO CXL:" in line:
                val = line.split(":")[-1].strip().split()[0]
                if val != "-":
                    slow_lat = float(val)
    if fast_lat is None or slow_lat is None:
        raise ValueError(f"Could not extract LLC latencies from {path} (DRAM={fast_lat}, CXL={slow_lat})")
    return fast_lat, slow_lat


def parse_heatmap(path: str) -> Tuple[int, Dict[int, PageStat]]:
    capacity_pages = None
    stats: Dict[int, PageStat] = {}
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#capacity:"):
                capacity_pages = int(line.split(":", 1)[1].strip())
                continue
            if line.startswith("#"):
                continue
            if ":" not in line:
                raise ValueError(f"Line {lineno}: bad format: {line}")
            vpn_str, rhs = line.split(":", 1)
            fields = rhs.strip().split()
            if len(fields) < 2:
                raise ValueError(f"Line {lineno}: need >=2 counters: {line}")
            if len(fields) == 2:
                fields.append("0")
            vpn = int(vpn_str.strip(), 16)
            stats[vpn] = PageStat(vpn, int(fields[0]), int(fields[1]), int(fields[2]))
    if capacity_pages is None:
        raise ValueError("Missing '#capacity:' header.")
    return capacity_pages, stats


def get_hotness(page: PageStat, metric: str) -> int:
    return page.llc_miss if metric == "llc_miss" else page.criticality


def region_base(vpn: int) -> int:
    return (vpn // PAGES_PER_2MB) * PAGES_PER_2MB


def compute_skewness(values: List[float], u_i: int) -> float:
    if u_i <= 0:
        return 0.0
    return sum(v * v for v in values) / (u_i * u_i)


def build_type2_bitmap_hex(base: int, hole_vpns: Set[int]) -> str:
    """Type=2 bitmap: base DRAM, bit=1 for holes -> CXL."""
    hole_offsets = {vpn - base for vpn in hole_vpns}
    words = [0] * 8
    for i in range(PAGES_PER_2MB):
        if i in hole_offsets:
            words[i // 64] |= (1 << (i % 64))
    return "".join(f"{w:016x}" for w in words)


def build_type3_bitmap_hex(base: int, hot_vpns: Set[int]) -> str:
    """Type=3 bitmap: base CXL, bit=1 for hot holes -> DRAM."""
    hot_offsets = {vpn - base for vpn in hot_vpns}
    words = [0] * 8
    for i in range(PAGES_PER_2MB):
        if i in hot_offsets:
            words[i // 64] |= (1 << (i % 64))
    return "".join(f"{w:016x}" for w in words)


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise ValueError(msg)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input_heatmap", help="4KB heatmap with #capacity header")
    p.add_argument("output_pmap", help="Output pmap file")
    p.add_argument("--tiered-output", required=True,
                   help="Path to 2MB tiered simulation stdout (for latencies)")
    p.add_argument("--ratio", required=True, help="DRAM:CXL ratio (e.g., 1:3)")
    p.add_argument("--metric", default="llc_miss", choices=["llc_miss", "criticality"])
    # Memtis parameters
    p.add_argument("--beta", type=float, default=0.4,
                   help="Aggressiveness in Ns formula. Default: 0.4")
    p.add_argument("--split-trigger", type=float, default=0.05,
                   help="Minimum eHR-rHR gap to trigger Memtis split. Default: 0.05")
    p.add_argument("--memtis-split-ratio", type=float, default=None,
                   help="Override Ns: force split this fraction of all HPs (e.g., 0.33, 0.67). "
                        "Candidates are still ranked by skewness.")
    p.add_argument("--skip-memtis", action="store_true",
                   help="Skip Phase 1 entirely. Perforation operates on pure 2MB tiering.")
    p.add_argument("--no-perforate", action="store_true",
                   help="Memtis only: skip Phase 2 perforation.")
    # Perforation parameters
    p.add_argument("--theta-low", type=float, default=0.0,
                   help="Minimum cold-subpage fraction for fast-tier HP eligibility. Default: 0.0")
    p.add_argument("--theta-high", type=float, default=1.0,
                   help="Maximum cold-subpage fraction for fast-tier HP eligibility. Default: 1.0 (no limit)")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()

    if args.skip_memtis and args.no_perforate:
        p.error("--skip-memtis and --no-perforate cannot both be set")

    args.fast_latency, args.slow_latency = parse_latency_from_tiered_output(args.tiered_output)
    return args


def cold_fraction(regions: Dict[int, RegionInfo], base: int) -> float:
    # cold + unused = 512 - hot. Unused subpages are implicitly cold.
    return (PAGES_PER_2MB - len(regions[base].ideal_hot_pages)) / PAGES_PER_2MB


def main() -> None:
    args = parse_args()
    capacity_pages, stats = parse_heatmap(args.input_heatmap)

    num, den = (int(x) for x in args.ratio.split(":"))
    cap = (capacity_pages * num) // (num + den)

    hotness = {vpn: float(get_hotness(ps, args.metric)) for vpn, ps in stats.items()}
    total_hotness = sum(hotness.values())
    if total_hotness == 0:
        print("No hotness data", file=sys.stderr)
        sys.exit(1)

    ranked = sorted(hotness.items(), key=lambda kv: (-kv[1], kv[0]))
    ideal_set = set(hotness.keys()) if cap >= len(stats) else {vpn for vpn, _ in ranked[:cap]}
    eHR = sum(hotness[v] for v in ideal_set) / total_hotness

    # Build 2MB regions
    per_region: Dict[int, List[int]] = defaultdict(list)
    for vpn in stats:
        per_region[region_base(vpn)].append(vpn)

    regions: Dict[int, RegionInfo] = {}
    for base, vpns in per_region.items():
        vpns = sorted(vpns)
        ideal_hot = [v for v in vpns if v in ideal_set]
        cold = [v for v in vpns if v not in ideal_set]
        vals = [hotness[v] for v in vpns]
        regions[base] = RegionInfo(base, vpns, sum(vals), ideal_hot, cold,
                                   compute_skewness(vals, len(ideal_hot)))

    total_regions = len(regions)
    huge_fit = cap // PAGES_PER_2MB
    region_ranked = sorted(regions.values(), key=lambda r: (-r.total_hotness, r.region_base))
    fast_huge: Set[int] = {r.region_base for r in region_ranked[:huge_fit]}
    slow_huge: Set[int] = {r.region_base for r in region_ranked[huge_fit:]}

    rHR_2mb = sum(regions[b].total_hotness for b in fast_huge) / total_hotness
    gap = max(0.0, eHR - rHR_2mb)

    # ══════════════════════════════════════════════════════════════════
    # PHASE 1: Memtis split + migrate (skipped if --skip-memtis)
    # ══════════════════════════════════════════════════════════════════

    split_fast_bases: Set[int] = set()
    split_slow_bases: Set[int] = set()
    fast_4kb: Set[int] = set()
    demoted_4kb: Set[int] = set()
    slow_leftover: List[int] = []
    R_memtis = cap - huge_fit * PAGES_PER_2MB

    if not args.skip_memtis:
        # Determine Ns
        if args.memtis_split_ratio is not None:
            # Override: split this fraction of all HPs
            ns = int(total_regions * args.memtis_split_ratio)
            ns = min(ns, total_regions)
        else:
            # Standard Memtis formula
            dl_lf = max(0.0, args.slow_latency - args.fast_latency) / args.fast_latency if args.fast_latency > 0 else 0.0
            ns = min(int(gap * dl_lf * total_regions * args.beta), total_regions)

        all_cands = [(b, i) for b, i in regions.items() if len(i.ideal_hot_pages) >= 1]
        all_cands.sort(key=lambda c: (-c[1].skewness, c[0]))

        # For forced split ratio, always split (no gap trigger check)
        if args.memtis_split_ratio is not None:
            do_split = ns > 0
        else:
            do_split = gap >= args.split_trigger and ns > 0

        selected = all_cands[:ns] if do_split else []

        fast_cands = [(b, i) for b, i in selected if b in fast_huge]
        slow_cands = [(b, i) for b, i in selected if b in slow_huge]

        for b, _ in fast_cands:
            fast_huge.discard(b)
            split_fast_bases.add(b)
        for b, _ in slow_cands:
            slow_huge.discard(b)
            split_slow_bases.add(b)

        promote_cands = sorted(
            [(vpn, hotness[vpn]) for b in split_slow_bases for vpn in regions[b].ideal_hot_pages],
            key=lambda x: (-x[1], x[0]))
        demote_cands = sorted(
            [(vpn, hotness[vpn]) for b in split_fast_bases for vpn in regions[b].cold_pages],
            key=lambda x: (x[1], x[0]))

        avail = R_memtis
        di = 0
        for vpn, _ in promote_cands:
            if avail > 0:
                fast_4kb.add(vpn); avail -= 1
            elif di < len(demote_cands):
                demoted_4kb.add(demote_cands[di][0]); fast_4kb.add(vpn); di += 1
            else:
                slow_leftover.append(vpn)

    memtis_h = sum(regions[b].total_hotness for b in fast_huge)
    for b in split_fast_bases:
        memtis_h += regions[b].total_hotness
    memtis_h -= sum(hotness[v] for v in demoted_4kb)
    memtis_h += sum(hotness[v] for v in fast_4kb)
    rHR_memtis = memtis_h / total_hotness

    fast_used = len(fast_huge) * PAGES_PER_2MB
    for b in split_fast_bases:
        fast_used += len(regions[b].pages)
    fast_used -= len(demoted_4kb)
    fast_used += len(fast_4kb)
    R = cap - fast_used
    require(R >= 0, f"Fast tier overfilled after Memtis: R={R}")

    # ══════════════════════════════════════════════════════════════════
    # PHASE 2: Perforation (skipped if --no-perforate)
    # ══════════════════════════════════════════════════════════════════

    punched_holes: Dict[int, Set[int]] = {}
    promoted_to_fast: Dict[int, Set[int]] = {}
    bp_cold_demoted: Set[int] = set()
    rHR_perf = rHR_memtis
    p1_count = p2_count = holes_punched = free_used = bp_demoted_count = 0

    if not args.no_perforate:
        # ── Candidate filtering ──
        # Fast-tier: theta_low <= phi_c(h) <= theta_high
        eligible_fast: Set[int] = {
            b for b in fast_huge
            if args.theta_low <= cold_fraction(regions, b) <= args.theta_high
        }
        # Slow-tier: all intact huge pages (no filtering per paper)
        eligible_slow: Set[int] = set(slow_huge)

        # ── Build supply queue (S1 → S2 → S3) ──
        # S1: free frames
        supply_s1 = R

        # S2: cold subpages from eligible fast-tier huge pages, coldest first
        supply_s2: List[Tuple[int, int, float]] = []
        for base in eligible_fast:
            for vpn in regions[base].cold_pages:
                supply_s2.append((base, vpn, hotness[vpn]))
        supply_s2.sort(key=lambda x: (x[2], x[1]))

        # S3: cold standalone 4KB pages in fast tier (from Memtis splits), coldest first
        supply_s3: List[Tuple[int, float]] = []
        for base in split_fast_bases:
            for vpn in regions[base].cold_pages:
                if vpn not in demoted_4kb:
                    supply_s3.append((vpn, hotness[vpn]))
        supply_s3.sort(key=lambda x: (x[1], x[0]))

        # ── Build demand queue (D1 → D2) ──
        # D1: stranded hot standalone 4KB pages in slow tier, hottest first
        demand_d1: List[int] = sorted(slow_leftover, key=lambda v: (-hotness[v], v))

        # D2: trapped hot subpages in eligible slow-tier huge pages, hottest first
        demand_d2: List[int] = sorted(
            [vpn for base in eligible_slow for vpn in regions[base].ideal_hot_pages],
            key=lambda v: (-hotness[v], v))

        # ── Two-level priority matching ──
        s1_avail = supply_s1
        s2_idx = 0
        s3_idx = 0
        d1_idx = 0
        d2_idx = 0
        current_fast_used = fast_used

        while True:
            # Pick demand: D1 first, then D2
            if d1_idx < len(demand_d1):
                vpn = demand_d1[d1_idx]
                d1_idx += 1
                src = "D1"
            elif d2_idx < len(demand_d2):
                vpn = demand_d2[d2_idx]
                d2_idx += 1
                src = "D2"
            else:
                break  # no more demand

            # Pick supply: S1 first, then S2, then S3
            if s1_avail > 0:
                s1_avail -= 1
                free_used += 1
                current_fast_used += 1
            elif s2_idx < len(supply_s2):
                hole_base, hole_vpn, _ = supply_s2[s2_idx]
                s2_idx += 1
                punched_holes.setdefault(hole_base, set()).add(hole_vpn)
                holes_punched += 1
            elif s3_idx < len(supply_s3):
                cold_vpn, _ = supply_s3[s3_idx]
                s3_idx += 1
                bp_cold_demoted.add(cold_vpn)
                bp_demoted_count += 1
            else:
                break  # no more supply

            require(current_fast_used <= cap,
                    f"Fast tier exceeded during perforation: used={current_fast_used}, cap={cap}")

            base = region_base(vpn)
            promoted_to_fast.setdefault(base, set()).add(vpn)
            if src == "D1":
                p1_count += 1
            else:
                p2_count += 1

        # Compute perforation rHR
        perf_h = memtis_h
        for holes in punched_holes.values():
            perf_h -= sum(hotness[v] for v in holes)
        perf_h -= sum(hotness[v] for v in bp_cold_demoted)
        for vpns in promoted_to_fast.values():
            perf_h += sum(hotness[v] for v in vpns)
        rHR_perf = perf_h / total_hotness

    if args.verbose:
        if args.skip_memtis:
            mode = "perforation_only"
        elif args.no_perforate:
            mode = "memtis_only"
        else:
            mode = "memtis+perforation"
        print(f"mode={mode}")
        if args.memtis_split_ratio is not None:
            print(f"memtis_split_ratio={args.memtis_split_ratio:.2f} (Ns={len(split_fast_bases)+len(split_slow_bases)})")
        elif args.skip_memtis:
            print(f"memtis=skipped")
        else:
            print(f"memtis=follow_policy (Ns={len(split_fast_bases)+len(split_slow_bases)})")
        print(f"split_fast={len(split_fast_bases)} split_slow={len(split_slow_bases)}")
        print(f"eHR={eHR:.6f} rHR_2mb={rHR_2mb:.6f} rHR_memtis={rHR_memtis:.6f} rHR_perf={rHR_perf:.6f}")
        if not args.no_perforate:
            print(f"theta_low={args.theta_low:.3f} theta_high={args.theta_high:.3f}")
            print(f"eligible_fast={len(eligible_fast)}/{len(fast_huge)} eligible_slow={len(eligible_slow)}/{len(slow_huge)}")
            print(f"supply: S1(free)={R} S2(hp_cold)={len(supply_s2)} S3(bp_cold)={len(supply_s3)}")
            print(f"demand: D1(stranded)={len(demand_d1)} D2(trapped)={len(demand_d2)}")
            print(f"placed: D1={p1_count} D2={p2_count} holes={holes_punched} bp_demoted={bp_demoted_count} free_used={free_used}")

    # ══════════════════════════════════════════════════════════════════
    # EMIT PMAP
    # ══════════════════════════════════════════════════════════════════

    lines: List[str] = []
    if args.skip_memtis:
        mode_str = "perforation_only"
    elif args.no_perforate:
        mode_str = "memtis_only"
    else:
        mode_str = "memtis+perforation"
    lines.append(f"# policy={mode_str} metric={args.metric} ratio={args.ratio}")
    lines.append(f"# eHR={eHR:.6f} rHR_2mb={rHR_2mb:.6f} rHR_memtis={rHR_memtis:.6f} rHR_perf={rHR_perf:.6f}")
    if not args.no_perforate:
        lines.append(f"# theta_low={args.theta_low:.3f} theta_high={args.theta_high:.3f}")
        lines.append(f"# D1={p1_count} D2={p2_count} holes={holes_punched} bp_demoted={bp_demoted_count}")

    perforated_fast_bases = set(punched_holes.keys())
    intact_fast_bases = fast_huge - perforated_fast_bases

    # Classify promoted D2 pages by slow HP base
    p2_promoted_bases: Dict[int, Set[int]] = {}
    p1_vpns: Set[int] = set()
    p1_set = set(slow_leftover)
    for base, vpns in promoted_to_fast.items():
        p2_vpns = {v for v in vpns if v not in p1_set}
        if p2_vpns:
            p2_promoted_bases[base] = p2_vpns
        for v in vpns:
            if v in p1_set:
                p1_vpns.add(v)

    # type=1: intact fast HPs
    for base in sorted(intact_fast_bases):
        lines.append(f"{base:x},1")

    # type=2: perforated fast HPs (cold holes demoted to slow tier)
    for base in sorted(perforated_fast_bases):
        bitmap_hex = build_type2_bitmap_hex(base, punched_holes[base])
        lines.append(f"{base:x},2,{bitmap_hex}")

    # type=0: Memtis split fast HPs (4KB pages staying in fast tier)
    extra_demoted = demoted_4kb | bp_cold_demoted
    for base in sorted(split_fast_bases):
        for vpn in sorted(regions[base].pages):
            if vpn not in extra_demoted:
                lines.append(f"{vpn:x},0")

    # type=0: Memtis promoted slow pages (in fast tier as 4KB)
    for vpn in sorted(fast_4kb):
        lines.append(f"{vpn:x},0")

    # type=3: slow HPs with promoted hot subpages (D2)
    for base in sorted(p2_promoted_bases):
        bitmap_hex = build_type3_bitmap_hex(base, p2_promoted_bases[base])
        lines.append(f"{base:x},3,{bitmap_hex}")

    # type=0: D1 promoted pages (stranded hot 4KB from slow splits)
    for vpn in sorted(p1_vpns):
        lines.append(f"{vpn:x},0")

    with open(args.output_pmap, "w", encoding="utf-8") as f:
        for line in lines:
            f.write(line + "\n")

    if args.verbose:
        print(f"Written {len(lines)} lines to {args.output_pmap}")


if __name__ == "__main__":
    main()
