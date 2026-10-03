#!/usr/bin/env python3
"""Measure DB waits and worker/pool sensitivity in disposable portfolio servers."""
import argparse
import asyncio
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import resource
import shlex
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--prepared', type=Path, required=True)
parser.add_argument('--profile-dir', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--repeats', type=int, default=3)
parser.add_argument('--seconds', type=float, default=4)
parser.add_argument('--improvement', action='store_true')
parser.add_argument('--client-processes', type=int, default=1, choices=[1, 2, 4])
parser.add_argument('--posts-only', action='store_true')
parser.add_argument('--variants', nargs='+')
parser.add_argument('--runtime-image')
parser.add_argument('--live', action='store_true', help=argparse.SUPPRESS)
args = parser.parse_args()
if not 3 <= args.repeats <= 10 or not 1 <= args.seconds <= 30:
    parser.error('repeats must be 3..10 and seconds 1..30')
prepared = json.loads(args.prepared.read_text())
snapshot = Path(prepared['snapshot_directory']) / 'after'
output = args.output.resolve()
profile_dir = args.profile_dir.resolve()


def docker(*values):
    return subprocess.check_output(['docker', *map(str, values)], text=True).strip()


runtime_image = args.runtime_image or prepared['images']['after']
runtime_image_id = docker('image', 'inspect', '-f', '{{.Id}}', runtime_image)


if not args.live:
    for name, image in prepared['images'].items():
        if docker('image', 'inspect', '-f', '{{.Id}}', image) != prepared['image_ids'][name]:
            raise RuntimeError('Prepared image changed: ' + name)
    binaries = (['portfolio-baseline-profile', 'portfolio-fixed-profile', 'portfolio-fixed']
                if args.improvement else ['portfolio-profile', 'portfolio-plain'])
    for name in binaries:
        if not (profile_dir / name).is_file():
            parser.error('Missing diagnostic executable: ' + name)
    original = snapshot / 'backend-forge/benchmark/run_throughput.sh'
    script = original.read_text()
    root_assignment = 'ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)'
    load_command = '"$PYTHON" backend-forge/benchmark/throughput_compare.py'
    required = [root_assignment, load_command, 'forge-throughput-',
                '20261001-throughput', '18211', '18212', '18214']
    if any(token not in script for token in required):
        raise RuntimeError('Unsupported prepared runner; refusing to start resources')
    if script.count(root_assignment) != 1 or script.count(load_command) != 1:
        raise RuntimeError('Ambiguous prepared runner; refusing to start resources')
    script = script.replace(root_assignment, 'ROOT=' + shlex.quote(str(snapshot)))
    script = script.replace('forge-throughput-', 'forge-diagnosis-')
    script = script.replace('20261001-throughput', '20261003-diagnosis')
    for before, after in [('18211', '18311'), ('18212', '18312'), ('18214', '18314')]:
        script = script.replace(before, after)
    invocation = [sys.executable, str(Path(__file__).resolve()), '--live',
                  '--prepared', str(args.prepared.resolve()), '--profile-dir', str(profile_dir),
                  '--output', str(output), '--repeats', str(args.repeats), '--seconds', str(args.seconds)]
    invocation += ['--client-processes', str(args.client_processes)]
    if args.improvement:
        invocation.append('--improvement')
    if args.posts_only:
        invocation.append('--posts-only')
    if args.variants:
        invocation += ['--variants', *args.variants]
    if args.runtime_image:
        invocation += ['--runtime-image', runtime_image]
    script = script.replace(load_command, shlex.join(invocation))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='forge-diagnosis-run-') as directory:
        runner = Path(directory) / 'run.sh'
        runner.write_text(script)
        subprocess.run(['sh', str(runner)], env={**os.environ, 'THROUGHPUT_SKIP_BUILD': '1',
                       'THROUGHPUT_PYTHON': sys.executable}, check=True)
    sys.exit(0)

import aiohttp

