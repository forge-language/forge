//! Rust concurrency-primitive benchmark, async-task variant: spawn N tokio
//! tasks on the multi-thread runtime, each yields once (tokio::task::yield_now)
//! and then increments a shared atomic counter; await all N JoinHandles.
//!
//! Built offline against the tokio already present in the local cargo
//! registry cache (same 1.53.0 the sibling axum benchmark pins), reusing that
//! project's CARGO_TARGET_DIR so no network access is needed.
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;
use std::time::Instant;

fn main() {
    let n: usize = match std::env::args().nth(1).and_then(|s| s.parse().ok()) {
        Some(v) if v > 0 => v,
        _ => {
            eprintln!("usage: bench_rust_async <n>");
            std::process::exit(2);
        }
    };

    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("failed to build tokio runtime");

    rt.block_on(async move {
        let counter = Arc::new(AtomicI64::new(0));
        let mut handles = Vec::with_capacity(n);

        let t0 = Instant::now();
        for _ in 0..n {
            let c = Arc::clone(&counter);
            handles.push(tokio::spawn(async move {
                tokio::task::yield_now().await;
                c.fetch_add(1, Ordering::Relaxed);
            }));
        }
        for h in handles {
            h.await.expect("task panicked");
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
    });
}
