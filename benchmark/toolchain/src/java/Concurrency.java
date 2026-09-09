// Toolchain devex benchmark: spawn 10000 lightweight units of work that
// each yield once, then join them all. JDK 17 (this benchmark's target)
// predates stable virtual threads (JEP 444 landed in JDK 21), so plain
// platform java.lang.Thread with a small stack size stands in as the
// idiomatic "unit of concurrent work" available on this JDK.
public class Concurrency {
    static final int WORKERS = 10000;

    public static void main(String[] args) throws InterruptedException {
        Thread[] threads = new Thread[WORKERS];
        for (int i = 0; i < WORKERS; i++) {
            threads[i] = new Thread(null, () -> Thread.yield(), "w", 256 * 1024);
            threads[i].start();
        }
        for (Thread t : threads) {
            t.join();
        }
        System.out.println("done: " + WORKERS);
    }
}
