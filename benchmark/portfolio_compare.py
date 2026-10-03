#!/usr/bin/env python3
"""Rebuild one portfolio snapshot with two Forge toolchains and its Rust backend."""
import argparse
from contextlib import nullcontext
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def fingerprint(root):
    value = hashlib.sha256()
    for path in sorted(root.rglob('*')):
        if path.is_file():
            value.update(path.relative_to(root).as_posix().encode() + b'\0')
            value.update(path.read_bytes() + b'\0')
    return value.hexdigest()


def stage(app, language, destination):
    ignore = shutil.ignore_patterns('.git', '.env', '.env.*', 'build', 'target',
                                   '__pycache__', 'node_modules', 'toolchain', '.omo', '.serena')
    for name in ['backend', 'backend-forge']:
        shutil.copytree(app / name, destination / name, ignore=ignore)
    toolchain = destination / 'backend-forge/toolchain'
    toolchain.mkdir()
    shutil.copy2(app / 'backend-forge/toolchain/CMakeLists.txt', toolchain / 'CMakeLists.txt')
    for name in ['compiler', 'include', 'runtime', 'stdlib']:
        shutil.copytree(language / name, toolchain / name)
    return {'application': fingerprint(destination / 'backend-forge/src'),
            'native_adapter': hashlib.sha256((destination / 'backend-forge/native/adapter.c').read_bytes()).hexdigest(),
            'toolchain': fingerprint(toolchain),
            'postgres': fingerprint(destination / 'backend-forge/vendor/forge-postgres'),
            'web': fingerprint(destination / 'backend-forge/vendor/forge-web'),
            'rust': fingerprint(destination / 'backend')}


