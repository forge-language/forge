// Node.js concurrency-primitive benchmark: spawn N async functions, each
// suspends once at an `await` (one microtask turn) and then increments a
// shared counter; wait for all with Promise.all.
//
// Node's JIT warmup is NOT excluded: the timer starts just before the spawn
// loop, but V8 has not yet tiered up the unit function at that point. Noted
// in the README as an accepted asymmetry.
const n = Number.parseInt(process.argv[2], 10);
if (!Number.isInteger(n) || n <= 0) {
  console.error("usage: bench_node.mjs <n>");
  process.exit(2);
}

let counter = 0;

async function unit() {
  await null; // genuine suspension: resumes on the microtask queue
  counter += 1;
}

const t0 = process.hrtime.bigint();
const pending = new Array(n);
for (let i = 0; i < n; i++) pending[i] = unit();
await Promise.all(pending);
const elapsedMs = Number(process.hrtime.bigint() - t0) / 1e6;

console.log(`counter=${counter} elapsed_ms=${elapsedMs.toFixed(3)}`);
if (counter !== n) {
  console.error(`FAIL: counter ${counter} != n ${n}`);
  process.exit(1);
}
