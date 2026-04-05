#!/usr/bin/env python3
"""
Latency-parity pipeline for multi-page-size experiments.

Per trace (22 jobs):
  1) 2MB dram_only  → heatmap + physical mapping                        [no deps]
  2) 4KB dram_only  → 4KB heatmap + annotated heatmaps + policy files   [deps: #1]
  3) 2MB tiered ×4  (access/criticality × r1_1/r1_3)                    [deps: #1]
  4) 4KB tiered ×4                                                       [deps: #2]
  5) 4KB skip   ×4                                                       [deps: #2]
  6) perf tiered ×8 (simple/memtis × access/crit × r1_1/r1_3)          [deps: #2]
"""

import os
import re
import shutil
import subprocess
import sys
import time

CHAMPSIM_BASE = '/crex/proj/uart_chp_cxl_trans/songtao/ChampSim-multi-page-size'
RESULTS_BASE  = '/proj/uart_chp_cxl_trans/songtao/results_mps'

RUN_2MB      = f'{CHAMPSIM_BASE}/scripts/run_2mb.sh'
RUN_4KB      = f'{CHAMPSIM_BASE}/scripts/run_4kb.sh'
BIN_DRAM_ONLY  = f'{CHAMPSIM_BASE}/bin/champsim_dram_only'
BIN_TIERED     = f'{CHAMPSIM_BASE}/bin/champsim_tiered_memory'
BIN_SKIP       = f'{CHAMPSIM_BASE}/bin/champsim_tiered_memory_skip'
BIN_PERF       = f'{CHAMPSIM_BASE}/bin/champsim_perf_tiered'

RESULTS_4KB      = f'{RESULTS_BASE}/results'
RESULTS_2MB      = f'{RESULTS_BASE}/results_2mb'
RESULTS_4KB_SKIP = f'{RESULTS_BASE}/results_skip'
RESULTS_PERF     = f'{RESULTS_BASE}/results_perf'

SPEC_DIR  = '/crex/proj/uart_chp_cxl_trans/champsim_traces'
GAPBS_DIR = '/crex/proj/uart_chp_cxl_trans/gapbs_traces'

TIERED_CONFIGS = ['access_r1_1', 'access_r1_3', 'criticality_r1_1', 'criticality_r1_3']
PERF_CONFIGS   = [
    'pemtis_access_r1_1', 'pemtis_access_r1_3',
    'pemtis_criticality_r1_1', 'pemtis_criticality_r1_3',
    'memtis_access_r1_1', 'memtis_access_r1_3',
    'memtis_criticality_r1_1', 'memtis_criticality_r1_3',
]

SBATCH_TIMEOUT = 20
SBATCH_RETRIES = 5
SBATCH_SLEEP   = 2

GOOGLE_DIR = '/crex/proj/uart_chp_cxl_trans/DPC-4-Google_traces'

