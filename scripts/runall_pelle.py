import os
import subprocess

# Configuration matrix for our ChampSim project
# Three binary configurations based on shim_layer settings:
# - cxl_only: cxl=1, dram=0 (CXL-only memory system)
# - dram_only: cxl=0, dram=1 (DRAM-only memory system)
# - hybrid variants: cxl=1, dram=1 (with 2 different allocation policies)
configs_outputs = [
    # Single-phase configurations
    ('cxl_only', 'results_cxl_only'),
    ('dram_only', 'results_dram_only'),
    ('hybrid-roundrobin', 'results_hybrid_roundrobin'),

    # Two-phase heatmap-based configurations (both use hybrid binary)
    ('hybrid-hotness-access', 'results_hybrid_hotness_access'),
    ('hybrid-hotness-criticality', 'results_hybrid_hotness_criticality'),
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

    # Create output directories
    for conf, outputdir in configs_outputs:
        full_outputdir = os.path.join(resultsdir, outputdir)
        os.makedirs(full_outputdir, exist_ok=True)

    # Submit jobs for each trace and configuration combination
    for trace_file in trace_files:
        full_trace_path = os.path.join(path_to_traces, trace_file)
        trace_name = os.path.splitext(trace_file)[0]  # Remove .xz extension

        cmds = []

        for conf, outputdir in configs_outputs:
            outputdir = os.path.join(resultsdir, outputdir)
            conf_prefix = f"{conf}_{trace_name}"

            cmd = [
                'sbatch',
                f'--output={outputdir}/{conf_prefix}.out',
                f'--error={outputdir}/{conf_prefix}.err',
                champsim_run,
                conf,                           # configuration
                resultsdir,                     # results directory (used for storing heatmap collection in run_pelle.sh)
                full_trace_path,                # trace file path
            ]
            cmds.append(cmd)

        # Run the commands
        try:
            for cmd in cmds:
                result = subprocess.run(cmd, check=True, capture_output=True, text=True)
                print(f"Jobs for {trace_name} submitted successfully!")
                print("SLURM output:", result.stdout)
        except subprocess.CalledProcessError as e:
            print("Failed to submit some command.")
            print("Error message:", e.stderr)
            fail_count += 1
    print(f"{len(trace_files)} traces submitted successfully, {fail_count} submissions failed")


# Example usage
path_to_traces = '/proj/uart_chp_cxl_trans/champsim_traces'
csrunscript = '/proj/uart_chp_cxl_trans/songtao/ChampSim-dev/scripts/run_pelle.sh'
resultsdir = '/proj/uart_chp_cxl_trans/songtao/results'
run_champsim(path_to_traces, csrunscript)