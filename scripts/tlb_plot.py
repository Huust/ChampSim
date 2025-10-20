#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from scipy.stats import gmean
import os

# --- Configuration ---
sns.set_theme(style="whitegrid", palette="colorblind")
CONFIG_ORDER = ['dram_only', 'hybrid-hotness-criticality', 'hybrid-roundrobin', 'hybrid-hotness-access', 'cxl_only']
NUM_INSTRUCTIONS = 100_000_000  # Make sure this matches your simulation instructions

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

# --- Plotting Functions for TLB Analysis ---

def plot_tlb_mpki_distribution(df, top_n=20, output_dir='./plots/tlb'):
    """Plots a grouped horizontal bar chart for ITLB, DTLB and STLB MPKI."""
    print("\n--- Generating Plot 1: Workload TLB Pressure (ITLB, DTLB & STLB MPKI) ---")

    # Use dram_only as the representative run for workload characterization
    baseline_df = df[df['config'] == 'dram_only'].copy()

    # Calculate MPKIs
    baseline_df['itlb_mpki'] = (baseline_df['itlb_miss'] * 1000) / NUM_INSTRUCTIONS
    baseline_df['dtlb_mpki'] = (baseline_df['dtlb_miss'] * 1000) / NUM_INSTRUCTIONS
    baseline_df['stlb_mpki'] = (baseline_df['stlb_miss'] * 1000) / NUM_INSTRUCTIONS

    # Sort by STLB MPKI as it's more impactful
    sorted_df = baseline_df.sort_values('stlb_mpki', ascending=False).head(top_n)

    # Use pandas.melt to transform data for grouped bar plot
    plot_df = sorted_df.melt(id_vars='benchmark', value_vars=['itlb_mpki', 'dtlb_mpki', 'stlb_mpki'],
                             var_name='TLB Type', value_name='MPKI')
    plot_df['TLB Type'] = plot_df['TLB Type'].replace({'itlb_mpki': 'ITLB', 'dtlb_mpki': 'DTLB', 'stlb_mpki': 'STLB'})

    plt.figure(figsize=(12, 10))
    sns.barplot(data=plot_df, y='benchmark', x='MPKI', hue='TLB Type', orient='h')

    plt.title(f'Top {top_n} Benchmarks by TLB Pressure (MPKI)', fontsize=16)
    plt.xlabel('Misses Per Kilo-Instruction (MPKI)', fontsize=12)
    plt.ylabel('Benchmark', fontsize=12)
    plt.legend(title='TLB Level')

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'plot_tlb_mpki_distribution.png'), dpi=300)
    print(f"Saved plot_tlb_mpki_distribution.png")
    plt.close()

def plot_stlb_latency_across_configs(df, output_dir='./plots/tlb'):
    """Plots STLB miss latency across configurations, grouped by memory intensity."""
    print("\n--- Generating Plot 2: STLB Miss Latency vs. Configuration ---")

    # Use LLC MPKI to group benchmarks (requires LLC miss data)
    if 'llc_misses' not in df.columns:
        df['llc_misses'] = df['dram_reads'] + df['cxl_reads'] # Approximate LLC misses from shim stats
        
    baseline_df = df[df['config'] == 'dram_only'].copy()
    baseline_df['llc_mpki'] = (baseline_df['llc_misses'] * 1000) / NUM_INSTRUCTIONS
    mpki_map = baseline_df.set_index('benchmark')['llc_mpki']
    df['llc_mpki'] = df['benchmark'].map(mpki_map)

    bins = [0, 5, 20, float('inf')]
    labels = ['Low Intensity (MPKI < 5)', 'Medium Intensity (5-20)', 'High Intensity (MPKI >= 20)']
    df['mpki_group'] = pd.cut(df['llc_mpki'], bins=bins, labels=labels, right=False)
    
    # Calculate arithmetic mean of latency for each group
    grouped_latency = df.groupby(['mpki_group', 'config'], observed=True)['stlb_avg_miss_latency'].mean().reset_index()

    plt.figure(figsize=(14, 8))
    sns.barplot(data=grouped_latency, x='mpki_group', y='stlb_avg_miss_latency', hue='config', hue_order=CONFIG_ORDER)

    plt.title('STLB Average Miss Latency by Workload Intensity', fontsize=18)
    plt.xlabel('Workload Type (Grouped by LLC MPKI)', fontsize=14)
    plt.ylabel('STLB Average Miss Latency (Cycles)', fontsize=14)
    plt.legend(title='Configuration')
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'plot_stlb_latency_by_group.png'), dpi=300)
    print(f"Saved plot_stlb_latency_by_group.png")
    plt.close()

