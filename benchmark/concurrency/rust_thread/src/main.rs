//! Rust concurrency-primitive benchmark, OS-thread variant: spawn N
//! std::thread threads, each calls yield_now() once and then increments a
//! shared atomic counter; join all N.
//!
//! Uses thread::Builder rather than thread::spawn so that hitting the
//! thread-creation limit surfaces as a reported io::Error instead of a panic
//! -- the error text is the interesting part of the result at large N.
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

fn main() {
    let n: usize = match std::env::args().nth(1).and_then(|s| s.parse().ok()) {
        Some(v) if v > 0 => v,
        _ => {
            eprintln!("usage: bench_rust_thread <n>");
            std::process::exit(2);
        }
    };

    let counter = Arc::new(AtomicI64::new(0));
    let mut handles = Vec::with_capacity(n);

    let t0 = Instant::now();
    for i in 0..n {
        let c = Arc::clone(&counter);
        match thread::Builder::new().spawn(move || {
            thread::yield_now();
            c.fetch_add(1, Ordering::Relaxed);
        }) {
            Ok(h) => handles.push(h),
            Err(e) => {
                eprintln!("FAIL: thread spawn failed at thread {} of {}: {}", i, n, e);
                let created = handles.len();
                for h in handles {
                    let _ = h.join();
                }
                eprintln!(
                    "created={} counter={}",
                    created,
                    counter.load(Ordering::Relaxed)
                );
                std::process::exit(1);
            }
        }
    }
    for h in handles {
        h.join().expect("worker thread panicked");
    }
    let elapsed = t0.elapsed();

    let c = counter.load(Ordering::Relaxed);
    println!(
        "counter={} elapsed_ms={:.3}",
        c,
        elapsed.as_secs_f64() * 1000.0
    );
    if c != n as i64 {
        eprintln!("FAIL: counter {} != n {}", c, n);
        std::process::exit(1);
    }
}
