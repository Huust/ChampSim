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


def parse_filename(filename):
    basename = os.path.splitext(filename)[0]
    known_configs = ['hybrid-hotness-criticality', 'hybrid-hotness-access', 'hybrid-roundrobin', 'dram_only', 'cxl_only']

    for config in known_configs:
        if basename.startswith(config + '_'):
            return config, basename[len(config) + 1:]

    # Fallback for unknown configs
    parts = basename.split('_', 1)
    return parts[0] if len(parts) > 0 else "unknown", parts[1] if len(parts) > 1 else basename


def read_stats(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()

        if "Region of Interest Statistics" not in content:
            return None

        config, benchmark = parse_filename(os.path.basename(file_path))
        ipc = extract_ipc(content)
        llc_miss_latency = extract_llc_miss_latency(content)
        shim_requests = extract_shim_requests(content)

        return [config, benchmark, ipc, llc_miss_latency,
                shim_requests['dram_reads'], shim_requests['cxl_reads'],
                shim_requests['dram_writes'], shim_requests['cxl_writes']]

    except Exception as e:
        print(f"Error parsing {file_path}: {e}")
        return None


def collect_stats(results_dir, output_file):
    if not os.path.exists(results_dir):
        print(f"Error: Directory {results_dir} does not exist")
        return

    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(output_file)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir, exist_ok=True)

    # Write header
    with open(output_file, 'w') as f:
        f.write("config,benchmark,ipc,llc_miss_latency,dram_reads,cxl_reads,dram_writes,cxl_writes\n")

    # Process all .out files
    for root, dirs, files in os.walk(results_dir):
        for file in files:
            if file.endswith('.out'):
                file_path = os.path.join(root, file)
                stats = read_stats(file_path)
                if stats:
                    with open(output_file, 'a') as f:
                        f.write(','.join(map(str, stats)) + '\n')


def generate_pickles(csv_path, pkl_path):
    if not os.path.exists(csv_path):
        print(f"Error: Input file {csv_path} does not exist.")
        return

    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(pkl_path)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir, exist_ok=True)

    try:
        df = pd.read_csv(csv_path)
        df.to_pickle(pkl_path)
    except Exception as e:
        print(f"An error occurred while processing the file: {e}")


# Hardcoded paths - modify these as needed
# Give full path name, and results directory shoule have the following structure:
# results/
# ├── dram_only/
# │   ├── dram_only_403.gcc-16B.champsimtrace.out
# │   └── dram_only_505.mcf_s-1152B.champsimtrace.out
# ├── cxl_only/
# │   └── cxl_only_403.gcc-16B.champsimtrace.out
# ├── hybrid-roundrobin/
# ├── hybrid-hotness-access/
# └── hybrid-hotness-criticality/
# └── collected_stats.pkl

results_dir = "./results"
output_file = "./results/collected_stats.csv"
collect_stats(results_dir, output_file)

input_csv = output_file
output_pkl = './results/collected_stats.pkl'
generate_pickles(input_csv, output_pkl)
