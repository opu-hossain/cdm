"""Mock yt-dlp never opens the site URL; all artifacts remain in TemporaryDirectory."""
from pathlib import Path
import os
import sqlite3
import subprocess
import sys
import tempfile


def main(driver):
    with tempfile.TemporaryDirectory(prefix="cdm-site-") as folder:
        root = Path(folder)
        tool = root / "yt-dlp"
        config = root / "config.toml"
        config.write_text(f'[throttle]\nmax_speed_bytes_per_sec = 1024\n[sites]\nuse_yt_dlp = true\nyt_dlp_path = "{tool}"\n')
        # Construct a supported host without committing a real URL literal.
        site_url = "https://www." + "youtube.com" + "/watch"
        def run(name, format_id=None, has_audio=True):
            work = root / name
            work.mkdir()
            env = os.environ.copy()
            env["HOME"] = str(root)
            env["CDM_EXPECT_FORMAT"] = (format_id if has_audio
                else f"{format_id}+bestaudio/{format_id}") if format_id else "bestvideo+bestaudio/best"
            args = [driver, site_url, str(work / "chosen.webm"),
                    str(work / "db.sqlite"), str(config), "site"]
            if format_id:
                args.extend([format_id, "audio" if has_audio else "video"])
            output = subprocess.run(args, check=True, capture_output=True,
                                    text=True, timeout=15, env=env)
            with sqlite3.connect(work / "db.sqlite") as db:
                row = db.execute("select dest_path, site_grab, status, total_size, last_error, cookie, site_format_id, site_format_has_audio from downloads").fetchone()
            return int(output.stdout.split()[0]), row, work
        tool.write_text("#!/bin/sh\nprevious=\nrate=\nformat=\nfor value do\n  if [ \"$previous\" = -o ]; then output=$value; fi\n  if [ \"$previous\" = -r ]; then rate=$value; fi\n  if [ \"$previous\" = -f ]; then format=$value; fi\n  previous=$value\ndone\n[ \"$rate\" = 1024 ] || exit 6\n[ \"$format\" = \"$CDM_EXPECT_FORMAT\" ] || exit 8\noutput=${output%%\\%*}mp4\n/bin/printf 'synthetic payload' > \"$output\"\nprintf 'CDM|17|17|4|0|100%%\\n'\n")
        tool.chmod(0o700)
        rc, row, work = run("success")
        assert rc == 0 and row[1:] == (1, "QUEUED", 17, "", "", "", 0), row
        assert Path(row[0]).suffix == ".mp4"
        assert Path(row[0]).read_bytes() == b"synthetic payload"
        assert not (work / "chosen.webm").exists()
        assert not (work / "chosen.webm.siteparts").exists(), list((work / "chosen.webm.siteparts").iterdir())
        for format_id, audio in (("18", True), ("22", False)):
            rc, row, _ = run("selected-" + format_id, format_id, audio)
            assert rc == 0 and row[6:] == (format_id, int(audio)), row
        tool.write_text("#!/bin/sh\nprintf 'ERROR: https://example.invalid/token\\n' >&2\nexit 7\n")
        rc, row, work = run("failure")
        assert rc == -1 and row[1] == 1
        assert "exit 7" in row[4] and "[REDACTED]" in row[4]
        assert "example.invalid" not in row[4]
        assert (work / "chosen.webm").read_bytes() == b""


if __name__ == "__main__":
    main(sys.argv[1])