def build(context, dockerfile, image, log):
    print('Building ' + image, flush=True)
    with log.open('w') as output:
        subprocess.run(['docker', 'build', '-t', image, '-f', str(dockerfile), str(context)],
                       stdout=output, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(
        description='Compare one disposable portfolio snapshot with before/after Forge and Rust images.',
        epilog='Use --build-only to prepare images, then --resume with the same output path. '
               'Preparation metadata is saved as <output-stem>-prepared.json; use --prepared '
               'to reuse it with a separate output for focused follow-ups. Source snapshots '
               'and images are retained; disposable benchmark containers/network are cleaned up.')
    parser.add_argument('--project-root', type=Path, required=True)
    parser.add_argument('--before-root', type=Path, required=True)
    parser.add_argument('--after-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--python', default='python3')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--clients', default='1,16,64',
                        help='Comma-separated timed client counts (default: 1,16,64)')
    parser.add_argument('--endpoints', default='/api/health,/api/posts,/api/projects',
                        help='Comma-separated timed endpoints; parity preflight always checks all three APIs')
    parser.add_argument('--prepared', type=Path,
                        help='Preparation metadata to reuse with --resume')
    parser.add_argument('--build-only', action='store_true',
                        help='Build images and save preparation metadata without measuring')
    parser.add_argument('--resume', action='store_true',
                        help='Measure previously prepared images for the same output path')
    args = parser.parse_args()
    if not 3 <= args.repeats <= 10 or not 1 <= args.seconds <= 60:
        parser.error('repeats must be 3..10 and seconds 1..60')
    try:
        clients = [int(value) for value in args.clients.split(',')]
    except ValueError:
        parser.error('clients must be comma-separated integers')
    if not clients or len(set(clients)) != len(clients) or any(n < 1 or n > 256 for n in clients):
        parser.error('clients must be distinct integers in 1..256')
    endpoints = args.endpoints.split(',')
    if len(set(endpoints)) != len(endpoints) or any(
            path not in ['/api/health', '/api/posts', '/api/projects'] for path in endpoints):
        parser.error('endpoints must be distinct supported public API paths')
    if args.prepared and not args.resume:
        parser.error('--prepared requires --resume')
    app = args.project_root.resolve()
    required = app / 'backend-forge/benchmark/run_throughput.sh'
    if not required.is_file():
        parser.error('project must contain the disposable portfolio throughput harness')
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    metadata_path = args.prepared.resolve() if args.prepared else output.with_name(output.stem + '-prepared.json')
    if args.build_only and args.resume:
        parser.error('--build-only and --resume are mutually exclusive')
    prepared = json.loads(metadata_path.read_text()) if args.resume else None
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S')
    images = prepared['images'] if prepared else {
        name: 'forge-continuation-' + name + ':' + stamp for name in ['before', 'after', 'rust']}
    if prepared:
        for name, image in images.items():
            actual = subprocess.check_output(['docker', 'image', 'inspect', '-f', '{{.Id}}', image],
                                             text=True).strip()
            if actual != prepared['image_ids'][name]:
                raise RuntimeError('Prepared image changed: ' + image)
    else:
        for image in images.values():
            if subprocess.run(['docker', 'image', 'inspect', image], capture_output=True).returncode == 0:
                raise RuntimeError('Refusing to overwrite an existing image: ' + image)
    directory = prepared['snapshot_directory'] if prepared else tempfile.mkdtemp(prefix='forge-portfolio-snapshot-')
    with nullcontext(directory) as directory:
        temp = Path(directory)
        stages = {name: temp / name for name in ['before', 'after']}
        if prepared:
            provenance = prepared['provenance']
            for name in ['before', 'after']:
                current = fingerprint(stages[name] / 'backend-forge/toolchain')
                if current != provenance[name]['toolchain']:
                    raise RuntimeError('Prepared toolchain snapshot changed: ' + name)
        else:
            provenance = {'before': stage(app, args.before_root.resolve(), stages['before'])}
            shutil.copytree(stages['before'], stages['after'])
            toolchain = stages['after'] / 'backend-forge/toolchain'
            for name in ['compiler', 'include', 'runtime', 'stdlib']:
                shutil.rmtree(toolchain / name)
                shutil.copytree(args.after_root.resolve() / name, toolchain / name)
            provenance['after'] = {**provenance['before'], 'toolchain': fingerprint(toolchain)}
            for name in ['before', 'after']:
                build(stages[name], stages[name] / 'backend-forge/Containerfile', images[name],
                      output.with_name(output.stem + '-' + name + '-build.log'))
            build(stages['before'] / 'backend', stages['before'] / 'backend/Containerfile', images['rust'],
                  output.with_name(output.stem + '-rust-build.log'))
            prepared = {
                'snapshot_directory': str(temp), 'images': images, 'provenance': provenance,
                'image_ids': {name: subprocess.check_output(
                    ['docker', 'image', 'inspect', '-f', '{{.Id}}', image], text=True).strip()
                    for name, image in images.items()},
            }
            metadata_path.write_text(json.dumps(prepared, indent=2) + '\n')
        if args.build_only:
            print('Prepared images; resume with the same arguments and --resume: ' + str(metadata_path),
                  flush=True)
            return
        baseline_id = subprocess.check_output(['docker', 'image', 'inspect', '-f', '{{.Id}}',
                                              images['before']], text=True).strip()
        runner = stages['after'] / 'backend-forge/benchmark/run_throughput.sh'
        script = runner.read_text()
        script = script.replace('forge-throughput-baseline:bc61821-20261001', images['before'])
        script = script.replace('forge-throughput-after:20261001', images['after'])
        script = script.replace('forge-compare-rust:local', images['rust'])
        original_id = 'sha256:db6a144d3184f1da1eb0f5e7a3020e9531af8c75f177d71a1cfd44aedf951ed7'
        script = script.replace(original_id, baseline_id)
        if 'export THROUGHPUT_AFTER_REVISION=' in script:
            start = script.index('export THROUGHPUT_AFTER_REVISION=')
            end = script.index('export THROUGHPUT_SOURCE_SHA256=', start)
            script = script[:start] + script[end:]
        marker = 'PYTHON=${THROUGHPUT_PYTHON:-python3}'
        if "-d rust_test -v ON_ERROR_STOP=1 -c 'TRUNCATE projects CASCADE'" not in script:
            script = script.replace(marker, '''docker exec forge-throughput-db psql -U bench -d rust_test -v ON_ERROR_STOP=1 -c 'TRUNCATE projects CASCADE'
docker exec -i forge-throughput-db psql -U bench -d rust_test -v ON_ERROR_STOP=1 < "$TEMP/projects.sql"
''' + marker)
        runner.write_text(script)
        compare = stages['after'] / 'backend-forge/benchmark/throughput_compare.py'
        code = compare.read_text()
        code = code.replace('order = names[repeat:] + names[:repeat]',
                            'order = names[repeat % len(names):] + names[:repeat % len(names)]')
        code = code.replace("'baseline_forge_commit': 'bc61821'", "'baseline_forge_commit': 'compiler source snapshot'")
        code = code.replace("'baseline_application_code_commit': '43871d3'",
                            "'baseline_application_code_commit': 'same application snapshot for both Forge builds'")
        if "'Project fixture mismatch'" not in code:
            code = code.replace("equivalent[path] =", """if path == '/api/projects':
                def normalized(items):
                    result = []
                    for value in items:
                        item = dict(value)
                        for key in ['created_at', 'updated_at']:
                            if isinstance(item.get(key), str):
                                item[key] = datetime.datetime.fromisoformat(item[key].replace('Z', '+00:00'))
                        result.append(item)
                    return sorted(result, key=lambda item: item['id'])
                assert normalized(values['rust']) == normalized(values['forge_after']), 'Project fixture mismatch'
            equivalent[path] =""")
        if 'TIMED_PATHS =' not in code:
            definition = "PATHS = ['/api/health', '/api/posts', '/api/projects']"
            code = code.replace(definition, definition + "\nTIMED_PATHS = os.environ.get('THROUGHPUT_ENDPOINTS', ','.join(PATHS)).split(',')")
        code = code.replace('for path in PATHS:\n        for clients in CLIENTS:',
                            'for path in TIMED_PATHS:\n        for clients in CLIENTS:')
        compare.write_text(code)
        env = {**os.environ, 'THROUGHPUT_SKIP_BUILD': '1', 'THROUGHPUT_OUTPUT': str(output),
               'THROUGHPUT_PYTHON': args.python, 'THROUGHPUT_REPEATS': str(args.repeats),
               'THROUGHPUT_SECONDS': str(args.seconds), 'THROUGHPUT_WARMUP': '1',
               'THROUGHPUT_CLIENTS': args.clients, 'THROUGHPUT_ENDPOINTS': args.endpoints,
               'THROUGHPUT_BASELINE_IMAGE': images['before'],
               'THROUGHPUT_AFTER_REVISION': 'application snapshot sha256:' + provenance['after']['application'],
               'THROUGHPUT_COMPILER_REVISION': 'source sha256:' + provenance['after']['toolchain'],
               'THROUGHPUT_POSTGRES_REVISION': 'source sha256:' + provenance['after']['postgres'],
               'THROUGHPUT_WEB_REVISION': 'source sha256:' + provenance['after']['web']}
        print('Running isolated three-way comparison; no builds during measurement.', flush=True)
        subprocess.run(['sh', str(runner)], cwd=stages['after'], env=env, check=True)
        report = json.loads(output.read_text())
        report['provenance'] = provenance
        report['prepared_image_ids'] = prepared['image_ids']
        report['images'] = images
        report['comparison_scope'] = 'Identical Forge application, native adapter and vendor sources; '
        report['comparison_scope'] += 'only compiler/runtime/stdlib snapshot changes. Rust application comparison '
        report['comparison_scope'] += 'includes libc/worker/ban-cache differences; scheduler is outside MHD request path.'
        report['limits']['project_equivalence'] = 'All fields after timestamp normalization and ID sort; identical DB rows'
        report['limits']['timed_endpoints'] = endpoints
        output.write_text(json.dumps(report, indent=2) + '\n')
        output.with_suffix('.md').unlink()
        print('Raw evidence: ' + str(output), flush=True)


if __name__ == '__main__':
    main()
