#!/usr/bin/env python3
"""
full_res submission script.

Per Q1 workload (61 expected):
  Radix: 3*(2mb_tiered) + 3*(4kb_tiered) + 3*(skip) + 9*(memtis) + 18*(perf) = 36
  ECPT:  3*(2mb_tiered) + 3*(4kb_tiered) + 3*(skip) + 9*(memtis) +  6*(perf) = 24
  Total per workload: 60
  Grand total: 60 * 61 = 3660

Q1 filter: IPC(2mb_dram/4kb_dram) >= 1.05 AND IPC(4kb_skip/2mb_tiered) >= 1.05
"""

import os
import re
import subprocess
import sys
import shutil
import time

# ── Paths ──
CHAMPSIM_BASE = '/crex/proj/uart_chp_cxl_trans/songtao/ChampSim-multi-page-size'
SOURCE_DATA   = '/crex/proj/uart_chp_cxl_trans/songtao/full_res_4kbcap'
OUTPUT_BASE   = '/crex/proj/uart_chp_cxl_trans/songtao/full_res'

PERFORATION_PY = f'{CHAMPSIM_BASE}/scripts/policy/perforation.py'
PMAP_TO_POLICY = f'{CHAMPSIM_BASE}/scripts/pmap_to_policy.py'

# Binaries
BIN = {
    'radix_tiered':    f'{CHAMPSIM_BASE}/bin/champsim_tiered_memory',
    'radix_skip':      f'{CHAMPSIM_BASE}/bin/champsim_tiered_memory_skip',
    'radix_perf':      f'{CHAMPSIM_BASE}/bin/champsim_perf_tiered',
    'ecpt_tiered':     f'{CHAMPSIM_BASE}/bin/champsim_ecpt',
    'ecpt_skip':       f'{CHAMPSIM_BASE}/bin/champsim_ecpt_skip',
    'ecpt_perf':       f'{CHAMPSIM_BASE}/bin/champsim_ecpt_perf',
}

# Trace dirs
SPEC_DIR   = '/crex/proj/uart_chp_cxl_trans/champsim_traces'
GAPBS_DIR  = '/crex/proj/uart_chp_cxl_trans/gapbs_traces'
GOOGLE_DIR = '/crex/proj/uart_chp_cxl_trans/DPC-4-Google_traces'

# Simulation parameters
W = 50000000
I = 100000000
RATIOS = ['r1_3', 'r1_1', 'r2_1']
RATIO_MAP = {'r1_3': '1:3', 'r1_1': '1:1', 'r2_1': '2:1'}

# Shared data paths
SHARED            = f'{OUTPUT_BASE}/shared'
HM_2MB_DIR        = f'{SHARED}/heatmaps_2mb'
HM_4KB_CAP_DIR    = f'{SHARED}/heatmaps_4kb_cap'
HM_4KB_DIR        = f'{SHARED}/heatmaps_4kb'
PMAP_DIR          = f'{SHARED}/physical_mappings'

# SLURM config
SBATCH_TIMEOUT = 20
SBATCH_RETRIES = 5
SBATCH_SLEEP   = 2


# ═══════════════════════════════════════════════════════════════════
# Utility functions
# ═══════════════════════════════════════════════════════════════════

def extract_ipc(filepath):
    """Extract simulation IPC from a ChampSim output file."""
    if not os.path.isfile(filepath):
        return None
    try:
        with open(filepath) as f:
            for line in f:
                if 'CPU 0 cumulative IPC:' in line:
                    parts = line.split()
                    idx = parts.index('IPC:') + 1
                    return float(parts[idx])
    except Exception:
        pass
    return None


