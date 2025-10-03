#!/usr/bin/env python3

import os
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from matplotlib.backends.backend_pdf import PdfPages
from collections import defaultdict
import math

# --- User Configuration ---
# Set the directory containing your heatmap data files
HEATMAP_DIR = './results/heatmaps/'
# Set the directory where the output PDFs will be saved
OUTPUT_DIR = './plots_final/'

# Define bright, vivid colors for heatmap plots
BRIGHT_COLORS = ['#FF1493', '#00BFFF', '#32CD32', '#FFD700', '#FF6347', '#9370DB']  # Hot pink, deep sky blue, lime green, gold, tomato, medium slate blue
HEATMAP_LINE_COLORS = ['#FF1493', '#00BFFF']  # Hot pink for access, deep sky blue for critical
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

def generate_reports():
    """
    Main function to find heatmap data and generate analysis PDFs.
    """
    if not os.path.isdir(HEATMAP_DIR):
        print(f"Error: Heatmap directory '{HEATMAP_DIR}' not found. Exiting.")
        return

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # --- Step 1: Discover all heatmap files ---
    all_traces = [os.path.join(HEATMAP_DIR, f) for f in os.listdir(HEATMAP_DIR) if os.path.isfile(os.path.join(HEATMAP_DIR, f))]
    print(f"Found {len(all_traces)} heatmap files to process.")

    # --- Step 2: Group traces by base program name ---
    program_groups = defaultdict(list)
    for trace_path in all_traces:
        trace_name = os.path.basename(trace_path)
        program_name = trace_name.split('.')[0]
        if 'ligra' in program_name:
             program_name = '_'.join(program_name.split('_')[:2])
        program_groups[program_name].append(trace_path)

    # --- Step 3: Generate the two PDF reports ---
    _generate_cdf_pdf(program_groups, os.path.join(OUTPUT_DIR, 'report_heatmap_cdf.pdf'))
    _generate_log_counts_pdf(program_groups, os.path.join(OUTPUT_DIR, 'report_heatmap_log_counts.pdf'))

def _generate_cdf_pdf(program_groups, output_filename):
    """Generates the Cumulative Distribution Function (CDF) analysis PDF."""
    print(f"\n--- Generating CDF PDF: {output_filename} ---")
    
    with PdfPages(output_filename) as pdf:
        for program_name, trace_paths in sorted(program_groups.items()):
            print(f"  Plotting page for program: {program_name}")
            
            num_plots = len(trace_paths)
            ncols = min(num_plots, 3)
            nrows = math.ceil(num_plots / ncols)
            fig, axes = plt.subplots(nrows, ncols, figsize=(8.5 * ncols, 7 * nrows), squeeze=False)
            fig.suptitle(f'Workload: {program_name} (CDF Analysis)', fontsize=20, y=0.98)
            axes = axes.flatten()

            for i, trace_path in enumerate(sorted(trace_paths)):
                ax = axes[i]
                try:
                    df = load_heatmap_file(trace_path)
                    
                    # Sort by access hotness to get Page Rank
                    df = df.sort_values(by='access_count', ascending=False).reset_index(drop=True)

                    # Calculate CDFs
                    df['access_cdf'] = df['access_count'].cumsum() / df['access_count'].sum()
                    df['critical_cdf'] = df['critical_count'].cumsum() / df['critical_count'].sum()
                    
                    correlation = df['access_count'].corr(df['critical_count'])

                    # Plotting with bright colors
                    df.plot(y='access_cdf', ax=ax, label='Access Hotness', color=HEATMAP_LINE_COLORS[0], linewidth=2.5)
                    df.plot(y='critical_cdf', ax=ax, label='Critical Accesses', color=HEATMAP_LINE_COLORS[1], linewidth=2.5)
                    
                    ax.set_title(os.path.basename(trace_path))
                    ax.set_xlabel('Page Rank (sorted by access hotness)')
                    ax.set_ylabel('Cumulative Fraction of Total')
                    ax.grid(True, which='both', linestyle='--', linewidth=0.5)
                    ax.legend()
                    ax.text(0.05, 0.95, f'Correlation: {correlation:.3f}', transform=ax.transAxes,
                            fontsize=12, verticalalignment='top', bbox=dict(boxstyle='round,pad=0.5', facecolor='white', alpha=0.8))

                except Exception as e:
                    ax.text(0.5, 0.5, f"Error processing file:\n{os.path.basename(trace_path)}\n{e}", ha='center', va='center', color='red')

            for i in range(num_plots, len(axes)):
                axes[i].set_visible(False)

            plt.tight_layout(rect=[0, 0, 1, 0.96])
            pdf.savefig(fig)
            plt.close()

def _generate_log_counts_pdf(program_groups, output_filename):
    """Generates the log-scale count analysis PDF."""
    print(f"\n--- Generating Log Counts PDF: {output_filename} ---")

    with PdfPages(output_filename) as pdf:
        for program_name, trace_paths in sorted(program_groups.items()):
            print(f"  Plotting page for program: {program_name}")
            
            num_plots = len(trace_paths)
            ncols = min(num_plots, 3)
            nrows = math.ceil(num_plots / ncols)
            fig, axes = plt.subplots(nrows, ncols, figsize=(8.5 * ncols, 7 * nrows), squeeze=False)
            fig.suptitle(f'Workload: {program_name} (Log-Scale Count Analysis)', fontsize=20, y=0.98)
            axes = axes.flatten()

            for i, trace_path in enumerate(sorted(trace_paths)):
                ax = axes[i]
                try:
                    df = load_heatmap_file(trace_path)
                    
                    df = df.sort_values(by='access_count', ascending=False).reset_index(drop=True)
                    
                    correlation = df['access_count'].corr(df['critical_count'])

                    df.plot(y='access_count', ax=ax, label='Access Counts', color=HEATMAP_LINE_COLORS[0], linewidth=2.5)
                    df.plot(y='critical_count', ax=ax, label='Critical Counts', color=HEATMAP_LINE_COLORS[1], linewidth=2.5, alpha=0.9)
                    
                    ax.set_yscale('log')
                    ax.set_title(os.path.basename(trace_path))
                    ax.set_xlabel('Page Rank (sorted by access counts)')
                    ax.set_ylabel('Count Value (Log Scale)')
                    ax.grid(True, which='both', linestyle='--', linewidth=0.5)
                    ax.legend()
                    ax.text(0.05, 0.95, f'Correlation: {correlation:.3f}', transform=ax.transAxes,
                            fontsize=12, verticalalignment='top', bbox=dict(boxstyle='round,pad=0.5', facecolor='white', alpha=0.8))

                except Exception as e:
                    ax.text(0.5, 0.5, f"Error processing file:\n{os.path.basename(trace_path)}\n{e}", ha='center', va='center', color='red')
            
            for i in range(num_plots, len(axes)):
                axes[i].set_visible(False)

            plt.tight_layout(rect=[0, 0, 1, 0.96])
            pdf.savefig(fig)
            plt.close()

# --- Main Execution Block ---
if __name__ == "__main__":
    # Apply global plotting styles with bright colors
    sns.set_theme(style="whitegrid", palette=BRIGHT_COLORS)
    
    generate_reports()
    
    print("\nAll heatmap PDF generation tasks complete!")
