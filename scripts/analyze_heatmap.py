#!/usr/bin/env python3

import os
import sys
import argparse

def load_heatmap_file(filepath):
    """
    Parses a custom heatmap file with the format 'address: access_count critical_count'.

    Returns:
        A list of dictionaries with 'vaddr', 'access_count', 'critical_count' keys.
    """
    data = []
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            parts = line.split(':')
            if len(parts) != 2:
                continue

            vaddr = parts[0].strip()
            counts = parts[1].strip().split()

            if len(counts) != 2:
                continue

            access_count = int(counts[0])
            critical_count = int(counts[1])
            data.append({
                'vaddr': vaddr,
                'access_count': access_count,
                'critical_count': critical_count
            })

    return data

def parse_ratio(ratio_str):
    """Parse ratio string like '1:3' into tuple (1, 3)"""
    try:
        parts = ratio_str.split(':')
        if len(parts) != 2:
            raise ValueError("Ratio must be in format 'X:Y'")
        dram = int(parts[0])
        cxl = int(parts[1])
        if dram <= 0 or cxl <= 0:
            raise ValueError("Ratio values must be positive")
        return dram, cxl
    except Exception as e:
        print(f"Error: Invalid ratio format '{ratio_str}'. Use format like '1:3'")
        sys.exit(1)

def analyze_heatmap(filepath, count_type, dram_ratio, cxl_ratio):
    """
    Analyze heatmap file and print statistics.

    Args:
        filepath: Path to heatmap file
        count_type: 'a' for access_count, 'c' for critical_count
        dram_ratio: DRAM allocation ratio
        cxl_ratio: CXL allocation ratio
    """
    # Check file exists
    if not os.path.isfile(filepath):
        print(f"Error: File not found: {filepath}")
        sys.exit(1)

    # Determine column to use
    if count_type == 'a':
        count_column = 'access_count'
        type_name = 'Access Count'
    elif count_type == 'c':
        count_column = 'critical_count'
        type_name = 'Criticality Count'
    else:
        print(f"Error: Invalid type '{count_type}'. Use 'a' for access or 'c' for criticality.")
        sys.exit(1)

    # Load data
    print(f"Loading: {os.path.basename(filepath)}")
    try:
        data = load_heatmap_file(filepath)
    except Exception as e:
        print(f"Error: Failed to load file: {e}")
        sys.exit(1)

    # Filter out zero values
    data_filtered = [entry for entry in data if entry[count_column] > 0]

    # Calculate split
    total_ratio = dram_ratio + cxl_ratio
    dram_percentage = dram_ratio / total_ratio
    cxl_percentage = cxl_ratio / total_ratio

    # Sort by selected column (descending)
    data_sorted = sorted(data_filtered, key=lambda x: x[count_column], reverse=True)

    split_index = int(len(data_sorted) * dram_percentage)

    # Calculate statistics
    total_pages = len(data)
    nonzero_pages = len(data_filtered)

    dram_pages = split_index
    cxl_pages = len(data_sorted) - split_index

    dram_count = sum(entry[count_column] for entry in data_sorted[:split_index])
    cxl_count = sum(entry[count_column] for entry in data_sorted[split_index:])
    total_count = dram_count + cxl_count

    # Print results
    print("\n" + "="*60)
    print(f"HEATMAP ANALYSIS")
    print("="*60)
    print(f"File:              {os.path.basename(filepath)}")
    print(f"Analysis Type:     {type_name}")
    print(f"DRAM:CXL Ratio:    {dram_ratio}:{cxl_ratio} ({dram_percentage*100:.1f}% DRAM, {cxl_percentage*100:.1f}% CXL)")
    print("-"*60)
    print(f"Total pages:       {total_pages:,}")
    print(f"Non-zero pages:    {nonzero_pages:,}")
    print(f"Zero pages:        {total_pages - nonzero_pages:,}")
    print("-"*60)
    print(f"\nDRAM Region (Top {dram_percentage*100:.1f}%):")
    print(f"  Pages:           {dram_pages:,}")
    print(f"  Total count:     {int(dram_count):,}")
    print(f"  Percentage:      {dram_count/total_count*100:.2f}%")

    # Top 5 pages in DRAM region
    print(f"\n  Top 5 pages in DRAM:")
    dram_top5 = data_sorted[:min(5, split_index)]
    for idx, entry in enumerate(dram_top5):
        print(f"    #{idx+1:2d}  {entry['vaddr']:20s}  count: {int(entry[count_column]):,}")

    print(f"\nCXL Region (Remaining {cxl_percentage*100:.1f}%):")
    print(f"  Pages:           {cxl_pages:,}")
    print(f"  Total count:     {int(cxl_count):,}")
    print(f"  Percentage:      {cxl_count/total_count*100:.2f}%")

    # Top 5 pages in CXL region
    print(f"\n  Top 5 pages in CXL:")
    cxl_top5 = data_sorted[split_index:min(split_index+5, len(data_sorted))]
    for idx, entry in enumerate(cxl_top5, start=split_index):
        print(f"    #{idx+1:2d}  {entry['vaddr']:20s}  count: {int(entry[count_column]):,}")

    print("-"*60)
    print(f"Total count:       {int(total_count):,}")
    print("="*60 + "\n")

def main():
    parser = argparse.ArgumentParser(
        description='Analyze heatmap file and calculate DRAM/CXL allocation statistics.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Analyze access count with default 1:3 ratio
  %(prog)s results/heatmaps/trace_access --type a

  # Analyze criticality count with 1:3 ratio
  %(prog)s results/heatmaps/trace_criticality --type c --ratio 1:3

  # Analyze with custom 1:7 ratio (12.5% DRAM, 87.5% CXL)
  %(prog)s results/heatmaps/trace_access --type a --ratio 1:7
        """
    )

    parser.add_argument('heatmap_file',
                        help='Path to heatmap file')
    parser.add_argument('--type', '-t',
                        required=True,
                        choices=['a', 'c'],
                        help="Type of analysis: 'a' for access count, 'c' for criticality count")
    parser.add_argument('--ratio', '-r',
                        default='1:3',
                        help='DRAM:CXL allocation ratio (default: 1:3)')

    args = parser.parse_args()

    # Parse ratio
    dram_ratio, cxl_ratio = parse_ratio(args.ratio)

    # Analyze
    analyze_heatmap(args.heatmap_file, args.type, dram_ratio, cxl_ratio)

if __name__ == "__main__":
    main()
