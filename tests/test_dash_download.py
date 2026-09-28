"""Loopback-only DASH dispatch, retained-track persistence and merge smoke."""
import http.server
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading


def main(driver):
    bodies = {"/video.mp4": b"synthetic-video", "/audio.m4a": b"synthetic-audio"}
    bodies["/main.mpd"] = b"""<MPD><Period><AdaptationSet contentType="video"><Representation bandwidth="100"><SegmentList><SegmentURL media="video.mp4"/></SegmentList></Representation></AdaptationSet><AdaptationSet contentType="audio"><Representation bandwidth="20"><SegmentList><SegmentURL media="audio.m4a"/></SegmentList></Representation></AdaptationSet></Period></MPD>"""
    counts = {}
    fail_audio = False
    class Handler(http.server.BaseHTTPRequestHandler):
        def serve(self, head):
            if self.path not in bodies:
                self.send_error(404)
                return
            if self.path == "/audio.m4a" and fail_audio:
                self.send_error(500)
                return
            if not head:
                counts[self.path] = counts.get(self.path, 0) + 1
            data = bodies[self.path]
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("ETag", '"fixture-v1"')
            self.end_headers()
            if not head:
                self.wfile.write(data)
        def do_HEAD(self): self.serve(True)
        def do_GET(self): self.serve(False)
        def log_message(self, *args): pass
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="cdm-dash-") as temp:
            root = Path(temp)
            config = root / "config.toml"
            config.write_text("[downloads]\nmax_connections_per_download = 2\n[retry]\nmax_attempts = 0\n")
            def attempt(folder, path, control="dash"):
                folder.mkdir(exist_ok=True)
                env = os.environ.copy()
                env["PATH"] = path
                env["DOWNLOADMGR_ROOT"] = str(root)
                result = subprocess.run([driver, f"http://127.0.0.1:{server.server_port}/main.mpd", str(folder / "output.mp4"), str(folder / "db.sqlite"), str(config), control], env=env, check=True, text=True, capture_output=True, timeout=30)
                return int(result.stdout.split()[0]), result.stderr
            def run(folder, path):
                rc, error = attempt(folder, path)
                assert rc == 0, error
                with sqlite3.connect(folder / "db.sqlite") as db:
                    row = db.execute("select dest_path, companion_path, total_size, status, media_kind from downloads").fetchone()
                assert row[3:] == ("QUEUED", 2), row
                return row
            missing = root / "missing"
            row = run(missing, "/definitely/missing")
            assert Path(row[0]).read_bytes() == bodies["/video.mp4"]
            assert Path(row[1]).read_bytes() == bodies["/audio.m4a"]
            assert row[2] == len(bodies["/video.mp4"]) + len(bodies["/audio.m4a"])
            assert not (missing / "output.mp4.dashparts").exists()
            # A completed video remains resumable while audio failed.
            partial = root / "partial"
            fail_audio = True
            before = counts.get("/video.mp4", 0)
            rc, _ = attempt(partial, "/definitely/missing")
            assert rc != 0
            assert (partial / "output.mp4.dashparts/video.mp4.hlsstate").exists()
            assert counts["/video.mp4"] == before + 1
            fail_audio = False
            row = run(partial, "/definitely/missing")
            assert counts["/video.mp4"] == before + 1, "resumed video redownloaded"
            assert Path(row[1]).read_bytes() == bodies["/audio.m4a"]
            for control in ("dash-pause", "dash-cancel"):
                folder = root / control
                before = counts.get("/main.mpd", 0)
                rc, _ = attempt(folder, "/definitely/missing", control)
                assert rc != 0 and counts.get("/main.mpd", 0) == before
                if control == "dash-cancel":
                    assert not (folder / "output.mp4.dashparts").exists()
                else:
                    run(folder, "/definitely/missing")
            context = root / "context"
            rc, _ = attempt(context, "/definitely/missing", "dash-context")
            assert rc == 0
            with sqlite3.connect(context / "db.sqlite") as db:
                assert db.execute("select cookie, requires_browser_context from downloads").fetchone() == ("", 1)
            before = counts["/main.mpd"]
            rc, _ = attempt(context, "/definitely/missing")
            assert rc == -5 and counts["/main.mpd"] == before
            tool = root / "tools"
            tool.mkdir()
            (tool / "ffmpeg").write_text("#!/bin/sh\nexit 7\n")
            (tool / "ffmpeg").chmod(0o700)
            row = run(root / "failed-tool", str(tool))
            assert Path(row[0]).read_bytes() == bodies["/video.mp4"] and Path(row[1]).read_bytes() == bodies["/audio.m4a"]
            ffmpeg = shutil.which("ffmpeg")
            if ffmpeg:
                v, a = root / "video.mp4", root / "audio.m4a"
                subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=black:s=16x16:d=0.25", "-c:v", "mpeg4", str(v)], check=True, capture_output=True, timeout=10)
                subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i", "sine=duration=0.25", "-c:a", "aac", str(a)], check=True, capture_output=True, timeout=10)
                bodies["/video.mp4"], bodies["/audio.m4a"] = v.read_bytes(), a.read_bytes()
                row = run(root / "merged", os.environ.get("PATH", ""))
                assert row[1] == ""
                assert Path(row[0]).read_bytes()[4:8] == b"ftyp"
                assert row[2] == Path(row[0]).stat().st_size
            else:
                print("SKIP real DASH merge: ffmpeg missing")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main(sys.argv[1])
