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


def extract_workload_info(benchmark_name):
    """
    Extracts workload information from full benchmark name.

    Supports two formats:
    1. Ligra/Parsec: [program]_[algorithm].[dataset].[compiler]_[opt_level].drop_[num]M.length_[num]M.champsimtrace.xz
    2. SPEC CPU: [num].[program]-[id]B.champsimtrace.xz (e.g., 619.lbm_s-2676B.champsimtrace.xz)

    Returns: (workload_key, instruction_label, drop_num, length_num)
    - workload_key: Grouping identifier (e.g., "ligra_BFS.com-lj.ungraph.gcc_6.3.0_03" or "619.lbm_s")
    - instruction_label: X-axis label (e.g., "drop_11500M.length_250M" or "2676B")
    - drop_num, length_num: Numeric values for sorting
    """
    # Remove file extensions
    name = benchmark_name.replace('.champsimtrace.xz', '').replace('.champsimtrace', '')

    # Try Ligra/Parsec format first: [prefix].drop_[num]M.length_[num]M
    ligra_pattern = r'(.+?)\.drop_(\d+)M\.length_(\d+)M'
    ligra_match = re.match(ligra_pattern, name)

    if ligra_match:
        workload_key = ligra_match.group(1)  # [program]_[algorithm].[dataset].[compiler]_[opt_level]
        drop_num = int(ligra_match.group(2))  # warmup number
        length_num = int(ligra_match.group(3))  # simulation number
        instruction_label = f"drop_{ligra_match.group(2)}M.length_{ligra_match.group(3)}M"
        return workload_key, instruction_label, drop_num, length_num

    # Try SPEC CPU format: [num].[program]-[id]B
    spec_pattern = r'(\d{3}\.[a-zA-Z0-9_]+)-(\d+)B'
    spec_match = re.match(spec_pattern, name)

    if spec_match:
        workload_key = spec_match.group(1)  # e.g., "619.lbm_s"
        trace_id = int(spec_match.group(2))  # e.g., 2676
        instruction_label = f"{spec_match.group(2)}B"  # e.g., "2676B"
        # For SPEC CPU, use trace_id for both sorting dimensions
        return workload_key, instruction_label, trace_id, trace_id

    # Fallback if format doesn't match
    return name, name, 0, 0


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

def generate_speedup_per_benchmark_pdf(df, output_filename="speedup_by_workload.pdf", baseline='dram_only'):
    """Generates a multi-page PDF showing per-benchmark normalized IPC."""
    print(f"\n--- Generating PDF 1: Per-Benchmark Speedup vs {baseline} ---")

    # Extract workload info: workload_key for grouping, instruction_label for x-axis
    df[['workload_key', 'instruction_label', 'drop_num', 'length_num']] = df['benchmark'].apply(
        lambda x: pd.Series(extract_workload_info(x))
    )

    baseline_ipc = df[df['config'] == baseline].set_index('benchmark')['ipc']
    df['normalized_ipc'] = df.apply(lambda row: row['ipc'] / baseline_ipc.get(row['benchmark'], 1), axis=1)

    with PdfPages(output_filename) as pdf:
        # Group by workload_key so same workload appears on same page
        for workload_name, workload_df in df.groupby('workload_key'):
            print(f"  Plotting page for: {workload_name}")

            # Sort by instruction counts for consistent ordering
            workload_df = workload_df.sort_values(['drop_num', 'length_num'])

            plt.figure(figsize=(16, 9))

            # Use instruction_label for X-axis
            ax = sns.barplot(data=workload_df, x='instruction_label', y='normalized_ipc', hue='config', hue_order=CONFIG_ORDER, palette=SOPHISTICATED_COLORS)

            ax.set_title(f'Normalized Performance for Workload: {workload_name}', fontsize=16)
            ax.set_xlabel('Trace Segment', fontsize=12)
            ax.set_ylabel(f'Performance (Normalized to {baseline})', fontsize=12)
            # Use 45-degree rotation for shorter labels
            ax.tick_params(axis='x', rotation=45, labelsize=9)
            plt.setp(ax.get_xticklabels(), ha='right')
            ax.axhline(1.0, color='black', linestyle='--', linewidth=1.2)
            ax.legend(title='Configuration', bbox_to_anchor=(1.05, 1), loc='upper left')

            # Add value labels on top of each bar
            for container in ax.containers:
                ax.bar_label(container, fmt='%.2f', fontsize=8, fontweight='bold')

            plt.tight_layout(rect=[0, 0, 0.85, 0.95]) # Adjust right margin for legend
            pdf.savefig()
            plt.close()

    print(f"Successfully saved {output_filename}")


