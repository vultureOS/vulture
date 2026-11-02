#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the system under test talks to, on the build machine. It can reach this at 10.0.2.2.
#
#   server.py DIRECTORY ECHO_PORT HTTP_PORT
#
# ECHO_PORT sends back all it is sent. HTTP_PORT answers the paths below, and any other from
# DIRECTORY (the certificate authority and the packages). Writes "ready" to stdout.

import gzip, hashlib, http.server, os, socket, socketserver, sys, threading


def pattern(size):
    """The same bytes each time: for checking a large download in the guest."""
    block = bytes((i * 31 + (i >> 8)) & 0xff for i in range(65536))
    return (block * (size // len(block) + 1))[:size]


LARGE = pattern(32 * 1024 * 1024)


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data:
                return
            self.request.sendall(data)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    directory = "."

    def log_message(self, *args):
        pass

    def reply(self, code, body, content_type="text/plain", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/hello":
            self.reply(200, b"hello from the build machine\n")
        elif path == "/redirect":
            self.reply(302, b"", extra=[("Location", "/hello")])
        elif path == "/loop":
            self.reply(302, b"", extra=[("Location", "/loop")])
        elif path == "/missing":
            self.reply(404, b"not here\n")
        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for piece in (b"first ", b"second ", b"third\n"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/gzip":
            self.reply(200, gzip.compress(b"it was compressed\n"), extra=[("Content-Encoding", "gzip")])
        elif path == "/large":
            self.reply(200, LARGE, "application/octet-stream")
        elif path == "/large.sha256":
            self.reply(200, hashlib.sha256(LARGE).hexdigest().encode() + b"\n")
        elif path == "/short":
            # Promises more than it gives.
            self.send_response(200)
            self.send_header("Content-Length", "1000")
            self.end_headers()
            self.wfile.write(b"too short")
            self.close_connection = True
        else:
            local = os.path.join(self.directory, os.path.basename(path))
            if os.path.isfile(local):
                with open(local, "rb") as f:
                    self.reply(200, f.read(), "application/octet-stream")
            else:
                self.reply(404, b"no such file\n")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        self.reply(200, b"you sent: " + body + b"\n")


class ThreadedTCP(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class ThreadedHTTP(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    directory, echo_port, http_port = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    Handler.directory = directory
    echo = ThreadedTCP(("127.0.0.1", echo_port), Echo)
    web = ThreadedHTTP(("127.0.0.1", http_port), Handler)
    threading.Thread(target=echo.serve_forever, daemon=True).start()
    print("ready", flush=True)
    web.serve_forever()


if __name__ == "__main__":
    main()