def find_q1_workloads():
    """Identify Q1 workloads: 2mb_dram/4kb_dram >= 1.05 AND 4kb_skip/2mb_tiered >= 1.05."""
    dir_2mb_dram = f'{SOURCE_DATA}/2mb/dram_only/stdout'
    dir_4kb_dram = f'{SOURCE_DATA}/4kb/dram_only/stdout'
    dir_2mb_tier = f'{SOURCE_DATA}/2mb/access_r1_3/stdout'
    dir_4kb_skip = f'{SOURCE_DATA}/skip/access_r1_3/stdout'

    all_wl = set()
    if os.path.isdir(dir_2mb_dram):
        for f in os.listdir(dir_2mb_dram):
            if f.endswith('.out'):
                all_wl.add(f[:-4])

    q1 = []
    for wl in sorted(all_wl):
        ipc_2mb_dram = extract_ipc(os.path.join(dir_2mb_dram, f'{wl}.out'))
        ipc_4kb_dram = extract_ipc(os.path.join(dir_4kb_dram, f'{wl}.out'))
        ipc_2mb_tier = extract_ipc(os.path.join(dir_2mb_tier, f'{wl}.out'))
        ipc_4kb_skip = extract_ipc(os.path.join(dir_4kb_skip, f'{wl}.out'))

        if any(x is None for x in [ipc_2mb_dram, ipc_4kb_dram, ipc_2mb_tier, ipc_4kb_skip]):
            continue
        if ipc_4kb_dram == 0 or ipc_2mb_tier == 0:
            continue

        ratio1 = ipc_2mb_dram / ipc_4kb_dram
        ratio2 = ipc_4kb_skip / ipc_2mb_tier

        if ratio1 >= 1.05 and ratio2 >= 1.05:
            q1.append(wl)

    return q1


def find_trace(name):
    """Find the trace file for a given workload name."""
    for d in [SPEC_DIR, GAPBS_DIR, GOOGLE_DIR]:
        for ext in ['.champsimtrace.xz', '.xz', '.gz', '.champsim.gz']:
            p = os.path.join(d, name + ext)
            if os.path.isfile(p):
                return p
    # Google traces: prefix match
    if os.path.isdir(GOOGLE_DIR):
        for f in sorted(os.listdir(GOOGLE_DIR)):
            if f.startswith(name):
                return os.path.join(GOOGLE_DIR, f)
    return None


def extract_job_id(out):
    m = re.search(r'(\d+)', out)
    return m.group(1) if m else None


def job_done(stdout_path):
    """Check if stdout file exists and contains the completion marker."""
    if not os.path.isfile(stdout_path):
        return False
    try:
        with open(stdout_path) as f:
            for line in f:
                if 'CPU 0 cumulative IPC' in line:
                    return True
    except Exception:
        pass
    return False


def sbatch_script(script_path, stdout_path, stderr_path, job_name=None):
    """Submit a wrapper script via sbatch."""
    cmd = ['sbatch', '--parsable',
           f'--output={stdout_path}',
           f'--error={stderr_path}',
           '--account=uppmax2025-2-337',
           '--ntasks=1',
           '--time=05:00:00']
    if job_name:
        cmd.append(f'--job-name={job_name}')
    cmd.append(script_path)

    for attempt in range(1, SBATCH_RETRIES + 1):
        try:
            r = subprocess.run(cmd, check=True, capture_output=True, text=True,
                               timeout=SBATCH_TIMEOUT)
            jid = extract_job_id(r.stdout)
            return jid
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
            detail = getattr(e, 'stderr', '') or getattr(e, 'stdout', '') or str(e)
            print(f'      ! attempt {attempt}: {detail.strip()[:80]}')
            if attempt < SBATCH_RETRIES:
                time.sleep(SBATCH_SLEEP)
    return None


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


# ═══════════════════════════════════════════════════════════════════
# Directory creation
# ═══════════════════════════════════════════════════════════════════

def create_all_dirs():
    """Create the complete output directory tree."""
    # Shared
    for d in [HM_2MB_DIR, HM_4KB_CAP_DIR, HM_4KB_DIR, PMAP_DIR]:
        ensure_dir(d)

    for ptw in ['radix', 'ecpt']:
        base = f'{OUTPUT_BASE}/{ptw}'

        # A/B: tiered
        for page in ['2mb_tiered', '4kb_tiered']:
            for r in RATIOS:
                ensure_dir(f'{base}/{page}/{r}/stdout')
                ensure_dir(f'{base}/{page}/{r}/stderr')

        # C: skip
        for r in RATIOS:
            ensure_dir(f'{base}/skip/{r}/stdout')
            ensure_dir(f'{base}/skip/{r}/stderr')

        # D: memtis variants
        for variant in ['follow', 'split33', 'split67']:
            for r in RATIOS:
                ensure_dir(f'{base}/memtis/{variant}/{r}/stdout')
                ensure_dir(f'{base}/memtis/{variant}/{r}/stderr')

        # E/F: perforation
        if ptw == 'radix':
            for perf_base in ['follow', 'full2mb']:
                for theta in ['none', 't33', 't67']:
                    for r in RATIOS:
                        ensure_dir(f'{base}/perf/{perf_base}/{theta}/{r}/stdout')
                        ensure_dir(f'{base}/perf/{perf_base}/{theta}/{r}/stderr')
        else:  # ecpt: theta=none only
            for perf_base in ['follow', 'full2mb']:
                for r in RATIOS:
                    ensure_dir(f'{base}/perf/{perf_base}/none/{r}/stdout')
                    ensure_dir(f'{base}/perf/{perf_base}/none/{r}/stderr')

        # Policies and job scripts
        ensure_dir(f'{base}/policies')
        ensure_dir(f'{base}/job_scripts')

    print('All directories created.')


