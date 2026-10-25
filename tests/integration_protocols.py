#!/usr/bin/env python3

import hashlib
import http.server
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    retry_started = {}
    retry_lock = threading.Lock()
    slow_tail_seen = False

    def __init__(self, *args, directory=None, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def do_HEAD(self):
        path = pathlib.Path(self.translate_path(self.path))
        if not path.is_file():
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        path = pathlib.Path(self.translate_path(self.path))
        if not path.is_file():
            self.send_error(404)
            return
        if urllib.parse.urlparse(self.path).path.endswith("/retry-test.bin"):
            with self.retry_lock:
                started = self.retry_started.setdefault(
                    self.path, time.monotonic()
                )
            if time.monotonic() - started < 1.2:
                body = b"rate limited"
                self.send_response(429)
                self.send_header("Retry-After", "1")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
        request_path = urllib.parse.urlparse(self.path).path
        if request_path.endswith("/slow-tail.bin"):
            range_header = self.headers.get("Range", "")
            if range_header.startswith("bytes=4194304-"):
                with self.retry_lock:
                    should_delay = not type(self).slow_tail_seen
                    type(self).slow_tail_seen = True
                if should_delay:
                    time.sleep(12)
        size = path.stat().st_size
        match = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        start, end, code = 0, size - 1, 200
        if match:
            start = int(match.group(1))
            end = int(match.group(2)) if match.group(2) else size - 1
            start, end = max(0, start), min(size - 1, end)
            code = 206
        length = end - start + 1
        self.send_response(code)
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if code == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        try:
            with path.open("rb") as source:
                source.seek(start)
                remaining = length
                while remaining:
                    chunk = source.read(min(256 * 1024, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


class Server:
    def __init__(self, root):
        handler = lambda *args, **kwargs: RangeHandler(
            *args, directory=str(root), **kwargs
        )
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    @property
    def base_url(self):
        return f"http://127.0.0.1:{self.httpd.server_port}"

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.httpd.shutdown()
        self.thread.join(timeout=5)
        self.httpd.server_close()


def run_app(app, url, root, name, extra=None, delay=5000):
    downloads = root / "downloads"
    downloads.mkdir(exist_ok=True)
    data = root / "data"
    data.mkdir(exist_ok=True)
    command = [
        str(app),
        "--url",
        url,
        "--autostart",
        "--proxy-mode",
        "direct",
        "--download-dir",
        str(downloads),
        "--screenshot",
        str(root / f"{name}.png"),
        "--screenshot-delay",
        str(delay),
    ]
    if extra:
        command.extend(extra)
    env = os.environ.copy()
    env["QUICKDL_DATA_DIR"] = str(data)
    result = subprocess.run(
        command,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
    )
    if result.returncode != 0:
        raise RuntimeError(f"{name} app failed:\n{result.stdout}")
    return downloads, result.stdout


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: integration_protocols.py <app-binary>")
    app = pathlib.Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="quickdl-integration-") as temporary:
        root = pathlib.Path(temporary)
        web = root / "web"
        web.mkdir()

        http_data = os.urandom(6 * 1024 * 1024)
        (web / "http-test.bin").write_bytes(http_data)
        (web / "retry-test.bin").write_bytes(http_data)
        slow_tail_data = os.urandom(32 * 1024 * 1024)
        (web / "slow-tail.bin").write_bytes(slow_tail_data)

        hls_expected = bytearray()
        (web / "high").mkdir()
        for index in range(8):
            segment = bytes([index]) * (256 * 1024)
            hls_expected.extend(segment)
            (web / "high" / f"segment-{index}.ts").write_bytes(segment)
        with (web / "high" / "index.m3u8").open("w") as playlist:
            playlist.write(
                "#EXTM3U\n#EXT-X-VERSION:3\n"
                "#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
            )
            for index in range(8):
                playlist.write(f"#EXTINF:2.0,\nsegment-{index}.ts\n")
            playlist.write("#EXT-X-ENDLIST\n")
        (web / "master.m3u8").write_text(
            "#EXTM3U\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1920x1080\n"
            "high/index.m3u8\n"
        )

        with Server(web) as server:
            downloads, app_output = run_app(
                app,
                f"{server.base_url}/http-test.bin",
                root,
                "http",
                delay=4500,
            )
            http_output = downloads / "http-test.bin"
            if not http_output.exists():
                raise AssertionError(
                    f"HTTP output missing; files={list(downloads.iterdir())}\n"
                    f"app output:\n{app_output}"
                )
            if digest(http_output) != hashlib.sha256(http_data).hexdigest():
                raise AssertionError("HTTP SHA-256 mismatch")

            downloads, app_output = run_app(
                app,
                f"{server.base_url}/retry-test.bin",
                root,
                "http-retry",
                delay=7000,
            )
            retry_output = downloads / "retry-test.bin"
            if not retry_output.exists():
                raise AssertionError(
                    f"HTTP retry output missing; "
                    f"files={list(downloads.iterdir())}\n"
                    f"app output:\n{app_output}"
                )
            if digest(retry_output) != hashlib.sha256(http_data).hexdigest():
                raise AssertionError("HTTP 429 retry SHA-256 mismatch")

            downloads, app_output = run_app(
                app,
                f"{server.base_url}/slow-tail.bin",
                root,
                "slow-tail",
                delay=11000,
            )
            slow_tail_output = downloads / "slow-tail.bin"
            if not slow_tail_output.exists():
                log_path = root / "data" / "logs" / "quickdl.log"
                app_log = log_path.read_text() if log_path.exists() else ""
                raise AssertionError(
                    f"Slow-tail output missing; "
                    f"files={list(downloads.iterdir())}\n"
                    f"app output:\n{app_output}\n"
                    f"app log:\n{app_log}"
                )
            if digest(slow_tail_output) != hashlib.sha256(slow_tail_data).hexdigest():
                raise AssertionError("Slow-tail SHA-256 mismatch")

            downloads, app_output = run_app(
                app,
                f"{server.base_url}/master.m3u8",
                root,
                "hls",
                ["--hls-variant", "0"],
                delay=8000,
            )
            hls_output = downloads / "master.ts"
            if not hls_output.exists():
                raise AssertionError(
                    f"HLS output missing; files={list(downloads.iterdir())}\n"
                    f"app output:\n{app_output}"
                )
            if digest(hls_output) != hashlib.sha256(hls_expected).hexdigest():
                raise AssertionError("HLS SHA-256 mismatch")

    print("HTTP and HLS integration tests passed")


if __name__ == "__main__":
    main()
