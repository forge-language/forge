# loadgen

A connection-per-request HTTP load generator for the Forge benchmark suite.
Go standard library only — no third-party modules, so it builds offline.

## Why this exists

The previous generator was Python + `ThreadPoolExecutor` and was GIL-bound at
roughly 8,000 req/s. Every server in the suite is faster than that, so the
generator was the bottleneck and all fast servers measured identically.

This generator sustains **~160,000 req/s** against the C reference server on an
18-core host, which puts the server back on the critical path.

## Workload shape

Every server under test answers with `Connection: close` and closes the socket
after one response — the Forge HTTP stack hardcodes this and has no keep-alive.
Connection-per-request is therefore the only fair shape, and each request is:

1. `dial host:port`
2. write `GET / HTTP/1.1\r\nHost: <host>\r\nConnection: close\r\n\r\n`
3. read until EOF (body discarded)
4. close

A request counts as `ok` only if the response began with `HTTP/1.1 2`. A failed
dial, an empty read, and a non-2xx status are all errors, never successes.

The client is raw `net.Dial` plus hand-written request bytes. `net/http` is
deliberately not used: its transport bookkeeping, response parsing and
connection pooling would land inside the measurement and distort a
connection-per-request benchmark.

## Build

```sh
cd benchmark/loadgen
go build -o loadgen .
```

## Usage

```sh
./loadgen -port 19099 -n 200000 -c 250
./loadgen -port 19099 -n 200000 -c 250 -json    # + one JSON line at the end
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `-host` | `127.0.0.1` | target host |
| `-port` | — | target port (**required**) |
| `-n` | `100000` | requests to measure |
| `-c` | `200` | concurrent connections in flight |
| `-warmup` | `2000` | requests run and discarded before measuring |
| `-timeout` | `5s` | per-request I/O timeout; `0` disables |
| `-json` | off | emit a final machine-readable JSON line |

Exit status is non-zero if no request succeeded, so a dead server fails a
scripted run instead of silently reporting 0 rps.

## Design notes

**Ephemeral port exhaustion.** At these rates the ~28k ephemeral ports
(`net.ipv4.ip_local_port_range`) would be consumed in well under a second, with
each closed socket pinned in `TIME_WAIT` for minutes. Each client socket
therefore sets `SO_LINGER {on, 0}`, so `close()` sends RST and the socket goes
straight to `CLOSED`. Measured effect: 400,000 requests in 2.6 s produced no net
`TIME_WAIT` growth (84 sockets before, 88 after).

**Retries.** Transient kernel back-pressure — `EADDRNOTAVAIL`, `ECONNRESET`,
`EMFILE`, `ENOBUFS`, accept-queue overflow — is retried up to 5 attempts with
exponential backoff from 250 µs. Retries are reported separately from hard
errors so a straining generator is visible rather than silently reported as
server latency.

**Hot path.** `-c` goroutines each pull from one shared atomic counter. There is
no channel per request and no mutex in the request path. Each goroutine
accumulates its own pre-sized latency slice and reuses one read buffer; slices
are merged only after all goroutines have stopped. `GOMAXPROCS` is left at the
default (all cores).

**File descriptors.** The generator raises its own `RLIMIT_NOFILE` soft limit to
the hard limit at startup and warns if the result is still below `-c` + 128. If
you see that warning, raise it with `ulimit -n`.

## Measured curve (C reference server, 18 cores, n=200000)

| `-c` | req/s | p50 | p99 |
| --- | --- | --- | --- |
| 50 | 143,193 | 0.241 ms | 1.373 ms |
| 200 | 160,955 | 1.006 ms | 4.346 ms |
| 500 | 163,678 | 2.725 ms | 8.763 ms |
| 1000 | 154,910 | 5.820 ms | 20.251 ms |

Throughput plateaus at `-c` 200–500 and degrades past that as latency inflates
without adding throughput. **Use `-c 250` for the suite.**

## Caveats for anyone reading the numbers

- **Generator and server share the same 18 cores.** Two generator instances in
  parallel reached 170,676 req/s combined versus 163,678 for one, so a single
  instance already achieves ~96% of the system's ceiling — but that ceiling
  includes CPU spent on the generator itself. These are loopback numbers and
  include no network hardware.
- **Latency covers only the successful attempt.** Time spent on retried
  attempts is reported through the retry counter instead of being folded into
  the percentiles, so percentiles stay a server measurement. Check the retry
  count when interpreting a run.
- **Run-to-run variance is real.** Three identical runs at `-c 200` gave
  144k / 174k / 173k req/s; the first run of a fresh server is typically the
  slowest even with warmup. Take a median of at least 3 runs before comparing
  servers, and treat differences under ~15% as noise.
- **`SO_LINGER 0` means connections are reset, not closed gracefully.** This is
  standard load-generator practice and required for sustained
  connection-per-request load, but a server that logs or meters resets will see
  them. It also means the generator does not exercise a graceful-shutdown path.
- **Percentiles use nearest-rank** on the exact set of successful requests — no
  interpolation, no bucketing, no sampling.