def generate_breakdown_per_benchmark_pdf(df, output_filename="memory_breakdown_by_workload.pdf"):
    """Generates a multi-page PDF showing memory read access breakdown for hybrid configurations (writes excluded)."""
    print(f"\n--- Generating PDF 2: Per-Benchmark Read Access Breakdown ---")

    hybrid_df = df[df['config'].str.contains('hybrid', case=False)].copy()
    if hybrid_df.empty:
        print("Warning: No hybrid configurations found. Skipping breakdown PDF.")
        return

    # Extract workload info: workload_key for grouping, instruction_label for x-axis
    hybrid_df[['workload_key', 'instruction_label', 'drop_num', 'length_num']] = hybrid_df['benchmark'].apply(
        lambda x: pd.Series(extract_workload_info(x))
    )

    # Only count read accesses for breakdown analysis (writes are not counted)
    hybrid_df['dram_accesses'] = hybrid_df['dram_reads']
    hybrid_df['cxl_accesses'] = hybrid_df['cxl_reads']

    with PdfPages(output_filename) as pdf:
        # Group by workload_key so same [program]_[algorithm].[dataset].[compiler]_[opt] appears on same page
        for workload_name, workload_df in hybrid_df.groupby('workload_key'):
            print(f"  Plotting page for: {workload_name}")

            fig, axes = plt.subplots(1, 3, figsize=(20, 8), sharey=True)
            fig.suptitle(f'Memory Read Access Breakdown for Workload: {workload_name}', fontsize=18)

            hybrid_configs = ['hybrid-hotness-access', 'hybrid-hotness-criticality', 'hybrid-roundrobin']

            for i, config_name in enumerate(hybrid_configs):
                if config_name not in workload_df['config'].values:
                    continue # Skip if this config doesn't exist for this workload

                config_data = workload_df[workload_df['config'] == config_name][['benchmark', 'instruction_label', 'drop_num', 'length_num', 'dram_accesses', 'cxl_accesses']].copy()

                # Sort by instruction counts: first by drop_num, then by length_num
                config_data = config_data.sort_values(['drop_num', 'length_num'])

                # Set instruction_label as index
                config_data = config_data[['instruction_label', 'dram_accesses', 'cxl_accesses']].set_index('instruction_label')

                config_perc = config_data.div(config_data.sum(axis=1), axis=0) * 100

                config_perc.plot(kind='bar', stacked=True, ax=axes[i], color=HYBRID_BREAKDOWN_COLORS, legend=False)

                axes[i].set_title(config_name)
                axes[i].set_ylabel('Percentage of Off-Chip Read Accesses (%)' if i == 0 else '')
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
            fig.legend(handles, ['Read from DRAM', 'Read from CXL'], title='Memory Type', bbox_to_anchor=(0.5, 0.01), loc='lower center', ncol=2)

            # Use subplots_adjust to give more space at the bottom
            plt.subplots_adjust(bottom=0.25)
            pdf.savefig()
            plt.close()

    print(f"Successfully saved {output_filename}")

