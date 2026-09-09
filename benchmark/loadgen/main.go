// loadgen is a connection-per-request HTTP load generator for the Forge
// benchmark suite.
//
// Every server under test answers with "Connection: close" and closes the
// socket after a single response, so keep-alive is not an option: one request
// means one fresh TCP connection. The client is raw net.Dial plus hand-written
// request bytes rather than net/http, which keeps transport bookkeeping and
// connection pooling out of the measurement.
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"runtime"
	"sort"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// maxAttempts bounds the retries spent on a single request before it is
// counted as a hard error. Retries exist to ride out transient kernel
// resource exhaustion (ephemeral ports, fd limits, accept-queue overflow),
// not to paper over a server that is genuinely refusing traffic.
const maxAttempts = 5

// okPrefix is the shortest byte string that proves a 2xx response arrived.
var okPrefix = []byte("HTTP/1.1 2")

type config struct {
	host        string
	port        int
	requests    int
	concurrency int
	warmup      int
	timeout     time.Duration
	emitJSON    bool
}

// result holds the outcome of one measured phase.
type result struct {
	ok        int64
	errs      int64
	retries   int64
	elapsed   time.Duration
	latencies []time.Duration // successful requests only
	firstErr  error
}

type report struct {
	Host        string  `json:"host"`
	Port        int     `json:"port"`
	Requested   int     `json:"requested"`
	Concurrency int     `json:"concurrency"`
	OK          int64   `json:"ok"`
	Errors      int64   `json:"errors"`
	Retries     int64   `json:"retries"`
	ElapsedSec  float64 `json:"elapsed_sec"`
	RPS         float64 `json:"rps"`
	P50Ms       float64 `json:"p50_ms"`
	P90Ms       float64 `json:"p90_ms"`
	P99Ms       float64 `json:"p99_ms"`
	MaxMs       float64 `json:"max_ms"`
}

func main() {
	cfg := config{}
	flag.StringVar(&cfg.host, "host", "127.0.0.1", "target host")
	flag.IntVar(&cfg.port, "port", 0, "target port (required)")
	flag.IntVar(&cfg.requests, "n", 100000, "total requests to measure")
	flag.IntVar(&cfg.concurrency, "c", 200, "concurrent connections in flight")
	flag.IntVar(&cfg.warmup, "warmup", 2000, "requests to run and discard before measuring")
	flag.DurationVar(&cfg.timeout, "timeout", 5*time.Second, "per-request I/O timeout; 0 disables")
	flag.BoolVar(&cfg.emitJSON, "json", false, "emit a final machine-readable JSON line")
	flag.Parse()

	if err := validate(&cfg); err != nil {
		fmt.Fprintf(os.Stderr, "loadgen: %v\n", err)
		flag.Usage()
		os.Exit(2)
	}

	softFDs, fdNote := raiseFDLimit(cfg.concurrency)

	target := net.JoinHostPort(cfg.host, fmt.Sprint(cfg.port))
	addr, err := net.ResolveTCPAddr("tcp", target)
	if err != nil {
		fmt.Fprintf(os.Stderr, "loadgen: cannot resolve %s: %v\n", target, err)
		os.Exit(1)
	}
	// Resolve once up front so no request pays for address parsing.
	resolved := addr.String()
	request := []byte("GET / HTTP/1.1\r\nHost: " + cfg.host + "\r\nConnection: close\r\n\r\n")

	if cfg.warmup > 0 {
		w := runPhase(&cfg, resolved, request, cfg.warmup)
		if w.ok == 0 {
			fmt.Fprintf(os.Stderr, "loadgen: warmup got 0 successful responses from %s", resolved)
			if w.firstErr != nil {
				fmt.Fprintf(os.Stderr, " (first error: %v)", w.firstErr)
			}
			fmt.Fprintln(os.Stderr)
			os.Exit(1)
		}
	}

	res := runPhase(&cfg, resolved, request, cfg.requests)
	rep := summarize(&cfg, &res)

	printHuman(&rep, &res, softFDs, fdNote)
	if cfg.emitJSON {
		line, err := json.Marshal(rep)
		if err != nil {
			fmt.Fprintf(os.Stderr, "loadgen: cannot encode JSON report: %v\n", err)
			os.Exit(1)
		}
		fmt.Println(string(line))
	}
	if res.ok == 0 {
		os.Exit(1)
	}
}

func validate(cfg *config) error {
	switch {
	case cfg.port <= 0 || cfg.port > 65535:
		return fmt.Errorf("-port must be in 1..65535, got %d", cfg.port)
	case cfg.requests <= 0:
		return fmt.Errorf("-n must be > 0, got %d", cfg.requests)
	case cfg.concurrency <= 0:
		return fmt.Errorf("-c must be > 0, got %d", cfg.concurrency)
	case cfg.warmup < 0:
		return fmt.Errorf("-warmup must be >= 0, got %d", cfg.warmup)
	case cfg.timeout < 0:
		return fmt.Errorf("-timeout must be >= 0, got %v", cfg.timeout)
	}
	return nil
}