def plot_l1_tlb_comparison(df, output_dir='./plots/tlb'):
    """Compares ITLB vs DTLB miss rates across configurations."""
    print("\n--- Generating Plot 3: L1 TLB Comparison (ITLB vs DTLB) ---")

    # Calculate miss rates
    epsilon = 1e-9
    df['itlb_miss_rate'] = df['itlb_miss'] / (df['itlb_access'] + epsilon)
    df['dtlb_miss_rate'] = df['dtlb_miss'] / (df['dtlb_access'] + epsilon)

    # Calculate geometric mean per config
    gmean_rates = df.groupby('config')[['itlb_miss_rate', 'dtlb_miss_rate']].apply(
        lambda x: x.apply(lambda col: gmean(col + epsilon)))
    gmean_rates = gmean_rates.reindex(CONFIG_ORDER).dropna()

    # Create grouped bar chart
    ax = gmean_rates.plot(kind='bar', figsize=(12, 7), width=0.8)

    plt.title('L1 TLB Performance Comparison (ITLB vs DTLB)', fontsize=16)
    plt.xlabel('Configuration', fontsize=12)
    plt.ylabel('Geometric Mean of Miss Rate (Lower is Better)', fontsize=12)
    plt.xticks(rotation=45, ha='right')
    plt.yscale('log')
    plt.legend(title='L1 TLB Type', labels=['ITLB', 'DTLB'])
    plt.grid(True, which='both', linestyle='--', linewidth=0.5, alpha=0.7)

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'plot_l1_tlb_comparison.png'), dpi=300)
    print(f"Saved plot_l1_tlb_comparison.png")
    plt.close()

def plot_tlb_miss_rate_summary(df, output_dir='./plots/tlb'):
    """Plots the geometric mean of ITLB, DTLB and STLB miss rates for each configuration."""
    print("\n--- Generating Plot 4: Overall TLB Miss Rate Summary ---")

    # Calculate miss rates for each run. Add a small epsilon to avoid division by zero.
    epsilon = 1e-9
    df['itlb_miss_rate'] = df['itlb_miss'] / (df['itlb_access'] + epsilon)
    df['dtlb_miss_rate'] = df['dtlb_miss'] / (df['dtlb_access'] + epsilon)
    df['stlb_miss_rate'] = df['stlb_miss'] / (df['stlb_access'] + epsilon)

    # Calculate geometric mean of miss rates per config
    # We use a small offset for gmean if rates can be zero
    gmean_miss_rates = df.groupby('config')[['itlb_miss_rate', 'dtlb_miss_rate', 'stlb_miss_rate']].apply(lambda x: x.apply(lambda col: gmean(col + epsilon)))
    gmean_miss_rates = gmean_miss_rates.reindex(CONFIG_ORDER).dropna()

    gmean_miss_rates.plot(kind='bar', figsize=(12, 7))

    plt.title('Overall TLB Performance (Geometric Mean of Miss Rates)', fontsize=16)
    plt.xlabel('Configuration', fontsize=12)
    plt.ylabel('Geometric Mean of Miss Rate (Lower is Better)', fontsize=12)
    plt.xticks(rotation=45, ha='right')
    plt.yscale('log') # Miss rates can be very small, so a log scale is helpful
    plt.grid(True, which='minor', linestyle='--', linewidth=0.5)
    plt.legend(title='TLB Level', labels=['ITLB', 'DTLB', 'STLB'])

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'summary_tlb_miss_rates.png'), dpi=300)
    print(f"Saved summary_tlb_miss_rates.png")
    plt.close()

# --- Main Execution Block ---
if __name__ == "__main__":
    pkl_path = './results/collected_stats.pkl'
    csv_path = './results/collected_stats.csv'
    
    try:
        output_dir = './plots/tlb'
        os.makedirs(output_dir, exist_ok=True)
        
        main_df = load_data(pkl_path, csv_path)

        # Check if TLB columns exist before plotting
        required_cols = ['itlb_access', 'itlb_miss', 'dtlb_access', 'dtlb_miss',
                        'stlb_access', 'stlb_miss', 'stlb_avg_miss_latency']
        if not all(col in main_df.columns for col in required_cols):
            print("\nError: Not all required TLB columns were found in the data.")
            print("Missing columns:", [col for col in required_cols if col not in main_df.columns])
            print("Please ensure your collect_stats.py script is correctly parsing TLB data.")
        else:
            # Generate all TLB plots
            plot_tlb_mpki_distribution(main_df, output_dir=output_dir)
            plot_stlb_latency_across_configs(main_df, output_dir=output_dir)
            plot_l1_tlb_comparison(main_df, output_dir=output_dir)
            plot_tlb_miss_rate_summary(main_df, output_dir=output_dir)
            print("\nAll TLB plot generation tasks complete!")

    except FileNotFoundError as e:
        print(f"\nError: {e}")
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}")
