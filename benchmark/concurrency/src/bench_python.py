"""Python concurrency-primitive benchmark: spawn N asyncio tasks, each yields
once to the event loop (await asyncio.sleep(0)) and then increments a shared
counter; wait for all with asyncio.gather.

CPython has no JIT to warm up, but the coroutine objects for all N units are
created inside the timed region, which is where most of the cost lives.
"""
import asyncio
import sys
import time

counter = 0


async def unit() -> None:
    global counter
    await asyncio.sleep(0)  # idiomatic "yield to the event loop"
    counter += 1


async def main(n: int) -> int:
    global counter
    t0 = time.perf_counter()
    await asyncio.gather(*(unit() for _ in range(n)))
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    print(f"counter={counter} elapsed_ms={elapsed_ms:.3f}")
    if counter != n:
        print(f"FAIL: counter {counter} != n {n}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: bench_python.py <n>", file=sys.stderr)
        raise SystemExit(2)
    sys.exit(asyncio.run(main(int(sys.argv[1]))))
