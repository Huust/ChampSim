#!/usr/bin/env python3

import os
import re
import pandas as pd
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Optional


def extract_ipc(content):
    roi_match = re.search(r'Region of Interest Statistics.*?cumulative IPC:\s*([\d\.]+)', content, re.DOTALL)
    return float(roi_match.group(1)) if roi_match else 0.0


def extract_llc_miss_latency(content):
    llc_match = re.search(r'cpu0->LLC.*?AVERAGE MISS LATENCY:\s*([\d\.]+)\s*cycles', content, re.DOTALL)
    return float(llc_match.group(1)) if llc_match else 0.0


def extract_shim_requests(content):
    dram_read_match = re.search(r'DRAM REQUESTS.*?READ:\s*(\d+)', content, re.DOTALL)
    dram_write_match = re.search(r'DRAM REQUESTS.*?WRITE:\s*(\d+)', content, re.DOTALL)
    cxl_read_match = re.search(r'CXL REQUESTS.*?READ:\s*(\d+)', content, re.DOTALL)
    cxl_write_match = re.search(r'CXL REQUESTS.*?WRITE:\s*(\d+)', content, re.DOTALL)

    return {
        'dram_reads': int(dram_read_match.group(1)) if dram_read_match else 0,
        'dram_writes': int(dram_write_match.group(1)) if dram_write_match else 0,
        'cxl_reads': int(cxl_read_match.group(1)) if cxl_read_match else 0,
        'cxl_writes': int(cxl_write_match.group(1)) if cxl_write_match else 0
    }


def extract_tlb_metrics(content):
    # Extract ITLB raw data
    # Pattern: cpu0->cpu0_ITLB TOTAL        ACCESS:   17534587 HIT:   17532610 MISS:       1977
    itlb_match = re.search(r'cpu0->cpu0_ITLB TOTAL\s+ACCESS:\s*(\d+)\s+HIT:\s*(\d+)\s+MISS:\s*(\d+)', content)
    if itlb_match:
        itlb_access = int(itlb_match.group(1))
        itlb_miss = int(itlb_match.group(3))
    else:
        itlb_access = itlb_miss = 0

    # Extract DTLB raw data
    # Pattern: cpu0->cpu0_DTLB TOTAL        ACCESS:   40792749 HIT:   40284213 MISS:     508536
    dtlb_match = re.search(r'cpu0->cpu0_DTLB TOTAL\s+ACCESS:\s*(\d+)\s+HIT:\s*(\d+)\s+MISS:\s*(\d+)', content)
    if dtlb_match:
        dtlb_access = int(dtlb_match.group(1))
        dtlb_miss = int(dtlb_match.group(3))
    else:
        dtlb_access = dtlb_miss = 0

    # Extract STLB raw data
    # Pattern: cpu0->cpu0_STLB TOTAL        ACCESS:     110130 HIT:     109334 MISS:        796
    stlb_match = re.search(r'cpu0->cpu0_STLB TOTAL\s+ACCESS:\s*(\d+)\s+HIT:\s*(\d+)\s+MISS:\s*(\d+)', content)
    if stlb_match:
        stlb_access = int(stlb_match.group(1))
        stlb_miss = int(stlb_match.group(3))
    else:
        stlb_access = stlb_miss = 0

    # Extract STLB average miss latency
    # Pattern: cpu0->cpu0_STLB AVERAGE MISS LATENCY: 317 cycles
    stlb_latency_match = re.search(r'cpu0->cpu0_STLB AVERAGE MISS LATENCY:\s*([\d\.]+)\s*cycles', content)
    stlb_avg_miss_latency = float(stlb_latency_match.group(1)) if stlb_latency_match else 0.0

    return {
        'itlb_access': itlb_access,
        'itlb_miss': itlb_miss,
        'dtlb_access': dtlb_access,
        'dtlb_miss': dtlb_miss,
        'stlb_access': stlb_access,
        'stlb_miss': stlb_miss,
        'stlb_avg_miss_latency': stlb_avg_miss_latency
    }


