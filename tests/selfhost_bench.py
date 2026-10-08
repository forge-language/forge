"""Compare two self-hosted compilers on identical C-emission workloads."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--current', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--runs', type=int, default=41)
    parser.add_argument('--warmups', type=int, default=3)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.runs < 1 or args.warmups < 0:
        parser.error('runs must be positive and warmups must be nonnegative')
    source = args.source.resolve()
    compilers = {'baseline': args.baseline.resolve(), 'current': args.current.resolve()}
    timings = {name: [] for name in compilers}
    with tempfile.TemporaryDirectory(prefix='forge-selfhost-bench-') as directory:
        for round_index in range(args.warmups + args.runs):
            order = ['baseline', 'current']
            if round_index % 2:
                order.reverse()
            for name in order:
                start = time.perf_counter_ns()
                subprocess.run([str(compilers[name]), str(source), '--emit-c',
                                '-o', str(Path(directory) / (name + '.c'))],
                               check=True, capture_output=True, timeout=120)
                elapsed = (time.perf_counter_ns() - start) / 1e6
                if round_index >= args.warmups:
                    timings[name].append(elapsed)
    result = {
        'platform': platform.platform(),
        'source': str(source),
        'source_bytes': source.stat().st_size,
        'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'compiler_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest()
                            for name, path in compilers.items()},
        'runs': args.runs,
        'warmups': args.warmups,
        'order': 'alternating baseline/current',
        'elapsed_ms': timings,
        'median_ms': {name: statistics.median(values) for name, values in timings.items()},
    }
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['median_ms'], indent=2))


if __name__ == '__main__':
    main()
