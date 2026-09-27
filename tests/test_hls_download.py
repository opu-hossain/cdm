#!/usr/bin/env python3
"""Local VOD fixtures exercise the real engine across process restarts."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import subprocess
import sqlite3
import sys
import tempfile
import threading


def main(driver):
    bodies = {
        "/simple.m3u8": b"#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\na.ts\n#EXTINF:1,\nb.ts\n#EXT-X-ENDLIST\n",
        "/master.m3u8": b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=50\nsimple.m3u8\n",
        "/a.ts": b"first segment", "/b.ts": b"second segment",
        "/map.m3u8": b"#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:1\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:1,\na.ts\n#EXTINF:1,\nb.ts\n#EXT-X-ENDLIST\n",
        "/init.mp4": b"initialization",
        "/live.m3u8": b"#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\na.ts\n",
    }
    # Synthetic AES-CBC vectors generated with OpenSSL; not production keys.
    bodies["/key"] = bytes(range(16))
    bodies["/encrypted.ts"] = bytes.fromhex("cd5079100dd9bcf8b4d98a01f08cdb77e2442ce532abd373d3a41986f1e12782")
    bodies["/encrypted-init.mp4"] = bytes.fromhex("68f64a346f35b2222a96801c954392a98892959d00782af3005b24dc97aab660")
    bodies["/aes.m3u8"] = (b'#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:258\n'
        b'#EXT-X-KEY:METHOD=AES-128,URI="key"\n#EXTINF:1,\nencrypted.ts\n#EXT-X-ENDLIST\n')
    bodies["/aes-map.m3u8"] = (b'#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:1\n'
        b'#EXT-X-KEY:METHOD=AES-128,URI="key",IV=0x1\n'
        b'#EXT-X-MAP:URI="encrypted-init.mp4"\n#EXT-X-KEY:METHOD=NONE\n'
        b'#EXTINF:1,\na.ts\n#EXT-X-ENDLIST\n')
    counts = {}
    fail = set()
    truncate = set()
    fail_once = set()
    cookies = []
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def respond(self, head):
            with lock:
                if not head:
                    counts[self.path] = counts.get(self.path, 0) + 1
                cookies.append((self.path, self.headers.get("Cookie")))
                broken = self.path in fail
                if not head and self.path in fail_once:
                    broken = True
                    fail_once.remove(self.path)
                body = bodies.get(self.path)
            if body is None or broken:
                self.send_error(503 if broken else 404)
                return
            validator = '"' + hashlib.sha256(body).hexdigest() + '"'
            if not head and self.path in {"/a.ts", "/b.ts", "/init.mp4", "/encrypted.ts", "/encrypted-init.mp4"}:
                if self.headers.get("If-Match") != validator:
                    self.send_error(412)
                    return
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("ETag", '"' + hashlib.sha256(body).hexdigest() + '"')
            self.end_headers()
            if not head:
                self.wfile.write(body[:2] if self.path in truncate else body)

        def do_HEAD(self): self.respond(True)
        def do_GET(self): self.respond(False)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    url = f"http://127.0.0.1:{server.server_port}"
    other = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    other_worker = threading.Thread(target=other.serve_forever, daemon=True)
    other_worker.start()
    bodies["/foreign.ts"] = b"foreign"
    bodies["/cross.m3u8"] = (f"#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\n"
        f"http://127.0.0.1:{other.server_port}/foreign.ts\n#EXT-X-ENDLIST\n").encode()
    try:
        with tempfile.TemporaryDirectory(prefix="cdm-hls-http-") as root:
            root = Path(root)
            config = root / "config.toml"
            config.write_text("[retry]\nmax_attempts = 0\n[downloads]\nmax_connections_per_download = 1\n")

            def run(manifest, folder, control=None):
                folder.mkdir(exist_ok=True)
                dest = folder / "output.ts"
                result = subprocess.run([driver, url + manifest, str(dest),
                                         str(folder / "db.sqlite"), str(config)] + ([control] if control else []),
                                        env={**os.environ, "DOWNLOADMGR_ROOT": str(root)},
                                        capture_output=True, timeout=20)
                assert result.returncode == 0, result.stderr.decode()
                return int(result.stdout.decode().splitlines()[-1].split()[0]), dest

            rc, dest = run("/master.m3u8", root / "simple")
            assert rc == 0
            assert dest.read_bytes() == bodies["/a.ts"] + bodies["/b.ts"]
            assert not Path(str(dest) + ".hlsstate").exists()
            rc, dest = run("/map.m3u8", root / "map")
            assert rc == 0
            assert dest.read_bytes() == bodies["/init.mp4"] + bodies["/a.ts"] + bodies["/b.ts"]

            rc, dest = run("/aes.m3u8", root / "aes")
            assert rc == 0 and dest.read_bytes() == b"synthetic AES fixture"
            rc, dest = run("/aes-map.m3u8", root / "aes-map")
            assert rc == 0 and dest.read_bytes() == b"synthetic AES fixture" + bodies["/a.ts"]
            bodies["/key"] += b"x"
            rc, dest = run("/aes.m3u8", root / "bad-key")
            assert rc != 0 and dest.read_bytes() == b""
            bodies["/key"] = bytes(range(16))

            truncate.add("/b.ts")
            counts.clear()
            rc, dest = run("/simple.m3u8", root / "resume")
            assert rc != 0
            assert Path(str(dest) + ".hlsstate").is_file()
            assert dest.read_bytes() == b"", "partial output published"
            completed_a = counts.get("/a.ts", 0)
            assert completed_a == 1, counts
            truncate.clear()
            rc, dest = run("/simple.m3u8", root / "resume")
            assert rc == 0
            assert counts["/a.ts"] == completed_a, "completed segment fetched again"
            assert dest.read_bytes() == bodies["/a.ts"] + bodies["/b.ts"]

            fail.add("/b.ts")
            rc, dest = run("/simple.m3u8", root / "stale")
            assert rc != 0
            bodies["/a.ts"] = b"changed first segment"
            fail.clear()
            rc, dest = run("/simple.m3u8", root / "stale")
            assert rc == 0
            assert dest.read_bytes() == bodies["/a.ts"] + bodies["/b.ts"]
            fail.add("/b.ts")
            rc, dest = run("/simple.m3u8", root / "corrupt")
            assert rc != 0
            Path(str(dest) + ".hlsparts/item-0").write_bytes(b"corrupted")
            fail.clear()
            rc, dest = run("/simple.m3u8", root / "corrupt")
            assert rc == 0 and dest.read_bytes() == bodies["/a.ts"] + bodies["/b.ts"]
            fail.add("/b.ts")
            rc, dest = run("/simple.m3u8", root / "cancel")
            assert rc != 0
            rc, dest = run("/simple.m3u8", root / "cancel", "pause")
            assert rc != 0 and Path(str(dest) + ".hlsstate").exists()
            rc, dest = run("/simple.m3u8", root / "cancel", "cancel")
            assert rc != 0 and not Path(str(dest) + ".hlsstate").exists()
            assert not Path(str(dest) + ".hlsparts").exists()
            rc, dest = run("/simple.m3u8", root / "published")
            assert rc != 0
            dest.write_bytes(bodies["/a.ts"] + bodies["/b.ts"])
            fail.clear()
            rc, dest = run("/simple.m3u8", root / "published")
            assert rc == 0 and not Path(str(dest) + ".hlsstate").exists()
            fail.add("/b.ts")
            rc, dest = run("/simple.m3u8", root / "protect")
            assert rc != 0
            dest.write_bytes(b"user replacement")
            fail.clear()
            rc, dest = run("/simple.m3u8", root / "protect")
            assert rc == -3 and dest.read_bytes() == b"user replacement"
            config.write_text("[retry]\nmax_attempts = 0\n[downloads]\nmax_connections_per_download = 4\n")
            rc, dest = run("/map.m3u8", root / "parallel")
            assert rc == 0 and dest.read_bytes() == bodies["/init.mp4"] + bodies["/a.ts"] + bodies["/b.ts"]
            rc, dest = run("/simple.m3u8", root / "context", "context")
            assert rc == 0
            assert ("/simple.m3u8", "synthetic=1") in cookies
            with sqlite3.connect(root / "context/db.sqlite") as db:
                assert db.execute("select media_kind, requires_browser_context, cookie from downloads").fetchone() == (1, 1, "")
            before = counts.get("/simple.m3u8", 0)
            rc, dest = run("/simple.m3u8", root / "context")
            assert rc == -5 and counts.get("/simple.m3u8", 0) == before
            rc, dest = run("/cross.m3u8", root / "cross", "context")
            assert rc != 0 and counts.get("/foreign.ts", 0) == 0, "browser context leaked across origins"
            rc, dest = run("/simple.m3u8", root / "checksum", "bad-sha")
            assert rc == -2 and not dest.exists()
            assert not Path(str(dest) + ".hlsstate").exists()
            config.write_text("[retry]\nmax_attempts = 1\nbase_delay_sec = 1\nmax_delay_sec = 1\n[downloads]\nmax_connections_per_download = 1\n")
            fail_once.add("/b.ts")
            before = counts.get("/b.ts", 0)
            rc, dest = run("/simple.m3u8", root / "retry")
            assert rc == 0 and counts["/b.ts"] == before + 2
            rc, dest = run("/live.m3u8", root / "live")
            assert rc != 0 and dest.read_bytes() == b""
    finally:
        other.shutdown()
        other.server_close()
        other_worker.join()
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    main(sys.argv[1])
