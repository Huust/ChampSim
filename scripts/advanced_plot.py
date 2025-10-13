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

# Define the order for configurations to ensure consistent plotting
CONFIG_ORDER = ['dram_only', 'hybrid-hotness-criticality', 'hybrid-roundrobin', 'hybrid-hotness-access', 'cxl_only']

# Define sophisticated color palette with brighter colors
SOPHISTICATED_COLORS = ['#2E1065', '#7C3AED', '#EC4899', '#FF6B35', '#FFD700']  # Deep purple, purple, pink, bright orange, bright yellow
HYBRID_BREAKDOWN_COLORS = ['#7C3AED', '#FF6B35']  # Purple for DRAM, Bright orange for CXL

# --- Helper Function for Naming ---
def shorten_benchmark_name(full_name):
    """
    Shortens long benchmark trace names for better plot readability.
    Example: '403.gcc-16B.champsimtrace' -> 'gcc-16B'
    Example: 'parsec_2.1.vips.simlarge...' -> 'vips-...'
    """
    # Handle SPEC CPU benchmarks (e.g., 403.gcc-16B)
    spec_match = re.match(r'\d{3}\.([a-zA-Z0-9_]+-\d+B)', full_name)
    if spec_match:
        return spec_match.group(1)

    # Handle parsec benchmarks (e.g., parsec_2.1.vips.simlarge...)
    parsec_match = re.match(r'parsec_\d\.\d\.([a-zA-Z_]+)\.', full_name)
    if parsec_match:
        # Try to find a number part to append
        num_match = re.search(r'drop_(\d+)M', full_name)
        if num_match:
            return f"{parsec_match.group(1)}-{num_match.group(1)}M"
        return parsec_match.group(1)

    # Handle ligra benchmarks (e.g., ligra_PageRank.com-lj...)
    ligra_match = re.match(r'(ligra_[a-zA-Z_-]+)\.', full_name)
    if ligra_match:
        return ligra_match.group(1)

    # Fallback for any other format
    return full_name.split('.')[0]


def extract_program_base(full_name):
    """
    Extracts [program]_[algorithm].[dataset].[compiler]_[opt_level] from trace name.
    Example: 'ligra_BFS.com-lj.ungraph.gcc_6.3.0_O3.drop_11500M.length_250M.champsimtrace'
             -> 'ligra_BFS.com-lj.ungraph.gcc_6.3.0_O3'
    """
    # Pattern: everything before .drop_ (or .length_ if no drop_)
    match = re.match(r'(.+?)\.(?:drop_|length_)', full_name)
    if match:
        return match.group(1)

    # Fallback: take first few parts
    parts = full_name.split('.')
    if len(parts) >= 4:
        return '.'.join(parts[:4])

    return full_name.split('.champsimtrace')[0]


def extract_instruction_info(full_name):
    """
    Extracts [warmup_instructions].[simulation_instructions] from trace name.
    Example: 'ligra_BFS.com-lj.ungraph.gcc_6.3.0_O3.drop_11500M.length_250M.champsimtrace'
             -> '11500M.250M'
    Returns tuple: (display_string, warmup_value, length_value) for sorting
    """
    drop_match = re.search(r'drop_(\d+)M', full_name)
    length_match = re.search(r'length_(\d+)M', full_name)

    warmup = int(drop_match.group(1)) if drop_match else 0
    length = int(length_match.group(1)) if length_match else 0

    display = f"{warmup}M.{length}M"
    return display, warmup, length


# --- Data Loading ---
def load_data(pkl_path, csv_path):
    """Loads data from a Pickle file if it exists, otherwise from a CSV file."""
    if os.path.exists(pkl_path):
        print(f"Loading data from fast Pickle file: {pkl_path}")
        df = pd.read_pickle(pkl_path)
    elif os.path.exists(csv_path):
        print(f"Loading data from CSV file: {csv_path}")
        df = pd.read_csv(csv_path)
    else:
        raise FileNotFoundError(f"Neither {pkl_path} nor {csv_path} were found.")
    
    print("Data loaded successfully!")
    return df

# --- PDF Generation Functions ---

