#!/usr/bin/env python3
"""
ChampSim Job Submission Script with SLURM Dependencies

This script submits ChampSim simulation jobs with proper dependency management:
1. Generate heatmap/interleaving files first (prerequisite jobs)
2. Submit dependent jobs that use the generated files

Per trace dependency structure:
  dram_only (generate heatmap) → heatmap-based configs (access/criticality + ratio + if_skip)
  interleaving (generate interleaving) → interleaving_skip

Expected output directory structure:
  results/
  ├── heatmaps/                    # Shared heatmap files (one per trace)
  │   ├── trace1.heatmap
  │   └── trace2.heatmap
  ├── interleavings/               # Shared interleaving files (one per trace)
  │   ├── trace1.interleaving
  │   └── trace2.interleaving
  ├── dram_only/                   # DRAM-only + heatmap generation
  │   ├── stdout/
  │   │   ├── trace1.out
  │   │   └── trace2.out
  │   └── stderr/
  │       ├── trace1.err
  │       └── trace2.err
  ├── interleaving/                # Interleaving + map generation
  │   ├── stdout/
  │   └── stderr/
  ├── access_r1_1/                 # Access-based (1:1)
  │   ├── stdout/
  │   └── stderr/
  ├── access_r1_3/                 # Access-based (1:3)
  │   ├── stdout/
  │   └── stderr/
  ├── criticality_r1_1/            # Criticality-based (1:1)
  │   ├── stdout/
  │   └── stderr/
  ├── criticality_r1_3/            # Criticality-based (1:3)
  │   ├── stdout/
  │   └── stderr/
  ├── access_r1_1_skip/            # Access-based (1:1) + skip translation
  │   ├── stdout/
  │   └── stderr/
  ├── access_r1_3_skip/            # Access-based (1:3) + skip translation
  │   ├── stdout/
  │   └── stderr/
  ├── criticality_r1_1_skip/       # Criticality-based (1:1) + skip translation
  │   ├── stdout/
  │   └── stderr/
  ├── criticality_r1_3_skip/       # Criticality-based (1:3) + skip translation
  │   ├── stdout/
  │   └── stderr/
  └── interleaving_skip/           # Interleaving + skip translation
      ├── stdout/
      └── stderr/
"""

import os
import subprocess
import shutil
import re

# Configuration matrix
# Format: (config_name, output_dir, dependency_type)
# dependency_type: None (independent), 'heatmap' (depends on dram_only), 'interleaving' (depends on interleaving)
CONFIGS = [
    # Prerequisite jobs (generate files for dependent jobs)
    ('dram_only', 'dram_only', None),  # Generates heatmap
    ('interleaving', 'interleaving', None),  # Generates interleaving map

    # Heatmap-based configurations (depend on dram_only)
    ('access_r1_1', 'access_r1_1', 'heatmap'),
    ('access_r1_3', 'access_r1_3', 'heatmap'),
    ('criticality_r1_1', 'criticality_r1_1', 'heatmap'),
    ('criticality_r1_3', 'criticality_r1_3', 'heatmap'),

    # Skip-translation heatmap-based configurations (depend on dram_only)
    ('access_r1_1_skip', 'access_r1_1_skip', 'heatmap'),
    ('access_r1_3_skip', 'access_r1_3_skip', 'heatmap'),
    ('criticality_r1_1_skip', 'criticality_r1_1_skip', 'heatmap'),
    ('criticality_r1_3_skip', 'criticality_r1_3_skip', 'heatmap'),

    # Allocation-based skip configuration (depends on interleaving)
    ('interleaving_skip', 'interleaving_skip', 'interleaving'),
]


def extract_job_id(sbatch_output):
    """Extract SLURM job ID from sbatch output"""
    match = re.search(r'Submitted batch job (\d+)', sbatch_output)
    if match:
        return match.group(1)
    return None


def submit_job(config_name, output_dir, trace_path, trace_name, champsim_run, dependency_job_id=None):
    """
    Submit a SLURM job with optional dependency

    Args:
        config_name: Configuration name
        output_dir: Output directory for results
        trace_path: Full path to trace file
        trace_name: Clean trace name (without extensions)
        champsim_run: Path to run_pelle.sh script
        dependency_job_id: Optional job ID to depend on

    Returns:
        Job ID of submitted job, or None if submission failed
    """
    cmd = [
        'sbatch',
        f'--output={output_dir}/stdout/{trace_name}.out',
        f'--error={output_dir}/stderr/{trace_name}.err',
    ]

    # Add dependency if specified
    if dependency_job_id:
        cmd.append(f'--dependency=afterok:{dependency_job_id}')

    cmd.extend([
        champsim_run,
        config_name,
        resultsdir,
        trace_path,
    ])

    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        job_id = extract_job_id(result.stdout)

        dependency_str = f" (depends on {dependency_job_id})" if dependency_job_id else ""
        print(f"  ✓ {config_name}/{trace_name}: Job {job_id}{dependency_str}")

        return job_id
    except subprocess.CalledProcessError as e:
        print(f"  ✗ {config_name}/{trace_name}: FAILED")
        print(f"    Error: {e.stderr}")
        return None