def parse_file_path(file_path, add_skip_suffix=False):
    """
    Extract config and benchmark from file path.
    New structure: results/{config}/{benchmark}.out or results_skip/{config}/{benchmark}.out
    Example: results/access_r1_3/403.gcc-16B.out
    Returns: ('access_r1_3', '403.gcc-16B') or ('access_r1_3_skip', '403.gcc-16B') if add_skip_suffix=True
    """
    # Get the parent directory name as config
    parent_dir = os.path.basename(os.path.dirname(file_path))

    # Get the filename without extension as benchmark
    filename = os.path.basename(file_path)
    benchmark = os.path.splitext(filename)[0]

    # Skip if parent directory is the results root (not a config subdirectory)
    # or if it's a special directory like 'heatmaps'
    if parent_dir in ['results', 'results_skip', 'heatmaps']:
        return None, None

    # Add _skip suffix if this is from results_skip directory
    config = parent_dir
    if add_skip_suffix and 'results_skip' in file_path:
        # Don't add _skip to dram_only, cxl_only, or interleaving (these don't have skip variants)
        if config not in ['dram_only', 'cxl_only', 'interleaving']:
            config = f"{parent_dir}_skip"

    return config, benchmark


def read_stats(file_path, add_skip_suffix=False):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()

        if "Region of Interest Statistics" not in content:
            return None

        config, benchmark = parse_file_path(file_path, add_skip_suffix=add_skip_suffix)

        # Skip if config/benchmark couldn't be parsed
        if config is None or benchmark is None:
            return None

        ipc = extract_ipc(content)
        llc_miss_latency = extract_llc_miss_latency(content)
        shim_requests = extract_shim_requests(content)
        tlb_metrics = extract_tlb_metrics(content)

        return [config, benchmark, ipc, llc_miss_latency,
                shim_requests['dram_reads'], shim_requests['cxl_reads'],
                shim_requests['dram_writes'], shim_requests['cxl_writes'],
                tlb_metrics['itlb_access'], tlb_metrics['itlb_miss'],
                tlb_metrics['dtlb_access'], tlb_metrics['dtlb_miss'],
                tlb_metrics['stlb_access'], tlb_metrics['stlb_miss'], tlb_metrics['stlb_avg_miss_latency']]

    except Exception as e:
        print(f"Error parsing {file_path}: {e}")
        return None


def collect_stats(results_dirs, output_file, num_workers=8):
    """
    Collect stats from multiple results directories.

    Args:
        results_dirs: List of tuples (directory_path, add_skip_suffix)
        output_file: Path to output CSV file
        num_workers: Number of parallel worker threads
    """
    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(output_file)
    if output_dir and not os.path.exists(output_dir):
        try:
            os.makedirs(output_dir, exist_ok=True)
        except Exception as e:
            print(f"ERROR: Cannot create output directory: {output_dir}")
            print(f"Reason: {e}")
            return False

    # Collect all .out file paths from all directories
    out_files = []  # List of tuples (file_path, add_skip_suffix)

    for results_dir, add_skip_suffix in results_dirs:
        if not os.path.exists(results_dir):
            print(f"WARNING: Results directory not found: {results_dir} (skipping)")
            continue

        print(f"Scanning for .out files in {results_dir}...")
        dir_count = 0
        for root, dirs, files in os.walk(results_dir):
            for file in files:
                if file.endswith('.out'):
                    out_files.append((os.path.join(root, file), add_skip_suffix))
                    dir_count += 1
        print(f"  Found {dir_count} .out files")

    print(f"\nTotal: {len(out_files)} .out files to process")
    if not out_files:
        print("No .out files found to process")
        return False

    # Process files in parallel using ThreadPoolExecutor
    print(f"Processing with {num_workers} worker threads...")
    all_stats = []

    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        # Submit all tasks
        future_to_file = {executor.submit(read_stats, fp, add_skip): (fp, add_skip)
                         for fp, add_skip in out_files}

        # Collect results as they complete
        completed = 0
        for future in as_completed(future_to_file):
            file_path, add_skip = future_to_file[future]
            try:
                stats = future.result()
                if stats:
                    all_stats.append(stats)
                completed += 1
                if completed % 10 == 0 or completed == len(out_files):
                    print(f"  Progress: {completed}/{len(out_files)} files processed")
            except Exception as e:
                print(f"Error processing {file_path}: {e}")

    # Write all results to CSV
    print(f"Writing {len(all_stats)} results to {output_file}...")
    try:
        with open(output_file, 'w') as f:
            f.write("config,trace,ipc,llc_miss_latency,dram_reads,cxl_reads,dram_writes,cxl_writes,itlb_access,itlb_miss,dtlb_access,dtlb_miss,stlb_access,stlb_miss,stlb_avg_miss_latency\n")
            for stats in all_stats:
                f.write(','.join(map(str, stats)) + '\n')
    except Exception as e:
        print(f"ERROR: Cannot write to: {output_file}")
        print(f"Reason: {e}")
        return False

    print(f"Successfully processed {len(all_stats)} files")
    return True


