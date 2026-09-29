#!/usr/bin/env python3
"""Exercise the native host's actual stdin/stdout framing contract."""

import json
import ctypes
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading


def frame(payload: bytes) -> bytes:
    return struct.pack("=I", len(payload)) + payload


def decode_frames(data: bytes) -> list[dict]:
    messages = []
    offset = 0
    while offset < len(data):
        assert len(data) - offset >= 4, "stray bytes on protocol stdout"
        length = struct.unpack_from("=I", data, offset)[0]
        offset += 4
        assert 0 < length <= 1024 * 1024
        assert len(data) - offset >= length, "truncated native response"
        messages.append(json.loads(data[offset : offset + length]))
        offset += length
    return messages


def main(host: str) -> None:
    input_bytes = frame(b"{bad json") + frame(
        json.dumps({"type": "unsupported", "request_id": "second"}).encode()
    )
    with tempfile.TemporaryDirectory(prefix="cdm-host-default-") as root:
        default_env = os.environ.copy()
        default_env["HOME"] = root
        result = subprocess.run([host], input=input_bytes, env=default_env,
                                capture_output=True, timeout=5)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        messages = decode_frames(result.stdout)
        assert len(messages) == 2, messages
        assert messages[0]["type"] == "error" and messages[0]["error"] == "invalid JSON"
        assert messages[1]["type"] == "error"
        assert messages[1]["request_id"] == "second"

    with tempfile.TemporaryDirectory(prefix="cdm-host-locale-") as root:
        config_dir = Path(root) / ".local" / "share" / "cdm"
        config_dir.mkdir(parents=True)
        (config_dir / "config.toml").write_text('[ui]\nlocale = "es"\n')
        locale_env = os.environ.copy()
        locale_env["HOME"] = root
        result = subprocess.run([host], input=frame(b"{bad json"),
                                env=locale_env, capture_output=True, timeout=5)
        assert decode_frames(result.stdout)[0]["error"] == "JSON no válido"

    result = subprocess.run(
        [host], input=struct.pack("=I", 1024 * 1024 + 1),
        capture_output=True, timeout=5
    )
    assert result.returncode != 0
    assert result.stdout == b"", "diagnostics leaked to protocol stdout"

    base = {"type": "download_offer", "request_id": "context-1",
            "url": "https://example.invalid/file", "filename": "file"}
    invalid_root = tempfile.TemporaryDirectory(prefix="cdm-host-invalid-")
    invalid_env = os.environ.copy()
    invalid_env.update({"HOME": invalid_root.name,
                        "XDG_RUNTIME_DIR": invalid_root.name})
    configuration = {"type": "set_site_exclusions", "sites": ["*.EXAMPLE.invalid."]}
    blocked = {**base, "url": "https://example.invalid/file", "automatic": True}
    frames = [configuration, blocked,
              {**blocked, "url": "https://sub.example.invalid/file"},
              {**blocked, "url": "https://EXAMPLE.invalid.:443/file"},
              {**blocked, "url": "https://exa%6dple.invalid/file"},
              {"type": "set_site_exclusions", "sites": [".*regex.invalid"]}, blocked,
              {"type": "set_site_exclusions", "sites": ["example.invalid"] * 65}]
    result = subprocess.run([host], input=b"".join(frame(json.dumps(value).encode())
        for value in frames), env=invalid_env, capture_output=True, timeout=5)
    replies = decode_frames(result.stdout)
    assert replies[0]["type"] == "site_exclusions_set", replies
    assert replies[1]["type"] == "offer_skipped", replies
    assert replies[2]["type"] == "offer_skipped", replies
    assert replies[3]["type"] == "offer_skipped", replies
    assert replies[4]["type"] == "offer_skipped", replies
    assert replies[5]["type"] == "error", replies
    assert replies[6]["type"] == "offer_skipped", "invalid configuration replaced valid policy"
    assert replies[7]["type"] == "error", replies
    probes = [
        {"type": "site_probe", "request_id": "probe-1",
         "url": "https://example.invalid/watch"},
        {"type": "site_probe", "request_id": "probe-2", "url": "file:///tmp/video"},
        {"type": "site_probe", "request_id": "bad\nrequest",
         "url": "https://example.invalid/watch"},
        {"type": "site_probe", "request_id": "probe-4",
         "url": "https://example.invalid/watch", "cookie": "secret"},
        {"type": "site_probe", "request_id": "probe-5",
         "url": "https://example.invalid/" + "x" * 2048},
        {"type": "site_probe", "request_id": "probe-6",
         "url": "https://example.invalid/watch\nsecret"},
        {"type": "site_probe", "request_id": "probe-7",
         "url": "https://example.invalid/watch", "explicit_consent": True},
        {"type": "site_probe", "request_id": "probe-8",
         "url": "https://127.0.0.1/watch", "explicit_consent": True},
        {"type": "site_probe", "request_id": "probe-9",
         "url": "https://example.invalid/watch", "explicit_consent": "yes"},
    ]
    result = subprocess.run([host], input=b"".join(frame(json.dumps(value).encode())
        for value in probes), env=invalid_env, capture_output=True, timeout=5)
    probe_replies = decode_frames(result.stdout)
    assert len(probe_replies) == len(probes), probe_replies
    assert all(reply["type"] == "error" for reply in probe_replies), probe_replies
    assert [reply["request_id"] for reply in probe_replies] == [
        "probe-1", "probe-2", "", "", "", "", "probe-7", "probe-8", ""], probe_replies
    assert b"secret" not in result.stdout + result.stderr
    for field, value in (("cookie", "x\r\nY"), ("cookie", "x" * 4097),
                         ("user_agent", "x" * 257), ("referer", "x" * 2049),
                         ("cookie", 3), ("cookie", "x\x00y"),
                         ("cookie", "\x01" * 4096)):
        result = subprocess.run([host], input=frame(json.dumps({**base, field: value}).encode()),
                                env=invalid_env, capture_output=True, timeout=5)
        replies = decode_frames(result.stdout)
        assert replies[0]["type"] == "error", (field, replies)
        assert "invalid" in replies[0]["error"], (field, replies)
    for kind in (None, 3, "unknown", "hls\x00video"):
        offer = {**base, "type": "media_offer"}
        if kind is not None:
            offer["kind"] = kind
        result = subprocess.run([host], input=frame(json.dumps(offer).encode()),
                                env=invalid_env, capture_output=True, timeout=5)
        assert "invalid" in decode_frames(result.stdout)[0]["error"]
    for fields in (
        {"site_format_id": "bad;id", "site_format_label": "360p",
         "site_format_has_audio": True},
        {"site_format_id": "18", "site_format_label": "360p"},
        {"site_format_id": "18", "site_format_label": "360p",
         "site_format_has_audio": True, "cookie": "secret"},
        {"site_format_id": "18", "site_format_label": "360p",
         "site_format_has_audio": True, "site_public_consent": "yes"},
    ):
        offer = {**base, "type": "media_offer", "kind": "video", **fields}
        result = subprocess.run([host], input=frame(json.dumps(offer).encode()),
                                env=invalid_env, capture_output=True, timeout=5)
        assert "invalid" in decode_frames(result.stdout)[0]["error"]
        assert b"secret" not in result.stdout + result.stderr
    malformed = json.dumps({**base, "cookie": "REPLACE"}).encode().replace(
        b"REPLACE", b"\xc0\x80")
    result = subprocess.run([host], input=frame(malformed), env=invalid_env,
                            capture_output=True, timeout=5)
    assert "invalid" in decode_frames(result.stdout)[0]["error"]
    invalid_root.cleanup()

    class Offer(ctypes.Structure):
        _fields_ = [("offer_id", ctypes.c_uint32), ("download_id", ctypes.c_uint32),
                    ("total_bytes", ctypes.c_uint64), ("state", ctypes.c_int),
                    ("request_id", ctypes.c_char * 128), ("url", ctypes.c_char * 2048),
                    ("filename", ctypes.c_char * 512), ("mime", ctypes.c_char * 128),
                    ("referrer", ctypes.c_char * 2048)]

    for media, version, with_context, format_id, public in (
            (None, 8, True, None, False), ("hls", 10, True, None, False),
            ("dash", 10, False, None, False), ("video", 10, True, None, False),
            ("hls", 9, False, None, False), ("video", 14, False, "18", False),
            ("video", 13, False, "18", False), ("video", 15, False, "18", True),
            ("video", 14, False, "18", True)):
        with tempfile.TemporaryDirectory(prefix="cdm-host-context-") as root:
            runtime = Path(root) / "runtime"
            runtime.mkdir(mode=0o700)
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(runtime / "cdm.sock"))
            listener.listen(4)
            listener.settimeout(5)
            observed = []
            failures = []

            def exact(peer, length):
                data = b""
                while len(data) < length:
                    chunk = peer.recv(length - len(data))
                    assert chunk, "host disconnected during request"
                    data += chunk
                return data

            def serve():
                with listener:
                    # One connection probes daemon liveness, one negotiates HELLO.
                    with listener.accept()[0]:
                        pass
                    with listener.accept()[0] as peer:
                        peer.settimeout(5)
                        header = exact(peer, 8)
                        assert struct.unpack("=II", header) == (0, 41), header
                        peer.sendall(struct.pack("=H", version))
                        if (media and version < 10) or (format_id and version < 14) or (
                                public and version < 15):
                            assert peer.recv(1) == b"", "media sent to an old daemon"
                            return
                        header = exact(peer, 8)
                        size, kind = struct.unpack("=II", header)
                        assert size <= 16384, size
                        payload = exact(peer, size)
                        observed.append((kind, json.loads(payload)))
                        reply = Offer(offer_id=1, state=2, request_id=b"context-1",
                                      url=b"https://example.invalid/file")
                        peer.sendall(bytes(reply))

            def fake_daemon():
                try:
                    serve()
                except Exception as error:
                    failures.append(error)

            worker = threading.Thread(target=fake_daemon, daemon=True)
            worker.start()
            env = os.environ.copy()
            env.update({"HOME": root, "XDG_RUNTIME_DIR": str(runtime)})
            payload = {**base, "automatic": False}
            if with_context:
                payload.update(cookie="x" * 4096, user_agent="u" * 256, referer="r" * 2048)
            if media:
                payload.update(type="media_offer", kind=media)
            if format_id:
                payload.update(site_format_id=format_id, site_format_label="360p MP4",
                               site_format_has_audio=True)
            if public:
                payload["site_public_consent"] = True
            result = subprocess.run([host], input=frame(json.dumps(configuration).encode()) +
                                    frame(json.dumps(payload).encode()),
                                    env=env, capture_output=True, timeout=5)
            worker.join(timeout=2)
            assert not worker.is_alive(), "fake daemon did not finish"
            assert not failures, failures
            if (media and version < 10) or (format_id and version < 14) or (
                    public and version < 15):
                replies = decode_frames(result.stdout)
                assert not observed
                assert ("support media" if version < 10 else
                        "selected site formats" if version < 14 else "public site offers"
                        ) in replies[1]["error"], replies
                continue
            assert observed, result.stderr.decode(errors="replace")
            assert observed[0][0] == (65 if public else 63 if format_id else 56), observed
            if media:
                assert observed[0][1]["kind"] == media, observed
            if format_id:
                assert observed[0][1]["site_format_id"] == format_id, observed
                assert observed[0][1]["site_format_has_audio"] is True, observed
            if public:
                assert observed[0][1]["site_public_consent"] is True, observed
            assert all(observed[0][1].get(key, "") == payload.get(key, "")
                       for key in ("cookie", "user_agent", "referer")), observed
            replies = decode_frames(result.stdout)
            assert replies[0]["type"] == "site_exclusions_set", replies
            assert replies[1]["type"] == "offer_registered", replies
            assert ("x" * 4096).encode() not in result.stderr


if __name__ == "__main__":
    main(sys.argv[1])
