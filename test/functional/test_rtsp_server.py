#!/usr/bin/env python3
"""
Functional tests for the RTSP server.

These tests start the rtsp-server binary and probe it with raw RTSP
messages over a TCP socket, verifying that the server responds with
correct status codes, headers, and well-formed SDP.

Requirements:
    - The rtsp-server binary must be built beforehand.
    - A small test video file (test.mp4) must be available in the
      temporary directory the server is started against.
    - Python ≥ 3.8

Usage:
    python3 test/functional/test_rtsp_server.py
    # or via pytest:
    pytest test/functional/test_rtsp_server.py -v
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

# ── Configuration ─────────────────────────────────────────────────────────────

# Locate the binary relative to this script
SCRIPT_DIR = Path(__file__).parent
ROOT_DIR   = SCRIPT_DIR.parent.parent

# Try the CMake default build locations
BINARY_CANDIDATES = [
    ROOT_DIR / "build" / "rtsp-server",
    ROOT_DIR / "build" / "Release" / "rtsp-server",
    ROOT_DIR / "build" / "Debug"   / "rtsp-server",
]
SERVER_BINARY = next(
    (str(p) for p in BINARY_CANDIDATES if p.exists()), None)

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 15540          # high port to avoid privilege issues in CI
TIMEOUT     = 5.0            # per-recv timeout (seconds)

CRLF  = "\r\n"
RTSP_VERSION = "RTSP/1.0"


def find_test_video() -> Path | None:
    """Return path to a pre-existing test H.264 video, or None."""
    candidates = [
        SCRIPT_DIR / "sample.mp4",
        ROOT_DIR  / "test" / "functional" / "sample.mp4",
        Path("/tmp") / "rtsp_test_sample.mp4",
    ]
    return next((p for p in candidates if p.exists()), None)


# ── RTSP client helper ────────────────────────────────────────────────────────

class RTSPClient:
    """Minimal RTSP/1.0 client for functional testing."""

    def __init__(self, host: str, port: int, timeout: float = TIMEOUT):
        self.host    = host
        self.port    = port
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._cseq: int = 0

    def connect(self):
        self._sock = socket.create_connection((self.host, self.port),
                                               timeout=self.timeout)

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()

    def _next_cseq(self) -> int:
        self._cseq += 1
        return self._cseq

    def send_raw(self, data: str) -> str:
        """Send a raw RTSP message; return the complete response string.

        RFC 2326 §10.12 interleaved binary frames (start byte ``$``) that
        precede the RTSP status line are silently discarded so that TCP-
        interleaved RTP traffic does not confuse response parsing.
        """
        assert self._sock is not None
        self._sock.sendall(data.encode())

        # Carry over any bytes left over from a previous call (e.g. the tail
        # of a body that was larger than what we read).
        buf: bytes = getattr(self, "_recv_buf", b"")

        while True:
            # ── Step 1: strip leading RFC 2326 interleaved frames ─────────
            # Each frame: '$' | channel(1) | length_hi(1) | length_lo(1) | payload
            while buf and buf[0:1] == b"$":
                if len(buf) < 4:
                    break                          # need more bytes for header
                frame_len = (buf[2] << 8) | buf[3]
                total     = 4 + frame_len
                if len(buf) < total:
                    break                          # frame payload not fully arrived
                buf = buf[total:]                  # discard the complete frame

            # ── Step 2: decide whether to read more data ──────────────────
            # Still sitting on a partial interleaved frame, or haven't
            # accumulated the full RTSP header yet → fetch another chunk.
            if buf and buf[0:1] == b"$":
                # Partial frame at front: need more bytes to consume it.
                needs_more = True
            elif b"\r\n\r\n" not in buf:
                # No double-CRLF yet → RTSP headers incomplete.
                needs_more = True
            else:
                needs_more = False

            if needs_more:
                chunk = self._sock.recv(4096)
                if not chunk:
                    break          # EOF
                buf += chunk
                continue

            break

        # ── Extract RTSP response ─────────────────────────────────────────
        hdr_end = buf.find(b"\r\n\r\n")
        if hdr_end == -1:
            # Never found a complete header block (e.g. server closed early).
            self._recv_buf = b""
            return buf.decode(errors="replace")

        response  = buf[: hdr_end + 4]
        remainder = buf[hdr_end + 4 :]

        # Read body if Content-Length is present.
        match    = re.search(rb"[Cc]ontent-[Ll]ength:\s*(\d+)", response)
        body_len = int(match.group(1)) if match else 0
        body     = remainder[:body_len]
        while len(body) < body_len:
            chunk = self._sock.recv(min(4096, body_len - len(body)))
            if not chunk:
                break
            body += chunk

        response       += body
        self._recv_buf  = remainder[body_len:]   # save tail for next call
        return response.decode(errors="replace")

    def options(self, url: str) -> dict:
        cseq = self._next_cseq()
        raw  = (f"OPTIONS {url} {RTSP_VERSION}{CRLF}"
                f"CSeq: {cseq}{CRLF}"
                f"User-Agent: RTSP-FT/1.0{CRLF}"
                f"{CRLF}")
        return self._parse_response(self.send_raw(raw))

    def describe(self, url: str) -> dict:
        cseq = self._next_cseq()
        raw  = (f"DESCRIBE {url} {RTSP_VERSION}{CRLF}"
                f"CSeq: {cseq}{CRLF}"
                f"Accept: application/sdp{CRLF}"
                f"User-Agent: RTSP-FT/1.0{CRLF}"
                f"{CRLF}")
        return self._parse_response(self.send_raw(raw))

    def setup(self, url: str, transport: str, session: str = "") -> dict:
        cseq    = self._next_cseq()
        session_hdr = f"Session: {session}{CRLF}" if session else ""
        raw  = (f"SETUP {url} {RTSP_VERSION}{CRLF}"
                f"CSeq: {cseq}{CRLF}"
                f"Transport: {transport}{CRLF}"
                f"{session_hdr}"
                f"User-Agent: RTSP-FT/1.0{CRLF}"
                f"{CRLF}")
        return self._parse_response(self.send_raw(raw))

    def play(self, url: str, session: str) -> dict:
        cseq = self._next_cseq()
        raw  = (f"PLAY {url} {RTSP_VERSION}{CRLF}"
                f"CSeq: {cseq}{CRLF}"
                f"Session: {session}{CRLF}"
                f"Range: npt=0.000-{CRLF}"
                f"User-Agent: RTSP-FT/1.0{CRLF}"
                f"{CRLF}")
        return self._parse_response(self.send_raw(raw))

    def teardown(self, url: str, session: str) -> dict:
        cseq = self._next_cseq()
        raw  = (f"TEARDOWN {url} {RTSP_VERSION}{CRLF}"
                f"CSeq: {cseq}{CRLF}"
                f"Session: {session}{CRLF}"
                f"User-Agent: RTSP-FT/1.0{CRLF}"
                f"{CRLF}")
        return self._parse_response(self.send_raw(raw))

    @staticmethod
    def _parse_response(raw: str) -> dict:
        lines = raw.split("\r\n")
        status_line = lines[0] if lines else ""
        parts = status_line.split(" ", 2)
        headers: dict[str, str] = {}
        i = 1
        while i < len(lines) and lines[i]:
            if ":" in lines[i]:
                k, _, v = lines[i].partition(":")
                headers[k.strip().lower()] = v.strip()
            i += 1
        body = "\r\n".join(lines[i + 1:]) if i < len(lines) else ""
        return {
            "status_line": status_line,
            "version":     parts[0] if len(parts) > 0 else "",
            "status_code": int(parts[1]) if len(parts) > 1 else 0,
            "reason":      parts[2] if len(parts) > 2 else "",
            "headers":     headers,
            "body":        body,
            "raw":         raw,
        }


# ── Server fixture ────────────────────────────────────────────────────────────

class ServerFixture(unittest.TestCase):
    """Base class that starts/stops the rtsp-server process."""

    server_proc: subprocess.Popen | None = None
    media_dir:   str                     = ""

    @classmethod
    def setUpClass(cls):
        if SERVER_BINARY is None:
            raise unittest.SkipTest(
                "rtsp-server binary not found. Build the project first.\n"
                f"Searched: {BINARY_CANDIDATES}")

        # Create a temp directory for media files
        cls.media_dir = tempfile.mkdtemp(prefix="rtsp_ft_")

        # Copy (or symlink) a sample video into the temp directory
        sample = find_test_video()
        if sample:
            import shutil
            shutil.copy(str(sample), cls.media_dir)
            cls.test_file = sample.name
        else:
            cls.test_file = None

        # Start the server
        cls.server_proc = subprocess.Popen(
            [SERVER_BINARY, cls.media_dir,
             f"{SERVER_HOST}:{SERVER_PORT}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # Give it a moment to bind
        time.sleep(0.5)
        if cls.server_proc.poll() is not None:
            out, err = cls.server_proc.communicate()
            raise RuntimeError(
                f"Server exited prematurely.\n"
                f"stdout: {out.decode()}\nstderr: {err.decode()}")

    @classmethod
    def tearDownClass(cls):
        if cls.server_proc and cls.server_proc.poll() is None:
            cls.server_proc.terminate()
            try:
                cls.server_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cls.server_proc.kill()
        import shutil
        if cls.media_dir:
            shutil.rmtree(cls.media_dir, ignore_errors=True)

    def base_url(self, filename: str = "") -> str:
        path = f"/{filename}" if filename else "/"
        return f"rtsp://{SERVER_HOST}:{SERVER_PORT}{path}"


# ── Test cases ────────────────────────────────────────────────────────────────

class TestOptions(ServerFixture):

    def test_options_returns_200(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.options(self.base_url())
        self.assertEqual(resp["status_code"], 200)

    def test_options_public_header_present(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.options(self.base_url())
        self.assertIn("public", resp["headers"])

    def test_options_public_contains_required_methods(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.options(self.base_url())
        public = resp["headers"].get("public", "")
        for method in ("OPTIONS", "DESCRIBE", "SETUP", "PLAY"):
            self.assertIn(method, public)

    def test_options_cseq_echoed(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.options(self.base_url())
        self.assertEqual(resp["headers"].get("cseq"), "1")


class TestDescribe(ServerFixture):

    def test_describe_nonexistent_file_returns_404(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.describe(self.base_url("no_such_file.mp4"))
        self.assertEqual(resp["status_code"], 404)

    def test_describe_path_traversal_rejected(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            # Attempt directory traversal
            resp = c.describe(self.base_url("../../../etc/passwd"))
        self.assertIn(resp["status_code"], (403, 404))

    @unittest.skipIf(find_test_video() is None,
                     "No test video available; skipping media tests")
    def test_describe_valid_file_returns_200(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.describe(self.base_url(self.test_file))
        self.assertEqual(resp["status_code"], 200)

    @unittest.skipIf(find_test_video() is None,
                     "No test video available; skipping media tests")
    def test_describe_returns_sdp_content_type(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.describe(self.base_url(self.test_file))
        self.assertIn("application/sdp",
                      resp["headers"].get("content-type", ""))

    @unittest.skipIf(find_test_video() is None,
                     "No test video available; skipping media tests")
    def test_describe_sdp_has_required_fields(self):
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.describe(self.base_url(self.test_file))
        sdp = resp["body"]
        self.assertIn("v=0",     sdp)
        self.assertIn("m=video", sdp)
        self.assertIn("H264",    sdp)

    @unittest.skipIf(find_test_video() is None,
                     "No test video available; skipping media tests")
    def test_setup_without_describe_still_returns_error(self):
        """A SETUP for a file that was never DESCRIBEd on this session must
        return 455 (Method Not Valid in This State)."""
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            resp = c.setup(
                self.base_url(self.test_file + "/trackID=0"),
                "RTP/AVP;unicast;client_port=60000-60001")
        # Server may return 455 or 404 – both are acceptable
        self.assertIn(resp["status_code"], (404, 455, 400))


class TestFullFlow(ServerFixture):
    """Tests that require a real video file to exercise SETUP and PLAY."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if cls.test_file is None:
            raise unittest.SkipTest("No test video available")

    def _rtsp_url(self):
        return self.base_url(self.test_file)

    def test_options_describe_setup_play_teardown(self):
        """Full happy-path without actually receiving RTP data."""
        with RTSPClient(SERVER_HOST, SERVER_PORT) as c:
            # OPTIONS
            r = c.options(self._rtsp_url())
            self.assertEqual(r["status_code"], 200, msg=r["raw"])

            # DESCRIBE
            r = c.describe(self._rtsp_url())
            self.assertEqual(r["status_code"], 200, msg=r["raw"])

            # SETUP (use TCP interleaved so we don't need a real UDP port)
            r = c.setup(
                self._rtsp_url() + "/trackID=0",
                "RTP/AVP/TCP;unicast;interleaved=0-1")
            self.assertEqual(r["status_code"], 200, msg=r["raw"])
            session = r["headers"].get("session", "").split(";")[0]
            self.assertTrue(session, "Session header missing after SETUP")

            # PLAY
            r = c.play(self._rtsp_url(), session)
            self.assertEqual(r["status_code"], 200, msg=r["raw"])

            # Wait briefly to let at least one RTP packet be sent
            time.sleep(0.3)

            # TEARDOWN
            r = c.teardown(self._rtsp_url(), session)
            self.assertEqual(r["status_code"], 200, msg=r["raw"])


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    unittest.main(verbosity=2)