def generate_pickles(csv_path, pkl_path):
    if not os.path.exists(csv_path):
        print(f"ERROR: CSV file not found: {csv_path}")
        return False

    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(pkl_path)
    if output_dir and not os.path.exists(output_dir):
        try:
            os.makedirs(output_dir, exist_ok=True)
        except Exception as e:
            print(f"ERROR: Cannot create directory: {output_dir}")
            print(f"Reason: {e}")
            return False

    try:
        df = pd.read_csv(csv_path)
        df.to_pickle(pkl_path)
        print(f"Generated pickle: {df.shape[0]} rows, {df.shape[1]} columns")
        return True
    except Exception as e:
        print(f"ERROR: Failed to generate pickle file")
        print(f"Reason: {e}")
        return False


# Hardcoded paths - modify these as needed
# Directory structure:
# /proj/uart_chp_cxl_trans/songtao/
# ├── results/           (input: SLURM outputs)
# │   ├── dram_only/
# │   │   ├── 403.gcc-16B.out
# │   │   └── 505.mcf_s-1152B.out
# │   ├── cxl_only/
# │   ├── interleaving/
# │   ├── access_r1_1/
# │   ├── access_r1_3/
# │   ├── criticality_r1_1/
# │   ├── criticality_r1_3/
# │   └── heatmaps/      (ignored by parser)
# └── analysis/          (output: statistics and plots)
#     ├── collected_stats.csv
#     └── collected_stats.pkl

if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser(description='Collect ChampSim statistics from .out files')
    parser.add_argument('-d', '--results-dir', default='../results',
                        help='Results directory for no-skip configurations (default: ../results)')
    parser.add_argument('-s', '--results-skip-dir', default='../results_skip',
                        help='Results directory for skip-translation configurations (default: ../results_skip)')
    parser.add_argument('-o', '--output', default='./analysis/collected_stats.csv',
                        help='Output CSV file path (default: ./analysis/collected_stats.csv)')
    parser.add_argument('-p', '--pickle', default='./analysis/collected_stats.pkl',
                        help='Output pickle file path (default: ./analysis/collected_stats.pkl)')
    parser.add_argument('-j', '--jobs', type=int, default=8,
                        help='Number of parallel worker threads (default: 8)')
    parser.add_argument('--skip-only', action='store_true',
                        help='Only collect from skip-translation directory')
    parser.add_argument('--no-skip-only', action='store_true',
                        help='Only collect from no-skip directory')

    args = parser.parse_args()

    # Prepare list of directories to process
    results_dirs = []

    if args.skip_only:
        # Only process skip directory
        results_dirs.append((args.results_skip_dir, True))
    elif args.no_skip_only:
        # Only process no-skip directory
        results_dirs.append((args.results_dir, False))
    else:
        # Process both directories (default)
        results_dirs.append((args.results_dir, False))
        results_dirs.append((args.results_skip_dir, True))

    print(f"Collecting statistics from {len(results_dirs)} directory/directories...")

    if not collect_stats(results_dirs, args.output, num_workers=args.jobs):
        exit(1)

    if not generate_pickles(args.output, args.pickle):
        exit(1)
