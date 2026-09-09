// Minimal HTTP server for Forge vs Go benchmark.
//
// Stdlib only (no third-party imports) so `go build` works with no network
// access. Two response paths, selected by MODE:
//
//	MODE=nethttp (default) - idiomatic net/http with a Handler. This is
//	    what a real Go team would ship.
//	MODE=raw                - raw net.Listener accept loop, one goroutine
//	    per connection, hand-written response bytes. This is the
//	    apples-to-apples match for the C and Forge servers, which do no
//	    HTTP parsing beyond a single recv().
//
// Both modes bind 127.0.0.1:$PORT and reply to every request with exactly:
//
//	HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nHello, World
//
// then close the connection. No keep-alive, matching the rest of the suite
// (the Forge HTTP stack hardcodes Connection: close and has no keep-alive).
package main

import (
	"context"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"runtime"
	"strconv"
	"syscall"
	"time"
)

const (
	defaultPort = "19085"
	body        = "Hello, World"
)

// SO_REUSEPORT is 15 on every Linux architecture (see
// asm-generic/socket.h), but Go's syscall package only defines the
// SO_REUSEPORT constant for some GOARCH values (arm64, ppc64, ...) and
// omits it for amd64. Define it ourselves rather than depend on
// golang.org/x/sys/unix, which is unavailable in this offline build.
const soReusePort = 0xf

func main() {
	// Forge and Go both claim an M:N work-stealing scheduler; make sure Go
	// actually gets to use every core rather than whatever GOMAXPROCS the
	// environment happened to set.
	cores := runtime.NumCPU()
	runtime.GOMAXPROCS(cores)

	port := os.Getenv("PORT")
	if port == "" {
		port = defaultPort
	}
	mode := os.Getenv("MODE")
	if mode == "" {
		mode = "nethttp"
	}

	switch mode {
	case "nethttp":
		runNetHTTP(port, cores)
	case "raw":
		runRaw(port, cores)
	default:
		fmt.Fprintf(os.Stderr, "unknown MODE %q (want nethttp or raw)\n", mode)
		os.Exit(1)
	}
}

// ---------------------------------------------------------------------
// MODE=nethttp: idiomatic net/http server.
// ---------------------------------------------------------------------

func runNetHTTP(port string, cores int) {
	addr := "127.0.0.1:" + port

	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		// net/http's ResponseWriter.Header doc: "To suppress automatic
		// response headers (such as Date), set their value to nil."
		// Without this, net/http always injects "Date" and, absent an
		// explicit Content-Type, sniffs and injects one too - setting
		// both to nil (present-but-empty, so no line is written at all,
		// per Header.writeSubset) gets MODE=nethttp byte-identical to
		// the C/Python/Rust/Forge/raw servers on the wire.
		h["Date"] = nil
		h["Content-Type"] = nil
		h.Set("Content-Length", strconv.Itoa(len(body)))
		h.Set("Connection", "close")
		w.WriteHeader(http.StatusOK)
		io.WriteString(w, body)
	})

	srv := &http.Server{
		Addr:    addr,
		Handler: handler,
		// Every response asks the client to close, but be explicit that
		// the server itself will not keep connections idle either.
		IdleTimeout: 1 * time.Millisecond,
	}
	// SetKeepAlivesEnabled(false) makes net/http itself send
	// "Connection: close" and drop the connection after each response,
	// reinforcing the per-request Header().Set above.
	srv.SetKeepAlivesEnabled(false)

	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("listen: %v", err)
	}

	// With the h["Date"]/h["Content-Type"] = nil trick above, MODE=nethttp
	// is byte-identical on the wire to the C/Python/Rust/Forge/raw-Go
	// servers - verified with a raw-socket client, not curl (curl/browsers
	// hide this). This does cost something real: every response now pays
	// for a nil-map header lookup net/http wouldn't otherwise do, and a
	// production server would almost never suppress Date. Treat this mode
	// as "net/http with the wire made comparable," not "how Go people
	// actually configure net/http."
	fmt.Printf("Go net/http benchmark server on port %s (%d cores, GOMAXPROCS=%d)\n",
		port, cores, runtime.GOMAXPROCS(0))
	os.Stdout.Sync()

	log.Fatal(srv.Serve(ln))
}

// ---------------------------------------------------------------------
// MODE=raw: hand-written response bytes over a raw accept loop.
// ---------------------------------------------------------------------

var rawResponse = []byte("HTTP/1.1 200 OK\r\n" +
	"Content-Length: 12\r\n" +
	"Connection: close\r\n\r\n" +
	"Hello, World")

func runRaw(port string, cores int) {
	addr := "127.0.0.1:" + port

	// One SO_REUSEPORT listener per core, mirroring how the C and Forge
	// servers scale accept() across cores at the kernel level, rather
	// than fanning a single listener's connections out to workers in
	// userspace. Each accepted connection is still handled on its own
	// goroutine (per the spec), so this is: N kernel-level accept queues
	// (one per core) feeding Go's regular one-goroutine-per-connection
	// model.
	listeners := make([]net.Listener, 0, cores)
	for i := 0; i < cores; i++ {
		ln, err := listenReusePort(addr)
		if err != nil {
			log.Fatalf("listen (reuseport #%d): %v", i, err)
		}
		listeners = append(listeners, ln)
	}

	fmt.Printf("Go raw benchmark server on port %s (%d cores, %d SO_REUSEPORT listeners, GOMAXPROCS=%d)\n",
		port, cores, len(listeners), runtime.GOMAXPROCS(0))
	os.Stdout.Sync()

	for _, ln := range listeners {
		go acceptLoop(ln)
	}
	select {} // block forever; acceptLoop goroutines run for the process lifetime
}

func acceptLoop(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			continue
		}
		go serveRaw(conn)
	}
}

func serveRaw(conn net.Conn) {
	defer conn.Close()
	buf := make([]byte, 4096)
	// Best-effort read of the request, exactly like the C server's single
	// recv() - no parsing, no loop for slow/partial requests.
	_, _ = conn.Read(buf)
	_, _ = conn.Write(rawResponse)
}

// listenReusePort opens a TCP listener on addr with SO_REUSEPORT set on
// the socket before bind(2), so multiple listeners can share the same
// port and the kernel load-balances incoming connections across them.
func listenReusePort(addr string) (net.Listener, error) {
	lc := net.ListenConfig{
		Control: func(_, _ string, c syscall.RawConn) error {
			var sockErr error
			ctrlErr := c.Control(func(fd uintptr) {
				sockErr = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, soReusePort, 1)
			})
			if ctrlErr != nil {
				return ctrlErr
			}
			return sockErr
		},
	}
	return lc.Listen(context.Background(), "tcp", addr)
}