# ═══════════════════════════════════════════════════════════════════
# Copy shared data
# ═══════════════════════════════════════════════════════════════════

def copy_shared_data(q1_workloads):
    """Copy heatmaps, pmaps from SOURCE_DATA to OUTPUT_BASE/shared/."""
    copied = 0
    missing = 0

    for wl in q1_workloads:
        # 2mb heatmaps (both regular and subpage)
        for suffix in ['.heatmap', '_subpage.heatmap']:
            src = f'{SOURCE_DATA}/2mb/heatmaps/{wl}{suffix}'
            dst = f'{HM_2MB_DIR}/{wl}{suffix}'
            if os.path.isfile(src) and not os.path.isfile(dst):
                shutil.copy2(src, dst)
                copied += 1
            elif not os.path.isfile(src):
                missing += 1

        # Physical mappings
        src = f'{SOURCE_DATA}/2mb/physical_mappings/{wl}.pmap'
        dst = f'{PMAP_DIR}/{wl}.pmap'
        if os.path.isfile(src) and not os.path.isfile(dst):
            shutil.copy2(src, dst)
            copied += 1
        elif not os.path.isfile(src):
            missing += 1

        # 4kb heatmaps
        src = f'{SOURCE_DATA}/4kb/heatmaps/{wl}.heatmap'
        dst = f'{HM_4KB_DIR}/{wl}.heatmap'
        if os.path.isfile(src) and not os.path.isfile(dst):
            shutil.copy2(src, dst)
            copied += 1
        elif not os.path.isfile(src):
            missing += 1

        # 4kb_cap heatmaps
        src_cap = f'{SOURCE_DATA}/2mb/heatmaps_4kb_cap/{wl}.heatmap'
        dst_cap = f'{HM_4KB_CAP_DIR}/{wl}.heatmap'
        if os.path.isfile(src_cap) and not os.path.isfile(dst_cap):
            shutil.copy2(src_cap, dst_cap)
            copied += 1
        elif not os.path.isfile(src_cap) and not os.path.isfile(dst_cap):
            # Create from capacity line + subpage heatmap
            hm_2mb = f'{SOURCE_DATA}/2mb/heatmaps/{wl}.heatmap'
            subpage = f'{SOURCE_DATA}/2mb/heatmaps/{wl}_subpage.heatmap'
            if os.path.isfile(hm_2mb) and os.path.isfile(subpage):
                cap_line = None
                with open(hm_2mb) as f:
                    first = f.readline().strip()
                    if first.startswith('#capacity'):
                        cap_line = first
                if cap_line:
                    with open(dst_cap, 'w') as out:
                        out.write(cap_line + '\n')
                        with open(subpage) as sp:
                            for line in sp:
                                out.write(line)
                    copied += 1
                    print(f'    Created 4kb_cap heatmap for {wl}')
                else:
                    missing += 1
            else:
                missing += 1

    print(f'  Shared data: {copied} files copied/created, {missing} missing sources')


# ═══════════════════════════════════════════════════════════════════
# Job submission
# ═══════════════════════════════════════════════════════════════════

