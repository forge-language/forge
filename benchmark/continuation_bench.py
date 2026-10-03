#!/usr/bin/env python3
"""Alternate frozen/current builds and retain every measured observation."""
import argparse
import csv
import datetime
import hashlib
import io
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--before-root', type=Path, required=True)
    parser.add_argument('--before-build', type=Path, required=True)
    parser.add_argument('--after-root', type=Path, required=True)
    parser.add_argument('--after-build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=7)
    args = parser.parse_args()
    if not 3 <= args.repeats <= 20:
        parser.error('repeats must be 3..20')
    builds = {'before': args.before_build.resolve(), 'after': args.after_build.resolve()}
    roots = {'before': args.before_root.resolve(), 'after': args.after_root.resolve()}
    cc = subprocess.check_output(['cc', '--version'], text=True).splitlines()[0]
    rows = []
    with tempfile.TemporaryDirectory(prefix='forge-continuation-measure-') as directory:
        temp = Path(directory)
        scheduler = {}
        source = args.after_root.resolve() / 'benchmark/scheduler_bench.c'
        for variant, build in builds.items():
            binary = temp / ('scheduler-' + variant)
            subprocess.run(['cc', '-std=c11', '-O3', '-DNDEBUG', '-I', str(roots[variant] / 'include'),
                            str(source), str(build / 'lib/libforge_runtime.a'),
                            str(build / 'lib/libforge_std.a'), '-pthread', '-o', str(binary)], check=True)
            scheduler[variant] = binary
        common = roots['before'] / 'bootstrap/compiler.fg'
        outputs = {}
        for variant in builds:
            output = temp / (variant + '.c')
            subprocess.run([str(builds[variant] / 'bin/forge-stage2'), str(common), '-o', str(output)],
                           check=True, capture_output=True, timeout=60)
            outputs[variant] = output.read_bytes()
        # New diagnostics and OS headers can change emitted C. Validate behavior
        # by compiling each emitted compiler and using it on one shared program.
        fixture = temp / 'fixture.fg'
        fixture.write_text('native main {\n println("benchmark fixture");\n return 0;\n}\n')
        for variant, build in builds.items():
            compiler = temp / ('common-' + variant)
            subprocess.run(['cc', '-std=c11', '-O3', '-I', str(roots[variant] / 'include'),
                            str(temp / (variant + '.c')), '-L', str(build / 'lib'),
                            '-lforge_runtime', '-lforge_std', '-lm', '-pthread', '-o', str(compiler)],
                           check=True, capture_output=True)
            emitted = temp / ('fixture-' + variant + '.c')
            subprocess.run([str(compiler), str(fixture), '-o', str(emitted)], check=True,
                           capture_output=True, timeout=60)
            executable = temp / ('fixture-' + variant)
            subprocess.run(['cc', '-std=c11', '-O3', '-I', str(roots[variant] / 'include'),
                            str(emitted), '-L', str(build / 'lib'), '-lforge_runtime',
                            '-lforge_std', '-lm', '-pthread', '-o', str(executable)], check=True,
                           capture_output=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
            if result.stdout != 'benchmark fixture\n':
                raise RuntimeError('Common-input compiler produced incorrect native behavior')
        cases = [('scheduler_dispatch_10000_1', ['throughput', '10000', '1']),
                 ('scheduler_dispatch_100000_4', ['throughput', '100000', '4']),
                 ('scheduler_idle_500ms', ['500']), ('bootstrap_common_input', None)]
        for case, command in cases:
            for repeat in range(args.repeats):
                order = ['before', 'after'] if repeat % 2 == 0 else ['after', 'before']
                for variant in order:
                    if command is None:
                        output = temp / (variant + '.c')
                        start = time.perf_counter()
                        subprocess.run([str(builds[variant] / 'bin/forge-stage2'), str(common), '-o', str(output)],
                                       check=True, capture_output=True, timeout=60)
                        sample = {'wall_ms': (time.perf_counter() - start) * 1000}
                        if output.read_bytes() != outputs[variant]:
                            raise RuntimeError('Non-deterministic bootstrap output')
                    else:
                        result = subprocess.run([str(scheduler[variant]), *command], check=True,
                                                capture_output=True, text=True, timeout=60)
                        sample = dict(next(csv.DictReader(io.StringIO(result.stdout))))
                        sample = {key: float(value) for key, value in sample.items()}
                        if 'completed' in sample and sample['completed'] != sample['coroutines']:
                            raise RuntimeError('Coroutine completion mismatch')
                    row = {'case': case, 'variant': variant, 'repeat': repeat + 1, **sample}
                    rows.append(row)
                    print(json.dumps(row), flush=True)
        summary = {}
        for case, _ in cases:
            groups = {}
            for variant in builds:
                samples = [r for r in rows if r['case'] == case and r['variant'] == variant]
                groups[variant] = {key: {'median': statistics.median(r[key] for r in samples),
                                         'min': min(r[key] for r in samples),
                                         'max': max(r[key] for r in samples)}
                                   for key in ['wall_ms', 'process_cpu_ms'] if key in samples[0]}
            groups['wall_speedup'] = groups['before']['wall_ms']['median'] / groups['after']['wall_ms']['median']
            summary[case] = groups
        report = {'date_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  'host': platform.platform(), 'compiler': cc, 'repeats': args.repeats,
                  'order': 'alternating before/after and after/before',
                  'scope': 'Shared host; dispatch excludes spawn/destruction; includes worker startup/stop. '
                           'One yield is consumed within one reduction budget. Bootstrap excludes C compilation.',
                  'roots': {k: str(v) for k, v in roots.items()},
                  'builds': {k: str(v) for k, v in builds.items()},
                  'binary_sha256': {v: {name: digest(build / 'bin' / name) for name in ['forge', 'forge-stage2']}
                                    for v, build in builds.items()},
                  'library_sha256': {v: {name: digest(build / 'lib' / name)
                                        for name in ['libforge_std.a', 'libforge_runtime.a']}
                                     for v, build in builds.items()},
                  'benchmark_source_sha256': digest(source), 'input_sha256': digest(common),
                  'common_input_bytes': common.stat().st_size,
                  'output_sha256': {v: hashlib.sha256(data).hexdigest() for v, data in outputs.items()},
                  'correctness': 'Both emitted compilers compile/execute the same native fixture; per-variant deterministic C',
                  'summary': summary, 'runs': rows}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
