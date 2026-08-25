#!/usr/bin/env python3
"""Controllable HTTP source for TPCR integration tests (05-TEST-PLAN.md).

Serves one file with proper HEAD and Range/206 semantics, plus the fault
injection the test matrix needs:

  * ``--ignore-range``          answer 200 + full body to ranged GETs (T-I4)
  * ``--fail-first CODE:N``     answer HTTP CODE to the first N range GETs (T-I3)
  * ``--reset-after BYTES:N``   close the connection after BYTES body bytes,
                                for the first N range GETs (T-I3)
  * ``--stall SECS:N``          delay the response body by SECS for the first
                                N range GETs
  * ``--throttle BYTES_PER_S``  pace the body (so perf markers are observable)
  * ``--headers-log FILE``      append one JSON line per request (method,
                                path, headers) -- T-I2 asserts passthrough
  * ``/ctl?...``                runtime control (used by the WP-5 outage
                                tests): mode=refuse|ok, etag=..., etc.

Validators (ETag/Last-Modified) are stable per file unless overridden, which
the resume tests (WP-8) flip via /ctl.
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import zlib
import socketserver
from http.server import BaseHTTPRequestHandler, HTTPServer


class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    """http.server.ThreadingHTTPServer needs Python 3.7; EL8 ships 3.6."""
    daemon_threads = True
from urllib.parse import urlparse, parse_qs


class SourceState:
    """Mutable fault-injection state, shared across request threads."""

    def __init__(self, args):
        self.lock = threading.Lock()
        self.ignore_range = args.ignore_range
        self.fail_first = self._parse_pair(args.fail_first)     # [code, n]
        self.reset_after = self._parse_pair(args.reset_after)   # [bytes, n]
        self.stall = self._parse_pair(args.stall)               # [secs, n]
        self.throttle = args.throttle
        # /ctl?mode=...: "ok" serves normally; "refuse" 503s everything
        # (HEAD included -- a total outage); "refuse-data" 503s only the
        # ranged GETs while HEAD keeps working (lets the degraded re-probe
        # see validators, e.g. for the source-changed scenarios).
        self.refuse = "ok"
        self.etag = '"tpcr-mock-etag-1"'
        self.last_modified = "Wed, 01 Jan 2025 00:00:00 GMT"
        # --repr-digest good|bad: advertise the payload adler32 (or a wrong
        # value) as an RFC 9530 Repr-Digest on every response (FR-29 tests).
        self.repr_digest_mode = args.repr_digest
        self.headers_log = args.headers_log

    @staticmethod
    def _parse_pair(value):
        if not value:
            return None
        first, second = value.split(":")
        return [int(first), int(second)]

    def take(self, attr):
        """Consume one shot of a counted fault; returns the parameter or None."""
        with self.lock:
            pair = getattr(self, attr)
            if pair and pair[1] > 0:
                pair[1] -= 1
                return pair[0]
        return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    # Injected by main():
    state = None      # type: SourceState
    payload = b""

    def log_message(self, fmt, *log_args):  # quiet
        pass

    def _log_headers(self):
        state = self.state
        if not state.headers_log:
            return
        entry = {
            "time": time.time(),
            "method": self.command,
            "path": self.path,
            "mode": state.refuse,
            "headers": {k: v for k, v in self.headers.items()},
        }
        with state.lock:
            with open(state.headers_log, "a") as log:
                log.write(json.dumps(entry) + "\n")

    def _send_validators(self):
        self.send_header("ETag", self.state.etag)
        self.send_header("Last-Modified", self.state.last_modified)
        if self.state.repr_digest_mode:
            adler = zlib.adler32(self.payload) & 0xffffffff
            if self.state.repr_digest_mode == "bad":
                adler ^= 0xdeadbeef
            value = base64.b64encode(struct.pack(">I", adler)).decode()
            self.send_header("Repr-Digest", "adler=:%s:" % value)

    def do_HEAD(self):
        self._log_headers()
        if self.path.startswith("/ctl"):
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.state.refuse == "refuse":
            self._plain_status(503, "source down (test-controlled)")
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(self.payload)))
        self._send_validators()
        self.end_headers()

    def do_PUT(self):
        # Push-mode target (CON-6 smoke): store the body for byte-compare.
        self._log_headers()
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        # Persist for the test script to compare.
        if self.state.headers_log:
            with open(self.state.headers_log + ".put" , "wb") as out:
                out.write(body)
        self._plain_status(201, "Created")

    def do_GET(self):
        state = self.state
        if self.path.startswith("/ctl"):
            self._handle_ctl()
            return
        self._log_headers()

        if state.refuse in ("refuse", "refuse-data"):
            self._plain_status(503, "source down (test-controlled)")
            return

        fail_code = state.take("fail_first")
        if fail_code is not None:
            self._plain_status(fail_code, "injected failure")
            return

        stall_secs = state.take("stall")
        if stall_secs is not None:
            time.sleep(stall_secs)

        range_header = self.headers.get("Range")
        # RFC 9110 If-Range: when the validator no longer matches, a
        # compliant server ignores Range and sends the full current entity
        # with 200 -- the WP-14/H6 changed-source signal TPCR relies on.
        if_range = self.headers.get("If-Range")
        if if_range is not None and if_range != state.etag:
            self._serve_full()
            return
        if range_header and not state.ignore_range:
            self._serve_range(range_header)
        else:
            self._serve_full()

    def _handle_ctl(self):
        """Runtime control: /ctl?mode=refuse|ok&etag=...&lastmod=..."""
        query = parse_qs(urlparse(self.path).query)
        state = self.state
        with state.lock:
            if "mode" in query:
                state.refuse = query["mode"][0]
            if "etag" in query:
                state.etag = query["etag"][0]
            if "lastmod" in query:
                state.last_modified = query["lastmod"][0]
        self._plain_status(200, "ok")

    def _plain_status(self, code, text):
        body = (text + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass

    def _parse_range(self, range_header):
        # "bytes=start-end" (inclusive), as the TPCR scheduler issues them.
        units, _, spec = range_header.partition("=")
        if units.strip() != "bytes":
            return None
        start_s, _, end_s = spec.partition("-")
        try:
            start = int(start_s)
            end = int(end_s) if end_s else len(self.payload) - 1
        except ValueError:
            return None
        if start > end or start >= len(self.payload):
            return None
        return start, min(end, len(self.payload) - 1)

    def _serve_range(self, range_header):
        parsed = self._parse_range(range_header)
        if parsed is None:
            self._plain_status(416, "bad range")
            return
        start, end = parsed
        chunk = self.payload[start:end + 1]
        self.send_response(206)
        self.send_header("Content-Range",
                         "bytes %d-%d/%d" % (start, end, len(self.payload)))
        self.send_header("Content-Length", str(len(chunk)))
        self._send_validators()
        self.end_headers()
        self._send_body(chunk)

    def _serve_full(self):
        # Either no Range was requested, or we are deliberately ignoring it
        # (the BUG-2 scenario: status 200 + whole file).
        self.send_response(200)
        self.send_header("Content-Length", str(len(self.payload)))
        self._send_validators()
        self.end_headers()
        self._send_body(self.payload)

    def _send_body(self, chunk):
        state = self.state
        reset_at = state.take("reset_after")
        try:
            if reset_at is not None:
                # Send a prefix, then kill the connection mid-body (T-I3).
                self.wfile.write(chunk[:reset_at])
                self.wfile.flush()
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                           b"\x01\x00\x00\x00\x00\x00\x00\x00")
                self.connection.close()
                return
            if state.throttle:
                # Pace the body so the transfer lasts long enough for perf
                # markers to be observable.
                step = max(1, state.throttle // 10)
                sent = 0
                while sent < len(chunk):
                    piece = chunk[sent:sent + step]
                    self.wfile.write(piece)
                    self.wfile.flush()
                    sent += len(piece)
                    time.sleep(0.1)
            else:
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--file", required=True,
                        help="file whose bytes are served (the reference)")
    parser.add_argument("--ignore-range", action="store_true")
    parser.add_argument("--fail-first", default=None, metavar="CODE:N")
    parser.add_argument("--reset-after", default=None, metavar="BYTES:N")
    parser.add_argument("--stall", default=None, metavar="SECS:N")
    parser.add_argument("--throttle", type=int, default=0, metavar="BYTES_PER_S")
    parser.add_argument("--headers-log", default=None)
    parser.add_argument("--repr-digest", default=None, choices=["good", "bad"])
    # Serve over TLS (both required): the source side of the WP-15 TLS leg.
    parser.add_argument("--tls-cert", default=None)
    parser.add_argument("--tls-key", default=None)
    args = parser.parse_args()

    with open(args.file, "rb") as ref:
        Handler.payload = ref.read()
    Handler.state = SourceState(args)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    if args.tls_cert and args.tls_key:
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.tls_cert, args.tls_key)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
    print("mock_source: serving %d bytes on port %d" %
          (len(Handler.payload), args.port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