spec = importlib.util.spec_from_file_location('portfolio_base', snapshot / 'backend-forge/benchmark/compare.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
base.SERVERS = {
    'rust': ('http://127.0.0.1:18311', 'forge-diagnosis-rust'),
    'original': ('http://127.0.0.1:18314', 'forge-diagnosis-after'),
}
profile_name = 'forge-diagnosis-profile'
profile_created = False


def cpu_state(group):
    value = {key: int(number) for key, number in
             (line.split() for line in (group / 'cpu.stat').read_text().splitlines())}
    value['pressure_some_us'] = int((group / 'cpu.pressure').read_text().splitlines()[0].split('total=')[1])
    return value


def threads(container):
    pid = docker('inspect', '-f', '{{.State.Pid}}', container)
    tasks = Path('/proc') / pid / 'task'
    return [p.read_text().strip() for p in sorted(tasks.glob('*/comm'))]


async def get_json(url):
    async with aiohttp.ClientSession() as session:
        async with session.get(url) as response:
            if response.status != 200:
                raise RuntimeError('Probe failed: ' + url)
            return await response.json()


async def start_profile(config):
    global profile_created
    if profile_created:
        docker('rm', '-f', profile_name)
        profile_created = False
    command = ['create', '--name', profile_name, '--label', 'forge.throughput=20261003-diagnosis',
               '--network', 'forge-diagnosis-20261001', '--cpus', '2', '--memory', '512m',
               '-p', '127.0.0.1:18316:8080', '-v', str(profile_dir) + ':/diagnostic:ro',
               '--entrypoint', '/diagnostic/' + config['binary']]
    variables = {'DATABASE_URL': 'postgres://bench:benchmark-only@forge-diagnosis-db/forge_after',
                 'DATABASE_POOL_SIZE': str(config['pool']), 'FORGE_DIAG_WORKERS': str(config['workers']),
                 'JWT_SECRET': 'benchmark-only-test-secret', 'GITHUB_CLIENT_ID': 'test',
                 'GITHUB_CLIENT_SECRET': 'test', 'ADMIN_GITHUB_USERNAME': 'Helloworld0822',
                 'FRONTEND_URL': 'http://localhost:18316', 'BACKEND_BASE_URL': 'http://localhost:18316',
                 'HOST': '0.0.0.0', 'PORT': '8080', 'RUST_LOG': 'off'}
    if 'threads' in config:
        variables['FORGE_WEB_THREADS'] = config['threads']
    if 'poll' in config:
        variables['FORGE_WEB_POLL'] = config['poll']
    for name, value in variables.items():
        command += ['-e', name + '=' + value]
    docker(*command, runtime_image)
    profile_created = True
    docker('start', profile_name)
    for _ in range(60):
        try:
            await get_json('http://127.0.0.1:18316/api/health')
            break
        except (aiohttp.ClientError, RuntimeError):
            await asyncio.sleep(.25)
    else:
        raise RuntimeError('Diagnostic server did not start')
    base.SERVERS[config['name']] = ('http://127.0.0.1:18316', profile_name)


async def main():
    for name in [profile_name]:
        if subprocess.run(['docker', 'inspect', name], capture_output=True).returncode == 0:
            raise RuntimeError('Refusing existing container: ' + name)
    originals = {}
    if args.runtime_image:
        if docker('inspect', '-f', '{{index .Config.Labels "forge.throughput"}}',
                  'forge-diagnosis-db') != '20261003-diagnosis':
            raise RuntimeError('Refusing schema change outside disposable diagnosis DB')
        for database in ['rust_test', 'forge_before', 'forge_after']:
            docker('exec', 'forge-diagnosis-db', 'psql', '-U', 'bench', '-d', database,
                   '-v', 'ON_ERROR_STOP=1', '-c',
                   'CREATE INDEX IF NOT EXISTS comments_post_id_created_at_idx ON comments(post_id,created_at)')
    for name in ['rust', 'original']:
        if docker('inspect', '-f', '{{index .Config.Labels "forge.throughput"}}', base.SERVERS[name][1]) != '20261003-diagnosis':
            raise RuntimeError('Not a disposable diagnosis container')
        originals[name] = {path: await get_json(base.SERVERS[name][0] + path)
                          for path in ['/api/health', '/api/posts', '/api/projects']}
    def normalized(values):
        import datetime
        rows = []
        for value in values:
            value = dict(value)
            for key in ['created_at', 'updated_at']:
                if isinstance(value.get(key), str):
                    value[key] = datetime.datetime.fromisoformat(value[key].replace('Z', '+00:00'))
            rows.append(value)
        return sorted(rows, key=lambda row: row['id'])
    for path in ['/api/posts', '/api/projects']:
        if normalized(originals['rust'][path]) != normalized(originals['original'][path]):
            raise RuntimeError('Original fixture mismatch: ' + path)
    configs = [
        {'name': 'rust'}, {'name': 'original'},
        {'name': 'rebuilt_plain_8_5', 'binary': 'portfolio-plain', 'workers': 8, 'pool': 5},
        {'name': 'profile_8_5', 'binary': 'portfolio-profile', 'workers': 8, 'pool': 5},
        {'name': 'profile_5_5', 'binary': 'portfolio-profile', 'workers': 5, 'pool': 5},
        {'name': 'profile_8_8', 'binary': 'portfolio-profile', 'workers': 8, 'pool': 8},
    ]
    if args.improvement:
        configs = [{'name': 'rust'},
            {'name': 'profile_baseline_pool', 'binary': 'portfolio-baseline-profile',
             'workers': 8, 'pool': 5, 'threads': 'pool'},
            {'name': 'profile_fixed_pool', 'binary': 'portfolio-fixed-profile',
             'workers': 8, 'pool': 5, 'threads': 'pool'},
            {'name': 'profile_fixed_connection', 'binary': 'portfolio-fixed-profile',
             'workers': 8, 'pool': 5, 'threads': 'connection'},
            {'name': 'plain_fixed_connection', 'binary': 'portfolio-fixed',
             'workers': 8, 'pool': 5, 'threads': 'connection'}]
        if args.variants and 'profile_fixed_pool_poll' in args.variants:
            configs.append({'name': 'profile_fixed_pool_poll', 'binary': 'portfolio-fixed-profile',
                            'workers': 8, 'pool': 5, 'threads': 'pool', 'poll': 'poll'})
    if args.variants:
        selected = set(args.variants)
        if not selected <= {config['name'] for config in configs}:
            raise RuntimeError('Unknown variant')
        configs = [config for config in configs if config['name'] in selected]
    observations = []
    thread_counts = {}
    cases = [('/api/health', 1), ('/api/health', 16), ('/api/posts', 16)]
    if args.improvement:
        cases.append(('/api/posts', 64))
    if args.posts_only:
        cases = [case for case in cases if case[0] == '/api/posts']
    db_group = base.cgroup('forge-diagnosis-db')
    async def load(name, path, clients, seconds):
        if args.client_processes > 1:
            from portfolio_load import trial
            return await trial(base, name, path, clients, seconds, args.client_processes, db_group)
        return await base.trial(name, path, clients, seconds)
    for repeat in range(args.repeats):
        shift = repeat % len(configs)
        order = configs[shift:] + configs[:shift]
        for config in order:
            name = config['name']
            if 'binary' in config:
                await start_profile(config)
                for path in originals['original']:
                    actual = await get_json(base.SERVERS[name][0] + path)
                    expected = originals['original'][path]
                    if path != '/api/health':
                        actual, expected = normalized(actual), normalized(expected)
                    if actual != expected:
                        raise RuntimeError('Profile payload changed: ' + path)
            thread_counts[name] = threads(base.SERVERS[name][1])
            group = base.cgroup(base.SERVERS[name][1])
            for path, clients in cases:
                await load(name, path, clients, 1)
                metric_url = base.SERVERS[name][0] + '/__forge_diagnosis_metrics'
                before_metrics = await get_json(metric_url) if name.startswith('profile_') else None
                server_before, db_before = cpu_state(group), cpu_state(db_group)
                client_before = resource.getrusage(resource.RUSAGE_SELF)
                start = time.monotonic()
                row = await load(name, path, clients, args.seconds)
                elapsed = time.monotonic() - start
                client_after = resource.getrusage(resource.RUSAGE_SELF)
                server_after, db_after = cpu_state(group), cpu_state(db_group)
                row.update(repeat=repeat + 1, config=config)
                if args.client_processes == 1:
                    row.update(client_cpu_core_percent=(client_after.ru_utime + client_after.ru_stime -
                               client_before.ru_utime - client_before.ru_stime) / elapsed * 100,
                           db_cpu_core_percent=(db_after['usage_usec'] - db_before['usage_usec']) / elapsed / 10000,
                           server_cpu_delta={k: server_after[k] - server_before[k] for k in server_before})
                if before_metrics:
                    after_metrics = await get_json(metric_url)
                    if any(value.get('worker_requests', [0])[-1]
                           for value in after_metrics.values() if isinstance(value, dict)):
                        raise RuntimeError('Profiler lifetime thread capacity reached')
                    key = 'health' if path.endswith('health') else 'posts'
                    delta = {k: ([a - b for a, b in zip(after_metrics[key][k], before_metrics[key][k])]
                                 if isinstance(after_metrics[key][k], list)
                                 else after_metrics[key][k] - before_metrics[key][k])
                             for k in after_metrics[key]}
                    if delta['requests'] == 0 or delta['queries'] != delta['requests']:
                        raise RuntimeError('Expected one measured SQL query per public request')
                    row['profile'] = delta
                observations.append(row)
                print(json.dumps(row), flush=True)
    if any(row['errors'] for row in observations):
        raise RuntimeError('Request errors during diagnosis')
    binary_hashes = {config['binary']: hashlib.sha256((profile_dir / config['binary']).read_bytes()).hexdigest()
                     for config in configs if 'binary' in config}
    candidate_source = profile_dir / 'candidate-web.c'
    output.write_text(json.dumps({'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'prepared': prepared, 'configs': configs, 'cases': cases, 'repeats': args.repeats,
        'profile_source_sha256': hashlib.sha256((Path(__file__).parent / 'portfolio_profile.c').read_bytes()).hexdigest(),
        'binary_sha256': binary_hashes,
        'candidate_web_source_sha256': hashlib.sha256(candidate_source.read_bytes()).hexdigest() if candidate_source.exists() else None,
        'client_processes': args.client_processes,
        'runtime_image': runtime_image, 'runtime_image_id': runtime_image_id,
        'load_source_sha256': hashlib.sha256((Path(__file__).parent / 'portfolio_load.py').read_bytes()).hexdigest(),
        'threads': thread_counts, 'results': observations,
        'scope': 'Shared host; same data and ban policy. Instrumented handler intervals include pool wait '
                 'and synchronous libpq calls; exclude MHD queueing and response transmission. '
                 'Compiler/packaging/instrumentation controls included. Client and DB CPU sampled; '
                 'one aiohttp process can constrain throughput.'}, indent=2) + '\n')


try:
    asyncio.run(main())
finally:
    if profile_created:
        docker('rm', '-f', profile_name)
