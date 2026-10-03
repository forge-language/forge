#!/usr/bin/env python3
"""Retain repeated observations for legacy/view and substring/view algorithms."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import statistics
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--repeats', type=int, default=7)
args = parser.parse_args()
if not 3 <= args.repeats <= 20:
    parser.error('repeats must be 3..20')
binary = args.binary.resolve()
runs = []
for repeat in range(args.repeats):
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=60)
    for row in csv.DictReader(io.StringIO(result.stdout)):
        runs.append({**row, 'repeat': repeat + 1, 'bytes': int(row['bytes']),
                     'seconds': float(row['seconds']), 'checksum': int(row['checksum'])})
summary = []
for operation, size in sorted({(row['operation'], row['bytes']) for row in runs}):
    groups = {}
    checksums = set()
    for mode in sorted({row['implementation'] for row in runs if row['operation'] == operation}):
        samples = [row for row in runs if row['operation'] == operation
                   and row['bytes'] == size and row['implementation'] == mode]
        checksums.update(row['checksum'] for row in samples)
        values = [row['seconds'] for row in samples]
        groups[mode] = {'median_seconds': statistics.median(values),
                        'min_seconds': min(values), 'max_seconds': max(values)}
    if len(checksums) != 1:
        raise RuntimeError('Algorithm checksum mismatch')
    old = 'legacy' if 'legacy' in groups else 'substring'
    new = 'builder' if 'builder' in groups else 'view'
    summary.append({'operation': operation, 'bytes': size, 'algorithms': groups,
                    'speedup': groups[old]['median_seconds'] / groups[new]['median_seconds']})
report = {'repeats': args.repeats, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
          'scope': 'Algorithm comparison in the current library; fixed old/new order; shared host. '
                   'Arena reset excluded; view creation and builder finish included. Short trials are noisy.',
          'summary': summary, 'runs': runs}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(summary, indent=2))
