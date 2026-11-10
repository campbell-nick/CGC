#!/usr/bin/env python3
"""Tiny smart-HTTP server backed by `git http-backend`, used by smoke.sh.

usage: serve-http.py <git-project-root> <port> [length|chunked|v0|flaky|stall]

The optional third argument selects how response bodies are framed. The
default is Content-Length; "chunked" exercises cgc's chunked decoder, and
"v0" hides the Git-Protocol header so git http-backend falls back to
protocol v0. "flaky" returns 503 for the first FLAKY_FAILS requests (default
1), and "stall" accepts the request but never responds, for timeout tests.
"""
import os
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])
MODE = sys.argv[3] if len(sys.argv) > 3 else "length"
FLAKY_FAILS = int(os.environ.get("FLAKY_FAILS", "1"))
_flaky_lock = threading.Lock()
_flaky_left = FLAKY_FAILS


def flaky_fail():
    global _flaky_left
    with _flaky_lock:
        if _flaky_left > 0:
            _flaky_left -= 1
            return True
        return False


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send_body(self, payload):
        if MODE == "chunked":
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for start in range(0, len(payload), 1024):
                chunk = payload[start:start + 1024]
                self.wfile.write(("%x\r\n" % len(chunk)).encode("ascii"))
                self.wfile.write(chunk)
                self.wfile.write(b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    def _run(self):
        if MODE == "stall":
            time.sleep(60)
            return
        if MODE == "flaky" and flaky_fail():
            payload = b"temporary failure\n"
            self.send_response(503)
            self.send_header("Retry-After", "0")
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        u = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        env = os.environ.copy()
        env.update({
            "GIT_PROJECT_ROOT": ROOT,
            "GIT_HTTP_EXPORT_ALL": "1",
            "REQUEST_METHOD": self.command,
            "PATH_INFO": u.path,
            "QUERY_STRING": u.query,
            "CONTENT_TYPE": self.headers.get("Content-Type", ""),
            "CONTENT_LENGTH": str(length),
            "REMOTE_ADDR": self.client_address[0],
            "SERVER_PROTOCOL": self.protocol_version,
            "GATEWAY_INTERFACE": "CGI/1.1",
            "HTTP_GIT_PROTOCOL":
                "" if MODE == "v0" else self.headers.get("Git-Protocol", ""),
        })
        p = subprocess.run(["git", "http-backend"], input=body, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = p.stdout
        idx = out.find(b"\r\n\r\n")
        sep = 4
        if idx < 0:
            idx = out.find(b"\n\n")
            sep = 2
        if idx < 0:
            self.send_response(500)
            self.send_header("Content-Length", str(len(out) + len(p.stderr)))
            self.end_headers()
            self.wfile.write(out + p.stderr)
            return

        status = 200
        headers = []
        for line in out[:idx].decode("latin1").splitlines():
            if line.lower().startswith("status:"):
                status = int(line.split(None, 2)[1])
            elif ":" in line:
                k, v = line.split(":", 1)
                headers.append((k.strip(), v.strip()))

        payload = out[idx + sep:]
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self._send_body(payload)

    do_GET = _run
    do_POST = _run

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit("usage: serve-http.py <git-project-root> <port> "
                 "[length|chunked|v0|flaky|stall]")
    if MODE not in ("length", "chunked", "v0", "flaky", "stall"):
        sys.exit("serve-http.py: mode must be length, chunked, v0, flaky, "
                 "or stall")
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), Handler).serve_forever()
