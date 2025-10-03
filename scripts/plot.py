import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from scipy.stats import gmean
import os

# Global variable
num_of_instructions = 1_0000_0000
plot_count = 1

# --- Configuration ---
# Set a consistent theme for all plots
sns.set_theme(style="whitegrid", palette="colorblind")
plt.rcParams['figure.figsize'] = (12, 7)    # Default figure size


# --- Data Loading ---
def load_data(pkl_path, csv_path):
    """Loads data from a Pickle file if it exists, otherwise from a CSV file."""
    if os.path.exists(pkl_path):
        df = pd.read_pickle(pkl_path)
    elif os.path.exists(csv_path):
        df = pd.read_csv(csv_path)
    else:
        raise FileNotFoundError(f"Neither {pkl_path} nor {csv_path} were found.")
    return df


# --- Plotting Functions ---
def plot_mpki_distribution(df, top_n=20):
    """Plots the LLC MPKI distribution for the DRAM-only configuration."""
    global plot_count

    # 1. Filter for DRAM-only config and calculate MPKI
    dram_only_df = df[df['config'] == 'dram_only'].copy()
    dram_only_df['llc_mpki'] = (dram_only_df['dram_reads'] * 1000) / num_of_instructions

    # 2. Sort by MPKI and get the top N
    sorted_df = dram_only_df.sort_values('llc_mpki', ascending=False).head(top_n)

    # 3. Plotting
    plt.figure()    # Create a new figure
    ax = sns.barplot(data=sorted_df, y='benchmark', x='llc_mpki', orient='h')
    plt.title(f'Top {top_n} Memory-Intensive Benchmarks (LLC MPKI)', fontsize=16)
    plt.xlabel('LLC Misses Per Kilo-Instruction (MPKI)', fontsize=12)
    plt.ylabel('Benchmark', fontsize=12)

    # Add value labels at the end of each bar
    for i, (idx, row) in enumerate(sorted_df.iterrows()):
        ax.text(row['llc_mpki'] + 0.5, i, f'{row["llc_mpki"]:.1f}', va='center', fontsize=9, fontweight='bold')

    plt.tight_layout()
    plt.savefig('./plots/mpki_distribution.png', dpi=300)
    print(f'Saved plot {plot_count}: mpki_distribution.png')
    plot_count += 1
    plt.close()


def plot_geomean_speedup(df, baseline='dram_only'):
    """Plots the geometric mean of normalized IPC for all configurations."""
    global plot_count

    # 1. Get the baseline IPCs for each benchmark
    baseline_ipc = df[df['config'] == baseline].set_index('benchmark')['ipc']
    # 2. Calculate normalized IPC (speedup) for each row
    df['normalized_ipc'] = df.apply(lambda row: row['ipc'] / baseline_ipc[row['benchmark']], axis=1)
    # 3. Calculate geometric mean for each configuration
    geomean_speedup = df.groupby('config')['normalized_ipc'].apply(gmean)
    geomean_speedup = geomean_speedup.sort_values(ascending=False)

    # 4. Plotting
    plt.figure()
    ax = geomean_speedup.plot(kind='bar', color=sns.color_palette("colorblind"))
    plt.title(f'Overall Performance (Normalized to {baseline})', fontsize=16)
    plt.xlabel('Configuration', fontsize=12)
    plt.ylabel('Geometric Mean of Speedup\n(Higher is Better)', fontsize=12)
    plt.xticks(rotation=45, ha='right')
    plt.axhline(1.0, color='black', linestyle='--', linewidth=1.0)  # Add baseline reference line

    # Add value labels on top of each bar
    for i, (config, value) in enumerate(geomean_speedup.items()):
        ax.text(i, value + 0.01, f'{value:.3f}', ha='center', va='bottom', fontsize=10, fontweight='bold')

    plt.tight_layout()
    plt.savefig('./plots/geomean_speedup.png', dpi=300)
    print(f'Saved plot {plot_count}: geomean_speedup.png')
    plot_count += 1
    plt.close()


def plot_access_breakdown(df):
    """Plots the memory access breakdown for hybrid configurations."""
    global plot_count

    # 1. Filter for hybrid configurations only
    hybrid_df = df[df['config'].str.contains('hybrid', case=False)].copy()
    if hybrid_df.empty:
        print("Warning: No hybrid configurations found. Skipping access breakdown plot.")
        return
    # 2. Calculate total accesses for each memory type
    hybrid_df['dram_accesses'] = hybrid_df['dram_reads'] + hybrid_df['dram_writes']
    hybrid_df['cxl_accesses'] = hybrid_df['cxl_reads'] + hybrid_df['cxl_writes']
    # 3. Aggregate totals for each config
    breakdown = hybrid_df.groupby('config')[['dram_accesses', 'cxl_accesses']].sum()
    # 4. Convert to percentages
    breakdown_perc = breakdown.div(breakdown.sum(axis=1), axis=0) * 100
    # 5. Plotting
    plt.figure()
    ax = breakdown_perc.plot(kind='bar', stacked=True, color=['#0173B2', '#DE8F05'])     # Blue for DRAM, Orange for CXL
    plt.title('Memory Access Breakdown for Hybrid Configurations', fontsize=16)
    plt.xlabel('Hybrid Configuration', fontsize=12)
    plt.ylabel('Percentage of Off-Chip Accesses (%)', fontsize=12)
    plt.xticks(rotation=45, ha='right')
    plt.legend(['Serviced by DRAM', 'Serviced by CXL'])

    # Add percentage labels in the middle of each segment
    for i, (config, row) in enumerate(breakdown_perc.iterrows()):
        dram_pct = row['dram_accesses']
        cxl_pct = row['cxl_accesses']
        # Add DRAM percentage label
        ax.text(i, dram_pct/2, f'{dram_pct:.1f}%', ha='center', va='center', fontsize=9, fontweight='bold', color='white')
        # Add CXL percentage label
        ax.text(i, dram_pct + cxl_pct/2, f'{cxl_pct:.1f}%', ha='center', va='center', fontsize=9, fontweight='bold', color='white')

    plt.tight_layout()
    plt.savefig('./plots/access_breakdown.png', dpi=300)
    print(f'Saved plot {plot_count}: access_breakdown.png')
    plot_count += 1
    plt.close()


# --- Main Execution ---
def plot(pkl_path='./results/collected_stats.pkl', csv_path='./results/collected_stats.csv'):
    try:
        # Create plots directory if it doesn't exist
        os.makedirs('./plots', exist_ok=True)

        # Load the master dataframe
        main_df = load_data(pkl_path, csv_path)

        # Generate all plots
        plot_mpki_distribution(main_df)
        plot_geomean_speedup(main_df, baseline='dram_only')
        plot_access_breakdown(main_df)

    except FileNotFoundError as e:
        print(f"Error: {e}")
        print("Please ensure your '.csv' or '.pkl' file is in right directory.")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")


pkl_path = './results/collected_stats.pkl'
csv_path = './results/collected_stats.csv'
plot(pkl_path, csv_path)