def submit_workload(wl, trace_path):
    """Submit all 60 jobs for one Q1 workload. Returns (submitted, skipped)."""
    submitted = 0
    skipped = 0

    hm_2mb    = f'{HM_2MB_DIR}/{wl}.heatmap'
    hm_4kb    = f'{HM_4KB_DIR}/{wl}.heatmap'
    hm_4kbcap = f'{HM_4KB_CAP_DIR}/{wl}.heatmap'
    pmap      = f'{PMAP_DIR}/{wl}.pmap'

    # Check shared files exist
    shared_ok = True
    for path, label in [(hm_2mb, '2mb heatmap'), (hm_4kb, '4kb heatmap'),
                        (hm_4kbcap, '4kb_cap heatmap'), (pmap, 'pmap')]:
        if not os.path.isfile(path):
            print(f'    WARN: missing {label}: {path}')
            shared_ok = False

    # ── PTW loop: radix + ecpt ──
    for ptw in ['radix', 'ecpt']:
        base = f'{OUTPUT_BASE}/{ptw}'
        scripts_dir = f'{base}/job_scripts'
        policies_dir = f'{base}/policies'

        if ptw == 'radix':
            bin_tiered = BIN['radix_tiered']
            bin_skip   = BIN['radix_skip']
            bin_perf   = BIN['radix_perf']
        else:
            bin_tiered = BIN['ecpt_tiered']
            bin_skip   = BIN['ecpt_skip']
            bin_perf   = BIN['ecpt_perf']

        # ── A. 2mb_tiered (per ratio) ──
        for rtag in RATIOS:
            ratio = RATIO_MAP[rtag]
            out_path = f'{base}/2mb_tiered/{rtag}/stdout/{wl}.out'
            err_path = f'{base}/2mb_tiered/{rtag}/stderr/{wl}.err'

            if job_done(out_path):
                skipped += 1
                continue
            if not os.path.isfile(hm_2mb) or not os.path.isfile(pmap):
                skipped += 1
                continue

            script = f'{scripts_dir}/{wl}.2mb_tiered_{rtag}.sh'
            with open(script, 'w') as f:
                f.write(f"""#!/bin/bash
#SBATCH --account=uppmax2025-2-337
#SBATCH --ntasks=1
#SBATCH --time=05:00:00
set -e
"{bin_tiered}" -w {W} -i {I} --page-mode 2mb \\
    --use-heatmap "{hm_2mb}" --ratio {ratio} \\
    --use-pmap "{pmap}" \\
    "{trace_path}" 2>&1
""")
            os.chmod(script, 0o755)
            jname = f'v2_{ptw[0]}2t{rtag[-1]}_{wl[:15]}'
            jid = sbatch_script(script, out_path, err_path, job_name=jname)
            if jid:
                submitted += 1
            else:
                print(f'    FAIL: {ptw}/2mb_tiered/{rtag}/{wl}')

        # ── B. 4kb_tiered (per ratio) ──
        for rtag in RATIOS:
            ratio = RATIO_MAP[rtag]
            out_path = f'{base}/4kb_tiered/{rtag}/stdout/{wl}.out'
            err_path = f'{base}/4kb_tiered/{rtag}/stderr/{wl}.err'

            if job_done(out_path):
                skipped += 1
                continue
            if not os.path.isfile(hm_4kb) or not os.path.isfile(pmap):
                skipped += 1
                continue

            script = f'{scripts_dir}/{wl}.4kb_tiered_{rtag}.sh'
            with open(script, 'w') as f:
                f.write(f"""#!/bin/bash
#SBATCH --account=uppmax2025-2-337
#SBATCH --ntasks=1
#SBATCH --time=05:00:00
set -e
"{bin_tiered}" -w {W} -i {I} --page-mode 4kb \\
    --use-heatmap "{hm_4kb}" --ratio {ratio} \\
    --use-pmap "{pmap}" \\
    "{trace_path}" 2>&1
""")
            os.chmod(script, 0o755)
            jname = f'v2_{ptw[0]}4t{rtag[-1]}_{wl[:15]}'
            jid = sbatch_script(script, out_path, err_path, job_name=jname)
            if jid:
                submitted += 1
            else:
                print(f'    FAIL: {ptw}/4kb_tiered/{rtag}/{wl}')

        # ── C. 4kb_skip (per ratio) ──
        for rtag in RATIOS:
            ratio = RATIO_MAP[rtag]
            out_path = f'{base}/skip/{rtag}/stdout/{wl}.out'
            err_path = f'{base}/skip/{rtag}/stderr/{wl}.err'

            if job_done(out_path):
                skipped += 1
                continue
            if not os.path.isfile(hm_4kb) or not os.path.isfile(pmap):
                skipped += 1
                continue

            script = f'{scripts_dir}/{wl}.skip_{rtag}.sh'
            with open(script, 'w') as f:
                f.write(f"""#!/bin/bash
#SBATCH --account=uppmax2025-2-337
#SBATCH --ntasks=1
#SBATCH --time=05:00:00
set -e
"{bin_skip}" -w {W} -i {I} --page-mode 4kb \\
    --use-heatmap "{hm_4kb}" --ratio {ratio} \\
    --use-pmap "{pmap}" \\
    "{trace_path}" 2>&1
""")
            os.chmod(script, 0o755)
            jname = f'v2_{ptw[0]}sk{rtag[-1]}_{wl[:15]}'
            jid = sbatch_script(script, out_path, err_path, job_name=jname)
            if jid:
                submitted += 1
            else:
                print(f'    FAIL: {ptw}/skip/{rtag}/{wl}')

        # ── D. Memtis (per variant x per ratio) ──
        memtis_variants = {
            'follow':  [],                              # no extra args
            'split33': ['--memtis-split-ratio', '0.33'],
            'split67': ['--memtis-split-ratio', '0.67'],
        }

        for variant, extra_args in memtis_variants.items():
            for rtag in RATIOS:
                ratio = RATIO_MAP[rtag]
                out_path = f'{base}/memtis/{variant}/{rtag}/stdout/{wl}.out'
                err_path = f'{base}/memtis/{variant}/{rtag}/stderr/{wl}.err'

                if job_done(out_path):
                    skipped += 1
                    continue
                if not os.path.isfile(hm_4kbcap) or not os.path.isfile(pmap):
                    skipped += 1
                    continue

                # tiered_ref: existing radix 2mb tiered output for LLC latency
                tiered_ref = f'{SOURCE_DATA}/2mb/access_{rtag}/stdout/{wl}.out'
                if not os.path.isfile(tiered_ref):
                    print(f'    SKIP: {ptw}/memtis/{variant}/{rtag}/{wl} (no tiered ref)')
                    skipped += 1
                    continue

                cfg_tag = f'memtis_{variant}_{rtag}'
                policy_file = f'{policies_dir}/{wl}.{cfg_tag}.policy'
                pmap_tmp = f'{policies_dir}/{wl}.{cfg_tag}.pmap.tmp'
                extra_str = ' '.join(extra_args)

                script = f'{scripts_dir}/{wl}.{cfg_tag}.sh'
                with open(script, 'w') as f:
                    f.write(f"""#!/bin/bash
#SBATCH --account=uppmax2025-2-337
#SBATCH --ntasks=1
#SBATCH --time=05:00:00
set -e

# Step 1: Generate memtis pmap
python3 "{PERFORATION_PY}" "{hm_4kbcap}" "{pmap_tmp}" \\
    --tiered-output "{tiered_ref}" --ratio {ratio} \\
    --no-perforate {extra_str} --verbose

# Step 2: Convert to policy
python3 "{PMAP_TO_POLICY}" "{pmap_tmp}" "{hm_4kbcap}" "{policy_file}"
rm -f "{pmap_tmp}"

# Step 3: Run simulation
"{bin_perf}" -w {W} -i {I} --page-mode mixed \\
    --use-pmap "{pmap}" \\
    --use-policy "{policy_file}" \\
    "{trace_path}" 2>&1
""")
                os.chmod(script, 0o755)
                jname = f'v2_{ptw[0]}m{variant[0]}{rtag[-1]}_{wl[:12]}'
                jid = sbatch_script(script, out_path, err_path, job_name=jname)
                if jid:
                    submitted += 1
                else:
                    print(f'    FAIL: {ptw}/memtis/{variant}/{rtag}/{wl}')

        # ── E/F. Perforation ──
        if ptw == 'radix':
            perf_configs = []
            for perf_base in ['follow', 'full2mb']:
                for theta_tag, theta_val in [('none', '1.0'), ('t33', '0.33'), ('t67', '0.67')]:
                    perf_configs.append((perf_base, theta_tag, theta_val))
        else:  # ecpt: theta=none only
            perf_configs = []
            for perf_base in ['follow', 'full2mb']:
                perf_configs.append((perf_base, 'none', '1.0'))

        for perf_base, theta_tag, theta_val in perf_configs:
            for rtag in RATIOS:
                ratio = RATIO_MAP[rtag]
                out_path = f'{base}/perf/{perf_base}/{theta_tag}/{rtag}/stdout/{wl}.out'
                err_path = f'{base}/perf/{perf_base}/{theta_tag}/{rtag}/stderr/{wl}.err'

                if job_done(out_path):
                    skipped += 1
                    continue
                if not os.path.isfile(hm_4kbcap) or not os.path.isfile(pmap):
                    skipped += 1
                    continue

                tiered_ref = f'{SOURCE_DATA}/2mb/access_{rtag}/stdout/{wl}.out'
                if not os.path.isfile(tiered_ref):
                    print(f'    SKIP: {ptw}/perf/{perf_base}/{theta_tag}/{rtag}/{wl} (no tiered ref)')
                    skipped += 1
                    continue

                cfg_tag = f'perf_{perf_base}_{theta_tag}_{rtag}'
                policy_file = f'{policies_dir}/{wl}.{cfg_tag}.policy'
                pmap_tmp = f'{policies_dir}/{wl}.{cfg_tag}.pmap.tmp'

                # Build perforation.py arguments
                perf_args = []
                if perf_base == 'full2mb':
                    perf_args.append('--skip-memtis')
                perf_args.extend(['--theta-high', theta_val])

                perf_args_str = ' '.join(perf_args)

                script = f'{scripts_dir}/{wl}.{cfg_tag}.sh'
                with open(script, 'w') as f:
                    f.write(f"""#!/bin/bash
#SBATCH --account=uppmax2025-2-337
#SBATCH --ntasks=1
#SBATCH --time=05:00:00
set -e

# Step 1: Generate perforation pmap
python3 "{PERFORATION_PY}" "{hm_4kbcap}" "{pmap_tmp}" \\
    --tiered-output "{tiered_ref}" --ratio {ratio} \\
    {perf_args_str} --verbose

# Step 2: Convert to policy
python3 "{PMAP_TO_POLICY}" "{pmap_tmp}" "{hm_4kbcap}" "{policy_file}"
rm -f "{pmap_tmp}"

# Step 3: Run simulation
"{bin_perf}" -w {W} -i {I} --page-mode mixed \\
    --use-pmap "{pmap}" \\
    --use-policy "{policy_file}" \\
    "{trace_path}" 2>&1
""")
                os.chmod(script, 0o755)
                jname = f'v2_{ptw[0]}p{perf_base[0]}{theta_tag[0]}{rtag[-1]}_{wl[:10]}'
                jid = sbatch_script(script, out_path, err_path, job_name=jname)
                if jid:
                    submitted += 1
                else:
                    print(f'    FAIL: {ptw}/perf/{perf_base}/{theta_tag}/{rtag}/{wl}')

    return submitted, skipped


