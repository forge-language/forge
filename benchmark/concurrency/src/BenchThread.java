/* Java concurrency-primitive benchmark on JDK 17: PLATFORM threads.
 *
 * JDK 17 has no virtual threads (JEP 444 landed in JDK 21), so this measures
 * OS threads, not a lightweight primitive. That asymmetry is disclosed in the
 * README and the results file -- Java here belongs with C and Rust-thread,
 * not with Forge/Go/Node/Python.
 *
 * Each thread calls Thread.yield() once and then increments a shared
 * AtomicLong; the main thread joins all N. */
import java.util.concurrent.atomic.AtomicLong;

public class BenchThread {
    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("usage: BenchThread <n>");
            System.exit(2);
        }
        int n = Integer.parseInt(args[0]);
        AtomicLong counter = new AtomicLong();

        Thread[] threads;
        try {
            threads = new Thread[n];
        } catch (OutOfMemoryError e) {
            System.err.println("FAIL: could not allocate Thread[" + n + "]: " + e);
            System.exit(1);
            return;
        }

        long t0 = System.nanoTime();
        int created = 0;
        try {
            for (int i = 0; i < n; i++) {
                threads[i] = new Thread(() -> {
                    Thread.yield();
                    counter.incrementAndGet();
                });
                threads[i].start();
                created++;
            }
        } catch (OutOfMemoryError e) {
            /* "unable to create native thread" -- a result, not a crash. */
            System.err.println("FAIL: thread creation failed at " + created + " of " + n + ": " + e);
            joinAll(threads, created);
            System.err.println("created=" + created + " counter=" + counter.get());
            System.exit(1);
            return;
        }
        joinAll(threads, created);
        long elapsed = System.nanoTime() - t0;

        long c = counter.get();
        System.out.printf("counter=%d elapsed_ms=%.3f%n", c, elapsed / 1e6);
        if (c != n) {
            System.err.println("FAIL: counter " + c + " != n " + n);
            System.exit(1);
        }
    }

    private static void joinAll(Thread[] threads, int count) {
        for (int i = 0; i < count; i++) {
            try {
                threads[i].join();
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
            }
        }
    }
}
