#!/usr/bin/env python3

import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns
from matplotlib.backends.backend_pdf import PdfPages
from collections import defaultdict
from scipy.interpolate import make_interp_spline
from scipy.ndimage import gaussian_filter1d
import math
from concurrent.futures import ThreadPoolExecutor, as_completed
import time

# --- User Configuration ---
# Set the directory containing your heatmap data files
HEATMAP_DIR = './results/heatmaps/'
# Set the directory where the output PDFs will be saved
OUTPUT_DIR = './plots_final/'

# DRAM:CXL allocation ratio (default 1:3 means 25% DRAM, 75% CXL)
DEFAULT_DRAM_RATIO = 1
DEFAULT_CXL_RATIO = 3

# Define bright, vivid colors for heatmap plots
BRIGHT_COLORS = ['#FF1493', '#00BFFF', '#32CD32', '#FFD700', '#FF6347', '#9370DB']
HEATMAP_LINE_COLORS = ['#FF1493', '#00BFFF']  # Hot pink for access, deep sky blue for critical
DRAM_COLOR = '#7C3AED'  # Purple for DRAM region
CXL_COLOR = '#FF6B35'   # Orange for CXL region
# --- End of Configuration ---

def load_heatmap_file(filepath):
    """
    Parses a custom heatmap file with the format 'address: access_count critical_count'.

    Returns:
        A pandas DataFrame with ['vaddr', 'access_count', 'critical_count'] columns.
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
            data.append([vaddr, access_count, critical_count])

    return pd.DataFrame(data, columns=['vaddr', 'access_count', 'critical_count'])

def smooth_curve(y_data, method='gaussian', window=50):
    """
    Generate a smooth curve approximation of discrete data.

    Args:
        y_data: Array of y values
        method: 'gaussian' or 'spline'
        window: Window size for smoothing

    Returns:
        Smoothed y values
    """
    if method == 'gaussian':
        # Use Gaussian filter for smoothing
        sigma = window / 6.0  # Standard deviation
        return gaussian_filter1d(y_data, sigma=sigma)
    elif method == 'spline':
        # Use spline interpolation
        x = np.arange(len(y_data))
        if len(y_data) < 4:
            return y_data
        # Use fewer points for spline to make it smoother
        sample_points = min(len(y_data), max(10, len(y_data) // 10))
        indices = np.linspace(0, len(y_data) - 1, sample_points, dtype=int)
        spl = make_interp_spline(x[indices], y_data[indices], k=min(3, len(indices) - 1))
        return spl(x)
    else:
        return y_data

def plot_bar_with_curve(ax, df, count_column, title, dram_ratio=1, cxl_ratio=3):
    """
    Plot bar chart with smooth curve overlay and DRAM/CXL region marking.

    Args:
        ax: Matplotlib axis
        df: DataFrame sorted by count (descending)
        count_column: 'access_count' or 'critical_count'
        title: Plot title
        dram_ratio: DRAM allocation ratio
        cxl_ratio: CXL allocation ratio
    """
    # Filter out zero values
    df_filtered = df[df[count_column] > 0].copy()

    if len(df_filtered) == 0:
        ax.text(0.5, 0.5, 'No non-zero data', ha='center', va='center')
        return

    # Calculate split point (DRAM:CXL ratio)
    total_ratio = dram_ratio + cxl_ratio
    dram_percentage = dram_ratio / total_ratio
    split_index = int(len(df_filtered) * dram_percentage)

    # Prepare data
    x = np.arange(len(df_filtered))
    y = df_filtered[count_column].values

    # Calculate areas (sum of counts in each region)
    dram_area = y[:split_index].sum()
    cxl_area = y[split_index:].sum()
    total_area = dram_area + cxl_area

    # Use fill_between instead of bar charts for much faster rendering
    ax.fill_between(x[:split_index], 0, y[:split_index], color=DRAM_COLOR,
                    alpha=0.5, label=f'DRAM ({dram_percentage*100:.0f}%)', step='mid')
    ax.fill_between(x[split_index:], 0, y[split_index:], color=CXL_COLOR,
                    alpha=0.5, label=f'CXL ({(1-dram_percentage)*100:.0f}%)', step='mid')

    # Generate and plot smooth spline curve (not piecewise linear)
    y_smooth = smooth_curve(y, method='spline')
    ax.plot(x, y_smooth, color='#666666', linewidth=2.5, zorder=10)

    # Mark the split point with a vertical line (no label for legend)
    ax.axvline(x=split_index, color='red', linestyle='--', linewidth=2, zorder=11)

    # Add text annotations in center of each region showing area
    if split_index > 0:
        dram_mid_x = split_index // 2
        dram_mid_y = y[dram_mid_x] if dram_mid_x < len(y) else y[0]
        ax.text(dram_mid_x, dram_mid_y * 1.5,
                f'DRAM Area:\n{dram_area:,}\n({dram_area/total_area*100:.1f}%)',
                ha='center', va='bottom', fontsize=11, fontweight='bold',
                bbox=dict(boxstyle='round,pad=0.5', facecolor=DRAM_COLOR, alpha=0.8, edgecolor='black'))

    if split_index < len(y):
        cxl_mid_x = split_index + (len(y) - split_index) // 2
        cxl_mid_y = y[cxl_mid_x] if cxl_mid_x < len(y) else y[-1]
        ax.text(cxl_mid_x, cxl_mid_y * 1.5,
                f'CXL Area:\n{cxl_area:,}\n({cxl_area/total_area*100:.1f}%)',
                ha='center', va='bottom', fontsize=11, fontweight='bold',
                bbox=dict(boxstyle='round,pad=0.5', facecolor=CXL_COLOR, alpha=0.8, edgecolor='black'))

    # Formatting - use linear scale instead of log
    ax.set_title(title, fontsize=12, fontweight='bold')
    ax.set_xlabel('Page Rank (sorted by count, descending)', fontsize=10)
    ax.set_ylabel('Access Count', fontsize=10)
    ax.grid(True, which='both', linestyle='--', linewidth=0.5, alpha=0.3)
    ax.legend(loc='upper right', fontsize=9)

def generate_reports():
    """
    Main function to find heatmap data and generate analysis PDFs.
    """
    if not os.path.isdir(HEATMAP_DIR):
        print(f"Error: Heatmap directory '{HEATMAP_DIR}' not found. Exiting.")
        return

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # --- Step 1: Discover all heatmap files ---
    all_traces = [os.path.join(HEATMAP_DIR, f) for f in os.listdir(HEATMAP_DIR)
                  if os.path.isfile(os.path.join(HEATMAP_DIR, f))]
    print(f"Found {len(all_traces)} heatmap files to process.")

    # --- Step 2: Group traces by base program name ---
    program_groups = defaultdict(list)
    for trace_path in all_traces:
        trace_name = os.path.basename(trace_path)
        # Remove _access/_criticality or -access/-criticality suffix before extracting program name
        clean_name = trace_name.replace('_access', '').replace('_criticality', '')
        clean_name = clean_name.replace('-access', '').replace('-criticality', '')
        program_name = clean_name.split('.')[0]
        if 'ligra' in program_name:
             program_name = '_'.join(program_name.split('_')[:2])
        program_groups[program_name].append(trace_path)

    # --- Step 3: Generate PDF report ---
    _generate_bar_curve_pdf(program_groups, os.path.join(OUTPUT_DIR, 'report_heatmap_bar_curve.pdf'))

def load_heatmap_parallel(trace_path):
    """Helper function to load heatmap file in parallel."""
    trace_name = os.path.basename(trace_path)
    try:
        df = load_heatmap_file(trace_path)
        return (trace_path, trace_name, df, None)
    except Exception as e:
        return (trace_path, trace_name, None, str(e))

def _generate_bar_curve_pdf(program_groups, output_filename):
    """Generates the bar chart with smooth curve and region analysis PDF."""
    print(f"\n--- Generating Bar+Curve PDF: {output_filename} ---")
    start_time = time.time()

    with PdfPages(output_filename) as pdf:
        for program_name, trace_paths in sorted(program_groups.items()):
            print(f"  Plotting page for program: {program_name} ({len(trace_paths)} traces)")

            # Parallel loading of all heatmap files for this program
            print(f"    Loading {len(trace_paths)} heatmap files in parallel...")
            load_start = time.time()
            loaded_data = {}

            with ThreadPoolExecutor(max_workers=min(8, len(trace_paths))) as executor:
                futures = {executor.submit(load_heatmap_parallel, tp): tp for tp in sorted(trace_paths)}
                for future in as_completed(futures):
                    trace_path, display_name, df, error = future.result()
                    loaded_data[trace_path] = (display_name, df, error)

            load_time = time.time() - load_start
            print(f"    Loaded all files in {load_time:.2f}s")

            num_plots = len(trace_paths)
            # Each trace gets 1 subplot (based on filename suffix)
            ncols = 1
            nrows = num_plots
            fig, axes = plt.subplots(nrows, ncols, figsize=(12, 6 * nrows), squeeze=False)
            fig.suptitle(f'Workload: {program_name} (Memory Allocation Analysis)',
                        fontsize=18, fontweight='bold', y=0.995)

            for i, trace_path in enumerate(sorted(trace_paths)):
                display_name, df, error = loaded_data[trace_path]
                trace_name = os.path.basename(trace_path)
                print(f"    Plotting trace {i+1}/{len(trace_paths)}: {display_name}")

                if error is not None:
                    axes[i, 0].text(0.5, 0.5, f"Error: {error}", ha='center', va='center', color='red')
                    continue

                try:
                    # Determine which column to use based on filename suffix
                    # Check for both underscore and hyphen separators
                    if trace_name.endswith('_access') or trace_name.endswith('-access'):
                        count_column = 'access_count'
                        sort_type = 'Access Count'
                    elif trace_name.endswith('_criticality') or trace_name.endswith('-criticality'):
                        count_column = 'critical_count'
                        sort_type = 'Criticality Count'
                    else:
                        # Default to access_count if no suffix
                        count_column = 'access_count'
                        sort_type = 'Access Count'

                    # Sort by the selected column
                    df_sorted = df.sort_values(by=count_column, ascending=False).reset_index(drop=True)
                    plot_bar_with_curve(axes[i, 0], df_sorted, count_column,
                                       f'{display_name}\n(Sorted by {sort_type})',
                                       DEFAULT_DRAM_RATIO, DEFAULT_CXL_RATIO)

                except Exception as e:
                    axes[i, 0].text(0.5, 0.5, f"Error: {e}", ha='center', va='center', color='red')

            print(f"    Saving page to PDF...")
            plt.tight_layout(rect=[0, 0, 1, 0.99])
            pdf.savefig(fig, dpi=100)
            plt.close()

    total_time = time.time() - start_time
    print(f"Successfully saved {output_filename} in {total_time:.2f}s")

# --- Main Execution Block ---
if __name__ == "__main__":
    # Apply global plotting styles with bright colors
    sns.set_theme(style="whitegrid", palette=BRIGHT_COLORS)

    generate_reports()

    print("\nAll heatmap PDF generation tasks complete!")
