#!/usr/bin/env python3
"""
Find the top N slowest ChampSim simulation runs by parsing .out files.
Extracts simulation time from the last occurrence of "Simulation time:" in each file.
Uses multithreading for faster processing.

Searches in both 'results' and 'results_skip' directories.

Usage:
    python3 find_slowest_runs.py [num_to_display] [--fastest] [-j THREADS]

    num_to_display: Number of runs to display (default: 30)
    --fastest: Show fastest runs instead of slowest
    -j THREADS: Number of worker threads (default: 8)
"""

import re
import sys
import glob
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

def parse_simulation_time(filepath):
    """Extract total simulation time in seconds from a .out file."""
    try:
        with open(filepath, 'rb') as f:
            # Read last 50KB to ensure we get two-phase simulation times
            f.seek(0, 2)  # Go to end
            file_size = f.tell()
            f.seek(max(0, file_size - 153600))  # Read last 50KB
            content = f.read().decode('utf-8', errors='ignore')

            matches = re.findall(r'Simulation time: (\d+) hr (\d+) min (\d+) sec', content)
            if not matches:
                return None
            # Use the last match (final simulation time)
            hours, minutes, seconds = map(int, matches[-1])
            return hours * 3600 + minutes * 60 + seconds
    except:
        return None

def parse_file_wrapper(filepath):
    """Wrapper for multithreading."""
    sim_time = parse_simulation_time(filepath)
    if sim_time is not None:
        return (sim_time, filepath.name, filepath.parent.name)
    return None

def main():
    # Parse command line arguments
    num_to_display = 30  # Default value
    show_fastest = False
    num_workers = 8  # Default number of threads

    i = 1
    while i < len(sys.argv):
        arg = sys.argv[i]
        if arg == '--fastest':
            show_fastest = True
        elif arg == '-j' or arg == '--jobs':
            if i + 1 < len(sys.argv):
                try:
                    num_workers = int(sys.argv[i + 1])
                    i += 1  # Skip next argument
                except ValueError:
                    print(f"Error: '{sys.argv[i + 1]}' is not a valid number for -j")
                    print("Usage: python3 find_slowest_runs.py [num_to_display] [--fastest] [-j THREADS]")
                    sys.exit(1)
            else:
                print("Error: -j requires a number argument")
                sys.exit(1)
        else:
            try:
                num_to_display = int(arg)
            except ValueError:
                print(f"Error: '{arg}' is not a valid argument")
                print("Usage: python3 find_slowest_runs.py [num_to_display] [--fastest] [-j THREADS]")
                sys.exit(1)
        i += 1

    # Find all .out files in results and results_skip subdirectories
    out_files = []

    # Search in results directory
    results_dir = Path('../results')
    if results_dir.exists():
        out_files.extend(results_dir.glob('*/*.out'))

    # Search in results_skip directory
    results_skip_dir = Path('../results_skip')
    if results_skip_dir.exists():
        out_files.extend(results_skip_dir.glob('*/*.out'))

    print(f"Found {len(out_files)} .out files to analyze...")

    if not out_files:
        print("No .out files found in 'results' or 'results_skip' directories")
        sys.exit(1)

    # Parse simulation time using multithreading
    print(f"Processing with {num_workers} worker threads...")
    times = []
    completed = 0
    total_files = len(out_files)

    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        futures = {executor.submit(parse_file_wrapper, f): f for f in out_files}
        for future in as_completed(futures):
            result = future.result()
            if result is not None:
                times.append(result)
            completed += 1
            if completed % 20 == 0 or completed == total_files:
                print(f"  Progress: {completed}/{total_files} files analyzed")

    print(f"Successfully parsed {len(times)} simulation runs\n")

    # Sort by time (descending for slowest, ascending for fastest)
    times.sort(reverse=not show_fastest, key=lambda x: x[0])

    if show_fastest:
        print(f"Top {num_to_display} fastest simulation runs:\n")
    else:
        print(f"Top {num_to_display} slowest simulation runs:\n")

    for i, (seconds, filename, config) in enumerate(times[:num_to_display], 1):
        hours = seconds // 3600
        minutes = (seconds % 3600) // 60
        secs = seconds % 60
        print(f"{i:2d}. {hours:02d}:{minutes:02d}:{secs:02d} - {config}/{filename}")

if __name__ == '__main__':
    main()
