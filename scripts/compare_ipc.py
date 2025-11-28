#!/usr/bin/env python3
"""
Compare IPC results from skip and non-skip experiments.
Extracts IPC from Region of Interest Statistics section.
Generates separate comparison reports for each configuration.
"""

import re
import os
from pathlib import Path
from collections import defaultdict
import statistics
from concurrent.futures import ThreadPoolExecutor, as_completed

# Base directories
# RESULTS_SKIP_DIR = "/proj/uart_chp_cxl_trans/songtao/results_skip"
# RESULTS_DIR = "/proj/uart_chp_cxl_trans/songtao/results"
# OUTPUT_DIR = "/proj/uart_chp_cxl_trans/songtao/analysis"

RESULTS_SKIP_DIR = "../results_skip"
RESULTS_DIR = "../results"
OUTPUT_DIR = "./analysis"

# Configurations to compare
CONFIGS = ['access_r1_1', 'access_r1_3', 'criticality_r1_1', 'criticality_r1_3']

def extract_ipc_from_file(filepath):
    """
    Extract IPC value from the Region of Interest Statistics section.

    Args:
        filepath: Path to the output file

    Returns:
        IPC value as float, or None if not found
    """
    try:
        with open(filepath, 'r') as f:
            content = f.read()

        # Find the Region of Interest Statistics section
        roi_match = re.search(r'Region of Interest Statistics.*?CPU 0 cumulative IPC:\s+([\d.]+)',
                             content, re.DOTALL)

        if roi_match:
            return float(roi_match.group(1))
        else:
            return None

    except Exception as e:
        return None

