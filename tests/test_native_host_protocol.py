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
    result = subprocess.run([host], input=input_bytes, capture_output=True, timeout=5)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    messages = decode_frames(result.stdout)
    assert len(messages) == 2, messages
    assert messages[0]["type"] == "error" and messages[0]["error"] == "invalid JSON"
    assert messages[1]["type"] == "error"
    assert messages[1]["request_id"] == "second"

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

    for media, version, with_context in ((None, 8, True), ("hls", 10, True),
                                         ("dash", 10, False), ("video", 10, True),
                                         ("hls", 9, False)):
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
                        if media and version < 10:
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
            result = subprocess.run([host], input=frame(json.dumps(configuration).encode()) +
                                    frame(json.dumps(payload).encode()),
                                    env=env, capture_output=True, timeout=5)
            worker.join(timeout=2)
            assert not worker.is_alive(), "fake daemon did not finish"
            assert not failures, failures
            if media and version < 10:
                replies = decode_frames(result.stdout)
                assert not observed
                assert "support media" in replies[1]["error"], replies
                continue
            assert observed, result.stderr.decode(errors="replace")
            assert observed[0][0] == 56, observed
            if media:
                assert observed[0][1]["kind"] == media, observed
            assert all(observed[0][1].get(key, "") == payload.get(key, "")
                       for key in ("cookie", "user_agent", "referer")), observed
            replies = decode_frames(result.stdout)
            assert replies[0]["type"] == "site_exclusions_set", replies
            assert replies[1]["type"] == "offer_registered", replies
            assert ("x" * 4096).encode() not in result.stderr


if __name__ == "__main__":
    main(sys.argv[1])