// setLingerZero disables the TIME_WAIT lingering that would otherwise pin one
// ephemeral port per request for minutes. With SO_LINGER {on, 0} the client's
// close() sends RST and the socket goes straight to CLOSED, which is what
// makes sustained connection-per-request load possible on a single host.
func setLingerZero(_, _ string, c syscall.RawConn) error {
	var sockErr error
	ctrlErr := c.Control(func(fd uintptr) {
		sockErr = syscall.SetsockoptLinger(int(fd), syscall.SOL_SOCKET, syscall.SO_LINGER,
			&syscall.Linger{Onoff: 1, Linger: 0})
	})
	if ctrlErr != nil {
		return ctrlErr
	}
	return sockErr
}

// runPhase drives `total` requests across cfg.concurrency goroutines. Work is
// handed out through a single atomic counter: no channel per request and no
// mutex anywhere in the request path. Each goroutine accumulates its own
// latency slice; the slices are merged only after every goroutine has stopped.
func runPhase(cfg *config, resolved string, request []byte, total int) result {
	workers := cfg.concurrency
	if workers > total {
		workers = total
	}

	dialer := &net.Dialer{Control: setLingerZero, Timeout: cfg.timeout}

	var issued atomic.Int64
	var okCount, errCount, retryCount atomic.Int64

	perWorker := make([][]time.Duration, workers)
	firstErrs := make([]error, workers)

	// Generous per-worker capacity so the hot path never reallocates: the
	// atomic counter hands out work unevenly by design, so a worker can
	// legitimately claim more than its even share.
	capHint := total/workers + total/(workers*2) + 64

	var wg sync.WaitGroup
	wg.Add(workers)
	start := time.Now()
	for w := 0; w < workers; w++ {
		go func(id int) {
			defer wg.Done()
			lats := make([]time.Duration, 0, capHint)
			buf := make([]byte, 8192)
			var myOK, myErr, myRetry int64
			var myFirstErr error

			for {
				if issued.Add(1) > int64(total) {
					break
				}
				d, retries, err := doRequest(dialer, resolved, request, buf, cfg.timeout)
				myRetry += retries
				if err != nil {
					myErr++
					if myFirstErr == nil {
						myFirstErr = err
					}
					continue
				}
				myOK++
				lats = append(lats, d)
			}

			perWorker[id] = lats
			firstErrs[id] = myFirstErr
			okCount.Add(myOK)
			errCount.Add(myErr)
			retryCount.Add(myRetry)
		}(w)
	}
	wg.Wait()
	elapsed := time.Since(start)

	res := result{
		ok:      okCount.Load(),
		errs:    errCount.Load(),
		retries: retryCount.Load(),
		elapsed: elapsed,
	}
	merged := make([]time.Duration, 0, res.ok)
	for _, lats := range perWorker {
		merged = append(merged, lats...)
	}
	res.latencies = merged
	for _, err := range firstErrs {
		if err != nil {
			res.firstErr = err
			break
		}
	}
	return res
}

// doRequest performs one dial/write/read-to-EOF/close cycle. The returned
// duration covers only the attempt that succeeded; time burned on retried
// attempts is reported through the retry count instead, so that transient
// kernel back-pressure on the generator does not silently inflate the
// server's latency percentiles.
func doRequest(dialer *net.Dialer, resolved string, request, buf []byte, timeout time.Duration) (time.Duration, int64, error) {
	var retries int64
	backoff := 250 * time.Microsecond

	for attempt := 0; attempt < maxAttempts; attempt++ {
		start := time.Now()
		conn, err := dialer.Dial("tcp", resolved)
		if err != nil {
			if retryable(err) && attempt < maxAttempts-1 {
				retries++
				time.Sleep(backoff)
				backoff *= 2
				continue
			}
			return 0, retries, err
		}

		d, err := exchange(conn, request, buf, timeout, start)
		if err == nil {
			return d, retries, nil
		}
		if retryable(err) && attempt < maxAttempts-1 {
			retries++
			time.Sleep(backoff)
			backoff *= 2
			continue
		}
		return 0, retries, err
	}
	return 0, retries, errors.New("exhausted retries")
}

// exchange writes the request, drains the response until the server closes,
// and reports how long that took. Success requires a 2xx status line: a failed
// dial, an empty read, or a non-2xx status is not a success.
func exchange(conn net.Conn, request, buf []byte, timeout time.Duration, start time.Time) (time.Duration, error) {
	defer conn.Close()

	if timeout > 0 {
		if err := conn.SetDeadline(start.Add(timeout)); err != nil {
			return 0, err
		}
	}
	if _, err := conn.Write(request); err != nil {
		return 0, err
	}

	// Keep the first bytes of the status line so it can be validated even if
	// the response arrives split across several reads.
	var head [len("HTTP/1.1 2")]byte
	headLen := 0
	total := 0

	for {
		n, err := conn.Read(buf)
		if n > 0 {
			total += n
			if headLen < len(head) {
				headLen += copy(head[headLen:], buf[:n])
			}
		}
		if err != nil {
			if err == io.EOF {
				break
			}
			return 0, err
		}
		if n == 0 {
			break
		}
	}
	elapsed := time.Since(start)

	if total == 0 {
		return 0, errors.New("empty response")
	}
	if headLen < len(head) || string(head[:]) != string(okPrefix) {
		return 0, fmt.Errorf("bad status line %q", trimStatus(head[:headLen]))
	}
	return elapsed, nil
}