def generate_speedup_per_benchmark_pdf(df, output_filename="report_detailed_speedup.pdf", baseline='dram_only'):
    """Generates a multi-page PDF showing per-benchmark normalized IPC."""
    print(f"\n--- Generating PDF 1: Per-Benchmark Speedup vs {baseline} ---")
    
    # Pre-processing: Calculate program name, short name, and normalized IPC
    df['program'] = df['benchmark'].apply(lambda x: x.split('-')[0])
    df['short_name'] = df['benchmark'].apply(shorten_benchmark_name)
    
    baseline_ipc = df[df['config'] == baseline].set_index('benchmark')['ipc']
    df['normalized_ipc'] = df.apply(lambda row: row['ipc'] / baseline_ipc.get(row['benchmark'], 1), axis=1)

    with PdfPages(output_filename) as pdf:
        # Group by base program name
        for program_name, program_df in df.groupby('program'):
            print(f"  Plotting page for: {program_name}")
            plt.figure(figsize=(16, 9))
            
            # Use the NEW 'short_name' for the X-axis with sophisticated colors
            ax = sns.barplot(data=program_df, x='short_name', y='normalized_ipc', hue='config', hue_order=CONFIG_ORDER, palette=SOPHISTICATED_COLORS)
            
            ax.set_title(f'Normalized Performance for Workload: {program_name}', fontsize=16)
            ax.set_xlabel('Benchmark Trace', fontsize=12)
            ax.set_ylabel(f'Performance (Normalized to {baseline})', fontsize=12)
            # Use 45-degree rotation for shorter labels
            ax.tick_params(axis='x', rotation=45, labelsize=9)
            plt.setp(ax.get_xticklabels(), ha='right')
            ax.axhline(1.0, color='black', linestyle='--', linewidth=1.2)
            ax.legend(title='Configuration', bbox_to_anchor=(1.05, 1), loc='upper left')

            # Add value labels on top of each bar
            for container in ax.containers:
                ax.bar_label(container, fmt='%.2f', fontsize=8, fontweight='bold')
            
            # This should now work without warnings
            plt.tight_layout(rect=[0, 0, 0.85, 0.95]) # Adjust right margin for legend
            pdf.savefig()
            plt.close()
            
    print(f"Successfully saved {output_filename}")


def generate_breakdown_per_benchmark_pdf(df, output_filename="report_detailed_breakdown.pdf"):
    """Generates a multi-page PDF showing memory access breakdown for hybrid configurations."""
    print(f"\n--- Generating PDF 2: Per-Benchmark Access Breakdown ---")

    hybrid_df = df[df['config'].str.contains('hybrid', case=False)].copy()
    if hybrid_df.empty:
        print("Warning: No hybrid configurations found. Skipping breakdown PDF.")
        return

    # Extract program base (detailed grouping key) and instruction info for labels and sorting
    hybrid_df['program_base'] = hybrid_df['benchmark'].apply(extract_program_base)
    hybrid_df['dram_accesses'] = hybrid_df['dram_reads'] + hybrid_df['dram_writes']
    hybrid_df['cxl_accesses'] = hybrid_df['cxl_reads'] + hybrid_df['cxl_writes']

    # Extract instruction info for sorting
    hybrid_df[['instr_label', 'warmup_val', 'length_val']] = hybrid_df['benchmark'].apply(
        lambda x: pd.Series(extract_instruction_info(x))
    )

    with PdfPages(output_filename) as pdf:
        for program_base, program_df in hybrid_df.groupby('program_base'):
            print(f"  Plotting page for: {program_base}")

            fig, axes = plt.subplots(1, 3, figsize=(20, 8), sharey=True)
            fig.suptitle(f'Memory Access Breakdown for Workload: {program_base}', fontsize=18)

            hybrid_configs = ['hybrid-hotness-access', 'hybrid-hotness-criticality', 'hybrid-roundrobin']

            for i, config_name in enumerate(hybrid_configs):
                config_subset = program_df[program_df['config'] == config_name]
                if config_subset.empty:
                    continue # Skip if this config doesn't exist for this program

                # Sort by instruction numbers (warmup first, then length)
                config_subset = config_subset.sort_values(['warmup_val', 'length_val'])

                config_data = config_subset[['instr_label', 'dram_accesses', 'cxl_accesses']].set_index('instr_label')
                config_perc = config_data.div(config_data.sum(axis=1), axis=0) * 100

                config_perc.plot(kind='bar', stacked=True, ax=axes[i], color=HYBRID_BREAKDOWN_COLORS, legend=False)

                axes[i].set_title(config_name)
                axes[i].set_ylabel('Percentage of Off-Chip Accesses (%)' if i == 0 else '')
                # Use 45-degree rotation, which is more readable
                axes[i].tick_params(axis='x', rotation=45, labelsize=8)
                plt.setp(axes[i].get_xticklabels(), ha='right')
                axes[i].set_xlabel('')

                # Add percentage labels in the middle of each segment
                for j, (instr_label, row) in enumerate(config_perc.iterrows()):
                    dram_pct = row['dram_accesses']
                    cxl_pct = row['cxl_accesses']
                    # Add DRAM percentage label
                    axes[i].text(j, dram_pct/2, f'{dram_pct:.1f}%', ha='center', va='center', fontsize=7, fontweight='bold', color='white')
                    # Add CXL percentage label
                    axes[i].text(j, dram_pct + cxl_pct/2, f'{cxl_pct:.1f}%', ha='center', va='center', fontsize=7, fontweight='bold', color='white')

            handles, labels = axes[0].get_legend_handles_labels()
            fig.legend(handles, ['Serviced by DRAM', 'Serviced by CXL'], title='Memory Type', bbox_to_anchor=(0.5, 0.01), loc='lower center', ncol=2)

            # Use subplots_adjust to give more space at the bottom
            plt.subplots_adjust(bottom=0.25)
            pdf.savefig()
            plt.close()

    print(f"Successfully saved {output_filename}")

