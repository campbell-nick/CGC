#!/usr/bin/env python3
"""Small HTTP proxy used by tests/network.sh.

It accepts absolute-form HTTP requests, forwards them to the origin, and
returns the response. Set PROXY_AUTH=user:pass to require Proxy-Authorization.
Requests are appended to PROXY_LOG when set.
"""
import base64
import http.client
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

AUTH = os.environ.get("PROXY_AUTH", "")
AUTH_HEADER = ""
if AUTH:
    AUTH_HEADER = "Basic " + base64.b64encode(AUTH.encode("latin1")).decode("ascii")
LOG = os.environ.get("PROXY_LOG", "")

HOP_BY_HOP = {
    "connection", "proxy-connection", "keep-alive", "proxy-authenticate",
    "proxy-authorization", "te", "trailer", "transfer-encoding", "upgrade",
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _log(self):
        if LOG:
            with open(LOG, "a", encoding="utf-8") as f:
                f.write(self.command + " " + self.path + "\n")

    def _handle(self):
        self._log()

        if AUTH and self.headers.get("Proxy-Authorization", "") != AUTH_HEADER:
            payload = b"proxy authentication required\n"
            self.send_response(407)
            self.send_header("Proxy-Authenticate", 'Basic realm="cgc-test"')
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)
            return

        u = urllib.parse.urlparse(self.path)
        if u.scheme != "http" or not u.hostname:
            payload = b"proxy expects an absolute http URL\n"
            self.send_response(400)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)
            return

        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None

        headers = {}
        for k, v in self.headers.items():
            if k.lower() not in HOP_BY_HOP and k.lower() != "host":
                headers[k] = v
        headers["Host"] = u.netloc

        path = u.path or "/"
        if u.query:
            path += "?" + u.query

        conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=30)
        try:
            conn.request(self.command, path, body=body, headers=headers)
            resp = conn.getresponse()
            payload = resp.read()
        except OSError as e:
            payload = ("proxy upstream error: %s\n" % e).encode()
            self.send_response(502)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)
            return
        finally:
            conn.close()

        self.send_response(resp.status)
        for k, v in resp.getheaders():
            if k.lower() not in HOP_BY_HOP and k.lower() != "content-length":
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    do_GET = _handle
    do_POST = _handle

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: proxy.py <port>")
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
