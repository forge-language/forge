// Go concurrency-primitive benchmark: spawn N goroutines, each yields once
// (runtime.Gosched) and then increments a shared atomic counter; wait for all.
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: bench_go <n>")
		os.Exit(2)
	}
	n, err := strconv.Atoi(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, "bad n:", err)
		os.Exit(2)
	}

	var counter int64
	var wg sync.WaitGroup
	wg.Add(n)

	t0 := time.Now()
	for i := 0; i < n; i++ {
		go func() {
			runtime.Gosched()
			atomic.AddInt64(&counter, 1)
			wg.Done()
		}()
	}
	wg.Wait()
	elapsed := time.Since(t0)

	c := atomic.LoadInt64(&counter)
	fmt.Printf("counter=%d elapsed_ms=%.3f\n", c, float64(elapsed.Nanoseconds())/1e6)
	if c != int64(n) {
		fmt.Fprintf(os.Stderr, "FAIL: counter %d != n %d\n", c, n)
		os.Exit(1)
	}
}