# ═══════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════

def main():
    print('=' * 70)
    print('full_res Submission')
    print(f'Output: {OUTPUT_BASE}')
    print('=' * 70)

    # Verify binaries
    for name, path in sorted(BIN.items()):
        if not os.path.isfile(path):
            print(f'ERROR: Binary not found: {path}')
            sys.exit(1)
        print(f'  Binary OK: {name}')

    # Verify scripts
    for name, path in [('perforation.py', PERFORATION_PY), ('pmap_to_policy.py', PMAP_TO_POLICY)]:
        if not os.path.isfile(path):
            print(f'ERROR: Script not found: {path}')
            sys.exit(1)

    # Find Q1 workloads
    print('\nIdentifying Q1 workloads...')
    q1 = find_q1_workloads()
    print(f'Found {len(q1)} Q1 workloads')
    if len(q1) != 61:
        print(f'WARNING: Expected 61 Q1 workloads, got {len(q1)}')

    if not q1:
        print('ERROR: No Q1 workloads found!')
        sys.exit(1)

    # Create directories
    print('\nCreating output directories...')
    create_all_dirs()

    # Copy shared data
    print('\nCopying shared data...')
    copy_shared_data(q1)

    # Submit jobs
    print(f'\nSubmitting jobs (60 per workload x {len(q1)} workloads = {60 * len(q1)} expected)...')
    total_submitted = 0
    total_skipped = 0
    total_missing_traces = 0

    for wl in q1:
        trace = find_trace(wl)
        if not trace:
            print(f'\n  {wl}: TRACE NOT FOUND')
            total_missing_traces += 1
            total_skipped += 60
            continue

        print(f'\n  {wl}:')
        s, sk = submit_workload(wl, trace)
        total_submitted += s
        total_skipped += sk
        print(f'    => submitted={s} skipped={sk}')

    print(f'\n{"=" * 70}')
    print(f'Q1 workloads:    {len(q1)}')
    print(f'Missing traces:  {total_missing_traces}')
    print(f'Submitted:       {total_submitted}')
    print(f'Skipped:         {total_skipped}')
    print(f'Total expected:  {60 * len(q1)}')
    print(f'{"=" * 70}')


if __name__ == '__main__':
    main()
