// Go concurrency-primitive benchmark, hold-live variant.
//
// bench_go.go measures spawn-and-retire throughput: goroutines start
// completing while the loop is still spawning, so the peak number alive at
// any instant is far below N. The Forge program it is compared against has
// no choice but to hold all N alive -- `process main` runs to completion
// before fr_scheduler_run() drains anything -- so comparing their peak RSS
// directly measures two different situations and flatters Go by a wide
// margin.
//
// This variant parks every goroutine on a channel receive until all N are
// confirmed alive, samples VmHWM at that instant, and only then releases
// them. That is the number that belongs beside Forge's.
package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// peakRSSKB reads VmHWM (peak resident set size) for this process. Reading
// our own /proc entry keeps the measurement inside the window where every
// goroutine is parked, which an external sampler cannot be trusted to hit.
func peakRSSKB() (int64, error) {
	f, err := os.Open("/proc/self/status")
	if err != nil {
		return 0, err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "VmHWM:") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return 0, fmt.Errorf("malformed VmHWM line: %q", line)
		}
		return strconv.ParseInt(fields[1], 10, 64)
	}
	if err := sc.Err(); err != nil {
		return 0, err
	}
	return 0, fmt.Errorf("VmHWM not found in /proc/self/status")
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: bench_go_live <n>")
		os.Exit(2)
	}
	n, err := strconv.Atoi(os.Args[1])
	if err != nil || n < 1 {
		fmt.Fprintln(os.Stderr, "bad n:", os.Args[1])
		os.Exit(2)
	}

	var live int64
	var counter int64
	var wg sync.WaitGroup
	wg.Add(n)
	gate := make(chan struct{})

	t0 := time.Now()
	for i := 0; i < n; i++ {
		go func() {
			atomic.AddInt64(&live, 1)
			<-gate
			atomic.AddInt64(&counter, 1)
			wg.Done()
		}()
	}

	// Spin until every goroutine has reached its park. Sleeping here would
	// be enough in practice, but a counter makes "all N alive" a checked
	// fact rather than an assumption about scheduler timing.
	for atomic.LoadInt64(&live) < int64(n) {
		time.Sleep(200 * time.Microsecond)
	}
	spawnMs := float64(time.Since(t0).Nanoseconds()) / 1e6

	rssKB, err := peakRSSKB()
	if err != nil {
		fmt.Fprintln(os.Stderr, "peak rss:", err)
		os.Exit(1)
	}

	close(gate)
	wg.Wait()
	totalMs := float64(time.Since(t0).Nanoseconds()) / 1e6

	if c := atomic.LoadInt64(&counter); c != int64(n) {
		fmt.Fprintf(os.Stderr, "FAIL: counter %d != n %d\n", c, n)
		os.Exit(1)
	}
	fmt.Printf("live=%d spawn_ms=%.3f total_ms=%.3f peak_rss_kb=%d bytes_per_task=%.1f\n",
		n, spawnMs, totalMs, rssKB, float64(rssKB*1024)/float64(n))
}
