// Toolchain devex benchmark: spawn 10000 lightweight units of work that
// each yield once, then join them all. Node is single-threaded, so its
// idiomatic "lightweight unit of concurrent work" is an async function
// scheduled on the event loop; setImmediate() is the yield point (it
// defers to the next turn of the event loop, same shape as a coroutine
// yield).
const WORKERS = 10000;

function tick() {
    return new Promise((resolve) => setImmediate(resolve));
}

async function main() {
    const tasks = [];
    for (let i = 0; i < WORKERS; i++) {
        tasks.push(tick());
    }
    await Promise.all(tasks);
    console.log("done:", WORKERS);
}

main();
