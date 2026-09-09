#!/usr/bin/env python3
"""Fetch one response and report exactly what came back on the wire.

The cross-language comparison is only valid if every server does the same
work per request. This probe makes that auditable instead of assumed: it
reports the status, the framing headers, the body, and the total response
byte count, so a server that sends extra headers (Go's net/http always adds
Date) shows up as a larger response rather than silently looking faster or
slower for reasons unrelated to the runtime.

Usage: probe_wire.py <port>   ->  one line of JSON
"""
import json, socket, sys

port = int(sys.argv[1])
req = b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"

try:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(req)
    chunks = []
    while True:
        b = s.recv(65536)
        if not b:
            break
        chunks.append(b)
    s.close()
except OSError as exc:
    print(json.dumps({"ok": False, "error": str(exc)}))
    raise SystemExit(1)

raw = b"".join(chunks)
head, _, body = raw.partition(b"\r\n\r\n")
lines = head.split(b"\r\n")
status = lines[0].decode("latin-1") if lines else ""
headers = {}
for line in lines[1:]:
    k, _, v = line.partition(b":")
    headers[k.decode("latin-1").strip().lower()] = v.decode("latin-1").strip()

print(json.dumps({
    "ok": raw.startswith(b"HTTP/1.1 200") and body == b"Hello, World",
    "status": status,
    "content_length": headers.get("content-length"),
    "connection": headers.get("connection"),
    "body": body.decode("latin-1"),
    "header_count": len(lines) - 1,
    "extra_headers": sorted(k for k in headers
                            if k not in ("content-length", "connection")),
    "response_bytes": len(raw),
}))
