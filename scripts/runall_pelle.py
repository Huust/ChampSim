import os
import subprocess
import shutil

# Configuration matrix for our ChampSim project
# Memory system configurations:
# - cxl_only: CXL memory system
# - dram_only: Local DRAM memory system
# - interleaving: Tiered memory with interleaving allocation
# - access_r1_X: Access-count-based heatmap with ratio 1:X
# - criticality_r1_X: Criticality-based heatmap with ratio 1:X
configs_outputs = [
    # Single-phase configurations
    ('cxl_only', 'cxl_only'),
    ('dram_only', 'dram_only'),
    ('interleaving', 'interleaving'),

    # Heatmap-based configurations with ratio 1:1
    ('access_r1_1', 'access_r1_1'),
    ('criticality_r1_1', 'criticality_r1_1'),

    # Heatmap-based configurations with ratio 1:3
    ('access_r1_3', 'access_r1_3'),
    ('criticality_r1_3', 'criticality_r1_3'),
]


def run_champsim(path_to_traces, champsim_run):
    """
    Submit ChampSim jobs for all configurations and traces via SLURM

    Args:
        path_to_traces: Directory containing trace files (.xz)
        champsim_run_script: Path to run_pelle.sh script
    """
    # List all trace files
    trace_files = [f for f in os.listdir(path_to_traces) if f.endswith('.xz')]
    fail_count = 0

    print(f"Found {len(trace_files)} trace files in {path_to_traces}")
    print(f"Will submit {len(configs_outputs)} configurations per trace")
    print(f"Total jobs to submit: {len(trace_files) * len(configs_outputs)}")

    # Clean and create output directories
    print(f"Cleaning output directories in {resultsdir}...")
    for conf, outputdir in configs_outputs:
        full_outputdir = os.path.join(resultsdir, outputdir)
        if os.path.exists(full_outputdir):
            shutil.rmtree(full_outputdir)
        os.makedirs(full_outputdir)

    # Clean heatmaps directory
    heatmap_dir = os.path.join(resultsdir, 'heatmaps')
    if os.path.exists(heatmap_dir):
        shutil.rmtree(heatmap_dir)
    os.makedirs(heatmap_dir)

    # Submit jobs for each trace and configuration combination
    for trace_file in trace_files:
        full_trace_path = os.path.join(path_to_traces, trace_file)
        # Clean trace name: remove extensions and .champsimtrace suffix
        trace_name = trace_file.replace('.champsimtrace.xz', '').replace('.xz', '')

        for conf, outputdir in configs_outputs:
            outputdir = os.path.join(resultsdir, outputdir)
            # No prefix needed - directory already indicates config
            output_filename = trace_name

            cmd = [
                'sbatch',
                f'--output={outputdir}/{output_filename}.out',
                f'--error={outputdir}/{output_filename}.err',
                champsim_run,
                conf,                           # configuration
                resultsdir,                     # results directory (used for storing heatmap collection in run_pelle.sh)
                full_trace_path,                # trace file path
            ]

            # Submit the job
            try:
                result = subprocess.run(cmd, check=True, capture_output=True, text=True)
                print(f"Job submitted: {conf}/{output_filename}")
                print("  SLURM output:", result.stdout.strip())
            except subprocess.CalledProcessError as e:
                print(f"Failed to submit job: {conf}/{output_filename}")
                print("  Error message:", e.stderr)
                fail_count += 1

    total_jobs = len(trace_files) * len(configs_outputs)
    success_jobs = total_jobs - fail_count
    print(f"\nSummary: {success_jobs}/{total_jobs} jobs submitted successfully, {fail_count} failed")


# Example usage
path_to_traces = '/proj/uart_chp_cxl_trans/champsim_traces'
csrunscript = '/proj/uart_chp_cxl_trans/songtao/ChampSim-dev/scripts/run_pelle.sh'
resultsdir = '/proj/uart_chp_cxl_trans/songtao/results'
run_champsim(path_to_traces, csrunscript)