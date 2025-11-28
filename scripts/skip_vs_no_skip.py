#!/usr/bin/env python3

import matplotlib.pyplot as plt
import pandas as pd
import numpy as np
import re
import os
from scipy.stats import gmean

# Configuration
INPUT_DIR = '../analysis'
OUTPUT_FILE = '../analysis/skip_vs_no_skip.pdf'

CONFIG_FILES = {
    'Access (1:1)': 'ipc_comparison_access_r1_1.txt',
    'Criticality (1:1)': 'ipc_comparison_criticality_r1_1.txt',
    'Access (1:3)': 'ipc_comparison_access_r1_3.txt',
    'Criticality (1:3)': 'ipc_comparison_criticality_r1_3.txt'
}

CONFIG_ORDER = ['Access (1:1)', 'Criticality (1:1)', 'Access (1:3)', 'Criticality (1:3)']
SUITES = ["SPEC CPU 2006", "SPEC CPU 2017", "PARSEC", "Ligra"]


def classify_benchmark(trace_name):
    """Classify trace into (suite, benchmark_name)"""
    if trace_name.startswith("4"):
        # SPEC 2006: 403.gcc-16B -> 403.gcc
        match = re.match(r"(\d+\.[a-zA-Z0-9_]+)", trace_name)
        bench = match.group(1) if match else trace_name.split('-')[0]
        return "SPEC CPU 2006", bench

    elif trace_name.startswith("6"):
        # SPEC 2017: 605.mcf_s-994B -> 605.mcf_s
        match = re.match(r"(\d+\.[a-zA-Z0-9_]+)", trace_name)
        bench = match.group(1) if match else trace_name.split('-')[0]
        return "SPEC CPU 2017", bench

    elif trace_name.startswith("parsec"):
        # PARSEC: parsec_2.1.blackscholes... -> blackscholes
        parts = trace_name.split('.')
        bench = parts[2] if len(parts) > 2 else trace_name
        return "PARSEC", bench

    elif trace_name.startswith("ligra"):
        # Ligra: ligra_BFS.com... -> ligra_BFS
        bench = trace_name.split('.')[0]
        return "Ligra", bench

    else:
        return "Other", trace_name


def parse_speedup_file(filepath, config_name):
    """Parse ipc_comparison_*.txt file and extract speedup data"""
    if not os.path.exists(filepath):
        print(f"Warning: {filepath} not found, skipping")
        return pd.DataFrame()

    data = []
    with open(filepath, 'r') as f:
        lines = f.readlines()

    parsing = False
    for line in lines:
        # Start parsing after header
        if "Trace Name" in line and "Speedup" in line:
            parsing = True
            continue

        if not parsing or not line.strip() or "SUMMARY" in line:
            continue

        if line.strip().startswith(("=", "-")):
            continue

        parts = line.split()
        if len(parts) < 4:
            continue

        # Extract speedup from last column (e.g., +47.28% or -18.76%)
        try:
            speedup = float(parts[-1].replace('%', '').replace('+', ''))
        except ValueError:
            continue

        trace = parts[0]
        suite, bench = classify_benchmark(trace)

        data.append({
            'Config': config_name,
            'Suite': suite,
            'Benchmark': bench,
            'Trace': trace,
            'Speedup_Pct': speedup
        })

    return pd.DataFrame(data)


def load_all_data():
    """Load and combine data from all config files"""
    df_list = []
    for config_name in CONFIG_ORDER:
        filepath = os.path.join(INPUT_DIR, CONFIG_FILES[config_name])
        df = parse_speedup_file(filepath, config_name)
        if not df.empty:
            df_list.append(df)

    if not df_list:
        raise ValueError("No data files found in " + INPUT_DIR)

    return pd.concat(df_list, ignore_index=True)


def compute_geomean_by_benchmark(df):
    """Calculate geometric mean of speedup for each benchmark"""
    # Convert percentage to ratio for geomean calculation
    df['Speedup_Ratio'] = 1 + (df['Speedup_Pct'] / 100.0)

    # Group by Config, Suite, Benchmark and calculate geomean
    grouped = df.groupby(['Config', 'Suite', 'Benchmark'])['Speedup_Ratio'].apply(gmean).reset_index()

    # Convert back to percentage
    grouped['Geomean_Speedup_Pct'] = (grouped['Speedup_Ratio'] - 1) * 100

    return grouped


def plot_speedup_comparison(grouped_data, output_file):
    """Generate 2x2 subplot figure with speedup comparisons"""
    plt.style.use('seaborn-v0_8-whitegrid')
    fig, axes = plt.subplots(2, 2, figsize=(20, 12))
    axes = axes.flatten()

    for i, suite in enumerate(SUITES):
        ax = axes[i]
        suite_data = grouped_data[grouped_data['Suite'] == suite]

        if suite_data.empty:
            ax.text(0.5, 0.5, 'No Data', ha='center', va='center')
            ax.set_title(suite, fontsize=14, fontweight='bold')
            continue

        # Pivot: rows=Benchmark, columns=Config
        pivot = suite_data.pivot(index='Benchmark', columns='Config', values='Geomean_Speedup_Pct')
        pivot = pivot[[c for c in CONFIG_ORDER if c in pivot.columns]]

        # Sort by total speedup (highest benefit first)
        pivot['_sort'] = pivot.sum(axis=1)
        pivot = pivot.sort_values(by='_sort', ascending=False).drop(columns='_sort')

        # Plot bars
        pivot.plot(kind='bar', ax=ax, width=0.85, edgecolor='black', linewidth=0.5, zorder=3)

        # Formatting
        ax.set_title(suite, fontsize=14, fontweight='bold')
        ax.set_ylabel("IPC Speedup vs. No-Skip (%)", fontsize=11)
        ax.set_xlabel("")
        ax.axhline(0, color='black', linewidth=1, zorder=4)
        ax.grid(axis='y', linestyle='--', alpha=0.7, zorder=0)
        ax.tick_params(axis='x', rotation=45, labelsize=9)

        # Legend only on first subplot
        if i == 0:
            ax.legend(title="Configuration", loc='upper right', fontsize=9)
        else:
            ax.get_legend().remove()

    plt.tight_layout()
    plt.savefig(output_file, format='pdf', bbox_inches='tight')
    print(f"Figure saved: {output_file}")


def main():
    try:
        # Load data from analysis directory
        all_data = load_all_data()

        # Calculate geomean per benchmark
        grouped = compute_geomean_by_benchmark(all_data)

        # Generate plot
        plot_speedup_comparison(grouped, OUTPUT_FILE)

    except Exception as e:
        print(f"Error: {e}")
        exit(1)


if __name__ == '__main__':
    main()
