#!/usr/bin/env python3
"""Exercise the native host's actual stdin/stdout framing contract."""

import json
import struct
import subprocess
import sys


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


if __name__ == "__main__":
    main(sys.argv[1])
