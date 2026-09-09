// Toolchain devex benchmark: spawn 10000 lightweight units of work
// (goroutines -- Go's own idiomatic lightweight concurrency primitive)
// that each yield once, then join them all via a WaitGroup.
package main

import (
	"fmt"
	"runtime"
	"sync"
)

const workers = 10000

func main() {
	var wg sync.WaitGroup
	wg.Add(workers)
	for i := 0; i < workers; i++ {
		go func() {
			defer wg.Done()
			runtime.Gosched()
		}()
	}
	wg.Wait()
	fmt.Println("done:", workers)
}
