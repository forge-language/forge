# Toolchain devex benchmark: spawn 10000 lightweight units of work that
# each yield once, then join them all. Python's idiomatic lightweight
# concurrency primitive is an asyncio task; asyncio.sleep(0) is the
# canonical "yield once to the event loop" call.
import asyncio

WORKERS = 10000


async def tick():
    await asyncio.sleep(0)


async def main():
    await asyncio.gather(*(tick() for _ in range(WORKERS)))
    print("done:", WORKERS)


asyncio.run(main())