TRACES_ALL = [
    # SPEC (15)
    '429.mcf-184B', '429.mcf-192B', '429.mcf-51B',
    '459.GemsFDTD-1169B',
    '471.omnetpp-188B',
    '473.astar-359B',
    '483.xalancbmk-127B',
    '605.mcf_s-1536B', '605.mcf_s-1644B', '605.mcf_s-782B', '605.mcf_s-994B',
    '620.omnetpp_s-141B', '620.omnetpp_s-874B',
    '623.xalancbmk_s-10B', '623.xalancbmk_s-592B',
    # GAPBS (11, excluding pr/pr_spmv/tc)
    'bc_uniform_28.drop_20B.length_250M',
    'bfs_bu_kronecker_28.drop_11B.length_250M', 'bfs_bu_uniform_28.drop_30B.length_250M',
    'bfs_td_kronecker_28.drop_16.5B.length_200M', 'bfs_td_uniform_28.drop_35B.length_250M',
    'cc_kronecker_28.drop_4B.length_250M', 'cc_sv_kronecker_28.drop_30B.length_250M',
    'cc_sv_uniform_28.drop_30B.length_250M', 'cc_uniform_28.drop_6B.length_250M',
    'sssp_kronecker_28.drop_25B.length_250M', 'sssp_uniform_28.drop_40B.length_250M',
    # Google (41)
    'arizona_0002.champsim-015',
    'charlie_0000.champsim-028', 'charlie_0001.champsim', 'charlie_0002.champsim-026',
    'charlie_0003.champsim-025', 'charlie_0004.champsim',
    'merced_0000.champsim-035', 'merced_0001.champsim-032', 'merced_0002.champsim-031',
    'merced_0003.champsim-030', 'merced_0004.champsim-029',
    'sierra.a.3_0000.champsim-043', 'sierra.a.3_0001.champsim-042', 'sierra.a.3_0002.champsim-041',
    'sierra.a.3_0003.champsim-040', 'sierra.a.3_0004.champsim-038',
    'sierra.a.4_0000.champsim-010', 'sierra.a.4_0001.champsim-006', 'sierra.a.4_0002.champsim-007',
    'sierra.a.4_0003.champsim-005', 'sierra.a.4_0004.champsim-003',
    'sierra.a.6_0000.champsim-039', 'sierra.a.6_0001.champsim-037', 'sierra.a.6_0002.champsim-036',
    'sierra.a.6_0003.champsim-034', 'sierra.a.6_0004.champsim-033',
    'tahoe_0000.champsim-023', 'tahoe_0001.champsim-022', 'tahoe_0002.champsim-019',
    'tahoe_0003.champsim-020', 'tahoe_0004.champsim-021',
    'tango_0000.champsim-018', 'tango_0001.champsim-017', 'tango_0002.champsim-016',
    'tango_0003.champsim-012', 'tango_0004.champsim-013',
    'yankee_0000.champsim-009', 'yankee_0001.champsim-004', 'yankee_0002.champsim-008',
    'yankee_0003.champsim-002', 'yankee_0004.champsim-001',
]

TRACES = TRACES_ALL


def find_trace(name):
    for d in [SPEC_DIR, GAPBS_DIR, GOOGLE_DIR]:
        for ext in ['.champsimtrace.xz', '.xz', '.gz', '.champsim.gz']:
            p = os.path.join(d, name + ext)
            if os.path.isfile(p):
                return p
    # Google traces have complex naming: try matching prefix
    if os.path.isdir(GOOGLE_DIR):
        for f in os.listdir(GOOGLE_DIR):
            if f.startswith(name):
                return os.path.join(GOOGLE_DIR, f)
    return None


def trace_short(name):
    # Google traces: strip .champsim[-NNN] suffix
    name = re.sub(r'\.champsim(-\d+)?$', '', name)
    # GAPBS traces: strip .drop_*B.length_*M suffix for display but keep full name for files
    return name


def extract_job_id(out):
    m = re.search(r'(\d+)', out)
    return m.group(1) if m else None


def sbatch(run_script, config, results_dir, output_dir, trace_path, trace_name,
           deps=None, env_vars=None):
    cmd = ['sbatch', '--parsable',
           f'--output={output_dir}/stdout/{trace_name}.out',
           f'--error={output_dir}/stderr/{trace_name}.err']
    dep_ids = [str(x) for x in (deps or []) if x]
    if dep_ids:
        cmd.append(f'--dependency=afterok:{":".join(dep_ids)}')
    if env_vars:
        items = [f'{k}={v}' for k, v in sorted(env_vars.items())]
        cmd.append(f'--export=ALL,{",".join(items)}')
    cmd.extend([run_script, config, results_dir, trace_path])

    for attempt in range(1, SBATCH_RETRIES + 1):
        try:
            r = subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=SBATCH_TIMEOUT)
            jid = extract_job_id(r.stdout)
            dep_str = f' (deps: {":".join(dep_ids)})' if dep_ids else ''
            print(f'  + {config}/{trace_name}: Job {jid}{dep_str}')
            return jid
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
            detail = getattr(e, 'stderr', '') or getattr(e, 'stdout', '') or str(e)
            print(f'  ! {config}/{trace_name}: attempt {attempt}: {detail.strip()[:80]}')
            if attempt < SBATCH_RETRIES:
                time.sleep(SBATCH_SLEEP)

    print(f'  x {config}/{trace_name}: FAILED')
    return None


