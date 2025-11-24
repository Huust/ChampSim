import os
import subprocess
import shutil

# Configuration: (config_name, heatmap_type, ratio, ratio_str, sort_by_criticality)
# Each config runs a two-phase process:
# Phase 1: champsim_dram_only generates heatmap
# Phase 2: champsim_tiered_memory_skip uses heatmap with different parameters
configs = [
    ('access_r1_1', 'access', '1:1', 'r1_1', False),
    ('access_r1_3', 'access', '1:3', 'r1_3', False),
    ('criticality_r1_1', 'criticality', '1:1', 'r1_1', True),
    ('criticality_r1_3', 'criticality', '1:3', 'r1_3', True),
]

def run_simple_champsim(path_to_traces, bin_dir, output_dir, heatmap_dir):
    """
    Submit two-phase ChampSim jobs (heatmap generation + simulation) via SLURM

    Args:
        path_to_traces: Directory containing trace files (.xz)
        bin_dir: Directory containing binary executables
        output_dir: Directory to store output files
        heatmap_dir: Directory to store heatmap files
    """
    # List all trace files
    trace_files = [f for f in os.listdir(path_to_traces) if f.endswith('.xz')]
    fail_count = 0

    print(f"Found {len(trace_files)} trace files in {path_to_traces}")
    print(f"Will submit {len(configs)} configs per trace (two-phase process)")
    print(f"Total jobs to submit: {len(trace_files) * len(configs)}")

    # Clean and create output directories for each configuration
    print(f"Cleaning output directories in {output_dir}...")
    for config_name, _, _, _, _ in configs:
        config_output_dir = os.path.join(output_dir, config_name)
        if os.path.exists(config_output_dir):
            shutil.rmtree(config_output_dir)
        os.makedirs(config_output_dir)

    # Clean heatmaps directory
    if os.path.exists(heatmap_dir):
        shutil.rmtree(heatmap_dir)
    os.makedirs(heatmap_dir)

    # Submit jobs for each trace and config combination
    for trace_file in trace_files:
        full_trace_path = os.path.join(path_to_traces, trace_file)
        # Remove .xz extension, keep rest of the name
        trace_name = os.path.splitext(trace_file)[0]

        for config_name, heatmap_type, ratio, ratio_str, sort_by_criticality in configs:
            # Paths for binaries
            dram_binary = os.path.join(bin_dir, 'champsim_dram_only')
            tiered_binary = os.path.join(bin_dir, 'champsim_tiered_memory_skip')

            # Heatmap file path - match run_pelle.sh naming: {trace}_{type}_{ratio}
            heatmap_file = os.path.join(heatmap_dir, f"{trace_name}_{heatmap_type}_{ratio_str}")

            # Output file naming and path
            config_output_dir = os.path.join(output_dir, config_name)
            output_name = f"{config_name}_{trace_name}"

            # Two-phase command:
            # Phase 1: Generate heatmap using champsim_dram_only
            phase1_cmd = f'{dram_binary} --generate-heatmap "{heatmap_file}" -w 50000000 -i 100000000 {full_trace_path}'

            # Phase 2: Use heatmap with champsim_tiered_memory_skip
            phase2_cmd = f'{tiered_binary} --use-heatmap "{heatmap_file}"'
            if sort_by_criticality:
                phase2_cmd += ' --sort-by-criticality'
            phase2_cmd += f' --ratio "{ratio}" -w 50000000 -i 100000000 {full_trace_path}'

            combined_cmd = f'{phase1_cmd} && {phase2_cmd}'

            # SLURM submission command
            cmd = [
                'sbatch',
                '--account=uppmax2025-2-337',  # Project account
                '--ntasks=1',                   # Number of cores
                '--time=24:00:00',              # Time limit
                f'--output={config_output_dir}/{output_name}.out',
                f'--error={config_output_dir}/{output_name}.err',
                '--wrap',
                combined_cmd
            ]

            # Submit the job
            try:
                result = subprocess.run(cmd, check=True, capture_output=True, text=True)
                print(f"Job submitted: {output_name}")
                print("  SLURM output:", result.stdout.strip())
            except subprocess.CalledProcessError as e:
                print(f"Failed to submit job: {output_name}")
                print("  Error message:", e.stderr)
                fail_count += 1

    total_jobs = len(trace_files) * len(configs)
    success_jobs = total_jobs - fail_count
    print(f"\nSummary: {success_jobs}/{total_jobs} jobs submitted successfully, {fail_count} failed")


# Configuration
path_to_traces = '/proj/uart_chp_cxl_trans/champsim_traces'
bin_dir = '/proj/uart_chp_cxl_trans/songtao/ChampSim-dev/bin'
results_base_dir = '/proj/uart_chp_cxl_trans/songtao/results_skip'
output_dir = results_base_dir
heatmap_dir = os.path.join(results_base_dir, 'heatmaps')

run_simple_champsim(path_to_traces, bin_dir, output_dir, heatmap_dir)