def generate_mpki_analysis_pdf(df, output_filename="mpki_analysis.pdf", baseline='dram_only', top_n=30):
    """
    Generates a multi-page PDF with comprehensive MPKI analysis:
    Page 1: Performance by memory intensity groups (MPKI-based grouping)
    Page 2: Top N memory-intensive traces with per-config MPKI breakdown
    """
    print(f"\n--- Generating PDF: MPKI Analysis ---")

    if 'dram_reads' not in df.columns or 'cxl_reads' not in df.columns:
        print("Warning: Required columns for MPKI calculation not found. Skipping MPKI analysis.")
        return

    with PdfPages(output_filename) as pdf:
        # ============================================================
        # Page 1: Performance by Memory Intensity Groups
        # ============================================================
        print("  Page 1: Performance grouped by memory intensity...")

        df_copy1 = df.copy()
        baseline_df = df_copy1[df_copy1['config'] == baseline].copy()
        baseline_df['llc_mpki'] = ((baseline_df['dram_reads'] + baseline_df['cxl_reads']) * 1000) / 100_000_000

        mpki_map = baseline_df.set_index('benchmark')['llc_mpki']
        df_copy1['llc_mpki'] = df_copy1['benchmark'].map(mpki_map)

        # Add "Very Low" category for MPKI < 1 to better distinguish cache-friendly workloads
        bins = [0, 1, 10, 50, float('inf')]
        labels = ['Very Low (MPKI < 1)', 'Low (1-10)', 'Medium (10-50)', 'High (MPKI >= 50)']
        df_copy1['mpki_group'] = pd.cut(df_copy1['llc_mpki'], bins=bins, labels=labels, right=False)

        baseline_ipc = df_copy1[df_copy1['config'] == baseline].set_index('benchmark')['ipc']
        df_copy1['normalized_ipc'] = df_copy1.apply(lambda row: row['ipc'] / baseline_ipc.get(row['benchmark'], 1), axis=1)

        grouped_perf = df_copy1.groupby(['mpki_group', 'config'], observed=False)['normalized_ipc'].apply(gmean).reset_index()

        fig1, ax1 = plt.subplots(figsize=(14, 8))
        sns.barplot(data=grouped_perf, x='mpki_group', y='normalized_ipc', hue='config',
                   hue_order=CONFIG_ORDER, palette=SOPHISTICATED_COLORS, ax=ax1)

        ax1.set_title('Performance by Workload Memory Intensity\n(Grouped by LLC MPKI, Geometric Mean of Normalized IPC)',
                     fontsize=18, fontweight='bold')
        ax1.set_xlabel('Workload Type (Grouped by LLC MPKI)', fontsize=14)
        ax1.set_ylabel(f'Normalized Performance (to {baseline})', fontsize=14)
        ax1.axhline(1.0, color='black', linestyle='--', linewidth=1.2)
        ax1.legend(title='Configuration')

        # Add value labels on top of each bar
        for container in ax1.containers:
            ax1.bar_label(container, fmt='%.3f', fontsize=9, fontweight='bold')

        plt.tight_layout()
        pdf.savefig(fig1)
        plt.close(fig1)

        # ============================================================
        # Page 2: Top N Memory-Intensive Traces
        # ============================================================
        print(f"  Page 2: Top {top_n} memory-intensive traces...")

        df_copy2 = df.copy()
        df_copy2['llc_mpki'] = ((df_copy2['dram_reads'] + df_copy2['cxl_reads']) * 1000) / 100_000_000

        # Calculate average MPKI across all configs for each trace
        avg_mpki = df_copy2.groupby('benchmark')['llc_mpki'].mean().sort_values(ascending=False)
        top_traces = avg_mpki.head(top_n).index.tolist()

        # Filter data for top traces and sort by average MPKI
        top_df = df_copy2[df_copy2['benchmark'].isin(top_traces)].copy()

        # Create unique labels that preserve ordering
        # Use shorten_benchmark_name but ensure uniqueness by keeping original order
        benchmark_to_label = {}
        for trace in top_traces:
            # Generate short label
            short = shorten_benchmark_name(trace)
            # Ensure uniqueness by appending counter if needed
            base_short = short
            counter = 1
            while short in benchmark_to_label.values():
                short = f"{base_short}_{counter}"
                counter += 1
            benchmark_to_label[trace] = short

        top_df['short_label'] = top_df['benchmark'].map(benchmark_to_label)

        # Create pivot table for plotting - now each row is unique
        pivot_df = top_df.pivot_table(index='short_label', columns='config', values='llc_mpki', aggfunc='mean')

        # Sort by the original top_traces order (which is sorted by avg MPKI descending)
        sorted_labels = [benchmark_to_label[trace] for trace in top_traces]
        pivot_df = pivot_df.reindex(sorted_labels)

        # Plot
        fig2, ax2 = plt.subplots(figsize=(18, 10))
        pivot_df.plot(kind='bar', ax=ax2, color=SOPHISTICATED_COLORS, width=0.8)

        ax2.set_title(f'Top {top_n} Memory-Intensive Traces\n(Ranked by Average MPKI Across All Configurations)',
                     fontsize=18, fontweight='bold')
        ax2.set_xlabel('Trace', fontsize=14)
        ax2.set_ylabel('LLC MPKI (Misses Per Kilo-Instruction)', fontsize=14)
        ax2.legend(title='Configuration', bbox_to_anchor=(1.02, 1), loc='upper left')
        ax2.tick_params(axis='x', rotation=45, labelsize=9)
        plt.setp(ax2.get_xticklabels(), ha='right')
        ax2.grid(axis='y', alpha=0.3)

        plt.tight_layout(rect=[0, 0, 0.88, 1])
        pdf.savefig(fig2)
        plt.close(fig2)

    print(f"Successfully saved {output_filename}")


