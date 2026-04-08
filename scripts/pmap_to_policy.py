#!/usr/bin/env python3
"""Convert pmap (vpn,0 / vpn,1 / vpn,2,bitmap) to policy format (base_vpn,page_type,tier_bitmap).

Reads a heatmap to know all 2MB regions in the workload, then converts:
  - vpn,1 → region is PAGE_2M, tier_bitmap = all-DRAM
  - vpn,0 → region is PAGE_4K, tier_bitmap per subpage
  - vpn,2,bitmap_hex → region is PAGE_PERF, tier_bitmap = hole bitmap (passed through)
  - absent regions → PAGE_2M, tier_bitmap = all-CXL

Usage: pmap_to_policy.py <input_pmap> <input_heatmap> <output_policy>
"""
import sys

PAGES_PER_2MB = 512

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <input_pmap> <input_heatmap> <output_policy>")
        sys.exit(1)

    pmap_path, heatmap_path, output_path = sys.argv[1], sys.argv[2], sys.argv[3]

    # Read capacity from heatmap
    capacity = None
    all_vpns = set()
    with open(heatmap_path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#capacity:"):
                capacity = int(line.split(":", 1)[1].strip())
            elif line and not line.startswith("#") and ":" in line:
                vpn = int(line.split(":")[0].strip(), 16)
                all_vpns.add(vpn)

    # Collect all 2MB regions from heatmap
    all_regions = set()
    for vpn in all_vpns:
        all_regions.add((vpn // PAGES_PER_2MB) * PAGES_PER_2MB)

    # Parse pmap
    huge_regions = set()       # bases with vpn,1
    dram_4kb = set()           # individual vpn,0
    perf_regions = {}          # base -> bitmap_hex for vpn,2,bitmap (standard PERF)
    iperf_regions = {}         # base -> bitmap_hex for vpn,3,bitmap (inverted PERF)
    idealperf_regions = {}     # base -> bitmap_hex for vpn,4,bitmap (ideal PERF)
    with open(pmap_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) < 2:
                continue
            vpn = int(parts[0], 16)
            ptype = int(parts[1])
            if ptype == 1:
                huge_regions.add(vpn)
            elif ptype == 2 and len(parts) > 2:
                perf_regions[vpn] = parts[2]
            elif ptype == 3 and len(parts) > 2:
                iperf_regions[vpn] = parts[2]
            elif ptype == 4 and len(parts) > 2:
                idealperf_regions[vpn] = parts[2]
            elif ptype == 0:
                dram_4kb.add(vpn)

    # Build policy
    ALL_DRAM = "0" * 128
    ALL_CXL = "f" * 128

    lines = []
    if capacity is not None:
        lines.append(f"#capacity: {capacity}")

    for base in sorted(all_regions):
        if base in huge_regions:
            lines.append(f"{base:x},1,{ALL_DRAM}")
        elif base in perf_regions:
            # Standard PERF: tier_bitmap == hole_bitmap (bit=0 kept/DRAM, bit=1 hole/CXL).
            lines.append(f"{base:x},2,{perf_regions[base]}")
        elif base in iperf_regions:
            # Inverted PERF (type=3): base frame in CXL, holes re-mapped to DRAM.
            # hole_bitmap: bit=1 = hot hole (hardware semantics, triggers re-route).
            # tier_bitmap: bit=0 = DRAM for hot holes (inverted from hole_bitmap).
            # Pass through as type=3; simulator handles the inversion.
            lines.append(f"{base:x},3,{iperf_regions[base]}")
        elif base in idealperf_regions:
            # Ideal PERF (type=4): 2MB TLB, per-subpage routing, zero overhead.
            # tier_bitmap: bit=1 → CXL, bit=0 → DRAM (same convention as type=2).
            lines.append(f"{base:x},4,{idealperf_regions[base]}")
        else:
            # Check if any 4KB pages in this region are in DRAM
            dram_offsets = set()
            for vpn in dram_4kb:
                if (vpn // PAGES_PER_2MB) * PAGES_PER_2MB == base:
                    dram_offsets.add(vpn - base)

            if not dram_offsets:
                lines.append(f"{base:x},1,{ALL_CXL}")
            else:
                # Build tier bitmap: bit=0 DRAM, bit=1 CXL
                words = [0] * 8
                for i in range(PAGES_PER_2MB):
                    if i not in dram_offsets:
                        words[i // 64] |= (1 << (i % 64))
                bitmap = "".join(f"{w:016x}" for w in words)
                lines.append(f"{base:x},0,{bitmap}")

    with open(output_path, "w") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Converted: {len(huge_regions)} 2MB, {len(perf_regions)} PERF, {len(idealperf_regions)} IDEAL_PERF, {len(dram_4kb)} 4KB pages, {len(all_regions)} total regions")


if __name__ == "__main__":
    main()
