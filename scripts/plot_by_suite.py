#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from scipy.stats import gmean
import os
import re
from matplotlib.backends.backend_pdf import PdfPages
import numpy as np

# --- Configuration ---
# Set a consistent theme for all plots
sns.set_theme(style="whitegrid", palette="colorblind")

# Define grayscale palette - from black to white (9 shades for 9 configurations)
# Black -> Dark Gray -> Medium Gray -> Light Gray -> White
COLORS = ['#000000', '#1a1a1a', '#333333', '#4d4d4d', '#666666', '#808080', '#999999', '#b3b3b3', '#cccccc']

# Define which configurations to plot (excluding baseline dram_only and cxl_only)
# Order matters for plotting
PLOT_CONFIGS = [
    'interleaving',
    'access_r1_1',
    'access_r1_3',
    'criticality_r1_1',
    'criticality_r1_3',
    'access_r1_1_skip',
    'access_r1_3_skip',
    'criticality_r1_1_skip',
    'criticality_r1_3_skip'
]

# Baseline configuration for reference line
BASELINE_CONFIG = 'dram_only'


# --- Helper Functions ---
def identify_suite(trace_name):
    """
    Identifies which benchmark suite a trace belongs to.

    Returns: 'SPEC2006', 'SPEC2017', 'Parsec', or 'Ligra'
    """
    # Remove file extensions
    name = trace_name.replace('.champsimtrace.xz', '').replace('.champsimtrace', '')

    # SPEC CPU 2006: starts with 4XX.xxx
    if re.match(r'^4\d{2}\.', name):
        return 'SPEC2006'

    # SPEC CPU 2017: starts with 6XX.xxx
    if re.match(r'^6\d{2}\.', name):
        return 'SPEC2017'

    # Parsec: starts with "parsec_"
    if name.startswith('parsec_'):
        return 'Parsec'

    # Ligra: starts with "ligra_"
    if name.startswith('ligra_'):
        return 'Ligra'

    # Unknown suite
    return 'Unknown'


def extract_benchmark_base(trace_name):
    """
    Extracts the base benchmark name (without trace-specific suffixes).

    Examples:
    - '403.gcc-16B' -> '403.gcc'
    - 'ligra_BFS.com-lj.ungraph.gcc_6.3.0_O3.drop_5000M.length_250M' -> 'ligra_BFS'
    - 'parsec_2.1.facesim.simlarge.prebuilt.drop_750M.length_250M' -> 'parsec_2.1.facesim'
    - '619.lbm_s-2676B' -> '619.lbm_s'
    """
    # Remove file extensions
    name = trace_name.replace('.champsimtrace.xz', '').replace('.champsimtrace', '')

    # SPEC CPU format: XXX.name-NNNNB -> XXX.name
    spec_match = re.match(r'(\d{3}\.[a-zA-Z0-9_]+)-\d+B', name)
    if spec_match:
        return spec_match.group(1)

    # Ligra format: ligra_Algorithm.dataset... -> ligra_Algorithm
    ligra_match = re.match(r'(ligra_[^.]+)', name)
    if ligra_match:
        return ligra_match.group(1)

    # Parsec format: parsec_X.Y.program.simlarge... -> parsec_X.Y.program
    parsec_match = re.match(r'(parsec_\d+\.\d+\.[^.]+)', name)
    if parsec_match:
        return parsec_match.group(1)

    # Fallback
    return name.split('.')[0]


def shorten_benchmark_name(benchmark_base):
    """
    Creates a short, readable label for plotting.

    Examples:
    - '403.gcc' -> 'gcc'
    - 'ligra_BFS' -> 'BFS'
    - 'parsec_2.1.facesim' -> 'facesim'
    """
    # SPEC CPU: remove number prefix
    if re.match(r'^\d{3}\.', benchmark_base):
        return benchmark_base.split('.')[1].replace('_s', '').replace('_r', '')

    # Ligra: remove "ligra_" prefix
    if benchmark_base.startswith('ligra_'):
        return benchmark_base.replace('ligra_', '')

    # Parsec: extract program name
    if benchmark_base.startswith('parsec_'):
        parts = benchmark_base.split('.')
        if len(parts) >= 3:
            return parts[2]

    return benchmark_base