def prepare_dir(path):
    if os.path.exists(path):
        shutil.rmtree(path)
    os.makedirs(os.path.join(path, 'stdout'))
    os.makedirs(os.path.join(path, 'stderr'))


def prepare_dirs():
    for cfg in ['dram_only'] + TIERED_CONFIGS:
        for base in [RESULTS_4KB, RESULTS_2MB]:
            prepare_dir(os.path.join(base, cfg))
    for cfg in TIERED_CONFIGS:
        prepare_dir(os.path.join(RESULTS_4KB_SKIP, cfg))
    for cfg in PERF_CONFIGS:
        prepare_dir(os.path.join(RESULTS_PERF, cfg))
    for special in [
        os.path.join(RESULTS_4KB, 'heatmaps'),
        os.path.join(RESULTS_4KB, 'policies'),
        os.path.join(RESULTS_2MB, 'heatmaps'),
        os.path.join(RESULTS_2MB, 'physical_mappings'),
    ]:
        if os.path.exists(special):
            shutil.rmtree(special)
        os.makedirs(special)
    print('Directories prepared.')


def submit_for_trace(trace_path):
    fname = os.path.basename(trace_path)
    for suffix in ['.champsimtrace.xz', '.champsimtrace.gz', '.xz', '.gz']:
        if fname.endswith(suffix):
            fname = fname[:-len(suffix)]
            break
    tname = trace_short(fname)
    print(f'\n--- {tname} ---')

    submitted = failed = 0
    heatmap_dir_4kb = os.path.join(RESULTS_4KB, 'heatmaps')
    heatmap_dir_2mb = os.path.join(RESULTS_2MB, 'heatmaps')
    physmap_dir     = os.path.join(RESULTS_2MB, 'physical_mappings')
    policy_dir      = os.path.join(RESULTS_4KB, 'policies')

    # ── Step 1: 2MB dram-only ──
    j_2mb = sbatch(RUN_2MB, 'dram_only', RESULTS_2MB,
                   os.path.join(RESULTS_2MB, 'dram_only'),
                   trace_path, tname,
                   env_vars={'PHYSICAL_MAPPING_DIR': physmap_dir})
    if j_2mb: submitted += 1
    else:     failed += 1; return submitted, failed

    # ── Step 2: 4KB dram-only (deps: 2MB dram-only) ──
    j_4kb = sbatch(RUN_4KB, 'dram_only', RESULTS_4KB,
                   os.path.join(RESULTS_4KB, 'dram_only'),
                   trace_path, tname,
                   deps=[j_2mb],
                   env_vars={'PHYSICAL_MAPPING_DIR': physmap_dir,
                             'HEATMAP_DIR_2MB': heatmap_dir_2mb})
    if j_4kb: submitted += 1
    else:     failed += 1

    # ── Step 3: 2MB tiered ×4 (deps: 2MB dram-only) ──
    # Track all job IDs — each pemtis config needs its matching 2mb-tiered LLC latency
    j_2mb_tiered = {}  # cfg → job_id
    for cfg in TIERED_CONFIGS:
        j = sbatch(RUN_2MB, cfg, RESULTS_2MB,
                   os.path.join(RESULTS_2MB, cfg),
                   trace_path, tname,
                   deps=[j_2mb],
                   env_vars={'SHARED_HEATMAP_DIR': heatmap_dir_2mb,
                             'PHYSICAL_MAPPING_DIR': physmap_dir})
        if j: submitted += 1
        else: failed += 1
        j_2mb_tiered[cfg] = j

    if not j_4kb:
        print(f'  ! Skip 4KB tiered/skip/perf: 4KB dram-only failed')
        return submitted, failed + len(TIERED_CONFIGS) * 2 + len(PERF_CONFIGS)  # 4+4+8=16

    # ── Step 4: 4KB tiered ×4 (deps: 4KB dram-only) ──
    for cfg in TIERED_CONFIGS:
        j = sbatch(RUN_4KB, cfg, RESULTS_4KB,
                   os.path.join(RESULTS_4KB, cfg),
                   trace_path, tname,
                   deps=[j_4kb],
                   env_vars={'SHARED_HEATMAP_DIR': heatmap_dir_4kb,
                             'PHYSICAL_MAPPING_DIR': physmap_dir})
        if j: submitted += 1
        else: failed += 1

    # ── Step 5: 4KB skip ×4 (deps: 4KB dram-only) ──
    for cfg in TIERED_CONFIGS:
        j = sbatch(RUN_4KB, cfg, RESULTS_4KB_SKIP,
                   os.path.join(RESULTS_4KB_SKIP, cfg),
                   trace_path, tname,
                   deps=[j_4kb],
                   env_vars={'SHARED_HEATMAP_DIR': heatmap_dir_4kb,
                             'PHYSICAL_MAPPING_DIR': physmap_dir,
                             'TIERED_BINARY': BIN_SKIP})
        if j: submitted += 1
        else: failed += 1

    # ── Step 6a: memtis perf ×4 (deps: 4KB dram-only for policy files) ──
    memtis_configs = [c for c in PERF_CONFIGS if c.startswith('memtis_')]
    for perf_cfg in memtis_configs:
        policy_file = os.path.join(policy_dir, f'{tname}.{perf_cfg}.policy')
        physmap_file = os.path.join(physmap_dir, f'{tname}.pmap')
        wrapper = os.path.join(policy_dir, f'{tname}.{perf_cfg}.sh')
        with open(wrapper, 'w') as f:
            f.write(f"""#!/bin/bash
#SBATCH --account=uppmax2025-2-337
#SBATCH --ntasks=1
#SBATCH --time=05:00:00
if [[ ! -f "{policy_file}" ]]; then echo "Missing policy: {policy_file}"; exit 1; fi
if [[ ! -f "{physmap_file}" ]]; then echo "Missing physmap: {physmap_file}"; exit 1; fi
echo "=== perf_tiered {perf_cfg} ==="
"{BIN_PERF}" -w 50000000 -i 100000000 --page-mode mixed \\
    --use-pmap "{physmap_file}" \\
    --use-policy "{policy_file}" \\
    "{trace_path}" 2>&1
""")
        j = sbatch(wrapper, f'perf_{perf_cfg}', RESULTS_PERF,
                   os.path.join(RESULTS_PERF, perf_cfg),
                   trace_path, tname,
                   deps=[j_4kb])
        if j: submitted += 1
        else: failed += 1

    # ── Step 6b: pemtis perf ×4 (deps: 4KB dram-only + matching 2MB tiered) ──
    # Each pemtis config uses LLC latency from its corresponding 2mb-tiered config:
    #   pemtis_access_r1_1       → 2mb-tiered access_r1_1
    #   pemtis_access_r1_3       → 2mb-tiered access_r1_3
    #   pemtis_criticality_r1_1  → 2mb-tiered criticality_r1_1
    #   pemtis_criticality_r1_3  → 2mb-tiered criticality_r1_3
    pemtis_configs = [c for c in PERF_CONFIGS if c.startswith('pemtis_')]
    annotated_2mb_heatmap = os.path.join(heatmap_dir_2mb, f'{tname}.heatmap')
    for perf_cfg in pemtis_configs:
        # e.g. perf_cfg = "pemtis_access_r1_1" → tiered_cfg = "access_r1_1"
        tiered_cfg = perf_cfg[len('pemtis_'):]  # strip "pemtis_" prefix
        ratio_suffix = perf_cfg.split('_')[-2] + '_' + perf_cfg.split('_')[-1]  # "r1_1"
        ratio = ratio_suffix.replace('r', '').replace('_', ':')  # "1:1"
        metric = 'llc_miss' if 'access' in perf_cfg else 'criticality'
        tiered_ref_stdout = os.path.join(RESULTS_2MB, tiered_cfg, 'stdout', f'{tname}.out')
        j_2mb_dep = j_2mb_tiered.get(tiered_cfg)
        policy_file = os.path.join(policy_dir, f'{tname}.{perf_cfg}.policy')
        physmap_file = os.path.join(physmap_dir, f'{tname}.pmap')
        pmap_tmp = os.path.join(policy_dir, f'{tname}.{perf_cfg}.pmap.tmp')
        wrapper = os.path.join(policy_dir, f'{tname}.{perf_cfg}.sh')
        with open(wrapper, 'w') as f:
            f.write(f"""#!/bin/bash
#SBATCH --account=uppmax2025-2-337
#SBATCH --ntasks=1
#SBATCH --time=05:00:00
set -e
# Validate dependencies
if [[ ! -f "{annotated_2mb_heatmap}" ]]; then echo "Missing heatmap: {annotated_2mb_heatmap}"; exit 1; fi
if [[ ! -f "{tiered_ref_stdout}" ]]; then echo "Missing 2mb-tiered output: {tiered_ref_stdout}"; exit 1; fi
if [[ ! -f "{physmap_file}" ]]; then echo "Missing physmap: {physmap_file}"; exit 1; fi

# Step 1: Generate pemtis policy with LLC latency from 2mb-tiered ({tiered_cfg})
echo "=== Generating pemtis policy ({perf_cfg}) with tiered latency from {tiered_cfg} ==="
python3 "{CHAMPSIM_BASE}/scripts/policy/pemtis.py" "{annotated_2mb_heatmap}" "{pmap_tmp}" \\
    --metric {metric} --ratio {ratio} --tiered-output "{tiered_ref_stdout}" --verbose
python3 "{CHAMPSIM_BASE}/scripts/pmap_to_policy.py" "{pmap_tmp}" "{annotated_2mb_heatmap}" "{policy_file}"
rm -f "{pmap_tmp}"

# Step 2: Run perf simulation
echo "=== perf_tiered {perf_cfg} ==="
"{BIN_PERF}" -w 50000000 -i 100000000 --page-mode mixed \\
    --use-pmap "{physmap_file}" \\
    --use-policy "{policy_file}" \\
    "{trace_path}" 2>&1
""")
        j = sbatch(wrapper, f'perf_{perf_cfg}', RESULTS_PERF,
                   os.path.join(RESULTS_PERF, perf_cfg),
                   trace_path, tname,
                   deps=[j_4kb, j_2mb_dep])
        if j: submitted += 1
        else: failed += 1

    return submitted, failed


def main():
    print('=' * 70)
    print('Multi-Page-Size Latency-Parity Pipeline')
    print(f'Results: {RESULTS_BASE}')
    print(f'Traces:  {len(TRACES)}')
    print(f'Jobs/trace: 22')
    print('=' * 70)

    for b in [BIN_DRAM_ONLY, BIN_TIERED, BIN_SKIP, BIN_PERF]:
        if not os.path.isfile(b):
            print(f'ERROR: Binary not found: {b}')
            sys.exit(1)

    prepare_dirs()

    total_s = total_f = 0
    for trace_base in TRACES:
        tp = find_trace(trace_base)
        if not tp:
            print(f'\nSKIP {trace_base}: not found')
            total_f += 22
            continue
        s, f = submit_for_trace(tp)
        total_s += s
        total_f += f

    print(f'\n{"=" * 70}')
    print(f'Submitted: {total_s}  Failed: {total_f}')
    print(f'{"=" * 70}')


if __name__ == '__main__':
    main()
