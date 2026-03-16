#!/usr/bin/env python3
"""Generate test pmap files for multi-page-size and perforated page verification.

Usage:
    # Step 1: Run ChampSim briefly to extract VPN footprint via heatmap
    python3 scripts/generate_test_pmap.py --extract-vpns \
        --binary bin/champsim \
        --trace /path/to/trace.champsimtrace.xz \
        --output-dir test_results/trace_name

    # Step 2: Generate pmaps from extracted VPNs
    python3 scripts/generate_test_pmap.py --generate \
        --vpn-file test_results/trace_name/vpns.txt \
        --output-dir test_results/trace_name

    # Combined: extract + generate in one step
    python3 scripts/generate_test_pmap.py --all \
        --binary bin/champsim \
        --trace /path/to/trace.champsimtrace.xz \
        --output-dir test_results/trace_name
"""

import argparse
import os
import subprocess
import sys


def extract_vpns_from_heatmap(binary, trace, output_dir, warmup=2000000, sim=5000000):
    """Run ChampSim briefly with --generate-heatmap to extract accessed VPNs."""
    os.makedirs(output_dir, exist_ok=True)
    heatmap_path = os.path.join(output_dir, "footprint.heatmap")

    cmd = [
        binary,
        "-w", str(warmup),
        "-i", str(sim),
        "--generate-heatmap", heatmap_path,
        trace,
    ]
    print(f"[extract] Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)

    if result.returncode != 0:
        print(f"[extract] FAILED (rc={result.returncode})")
        print(result.stderr[-2000:] if result.stderr else "(no stderr)")
        print(result.stdout[-2000:] if result.stdout else "(no stdout)")
        sys.exit(1)

    # Parse heatmap: format is "vpn_hex: access_count crit_num crit_den"
    vpns = []
    with open(heatmap_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(":")
            if len(parts) >= 2:
                try:
                    vpn = int(parts[0].strip(), 16)
                    vpns.append(vpn)
                except ValueError:
                    continue

    vpns.sort()
    vpn_file = os.path.join(output_dir, "vpns.txt")
    with open(vpn_file, "w") as f:
        for v in vpns:
            f.write(f"{v:x}\n")

    print(f"[extract] Extracted {len(vpns)} unique VPNs -> {vpn_file}")
    print(f"[extract] VPN range: 0x{vpns[0]:x} - 0x{vpns[-1]:x}")
    return vpns


def load_vpns(vpn_file):
    """Load VPNs from a text file (one hex VPN per line)."""
    vpns = []
    with open(vpn_file) as f:
        for line in f:
            line = line.strip()
            if line:
                vpns.append(int(line, 16))
    vpns.sort()
    return vpns


def generate_all_4k(vpns, output_path):
    """Generate pmap with all pages as 4KB."""
    with open(output_path, "w") as f:
        f.write("# All 4KB pages\n")
        for vpn in vpns:
            f.write(f"{vpn:x},0\n")
    print(f"[generate] all_4k: {len(vpns)} entries -> {output_path}")


def generate_all_2m(vpns, output_path):
    """Generate pmap with all pages as 2MB (aligned to 512-page boundaries)."""
    # Group VPNs into 2MB-aligned regions
    seen_bases = set()
    entries = []
    for vpn in vpns:
        base = (vpn >> 9) << 9
        if base not in seen_bases:
            seen_bases.add(base)
            entries.append(base)
    entries.sort()

    with open(output_path, "w") as f:
        f.write("# All 2MB pages (2MB-aligned base VPNs)\n")
        for base in entries:
            f.write(f"{base:x},1\n")
    print(f"[generate] all_2m: {len(entries)} 2MB pages (from {len(vpns)} VPNs) -> {output_path}")


def generate_mixed(vpns, output_path, ratio_2m=0.5):
    """Generate mixed 4KB + 2MB pmap. First half of 2MB regions as 4KB, second half as 2MB."""
    # Get unique 2MB-aligned bases
    bases = sorted(set((vpn >> 9) << 9 for vpn in vpns))
    split = int(len(bases) * (1 - ratio_2m))
    bases_4k = set(bases[:split])
    bases_2m = set(bases[split:])

    with open(output_path, "w") as f:
        f.write(f"# Mixed pmap: {len(bases_4k)} regions as 4KB, {len(bases_2m)} regions as 2MB\n")
        # Write 4KB entries for pages in 4KB regions
        for vpn in vpns:
            base = (vpn >> 9) << 9
            if base in bases_4k:
                f.write(f"{vpn:x},0\n")
        # Write 2MB entries
        for base in sorted(bases_2m):
            f.write(f"{base:x},1\n")

    print(f"[generate] mixed: {len(bases_4k)} 4KB regions + {len(bases_2m)} 2MB regions -> {output_path}")


def generate_mixed_with_perforated(vpns, output_path, ratio_4k=0.33, ratio_2m=0.33):
    """Generate mixed 4KB + 2MB + perforated pmap.

    Splits 2MB regions into three groups: 4KB, 2MB, perforated.
    Perforated pages have first 8 sub-pages as holes (known pattern for verification).
    """
    bases = sorted(set((vpn >> 9) << 9 for vpn in vpns))
    n = len(bases)
    split1 = int(n * ratio_4k)
    split2 = int(n * (ratio_4k + ratio_2m))
    bases_4k = set(bases[:split1])
    bases_2m = set(bases[split1:split2])
    bases_perf = set(bases[split2:])

    # Bitmap: first 8 sub-pages are holes (bits 0-7 of word 0)
    # Word 0 = 0x00000000000000FF, words 1-7 = 0
    bitmap_hex = "00000000000000FF" + "0000000000000000" * 7

    with open(output_path, "w") as f:
        f.write(f"# Mixed pmap: {len(bases_4k)} 4KB, {len(bases_2m)} 2MB, {len(bases_perf)} perforated\n")
        # 4KB entries
        for vpn in vpns:
            base = (vpn >> 9) << 9
            if base in bases_4k:
                f.write(f"{vpn:x},0\n")
        # 2MB entries
        for base in sorted(bases_2m):
            f.write(f"{base:x},1\n")
        # Perforated entries
        for base in sorted(bases_perf):
            f.write(f"{base:x},2,{bitmap_hex}\n")

    print(f"[generate] mixed_perf: {len(bases_4k)} 4KB + {len(bases_2m)} 2MB + {len(bases_perf)} PERF -> {output_path}")


def generate_all_2m_for_perforation(vpns, output_path):
    """Generate all-2MB pmap suitable for auto-perforation via CLI flags."""
    generate_all_2m(vpns, output_path)


def main():
    parser = argparse.ArgumentParser(description="Generate test pmap files")
    parser.add_argument("--extract-vpns", action="store_true", help="Extract VPNs from trace")
    parser.add_argument("--generate", action="store_true", help="Generate pmap files from VPN list")
    parser.add_argument("--all", action="store_true", help="Extract + generate in one step")
    parser.add_argument("--binary", default="bin/champsim", help="ChampSim binary path")
    parser.add_argument("--trace", help="Trace file path")
    parser.add_argument("--vpn-file", help="VPN list file (for --generate)")
    parser.add_argument("--output-dir", required=True, help="Output directory")
    parser.add_argument("--warmup", type=int, default=2000000, help="Warmup instructions for VPN extraction")
    parser.add_argument("--sim", type=int, default=5000000, help="Simulation instructions for VPN extraction")
    parser.add_argument("--include-perforated", action="store_true", help="Also generate perforated pmap files")

    args = parser.parse_args()

    if args.extract_vpns or args.all:
        if not args.trace:
            parser.error("--trace required for --extract-vpns / --all")
        vpns = extract_vpns_from_heatmap(args.binary, args.trace, args.output_dir,
                                         args.warmup, args.sim)
    elif args.generate:
        if not args.vpn_file:
            parser.error("--vpn-file required for --generate")
        vpns = load_vpns(args.vpn_file)
    else:
        parser.error("Specify --extract-vpns, --generate, or --all")

    if args.generate or args.all:
        os.makedirs(args.output_dir, exist_ok=True)
        generate_all_4k(vpns, os.path.join(args.output_dir, "all_4k.pmap"))
        generate_all_2m(vpns, os.path.join(args.output_dir, "all_2m.pmap"))
        generate_mixed(vpns, os.path.join(args.output_dir, "mixed.pmap"))

        if args.include_perforated:
            generate_mixed_with_perforated(vpns, os.path.join(args.output_dir, "mixed_perf.pmap"))
            generate_all_2m_for_perforation(vpns, os.path.join(args.output_dir, "all_2m_for_perf.pmap"))

        print(f"\n[done] All pmap files generated in {args.output_dir}")


if __name__ == "__main__":
    main()