# --- Data Loading ---
def load_data(pkl_path, csv_path):
    """Loads data from a Pickle file if it exists, otherwise from a CSV file."""
    if os.path.exists(pkl_path):
        print(f"Loading data from: {pkl_path}")
        df = pd.read_pickle(pkl_path)
    elif os.path.exists(csv_path):
        print(f"Loading data from: {csv_path}")
        df = pd.read_csv(csv_path)
    else:
        print(f"ERROR: Data files not found")
        print(f"  Pickle: {pkl_path}")
        print(f"  CSV: {csv_path}")
        raise FileNotFoundError(f"Data files not found. Check paths in this script.")

    print(f"Loaded {len(df)} rows")
    return df


# --- Main Plotting Function ---
def generate_suite_ipc_pdf(df, output_filename="ipc_by_suite.pdf"):
    """
    Generates a multi-page PDF with one page per suite.
    Each page shows per-benchmark IPC speedup (normalized to dram_only baseline).
    Baseline shown as horizontal line at y=1.0.
    """
    print(f"\n--- Generating Suite-based IPC Speedup PDF ---")

    # Add suite and benchmark_base columns
    df['suite'] = df['trace'].apply(identify_suite)
    df['benchmark_base'] = df['trace'].apply(extract_benchmark_base)

    # Filter out Unknown suite
    df = df[df['suite'] != 'Unknown'].copy()

    # First, calculate baseline IPC for each trace
    baseline_ipc = df[df['config'] == BASELINE_CONFIG].set_index('trace')['ipc']

    if baseline_ipc.empty:
        print(f"ERROR: No baseline configuration '{BASELINE_CONFIG}' found in data!")
        return

    # Normalize IPC to baseline for each trace
    df['normalized_ipc'] = df.apply(
        lambda row: row['ipc'] / baseline_ipc.get(row['trace'], 1.0),
        axis=1
    )

    # Calculate geomean of normalized IPC for each (suite, benchmark_base, config) combination
    print("\nCalculating geomean of normalized IPC for each benchmark...")
    grouped = df.groupby(['suite', 'benchmark_base', 'config'], observed=True)['normalized_ipc'].apply(
        lambda x: gmean(x) * 100 if len(x) > 0 else 100.0  # Convert to percentage
    ).reset_index()
    grouped.rename(columns={'normalized_ipc': 'geomean_speedup_pct'}, inplace=True)

    # Add short labels for plotting
    grouped['short_label'] = grouped['benchmark_base'].apply(shorten_benchmark_name)

    # Define suite order for consistent plotting
    suite_order = ['SPEC2006', 'SPEC2017', 'Parsec', 'Ligra']

    with PdfPages(output_filename) as pdf:
        for suite_name in suite_order:
            suite_df = grouped[grouped['suite'] == suite_name].copy()

            if suite_df.empty:
                print(f"  Skipping {suite_name} (no data)")
                continue

            print(f"  Plotting page for: {suite_name}")

            # Sort benchmarks alphabetically for consistent ordering
            benchmark_order = sorted(suite_df['short_label'].unique())

            # Create figure with larger size for better spacing
            fig, ax = plt.subplots(figsize=(20, 10))

            # Pivot data for plotting
            pivot_df = suite_df.pivot(index='short_label', columns='config', values='geomean_speedup_pct')
            pivot_df = pivot_df.reindex(benchmark_order)  # Sort by benchmark name

            # Filter to only plot specified configurations
            plot_pivot = pivot_df[[c for c in PLOT_CONFIGS if c in pivot_df.columns]]

            # Plot bars with adjusted spacing
            x_pos = np.arange(len(benchmark_order))
            bar_width = 0.08  # Narrower bars for less crowding
            group_gap = 0.2  # Extra space between benchmark groups

            for i, config in enumerate(PLOT_CONFIGS):
                if config in plot_pivot.columns:
                    offset = (i - len(PLOT_CONFIGS)/2 + 0.5) * bar_width
                    values = plot_pivot[config].values
                    # Add edgecolor for better distinction in grayscale
                    ax.bar(x_pos + offset, values, bar_width,
                          label=config, color=COLORS[i % len(COLORS)],
                          edgecolor='black', linewidth=0.5)

            # Add baseline reference as horizontal line at y=100
            ax.axhline(100.0, color='black', linestyle='--', linewidth=2,
                      label=f'{BASELINE_CONFIG} (baseline)', zorder=0)

            # Determine appropriate y-axis limits based on data
            all_values = plot_pivot.values.flatten()
            all_values = all_values[~np.isnan(all_values)]  # Remove NaN values
            data_min = np.min(all_values) if len(all_values) > 0 else 90
            data_max = np.max(all_values) if len(all_values) > 0 else 110

            # Set y-axis limits with some padding
            y_min = max(70, int(data_min / 10) * 10 - 10)  # Round down to nearest 10, min 70
            y_max = min(150, int(data_max / 10) * 10 + 20)  # Round up to nearest 10, max 150

            # Formatting
            ax.set_title(f'IPC Speedup for {suite_name} Benchmarks\n'
                        f'(Geometric Mean, Normalized to {BASELINE_CONFIG})',
                        fontsize=18, fontweight='bold')
            ax.set_xlabel('Benchmark', fontsize=14)
            ax.set_ylabel('Normalized IPC (%)', fontsize=14)
            ax.set_xticks(x_pos)
            ax.set_xticklabels(benchmark_order, rotation=45, ha='right', fontsize=11)

            # Set y-axis limits and ticks
            ax.set_ylim(y_min, y_max)

            # Major ticks every 10 units (with labels)
            major_ticks = np.arange(y_min, y_max + 1, 10)
            ax.set_yticks(major_ticks)

            # Minor ticks every 5 units (without labels)
            minor_ticks = np.arange(y_min, y_max + 1, 5)
            ax.set_yticks(minor_ticks, minor=True)

            # Grid lines
            ax.grid(axis='y', which='major', alpha=0.5, linestyle='-', linewidth=0.8)
            ax.grid(axis='y', which='minor', alpha=0.3, linestyle='-', linewidth=0.4)

            ax.legend(loc='upper left', fontsize=10, ncol=3, framealpha=0.9)

            plt.tight_layout()
            pdf.savefig(fig, bbox_inches='tight')
            plt.close(fig)

    print(f"\nSuccessfully saved {output_filename}")