def plot_top_mpki_traces(df, output_filename="top_mpki_traces.png", top_n=30):
    """
    Plots the top N traces by average MPKI across all configurations.
    Shows MPKI breakdown for each configuration.
    """
    print(f"\n--- Generating Plot: Top {top_n} Memory-Intensive Traces ---")

    if 'dram_reads' not in df.columns or 'cxl_reads' not in df.columns:
        print("Warning: Required columns for MPKI calculation not found. Skipping plot.")
        return

    # Calculate MPKI for each trace-config combination
    df_copy = df.copy()
    df_copy['llc_mpki'] = ((df_copy['dram_reads'] + df_copy['cxl_reads']) * 1000) / 100_000_000

    # Calculate average MPKI across all configs for each trace
    avg_mpki = df_copy.groupby('benchmark')['llc_mpki'].mean().sort_values(ascending=False)
    top_traces = avg_mpki.head(top_n).index.tolist()

    # Filter data for top traces
    top_df = df_copy[df_copy['benchmark'].isin(top_traces)].copy()

    # Create shortened labels for better readability
    top_df['short_label'] = top_df['benchmark'].apply(shorten_benchmark_name)

    # Create a mapping from short_label to average MPKI for sorting
    label_to_avg = {}
    for trace in top_traces:
        short = shorten_benchmark_name(trace)
        label_to_avg[short] = avg_mpki[trace]

    # Sort by average MPKI
    sorted_labels = sorted(label_to_avg.keys(), key=lambda x: label_to_avg[x], reverse=True)

    # Create pivot table for plotting
    pivot_df = top_df.pivot_table(index='short_label', columns='config', values='llc_mpki', aggfunc='mean')
    pivot_df = pivot_df.reindex(sorted_labels)  # Sort by average MPKI

    # Plot
    fig, ax = plt.subplots(figsize=(18, 10))
    pivot_df.plot(kind='bar', ax=ax, color=SOPHISTICATED_COLORS, width=0.8)

    ax.set_title(f'Top {top_n} Memory-Intensive Traces (by Average MPKI)', fontsize=18, fontweight='bold')
    ax.set_xlabel('Trace', fontsize=14)
    ax.set_ylabel('LLC MPKI (Misses Per Kilo-Instruction)', fontsize=14)
    ax.legend(title='Configuration', bbox_to_anchor=(1.02, 1), loc='upper left')
    ax.tick_params(axis='x', rotation=45, labelsize=9)
    plt.setp(ax.get_xticklabels(), ha='right')
    ax.grid(axis='y', alpha=0.3)

    plt.tight_layout(rect=[0, 0, 0.88, 1])
    plt.savefig(output_filename, dpi=300, bbox_inches='tight')
    print(f"Successfully saved {output_filename}")
    plt.close()


def plot_performance_by_mpki_groups(df, output_filename="performance_by_memory_intensity.png", baseline='dram_only'):
    """Groups benchmarks by LLC MPKI and plots the geometric mean of normalized IPC for each group."""
    print(f"\n--- Generating Summary Plot: Performance by Memory Intensity ---")

    if 'dram_reads' not in df.columns or 'cxl_reads' not in df.columns:
        print("Warning: Required columns for MPKI calculation not found. Skipping plot.")
        return

    baseline_df = df[df['config'] == baseline].copy()
    baseline_df['llc_mpki'] = ((baseline_df['dram_reads'] + baseline_df['cxl_reads']) * 1000) / 1_0000_0000

    mpki_map = baseline_df.set_index('benchmark')['llc_mpki']
    df['llc_mpki'] = df['benchmark'].map(mpki_map)

    # Add "Very Low" category for MPKI < 1 to better distinguish cache-friendly workloads
    bins = [0, 1, 10, 50, float('inf')]
    labels = ['Very Low (MPKI < 1)', 'Low (1-10)', 'Medium (10-50)', 'High (MPKI >= 50)']
    df['mpki_group'] = pd.cut(df['llc_mpki'], bins=bins, labels=labels, right=False)
    
    baseline_ipc = df[df['config'] == baseline].set_index('benchmark')['ipc']
    df['normalized_ipc'] = df.apply(lambda row: row['ipc'] / baseline_ipc.get(row['benchmark'], 1), axis=1)

    grouped_perf = df.groupby(['mpki_group', 'config'], observed=False)['normalized_ipc'].apply(gmean).reset_index()

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
        
        generate_speedup_per_benchmark_pdf(main_df, output_filename=os.path.join(output_dir, "speedup_by_workload.pdf"))
        generate_breakdown_per_benchmark_pdf(main_df, output_filename=os.path.join(output_dir, "memory_breakdown_by_workload.pdf"))
        generate_mpki_analysis_pdf(main_df, output_filename=os.path.join(output_dir, "mpki_analysis.pdf"), baseline='dram_only', top_n=30)

        print("\nAll plot generation tasks complete!")

    except FileNotFoundError as e:
        print(f"\nError: {e}")
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}")
