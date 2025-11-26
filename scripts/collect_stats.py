#!/usr/bin/env python3

import os
import re
import pandas as pd


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


def parse_file_path(file_path):
    """
    Extract config and benchmark from file path.
    New structure: results/{config}/{benchmark}.out
    Example: results/access_r1_3/403.gcc-16B.out
    Returns: ('access_r1_3', '403.gcc-16B')
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

    return parent_dir, benchmark


def read_stats(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()

        if "Region of Interest Statistics" not in content:
            return None

        config, benchmark = parse_file_path(file_path)

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


def collect_stats(results_dir, output_file):
    if not os.path.exists(results_dir):
        print(f"ERROR: Results directory not found: {results_dir}")
        print("Check 'results_dir' path in this script")
        return False

    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(output_file)
    if output_dir and not os.path.exists(output_dir):
        try:
            os.makedirs(output_dir, exist_ok=True)
        except Exception as e:
            print(f"ERROR: Cannot create output directory: {output_dir}")
            print(f"Reason: {e}")
            return False

    # Write header
    try:
        with open(output_file, 'w') as f:
            f.write("config,benchmark,ipc,llc_miss_latency,dram_reads,cxl_reads,dram_writes,cxl_writes,itlb_access,itlb_miss,dtlb_access,dtlb_miss,stlb_access,stlb_miss,stlb_avg_miss_latency\n")
    except Exception as e:
        print(f"ERROR: Cannot write to: {output_file}")
        print(f"Reason: {e}")
        return False

    # Process all .out files
    file_count = 0
    for root, dirs, files in os.walk(results_dir):
        for file in files:
            if file.endswith('.out'):
                file_path = os.path.join(root, file)
                stats = read_stats(file_path)
                if stats:
                    with open(output_file, 'a') as f:
                        f.write(','.join(map(str, stats)) + '\n')
                    file_count += 1

    print(f"Processed {file_count} files")
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
    results_dir = "/proj/uart_chp_cxl_trans/songtao/results"
    output_file = "/proj/uart_chp_cxl_trans/songtao/analysis/collected_stats.csv"
    output_pkl = '/proj/uart_chp_cxl_trans/songtao/analysis/collected_stats.pkl'

    if not collect_stats(results_dir, output_file):
        exit(1)

    if not generate_pickles(output_file, output_pkl):
        exit(1)