def print_suite_summary(df):
    """Prints a summary of benchmarks per suite."""
    df['suite'] = df['trace'].apply(identify_suite)
    df['benchmark_base'] = df['trace'].apply(extract_benchmark_base)

    print("\n--- Suite Summary ---")
    for suite in ['SPEC2006', 'SPEC2017', 'Parsec', 'Ligra']:
        suite_df = df[df['suite'] == suite]
        if not suite_df.empty:
            benchmarks = suite_df['benchmark_base'].unique()
            traces = suite_df['trace'].unique()
            print(f"{suite}:")
            print(f"  {len(benchmarks)} unique benchmarks")
            print(f"  {len(traces)} total traces")
            print(f"  Benchmarks: {', '.join(sorted([shorten_benchmark_name(b) for b in benchmarks]))}")
    print()


# --- Main Execution ---
if __name__ == "__main__":
    pkl_path = './analysis/collected_stats.pkl'
    csv_path = './analysis/collected_stats.csv'
    output_dir = './analysis/plots'

    try:
        os.makedirs(output_dir, exist_ok=True)

        main_df = load_data(pkl_path, csv_path)

        # Print summary of data
        print_suite_summary(main_df)

        # Generate the suite-based IPC speedup PDF
        generate_suite_ipc_pdf(
            main_df,
            output_filename=os.path.join(output_dir, "speedup_by_suite.pdf")
        )

        print("\nPlot saved to:", output_dir)

    except FileNotFoundError as e:
        print(f"\n{e}")
        exit(1)
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
        exit(1)
