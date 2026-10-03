#!/usr/bin/env python3
"""Synchronized process load generators for disposable portfolio benchmarks."""
import asyncio
import json
from pathlib import Path
import resource
import sys
import time

import aiohttp


def cpu_state(group):
    value = {key: int(number) for key, number in
             (line.split() for line in (group / 'cpu.stat').read_text().splitlines())}
    value['pressure_some_us'] = int((group / 'cpu.pressure').read_text().splitlines()[0].split('total=')[1])
    return value


async def worker(url, concurrency):
    latencies, errors = [], 0
    async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(limit=concurrency),
            timeout=aiohttp.ClientTimeout(total=10), auto_decompress=False) as session:
        print('ready', flush=True)
        schedule = json.loads(sys.stdin.readline())
        start, seconds = schedule['start'], schedule['seconds']
        await asyncio.sleep(max(0, start - time.monotonic()))
        actual_start = time.monotonic()
        before = resource.getrusage(resource.RUSAGE_SELF)

        async def request_loop():
            nonlocal errors
            while time.monotonic() < start + seconds:
                tick = time.monotonic()
                try:
                    async with session.get(url, headers={'Accept-Encoding': 'identity'}) as response:
                        body = await response.read()
                        if response.status != 200 or not body:
                            errors += 1
                except (aiohttp.ClientError, asyncio.TimeoutError):
                    errors += 1
                latencies.append((time.monotonic() - tick) * 1000)

        await asyncio.gather(*(request_loop() for _ in range(concurrency)))
        finished = time.monotonic()
        after = resource.getrusage(resource.RUSAGE_SELF)
        result = {'latencies': latencies, 'errors': errors, 'finished': finished,
                  'start_lateness_ms': max(0, actual_start - start) * 1000,
                  'client_cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime}
    print(json.dumps(result), flush=True)


async def trial(base, name, path, concurrency, seconds, processes, db_group):
    url, container = base.SERVERS[name]
    group = base.cgroup(container)
    pid = base.docker('inspect', '-f', '{{.State.Pid}}', container)
    processes = min(processes, concurrency)
    children, observer = [], None
    try:
        for i in range(processes):
            clients = concurrency // processes + (i < concurrency % processes)
            child = await asyncio.create_subprocess_exec(sys.executable, str(Path(__file__).resolve()),
                '--worker', url + path, str(clients), stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            children.append(child)
        for child in children:
            if await asyncio.wait_for(child.stdout.readline(), 10) != b'ready\n':
                raise RuntimeError('Load worker failed to initialize')
        observed_start = time.monotonic()
        before, db_before = cpu_state(group), cpu_state(db_group)
        start = time.monotonic() + .1
        schedule = (json.dumps({'start': start, 'seconds': seconds}) + '\n').encode()
        for child in children:
            child.stdin.write(schedule)
            await child.stdin.drain()
            child.stdin.close()
        memory, thread_counts = [], []

        async def observe():
            while True:
                memory.append(int((group / 'memory.current').read_text()))
                thread_counts.append(len(list((Path('/proc') / pid / 'task').iterdir())))
                await asyncio.sleep(.2)

        observer = asyncio.create_task(observe())
        outputs = await asyncio.wait_for(asyncio.gather(*(child.stdout.read() for child in children)),
                                         seconds + 15)
        for child in children:
            if await child.wait() != 0:
                raise RuntimeError('Load worker failed: ' + (await child.stderr.read()).decode())
        after, db_after = cpu_state(group), cpu_state(db_group)
        observed_seconds = time.monotonic() - observed_start
        values = [json.loads(output) for output in outputs]
        elapsed = max(value['finished'] for value in values) - start
        latencies = sorted(value for row in values for value in row['latencies'])
        if not latencies or elapsed <= 0:
            raise RuntimeError('Empty measurement')

        def percentile(p):
            return latencies[max(0, min(len(latencies) - 1, int(len(latencies) * p) - 1))]

        cpu_times = [value['client_cpu_seconds'] for value in values]
        return {'language': name, 'endpoint': path, 'concurrency': concurrency,
                'seconds': round(elapsed, 3), 'requests': len(latencies),
                'errors': sum(value['errors'] for value in values),
                'rps': round(len(latencies) / elapsed, 2),
                'mean_ms': round(sum(latencies) / len(latencies), 3),
                'p50_ms': round(percentile(.5), 3), 'p95_ms': round(percentile(.95), 3),
                'p99_ms': round(percentile(.99), 3),
                'cpu_core_percent': (after['usage_usec'] - before['usage_usec']) / observed_seconds / 10000,
                'db_cpu_core_percent': (db_after['usage_usec'] - db_before['usage_usec']) / observed_seconds / 10000,
                'cpu_observation_seconds': observed_seconds,
                'client_start_lateness_ms': [value['start_lateness_ms'] for value in values],
                'client_cpu_core_percent': sum(cpu_times) / elapsed * 100,
                'client_process_cpu_core_percent': [value / elapsed * 100 for value in cpu_times],
                'client_processes': processes, 'peak_threads': max(thread_counts, default=0),
                'memory_mib': max(memory, default=0) / 1024 ** 2,
                'server_cpu_delta': {k: after[k] - before[k] for k in before}}
    finally:
        if observer is not None:
            observer.cancel()
            await asyncio.gather(observer, return_exceptions=True)
        for child in children:
            if child.returncode is None:
                child.kill()
        await asyncio.gather(*(child.wait() for child in children))


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] != '--worker':
        raise SystemExit('Internal benchmark worker; invoke portfolio_diagnose.py instead')
    asyncio.run(worker(sys.argv[2], int(sys.argv[3])))
