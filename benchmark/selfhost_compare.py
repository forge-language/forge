#!/usr/bin/env python3
"""Compare isolated stage0/stage2 C emission with identical Forge inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import resource
import statistics
import subprocess
import tempfile
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage0', nargs=2, action='append', default=[], metavar=('NAME', 'BINARY'))
    parser.add_argument('--stage2', nargs=2, action='append', default=[], metavar=('NAME', 'BINARY'))
    parser.add_argument('--source', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=15)
    args = parser.parse_args()
    compilers = [(name, stage, Path(binary).resolve())
                 for stage, values in [('stage0', args.stage0), ('stage2', args.stage2)]
                 for name, binary in values]
    if len(compilers) < 2 or len({name for name, _, _ in compilers}) != len(compilers):
        parser.error('Select at least two compilers with unique names')
    if not 3 <= args.repeats <= 100:
        parser.error('repeats must be 3..100')
    if any(not binary.is_file() for _, _, binary in compilers):
        parser.error('Compiler binary missing')
    if any(not source.is_file() for source in args.source):
        parser.error('Input source missing')
    rows, hashes, summaries = [], {}, {}
    with tempfile.TemporaryDirectory(prefix='forge-selfhost-compare-') as directory:
        emitted = Path(directory) / 'output.c'
        for source in args.source:
            source = source.resolve()
            for repeat in range(args.repeats):
                order = compilers if repeat % 2 == 0 else list(reversed(compilers))
                for name, stage, binary in order:
                    command = [str(binary), str(source), '-o', str(emitted)]
                    if stage == 'stage0':
                        command.append('--emit-c')
                    before = resource.getrusage(resource.RUSAGE_CHILDREN)
                    started = time.perf_counter()
                    subprocess.run(command, check=True, capture_output=True)
                    wall_ms = (time.perf_counter() - started) * 1000
                    after = resource.getrusage(resource.RUSAGE_CHILDREN)
                    result_hash = digest(emitted)
                    key = (str(source), name)
                    if hashes.setdefault(key, result_hash) != result_hash:
                        raise RuntimeError('Nondeterministic C output: ' + name)
                    rows.append({'source': str(source), 'variant': name,
                                 'repeat': repeat + 1, 'wall_ms': wall_ms,
                                 'cpu_ms': (after.ru_utime + after.ru_stime -
                                            before.ru_utime - before.ru_stime) * 1000})
            summaries[str(source)] = {
                name: {metric: {'median': statistics.median(values),
                                'min': min(values), 'max': max(values)}
                       for metric in ['wall_ms', 'cpu_ms']
                       for values in [[row[metric] for row in rows
                                       if row['source'] == str(source) and row['variant'] == name]]}
                for name, _, _ in compilers}
    result = {
        'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'host': platform.platform(), 'repeats': args.repeats,
        'order': 'alternating forward and reverse compiler order',
        'scope': 'Process startup, Forge validation/parsing and C file emission; '
                 'excludes downstream C compilation. Child user+system CPU. Shared host.',
        'compilers': {name: {'stage': stage, 'path': str(binary), 'sha256': digest(binary)}
                      for name, stage, binary in compilers},
        'inputs': {str(source.resolve()): {'bytes': source.stat().st_size, 'sha256': digest(source)}
                   for source in args.source},
        'output_sha256': {str(source.resolve()): {name: hashes[(str(source.resolve()), name)]
                                                for name, _, _ in compilers}
                          for source in args.source},
        'summary': summaries, 'runs': rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(summaries, indent=2))


if __name__ == '__main__':
    main()
