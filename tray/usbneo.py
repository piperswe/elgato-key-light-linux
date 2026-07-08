#!/usr/bin/env python3
"""USB HID transport for the Elgato Key Light Neo.

The Neo speaks the same application protocol as the networked Key Lights — the
``GET/PUT /elgato/*`` text-and-JSON API from keylights.sh — but tunnels it over
USB HID instead of HTTP. Each message is split into fixed 512-byte frames:

    [0x02][frame_idx][total_frames][0x03][payload_len u16 LE][payload <=505][0x03][pad]

Protocol reverse-engineered by Zameer Manji:
https://zameermanji.com/blog/2026/3/4/elgato-key-light-neo-usb-protocol/

This module has no Qt imports so it can be exercised headlessly; run it directly
(``./tray/usbneo.py`` or ``make probe``) to dump the raw exchange with any
attached Neo.
"""

from __future__ import annotations

import json
import threading
import time
from typing import Optional

import hid  # python3-hidapi; on Fedora this is the libusb backend

VID, PID = 0x0FD9, 0x00A0

FRAME_SIZE = 512
MAX_PAYLOAD = 505  # 512 - 6-byte header - 1-byte terminator

# hid.read() timeout per call; a response may span several reads.
READ_TIMEOUT_MS = 100
# Overall budget for reassembling a response, mirroring lights.HTTP_TIMEOUT.
READ_DEADLINE_S = 2.0


class UsbError(Exception):
    """Any failure talking to the device: enumeration, framing or I/O."""


# --- per-device serialisation -----------------------------------------------
#
# A single HID handle cannot service interleaved exchanges, and the libusb
# backend refuses a second concurrent open of the same interface. The manager
# can fire a refresh GET and a slider PUT at the same light from different pool
# threads, so every exchange for a given path is serialised through one lock.

_locks: dict[bytes, threading.Lock] = {}
_locks_guard = threading.Lock()


def _lock_for(path: bytes) -> threading.Lock:
    with _locks_guard:
        lock = _locks.get(path)
        if lock is None:
            lock = threading.Lock()
            _locks[path] = lock
        return lock


# --- framing -----------------------------------------------------------------


def build_frames(payload: bytes) -> list[bytes]:
    """Split ``payload`` into 512-byte frames, each prefixed with a 0x00 report
    id for hidapi (so the written buffers are 513 bytes)."""
    chunks = [
        payload[i : i + MAX_PAYLOAD] for i in range(0, len(payload), MAX_PAYLOAD)
    ] or [b""]
    total = len(chunks)
    frames = []
    for idx, chunk in enumerate(chunks):
        header = bytes([0x02, idx, total, 0x03]) + len(chunk).to_bytes(2, "little")
        frame = (header + chunk + b"\x03").ljust(FRAME_SIZE, b"\x00")
        frames.append(b"\x00" + frame)  # leading report-id byte for hidapi
    return frames


def parse_frame(frame: bytes) -> tuple[int, int, bytes]:
    """Return ``(frame_idx, total_frames, payload)`` from a response frame."""
    data = bytes(frame)
    # Depending on the hidapi backend a read may keep the leading report-id byte.
    if len(data) > 1 and data[0] == 0x00 and data[1] == 0x02:
        data = data[1:]
    # Requests carry 0x03 in byte 3; responses put a status byte there (0x00 on
    # success), so only the 0x02 magic is a reliable header check. A bad length
    # is caught by the payload-truncation guard below.
    if len(data) < 6 or data[0] != 0x02:
        raise UsbError(f"bad frame header: {data[:6].hex()}")
    idx, total = data[1], data[2]
    length = int.from_bytes(data[4:6], "little")
    payload = data[6 : 6 + length]
    if len(payload) != length:
        raise UsbError("frame payload truncated")
    return idx, total, payload


# --- exchange ----------------------------------------------------------------


def enumerate_neos() -> list[bytes]:
    """Paths of attached Neo control interfaces, sorted for stable ordering."""
    paths = [
        dev["path"]
        for dev in hid.enumerate(VID, PID)
        if dev.get("interface_number", 0) in (0, -1)
    ]
    return sorted(paths)


def _read_response(dev: "hid.device") -> str:
    frames: dict[int, bytes] = {}
    total: Optional[int] = None
    deadline = time.monotonic() + READ_DEADLINE_S
    while time.monotonic() < deadline:
        data = dev.read(FRAME_SIZE + 1, READ_TIMEOUT_MS)
        if not data:
            continue
        idx, total, payload = parse_frame(data)
        frames[idx] = payload
        if len(frames) >= total:
            break
    if total is None or len(frames) < total:
        raise UsbError("response timed out")
    return b"".join(frames[i] for i in range(total)).decode(errors="replace")


def exchange(path: bytes, request: str) -> str:
    """Send ``request`` and return the raw response text, serialised per path."""
    with _lock_for(path):
        dev = hid.device()
        try:
            try:
                dev.open_path(path)
            except (OSError, ValueError) as exc:
                raise UsbError(f"cannot open {path!r}: {exc}") from exc
            for frame in build_frames(request.encode()):
                dev.write(frame)
            return _read_response(dev)
        finally:
            dev.close()


def decode_response(text: str) -> dict:
    """Extract the JSON object from a response, tolerating any status prefix."""
    start = text.find("{")
    if start < 0:
        raise UsbError(f"no JSON in response: {text!r}")
    try:
        return json.loads(text[start:])
    except json.JSONDecodeError as exc:
        raise UsbError(f"bad JSON in response: {text!r}") from exc


def usb_request(
    path: bytes, method: str, endpoint: str, body: Optional[dict] = None
) -> dict:
    """Perform ``<method> <endpoint> [<json>]`` over USB and return the reply."""
    request = f"{method} {endpoint}"
    if body is not None:
        request += " " + json.dumps(body, separators=(",", ":"))
    return decode_response(exchange(path, request))


if __name__ == "__main__":
    paths = enumerate_neos()
    print(f"Found {len(paths)} Neo interface(s): {[p.decode() for p in paths]}")
    for path in paths:
        print(f"\n=== {path.decode()} ===")
        for endpoint in ("/elgato/accessory-info", "/elgato/lights"):
            try:
                raw = exchange(path, f"GET {endpoint}")
                print(f"GET {endpoint}\n  raw: {raw!r}")
                print(f"  decoded: {decode_response(raw)}")
            except UsbError as exc:
                print(f"GET {endpoint} -> {exc}")
        # Round-trip check: resend the current state so nothing visibly changes.
        try:
            state = usb_request(path, "GET", "/elgato/lights")
            lights = state.get("lights") or [{}]
            echo = {"numberOfLights": state.get("numberOfLights", 1), "lights": lights}
            print(f"PUT /elgato/lights {echo}")
            print(f"  reply: {usb_request(path, 'PUT', '/elgato/lights', echo)}")
        except UsbError as exc:
            print(f"PUT round-trip -> {exc}")