def collect_trace_paths(trace_sources):
    """
    Collect trace file paths from multiple sources.

    Args:
        trace_sources: list of (directory, filter_fn) tuples.
            filter_fn takes a filename and returns True to include it.
            Use None to include all .xz files from that directory.

    Returns:
        Sorted list of full trace paths.
    """
    paths = []
    for directory, filter_fn in trace_sources:
        for f in os.listdir(directory):
            if not f.endswith('.xz'):
                continue
            if filter_fn is not None and not filter_fn(f):
                continue
            paths.append(os.path.join(directory, f))
    return sorted(paths)


def is_spec_trace(filename):
    """Return True if filename belongs to SPEC CPU 2006 or 2017 (starts with 4XX. or 6XX.)."""
    return bool(re.match(r'^[46]\d{2}\.', filename))


def run_champsim(trace_paths, champsim_run):
    """
    Submit ChampSim jobs for all configurations and traces with dependency management

    Args:
        trace_paths: List of full paths to trace files
        champsim_run: Path to run_pelle.sh script
    """
    print(f"=" * 80)
    print(f"ChampSim Job Submission")
    print(f"=" * 80)
    print(f"Results directory: {resultsdir}")
    print(f"Found {len(trace_paths)} trace files")
    print(f"Configurations: {len(CONFIGS)}")
    print(f"Total jobs: {len(trace_paths) * len(CONFIGS)}")
    print()

    # Clean and create output directories with stdout/stderr subdirectories
    print("Preparing output directories...")
    for _, output_dir, _ in CONFIGS:
        full_output_dir = os.path.join(resultsdir, output_dir)
        if os.path.exists(full_output_dir):
            shutil.rmtree(full_output_dir)

        # Create config dir with stdout and stderr subdirectories
        os.makedirs(os.path.join(full_output_dir, 'stdout'))
        os.makedirs(os.path.join(full_output_dir, 'stderr'))
        print(f"  Created: {output_dir}/ (stdout/, stderr/)")

    # Clean heatmaps and interleavings directories
    for dir_name in ['heatmaps', 'interleavings']:
        dir_path = os.path.join(resultsdir, dir_name)
        if os.path.exists(dir_path):
            shutil.rmtree(dir_path)
        os.makedirs(dir_path)
        print(f"  Created: {dir_name}/")

    print()

    # Submit jobs per trace with dependency tracking
    total_submitted = 0
    total_failed = 0

    for full_trace_path in trace_paths:
        trace_file = os.path.basename(full_trace_path)
        trace_name = trace_file.replace('.champsimtrace.xz', '').replace('.xz', '')

        print(f"Submitting jobs for trace: {trace_name}")

        # Track prerequisite job IDs for this trace
        dram_only_job_id = None
        interleaving_job_id = None

        for config_name, output_dir, dependency_type in CONFIGS:
            full_output_dir = os.path.join(resultsdir, output_dir)

            # Determine dependency
            dependency_job_id = None
            if dependency_type == 'heatmap':
                dependency_job_id = dram_only_job_id
            elif dependency_type == 'interleaving':
                dependency_job_id = interleaving_job_id

            # Submit job
            job_id = submit_job(
                config_name,
                full_output_dir,
                full_trace_path,
                trace_name,
                champsim_run,
                dependency_job_id
            )

            if job_id:
                total_submitted += 1

                # Track prerequisite job IDs
                if config_name == 'dram_only':
                    dram_only_job_id = job_id
                elif config_name == 'interleaving':
                    interleaving_job_id = job_id
            else:
                total_failed += 1

        print()

    # Summary
    print("=" * 80)
    print(f"Submission Summary:")
    print(f"  Total jobs submitted: {total_submitted}")
    print(f"  Failed submissions: {total_failed}")
    print(f"  Success rate: {total_submitted}/{total_submitted + total_failed}")
    print("=" * 80)


# Configuration
SPEC_TRACES_DIR = '/proj/uart_chp_cxl_trans/champsim_traces'
GAPBS_TRACES_DIR = '/crex/proj/uart_chp_cxl_trans/gapbs_traces'
resultsdir = '/proj/uart_chp_cxl_trans/songtao/results'
csrunscript = '/proj/uart_chp_cxl_trans/songtao/ChampSim-dev/scripts/run_pelle.sh'

if __name__ == '__main__':
    trace_paths = collect_trace_paths([
        (SPEC_TRACES_DIR, is_spec_trace),   # Only SPEC 2006/2017 from champsim_traces
        (GAPBS_TRACES_DIR, None),            # All traces from gapbs_traces
    ])
    run_champsim(trace_paths, csrunscript)
