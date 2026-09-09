// Toolchain devex benchmark: spawn 10000 lightweight units of work that
// each yield once, then join them all. Plain std::thread is used (not an
// async runtime) so this is a single-file rustc build with no extra
// crates to download/compile, keeping the compile-time comparison honest.
use std::thread;

const WORKERS: usize = 10000;

fn main() {
    let mut handles = Vec::with_capacity(WORKERS);
    for _ in 0..WORKERS {
        handles.push(thread::spawn(|| {
            thread::yield_now();
        }));
    }
    for h in handles {
        h.join().unwrap();
    }
    println!("done: {}", WORKERS);
}