func trimStatus(b []byte) string {
	for i, c := range b {
		if c == '\r' || c == '\n' {
			return string(b[:i])
		}
	}
	return string(b)
}

// retryable reports whether an error is transient kernel back-pressure that a
// short backoff can clear, rather than a genuine server failure.
func retryable(err error) bool {
	for _, e := range []syscall.Errno{
		syscall.EADDRNOTAVAIL, // ephemeral ports exhausted
		syscall.EADDRINUSE,
		syscall.ECONNRESET,
		syscall.ECONNREFUSED, // accept queue overflow
		syscall.ECONNABORTED,
		syscall.EMFILE, // per-process fd limit
		syscall.ENFILE, // system-wide fd limit
		syscall.ENOBUFS,
		syscall.EAGAIN,
		syscall.EPIPE,
		syscall.ETIMEDOUT,
	} {
		if errors.Is(err, e) {
			return true
		}
	}
	var nerr net.Error
	if errors.As(err, &nerr) && nerr.Timeout() {
		return true
	}
	return false
}

// raiseFDLimit lifts the soft fd limit toward the hard limit, since one
// in-flight request costs one descriptor. It returns the resulting soft limit
// and a note when that limit is too low for the requested concurrency.
func raiseFDLimit(concurrency int) (uint64, string) {
	var lim syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim); err != nil {
		return 0, ""
	}
	if lim.Cur < lim.Max {
		raised := lim
		raised.Cur = raised.Max
		if err := syscall.Setrlimit(syscall.RLIMIT_NOFILE, &raised); err == nil {
			lim = raised
		}
	}
	// Headroom for the runtime's own descriptors and for sockets still
	// unwinding in the kernel.
	if need := uint64(concurrency) + 128; lim.Cur < need {
		return lim.Cur, fmt.Sprintf("WARNING: fd limit %d is below the %d needed for -c %d; raise it with `ulimit -n %d`",
			lim.Cur, need, concurrency, need)
	}
	return lim.Cur, ""
}

func summarize(cfg *config, res *result) report {
	sorted := res.latencies
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })

	secs := res.elapsed.Seconds()
	rps := 0.0
	if secs > 0 {
		rps = float64(res.ok) / secs
	}
	return report{
		Host:        cfg.host,
		Port:        cfg.port,
		Requested:   cfg.requests,
		Concurrency: cfg.concurrency,
		OK:          res.ok,
		Errors:      res.errs,
		Retries:     res.retries,
		ElapsedSec:  secs,
		RPS:         rps,
		P50Ms:       percentileMs(sorted, 0.50),
		P90Ms:       percentileMs(sorted, 0.90),
		P99Ms:       percentileMs(sorted, 0.99),
		MaxMs:       percentileMs(sorted, 1.0),
	}
}

// percentileMs returns the nearest-rank percentile of an ascending slice.
func percentileMs(sorted []time.Duration, p float64) float64 {
	if len(sorted) == 0 {
		return 0
	}
	idx := int(p*float64(len(sorted))+0.5) - 1
	if idx < 0 {
		idx = 0
	}
	if idx >= len(sorted) {
		idx = len(sorted) - 1
	}
	return float64(sorted[idx].Nanoseconds()) / 1e6
}

func printHuman(rep *report, res *result, softFDs uint64, fdNote string) {
	fmt.Printf("target        %s:%d\n", rep.Host, rep.Port)
	fmt.Printf("concurrency   %d (GOMAXPROCS %d, fd limit %d)\n", rep.Concurrency, runtime.GOMAXPROCS(0), softFDs)
	fmt.Printf("ok            %d / %d\n", rep.OK, rep.Requested)
	fmt.Printf("hard errors   %d\n", rep.Errors)
	fmt.Printf("retries       %d\n", rep.Retries)
	fmt.Printf("elapsed       %.3f s\n", rep.ElapsedSec)
	fmt.Printf("requests/sec  %.0f\n", rep.RPS)
	fmt.Printf("latency       p50 %.3f ms  p90 %.3f ms  p99 %.3f ms  max %.3f ms\n",
		rep.P50Ms, rep.P90Ms, rep.P99Ms, rep.MaxMs)
	if res.firstErr != nil {
		fmt.Printf("first error   %v\n", res.firstErr)
	}
	if fdNote != "" {
		fmt.Println(fdNote)
	}
}
