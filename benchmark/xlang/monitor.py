#!/usr/bin/env python3
"""Sample a server's resource use for the duration of a load run.

Walks the process tree rooted at a given PID, so a server that forks workers
(Node cluster) is accounted for as a whole rather than only its parent, and a
server that uses threads (C, Forge, Go, Rust) is accounted for correctly
because threads share their process's RSS and CPU counters.

Tracks peak summed RSS and total CPU jiffies consumed, writing JSON on
SIGTERM. CPU comes from /proc/<pid>/stat utime+stime rather than `ps %cpu`,
because ps averages over the process's whole lifetime and so dilutes the
load-phase figure with idle startup time.

Two properties matter for this to be an instrument rather than a participant:

  * It must cost little. Discovering the tree means scanning every entry in
    /proc, which on a busy machine is thousands of stat reads. Doing that on
    every 100 ms tick cost about 0.4 of a core -- a fifth of what the servers
    under test consume -- so the monitor was perturbing its own measurement.
    Server topology is fixed after startup (Node forks its workers, then
    stops), so the tree is rediscovered once a second and the fast tick reads
    only the pids already known to belong to it.

  * It must always terminate. Waiting for a target that never appears used to
    spin here forever at 20 ms per iteration, scanning all of /proc each time;
    one such orphan survived its suite and sat at 36% of a core for seventeen
    minutes, quietly taxing every later measurement. The wait is now bounded.

Usage: monitor.py <root-pid> <out.json>
"""
import json, os, signal, sys, time

ROOT = int(sys.argv[1])
OUT = sys.argv[2]
HZ = os.sysconf("SC_CLK_TCK")

# How long to wait for the target to show up before giving up and writing an
# empty sample. Anything longer is an orphan in the making.
STARTUP_GRACE_S = 5.0
# Topology refresh period. Long relative to the tick because the thing it
# catches (a forked worker pool) happens once, at startup.
RETREE_S = 1.0
TICK_S = 0.1

peak_rss_kb = 0
peak_procs = 0
cpu_first = None
cpu_last = 0
samples = 0
running = True


def stop(signum, frame):
    global running
    running = False


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)


def read_stat(pid):
    """Return (ppid, utime+stime) for pid, or None if it vanished."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            data = fh.read()
        # comm may contain spaces and parens, so parse after the LAST ')'.
        # Remaining fields start at field 3 (state), so ppid is index 1
        # and utime/stime are indices 11 and 12.
        rest = data[data.rindex(b")") + 2:].split()
        return int(rest[1]), int(rest[11]) + int(rest[12])
    except (OSError, ValueError, IndexError):
        return None


def read_rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status", "rb") as fh:
            for line in fh:
                if line.startswith(b"VmRSS:"):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return 0


def discover_tree(root):
    """All live PIDs in the tree rooted at `root`, root included.

    Costs one pass over /proc, hence the caller's refresh interval.
    """
    children = {}
    live = set()
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        st = read_stat(entry)
        if st is None:
            continue
        live.add(int(entry))
        children.setdefault(st[0], []).append(int(entry))
    if root not in live:
        return []
    found, stack = [], [root]
    seen = set()
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        found.append(pid)
        stack.extend(children.get(pid, []))
    return found


tree = []
last_retree = 0.0
deadline = time.monotonic() + STARTUP_GRACE_S

while running:
    now = time.monotonic()
    if now - last_retree >= RETREE_S:
        fresh = discover_tree(ROOT)
        last_retree = now
        if fresh:
            tree = fresh
        elif samples:
            break          # target finished; we have what we came for
        elif now >= deadline:
            break          # target never appeared; do not become an orphan
    if not tree:
        time.sleep(TICK_S)
        continue

    # Fast path: only the pids already known to be in the tree.
    rss_kb = 0
    cpu_j = 0
    alive = 0
    for pid in tree:
        st = read_stat(pid)
        if st is None:
            continue
        alive += 1
        cpu_j += st[1]
        rss_kb += read_rss_kb(pid)
    if alive:
        peak_rss_kb = max(peak_rss_kb, rss_kb)
        peak_procs = max(peak_procs, alive)
        if cpu_first is None:
            cpu_first = cpu_j
        cpu_last = max(cpu_last, cpu_j)
        samples += 1
    time.sleep(TICK_S)

with open(OUT, "w") as fh:
    json.dump({
        "peak_rss_mb": round(peak_rss_kb / 1024.0, 1),
        "peak_procs": peak_procs,
        "cpu_seconds": round((cpu_last - (cpu_first or 0)) / HZ, 2),
        "samples": samples,
    }, fh)