def compare_config(config_name):
    """
    Compare skip vs non-skip results for a specific configuration.

    Args:
        config_name: Configuration name (e.g., 'access_r1_1')

    Returns:
        Success status (True/False)
    """
    print(f"\nProcessing configuration: {config_name}")

    skip_dir = Path(RESULTS_SKIP_DIR) / config_name
    regular_dir = Path(RESULTS_DIR) / config_name

    if not skip_dir.exists():
        print(f"  ERROR: Skip directory not found: {skip_dir}")
        return False

    if not regular_dir.exists():
        print(f"  ERROR: Regular directory not found: {regular_dir}")
        return False

    # Collect IPC results using multithreading
    results = defaultdict(dict)

    # Read skip results with multithreading
    skip_files = list(skip_dir.glob('*.out'))
    with ThreadPoolExecutor(max_workers=8) as executor:
        future_to_file = {executor.submit(extract_ipc_from_file, f): f for f in skip_files}
        for future in as_completed(future_to_file):
            out_file = future_to_file[future]
            ipc = future.result()
            if ipc is not None:
                results[out_file.stem]['skip'] = ipc

    # Read regular (non-skip) results with multithreading
    regular_files = list(regular_dir.glob('*.out'))
    with ThreadPoolExecutor(max_workers=8) as executor:
        future_to_file = {executor.submit(extract_ipc_from_file, f): f for f in regular_files}
        for future in as_completed(future_to_file):
            out_file = future_to_file[future]
            ipc = future.result()
            if ipc is not None:
                results[out_file.stem]['regular'] = ipc

    if not results:
        print(f"  ERROR: No IPC values found")
        return False

    # Generate report
    output_file = Path(OUTPUT_DIR) / f"ipc_comparison_{config_name}.txt"

    with open(output_file, 'w') as f:
        f.write("=" * 120 + "\n")
        f.write(f"IPC Comparison: {config_name} (Skip vs Regular)\n")
        f.write("=" * 120 + "\n\n")

        skip_ipcs = []
        regular_ipcs = []
        speedups = []
        improvements = []
        regressions = []
        missing_data = []

        # Collect all results
        for trace_name in results.keys():
            skip_ipc = results[trace_name].get('skip')
            regular_ipc = results[trace_name].get('regular')

            if skip_ipc is not None and regular_ipc is not None:
                speedup = (skip_ipc / regular_ipc - 1) * 100
                speedups.append(speedup)
                skip_ipcs.append(skip_ipc)
                regular_ipcs.append(regular_ipc)

                entry = {
                    'trace': trace_name,
                    'skip_ipc': skip_ipc,
                    'regular_ipc': regular_ipc,
                    'speedup': speedup
                }

                if speedup > 0:
                    improvements.append(entry)
                else:
                    regressions.append(entry)
            else:
                missing_data.append({
                    'trace': trace_name,
                    'skip_ipc': skip_ipc,
                    'regular_ipc': regular_ipc
                })

        # Sort improvements by speedup (descending - best first)
        improvements.sort(key=lambda x: x['speedup'], reverse=True)

        # Sort regressions by speedup (ascending - worst first)
        regressions.sort(key=lambda x: x['speedup'])

        # Write improvements section
        if improvements:
            f.write(f"IMPROVEMENTS: Skip faster than Regular ({len(improvements)} traces)\n")
            f.write("=" * 120 + "\n")
            f.write(f"{'Trace Name':<80} {'Skip IPC':>12} {'Regular IPC':>12} {'Speedup':>12}\n")
            f.write("-" * 120 + "\n")
            for entry in improvements:
                speedup_str = f"{entry['speedup']:+.2f}%"
                f.write(f"{entry['trace']:<80} {entry['skip_ipc']:>12.4f} {entry['regular_ipc']:>12.4f} {speedup_str:>12}\n")
            f.write("\n")

        # Write regressions section
        if regressions:
            f.write(f"REGRESSIONS: Regular faster than Skip ({len(regressions)} traces)\n")
            f.write("=" * 120 + "\n")
            f.write(f"{'Trace Name':<80} {'Skip IPC':>12} {'Regular IPC':>12} {'Speedup':>12}\n")
            f.write("-" * 120 + "\n")
            for entry in regressions:
                speedup_str = f"{entry['speedup']:+.2f}%"
                f.write(f"{entry['trace']:<80} {entry['skip_ipc']:>12.4f} {entry['regular_ipc']:>12.4f} {speedup_str:>12}\n")
            f.write("\n")

        # Write missing data section if any
        if missing_data:
            f.write(f"MISSING DATA ({len(missing_data)} traces)\n")
            f.write("=" * 120 + "\n")
            f.write(f"{'Trace Name':<80} {'Skip IPC':>12} {'Regular IPC':>12}\n")
            f.write("-" * 120 + "\n")
            for entry in missing_data:
                skip_str = f"{entry['skip_ipc']:.4f}" if entry['skip_ipc'] is not None else "N/A"
                regular_str = f"{entry['regular_ipc']:.4f}" if entry['regular_ipc'] is not None else "N/A"
                f.write(f"{entry['trace']:<80} {skip_str:>12} {regular_str:>12}\n")
            f.write("\n")

        f.write("=" * 120 + "\n\n")

        # Summary statistics
        if speedups:
            f.write("SUMMARY STATISTICS\n")
            f.write("-" * 120 + "\n")
            f.write(f"Total traces compared: {len(speedups)}\n")
            f.write(f"Improvements (skip faster): {len(improvements)}\n")
            f.write(f"Regressions (regular faster): {len(regressions)}\n\n")

            f.write(f"Median speedup: {statistics.median(speedups):+.2f}%\n")
            f.write(f"Mean speedup: {statistics.mean(speedups):+.2f}%\n")
            f.write(f"Std dev: {statistics.stdev(speedups):.2f}%\n")
            f.write(f"Min speedup: {min(speedups):+.2f}%\n")
            f.write(f"Max speedup: {max(speedups):+.2f}%\n\n")

            f.write(f"Average Skip IPC: {statistics.mean(skip_ipcs):.4f}\n")
            f.write(f"Average Regular IPC: {statistics.mean(regular_ipcs):.4f}\n")

        f.write("\n" + "=" * 120 + "\n")
        f.write("Note: Positive speedup means Skip is faster than Regular\n")
        f.write("      Negative speedup means Regular is faster than Skip\n")

    print(f"  Generated: {output_file}")
    return True

def main():
    """Main function to compare all configurations."""
    print("=" * 80)
    print("IPC Comparison: Skip vs Regular Translation")
    print("=" * 80)

    # Create output directory
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # Compare each configuration
    success_count = 0
    for config in CONFIGS:
        if compare_config(config):
            success_count += 1

    print("\n" + "=" * 80)
    print(f"Completed: {success_count}/{len(CONFIGS)} configurations")
    print(f"Reports saved to: {OUTPUT_DIR}")
    print("=" * 80)

    return 0 if success_count == len(CONFIGS) else 1

if __name__ == "__main__":
    exit(main())
