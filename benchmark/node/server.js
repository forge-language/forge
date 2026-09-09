#!/usr/bin/env node
// Minimal HTTP server for Forge vs Node.js benchmark.
//
// Core modules only, no npm install / dependencies. Binds 127.0.0.1:$PORT
// and replies to every request with exactly:
//
//   HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nHello, World
//
// then closes the connection. No keep-alive, matching the rest of the
// suite (the Forge HTTP stack hardcodes Connection: close and has no
// keep-alive).
//
// By default uses the `cluster` module to fork one worker process per CPU
// core, since Node is single-threaded per process and would otherwise use
// only 1 of the available cores. Set CLUSTER=0 to run single-process
// instead (worth comparing against, since that's Node's textbook
// single-threaded event-loop shape).
//
// Each worker uses the `net` module with a raw accept loop and
// hand-written response bytes, to match the rest of the suite rather than
// letting the `http` module parse/serialize for us. Set MODE=http to get
// an idiomatic `http.createServer` variant instead (for comparison; see
// the comment near startHttpServer below for how its wire bytes are kept
// identical too).

'use strict';

const cluster = require('cluster');
const net = require('net');
const http = require('http');
const os = require('os');
const fs = require('fs');

// console.log's writes to a pipe are asynchronous on POSIX (Node docs:
// stdout/stderr "may be synchronous or not depending on ... the OS");
// the harness needs the ready banner flushed immediately, so use a
// synchronous write for it instead of relying on the event loop.
function printReady(line) {
  fs.writeSync(1, line + '\n');
}

const PORT = parseInt(process.env.PORT || '19086', 10);
const HOST = '127.0.0.1';
const MODE = process.env.MODE || 'raw';
const CLUSTER = process.env.CLUSTER === undefined ? '1' : process.env.CLUSTER;
const useCluster = CLUSTER !== '0';

const BODY = 'Hello, World';
const RAW_RESPONSE = Buffer.from(
  'HTTP/1.1 200 OK\r\n' +
    'Content-Length: ' + Buffer.byteLength(BODY) + '\r\n' +
    'Connection: close\r\n\r\n' +
    BODY,
  'ascii'
);

function startRawServer() {
  const server = net.createServer({ pauseOnConnect: false }, (socket) => {
    socket.on('error', () => {}); // ignore ECONNRESET etc. from clients that
                                  // close before we finish writing
    // No parsing at all, matching the C/Forge/Go-raw servers: read
    // whatever the client sent (we don't even need the contents) and
    // write the fixed response, then close.
    socket.once('data', () => {
      socket.end(RAW_RESPONSE);
    });
  });
  server.on('error', (err) => {
    console.error(`worker ${process.pid} server error:`, err);
  });
  server.listen(PORT, HOST);
  return server;
}

// Node's `http` module always adds a "Date" header to every response
// unless the (documented but easy-to-miss) `response.sendDate` flag is
// set to false before headers go out - do that so MODE=http is
// byte-identical on the wire to the raw/C/Python/Rust/Forge servers too,
// verified with a raw-socket client. A real Go/Node team would almost
// never disable Date; this is "http module with the wire made
// comparable," not how you'd configure it in production.
function startHttpServer() {
  const server = http.createServer((req, res) => {
    res.sendDate = false;
    res.setHeader('Content-Length', Buffer.byteLength(BODY));
    res.setHeader('Connection', 'close');
    res.writeHead(200);
    res.end(BODY);
  });
  server.keepAliveTimeout = 0;
  server.on('error', (err) => {
    console.error(`worker ${process.pid} server error:`, err);
  });
  server.listen(PORT, HOST);
  return server;
}

function startServer() {
  return MODE === 'http' ? startHttpServer() : startRawServer();
}

if (useCluster && cluster.isPrimary) {
  const cores = os.cpus().length;
  for (let i = 0; i < cores; i++) {
    cluster.fork();
  }
  cluster.on('exit', (worker, code, signal) => {
    console.error(`worker ${worker.process.pid} exited (code=${code} signal=${signal})`);
  });

  let readyCount = 0;
  cluster.on('listening', () => {
    readyCount++;
    if (readyCount === cores) {
      // All workers bound the shared port (cluster round-robins/shares
      // the fd across workers under the hood) - only now is the server
      // actually ready to accept traffic on every core.
      printReady(
        `Node.js benchmark server on port ${PORT} (MODE=${MODE}, cluster with ${cores} workers)`
      );
    }
  });
} else {
  // Either a cluster worker, or CLUSTER=0 single-process mode.
  startServer();
  if (!useCluster) {
    printReady(`Node.js benchmark server on port ${PORT} (MODE=${MODE}, single process)`);
  }
}