def plot_performance_by_mpki_groups(df, output_filename="summary_perf_by_mpki.png", baseline='dram_only'):
    """Groups benchmarks by LLC MPKI and plots the geometric mean of normalized IPC for each group."""
    print(f"\n--- Generating Summary Plot: Performance by Memory Intensity ---")
    
    # (This function is the same as the previous version, including it for completeness)
    if 'dram_reads' not in df.columns or 'cxl_reads' not in df.columns:
        print("Warning: Required columns for MPKI calculation not found. Skipping plot.")
        return
    
    baseline_df = df[df['config'] == baseline].copy()
    baseline_df['llc_mpki'] = ((baseline_df['dram_reads'] + baseline_df['cxl_reads']) * 1000) / 1_0000_0000
    
    mpki_map = baseline_df.set_index('benchmark')['llc_mpki']
    df['llc_mpki'] = df['benchmark'].map(mpki_map)

    bins = [0, 10, 50, float('inf')]
    labels = ['Low Intensity (MPKI < 10)', 'Medium Intensity (10-50)', 'High Intensity (MPKI >= 50)']
    df['mpki_group'] = pd.cut(df['llc_mpki'], bins=bins, labels=labels, right=False)
    
    baseline_ipc = df[df['config'] == baseline].set_index('benchmark')['ipc']
    df['normalized_ipc'] = df.apply(lambda row: row['ipc'] / baseline_ipc.get(row['benchmark'], 1), axis=1)

    grouped_perf = df.groupby(['mpki_group', 'config'])['normalized_ipc'].apply(gmean).reset_index()

    plt.figure(figsize=(14, 8))
    ax = sns.barplot(data=grouped_perf, x='mpki_group', y='normalized_ipc', hue='config', hue_order=CONFIG_ORDER, palette=SOPHISTICATED_COLORS)

    plt.title('Performance by Workload Memory Intensity', fontsize=18)
    plt.xlabel('Workload Type (Grouped by LLC MPKI)', fontsize=14)
    plt.ylabel(f'Normalized Performance (to {baseline})', fontsize=14)
    plt.axhline(1.0, color='black', linestyle='--', linewidth=1.2)
    plt.legend(title='Configuration')

    # Add value labels on top of each bar
    for container in ax.containers:
        ax.bar_label(container, fmt='%.3f', fontsize=9, fontweight='bold')

    plt.tight_layout()
    plt.savefig(output_filename, dpi=300)
    print(f"Successfully saved {output_filename}")
    plt.close()

# --- Main Execution ---
if __name__ == "__main__":
    pkl_path = './results/collected_stats.pkl'
    csv_path = './results/collected_stats.csv'
    
    try:
        output_dir = './plots_final'
        os.makedirs(output_dir, exist_ok=True)
        
        main_df = load_data(pkl_path, csv_path)
        
        generate_speedup_per_benchmark_pdf(main_df, output_filename=os.path.join(output_dir, "report_detailed_speedup.pdf"))
        generate_breakdown_per_benchmark_pdf(main_df, output_filename=os.path.join(output_dir, "report_detailed_breakdown.pdf"))
        plot_performance_by_mpki_groups(main_df, output_filename=os.path.join(output_dir, "summary_perf_by_mpki.png"))

        print("\nAll plot generation tasks complete!")

    except FileNotFoundError as e:
        print(f"\nError: {e}")
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}")
