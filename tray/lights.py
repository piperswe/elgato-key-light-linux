"""Discovery and control of Elgato Key Lights.

This module holds the data model, mDNS discovery and HTTP I/O for the tray
app. It deliberately contains no Qt widget code so it can be exercised
headlessly (see the ``__main__`` block at the bottom).

Elgato quirk mirrored from keylights.sh: the devices mis-announce IPv6 and do
not answer requests on it, so discovery and control are IPv4-only.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional

import requests
from PySide6.QtCore import QObject, QRunnable, QThreadPool, QTimer, Signal
from zeroconf import IPVersion, ServiceBrowser, ServiceListener, Zeroconf
from zeroconf import ServiceInfo

try:
    import usbneo
except ImportError:  # python3-hidapi absent: run HTTP-only
    usbneo = None

USB_SUPPORTED = usbneo is not None

MDNS_TYPE = "_elg._tcp.local."
PORT = 9123

# HTTP endpoints (see keylights.sh:23-25).
EP_LIGHTS = "/elgato/lights"
EP_INFO = "/elgato/accessory-info"

# Elgato colour temperature is stored in mireds. 143..344 mireds spans roughly
# 7000K..2900K, which matches the range the hardware accepts.
MIRED_MIN, MIRED_MAX = 143, 344

# Short connect / read timeouts: a light that does not answer promptly is
# treated as offline rather than blocking the UI.
HTTP_TIMEOUT = (1.5, 2.0)

# Consecutive HTTP failures before a light is considered offline.
FAIL_LIMIT = 3

# Trailing debounce window for slider drags, milliseconds.
DEBOUNCE_MS = 150

# Periodic state refresh, milliseconds.
REFRESH_MS = 5000


def mired_to_kelvin(mired: int) -> int:
    """Convert mireds to Kelvin, rounded to the nearest 50K for a tidy label."""
    return round(1_000_000 / mired / 50) * 50


def kelvin_label(mired: int) -> str:
    return f"{mired_to_kelvin(mired)} K"


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def normalize_mac(raw: str) -> str:
    """Canonicalise a MAC to the mDNS TXT 'id' form (upper-case, colon-kept).

    The accessory-info ``macAddress`` and the mDNS ``id`` record already use the
    same ``3C:6A:9D:..`` format, so this only guards against case differences and
    keeps the two discovery paths deriving one identity for the same device.
    """
    return raw.strip().upper()


@dataclass
class LightState:
    on: bool
    brightness: int  # 0-100
    temperature: int  # mireds, MIRED_MIN..MIRED_MAX

    @classmethod
    def from_api(cls, light: dict) -> "LightState":
        return cls(
            on=bool(light.get("on", 0)),
            brightness=int(light.get("brightness", 0)),
            temperature=int(light.get("temperature", MIRED_MIN)),
        )


@dataclass
class KeyLight:
    id: str  # MAC (mDNS TXT 'id' / accessory-info macAddress); stable identity
    name: str  # displayName from accessory-info, falls back to mDNS name
    address: str  # IPv4 ("" for a USB-only light)
    port: int = PORT
    product: str = ""  # model from TXT 'md'
    online: bool = False
    num_lights: int = 1
    state: Optional[LightState] = None
    fail_count: int = 0
    transport: str = "http"  # "http" or "usb"
    usb_path: Optional[bytes] = None  # hidapi device path when USB-reachable
    serial: str = ""  # accessory-info serialNumber; dedup key across transports

    @property
    def base_url(self) -> str:
        return f"http://{self.address}:{self.port}"


# --- blocking HTTP helpers (run inside QRunnables, never on the main thread) --


def fetch_lights(light: KeyLight) -> dict:
    if light.transport == "usb":
        return usbneo.usb_request(light.usb_path, "GET", EP_LIGHTS)
    resp = requests.get(light.base_url + EP_LIGHTS, timeout=HTTP_TIMEOUT)
    resp.raise_for_status()
    return resp.json()


def fetch_accessory_info(light: KeyLight) -> dict:
    if light.transport == "usb":
        return usbneo.usb_request(light.usb_path, "GET", EP_INFO)
    resp = requests.get(light.base_url + EP_INFO, timeout=HTTP_TIMEOUT)
    resp.raise_for_status()
    return resp.json()


def put_lights(light: KeyLight, state: LightState) -> dict:
    body = {
        "numberOfLights": light.num_lights,
        "lights": [
            {
                "on": 1 if state.on else 0,
                "brightness": clamp(int(state.brightness), 0, 100),
                "temperature": clamp(int(state.temperature), MIRED_MIN, MIRED_MAX),
            }
        ]
        * max(1, light.num_lights),
    }
    if light.transport == "usb":
        return usbneo.usb_request(light.usb_path, "PUT", EP_LIGHTS, body)
    resp = requests.put(light.base_url + EP_LIGHTS, json=body, timeout=HTTP_TIMEOUT)
    resp.raise_for_status()
    return resp.json()


def usb_probe_info(path: bytes) -> dict:
    """Read accessory-info from a USB path before a KeyLight exists for it."""
    return usbneo.usb_request(path, "GET", EP_INFO)


# --- Qt thread-pool glue -----------------------------------------------------


class _Task(QRunnable):
    """Runs a blocking call on a pool thread and reports back via the manager.

    It is a plain QRunnable (not a QObject) and owns no QObject, so nothing Qt
    is ever created or destroyed on a worker thread. Results are delivered by
    emitting the manager's own signals, which — because the manager lives on
    the main thread — Qt marshals as a queued call onto the main thread.
    """

    def __init__(self, manager: "LightManager", key: tuple, fn: Callable, *args):
        super().__init__()
        self.setAutoDelete(False)  # freed by the manager on the main thread
        self._manager = manager
        self._key = key
        self._fn = fn
        self._args = args

    def run(self):  # executed on a pool thread
        try:
            result = self._fn(*self._args)
        except Exception as exc:  # noqa: BLE001 - reported to the main thread
            self._manager._worker_failed.emit(self, self._key, exc)
        else:
            self._manager._worker_done.emit(self, self._key, result)


# --- discovery ---------------------------------------------------------------


def _txt_value(info: ServiceInfo, key: str) -> str:
    raw = info.properties.get(key.encode())
    if raw is None:
        return ""
    return raw.decode(errors="replace") if isinstance(raw, bytes) else str(raw)


class _Discovered:
    """Plain data carried from a zeroconf thread to the main thread."""

    def __init__(self, name: str, address: str, port: int, mac: str, model: str):
        self.name = name
        self.address = address
        self.port = port
        self.mac = mac
        self.model = model


# --- manager -----------------------------------------------------------------


class LightManager(QObject, ServiceListener):
    """Owns the device cache. All cache mutation happens on the main thread.

    zeroconf callbacks run on their own threads and only emit ``_discovered`` /
    ``_removed`` signals, which Qt queues onto the main thread.
    """

    light_added = Signal(str)  # light id
    light_updated = Signal(str)  # light id
    light_removed = Signal(str)  # light id

    _discovered = Signal(object)  # _Discovered
    _removed = Signal(str)  # service name

    # Worker results, emitted from pool threads -> queued to the main thread.
    _worker_done = Signal(object, object, object)  # task, key, result
    _worker_failed = Signal(object, object, object)  # task, key, exception

    def __init__(self, parent: Optional[QObject] = None):
        super().__init__(parent)
        self._lights: dict[str, KeyLight] = {}
        self._by_service: dict[str, str] = {}  # mDNS service name -> light id
        self._by_usb_path: dict[bytes, str] = {}  # hidapi path -> light id
        self._by_serial: dict[str, str] = {}  # serialNumber -> light id (dedup)
        self._usb_pending: set[bytes] = set()  # paths awaiting accessory-info
        self._pool = QThreadPool.globalInstance()
        self._tasks: set[_Task] = set()  # keep runnables alive until they report
        self._zc: Optional[Zeroconf] = None
        self._browser: Optional[ServiceBrowser] = None

        # Debounce / coalescing for slider-driven PUTs.
        self._debounce: dict[str, QTimer] = {}
        self._inflight: set[str] = set()
        self._dirty: set[str] = set()

        self._refresh_timer = QTimer(self)
        self._refresh_timer.setInterval(REFRESH_MS)
        self._refresh_timer.timeout.connect(self._on_refresh_tick)
        self._refresh_ticks = 0

        self._discovered.connect(self._on_discovered)
        self._removed.connect(self._on_removed)
        self._worker_done.connect(self._on_worker_done)
        self._worker_failed.connect(self._on_worker_failed)

    # -- lifecycle --

    def start(self):
        self._zc = Zeroconf(ip_version=IPVersion.V4Only)
        self._browser = ServiceBrowser(self._zc, MDNS_TYPE, self)
        self._scan_usb()
        self._refresh_timer.start()

    def stop(self):
        self._refresh_timer.stop()
        if self._zc is not None:
            self._zc.close()
            self._zc = None
        self._browser = None
        self._pool.waitForDone(2000)

    # -- accessors --

    def lights(self) -> list[KeyLight]:
        return list(self._lights.values())

    def get(self, light_id: str) -> Optional[KeyLight]:
        return self._lights.get(light_id)

    def any_on(self) -> bool:
        return any(
            l.online and l.state is not None and l.state.on
            for l in self._lights.values()
        )

    def counts(self) -> tuple[int, int]:
        """(number on, number of online lights)."""
        online = [l for l in self._lights.values() if l.online and l.state]
        on = [l for l in online if l.state.on]
        return len(on), len(online)

    # -- zeroconf ServiceListener (called on zeroconf threads) --

    def add_service(self, zc: Zeroconf, type_: str, name: str):
        self._resolve(zc, type_, name)

    def update_service(self, zc: Zeroconf, type_: str, name: str):
        self._resolve(zc, type_, name)

    def remove_service(self, zc: Zeroconf, type_: str, name: str):
        self._removed.emit(name)

    def _resolve(self, zc: Zeroconf, type_: str, name: str):
        info = zc.get_service_info(type_, name, timeout=2000)
        if info is None:
            return
        addrs = info.parsed_addresses(IPVersion.V4Only)
        if not addrs:
            # Elgato v6-only announcement: skip, as the CLI does.
            return
        self._discovered.emit(
            _Discovered(
                name=name,
                address=addrs[0],
                port=info.port or PORT,
                mac=_txt_value(info, "id"),
                model=_txt_value(info, "md"),
            )
        )

    # -- main-thread handlers for discovery --

    def _on_discovered(self, d: _Discovered):
        light_id = normalize_mac(d.mac) if d.mac else d.name
        self._by_service[d.name] = light_id
        light = self._lights.get(light_id)
        if light is None:
            light = KeyLight(
                id=light_id,
                name=d.name.split(".")[0],
                address=d.address,
                port=d.port,
                product=d.model,
            )
            self._lights[light_id] = light
            self._fetch_info(light)
            self._fetch_state(light, announce=True)
        else:
            # Known light (possibly new IP after DHCP change).
            light.address = d.address
            light.port = d.port
            self._fetch_state(light, announce=not light.online)

    def _on_removed(self, name: str):
        light_id = self._by_service.pop(name, None)
        if light_id is None:
            return
        light = self._lights.get(light_id)
        if light is not None and light.online:
            light.online = False
            self.light_updated.emit(light_id)

    # -- USB discovery (main thread) --

    def _scan_usb(self):
        """Enumerate attached Neos and reconcile against the cache.

        Cheap enough to run on the refresh tick. A newly seen path gets its
        accessory-info fetched before a KeyLight is created (so its identity can
        be matched against mDNS); a path that has vanished is a definitive
        removal, unlike a flaky network timeout.
        """
        if not USB_SUPPORTED:
            return
        try:
            paths = set(usbneo.enumerate_neos())
        except Exception:  # noqa: BLE001 - enumeration must never break the tick
            return
        for path in paths:
            if path not in self._by_usb_path and path not in self._usb_pending:
                self._usb_pending.add(path)
                self._submit(("usbinfo", path), usb_probe_info, path)
        for path in list(self._by_usb_path):
            if path not in paths:
                self._on_usb_removed(path)

    def _apply_usb_info(self, path: bytes, info: dict):
        self._usb_pending.discard(path)
        serial = info.get("serialNumber", "")
        mac = normalize_mac(info.get("macAddress", ""))
        light_id = mac or serial or "usb:" + path.decode(errors="replace")
        name = info.get("displayName") or info.get("productName") or light_id

        # A device already known under a serial-based id (no MAC) matches here.
        light = self._lights.get(light_id)
        if light is None and serial and serial in self._by_serial:
            light = self._lights.get(self._by_serial[serial])
            if light is not None:
                light_id = light.id

        self._by_usb_path[path] = light_id
        if serial:
            self._by_serial[serial] = light_id

        if light is not None:
            # Existing entry (mDNS won the discovery race, or serial match):
            # adopt USB as the transport, since it needs no network.
            light.usb_path = path
            light.transport = "usb"
            light.name = name
            light.serial = serial or light.serial
            self._fetch_state(light)
            self.light_updated.emit(light.id)
            return

        light = KeyLight(
            id=light_id,
            name=name,
            address="",
            product=info.get("productName", ""),
            transport="usb",
            usb_path=path,
            serial=serial,
        )
        self._lights[light_id] = light
        self._fetch_state(light, announce=True)

    def _on_usb_removed(self, path: bytes):
        light_id = self._by_usb_path.pop(path, None)
        self._usb_pending.discard(path)
        if light_id is None:
            return
        light = self._lights.get(light_id)
        if light is None:
            return
        light.usb_path = None
        if light.address:
            # Still reachable over the network: fall back to HTTP.
            light.transport = "http"
            self.light_updated.emit(light_id)
        elif light.online:
            light.online = False
            self.light_updated.emit(light_id)

    def _merge(self, keep_id: str, drop_id: str):
        """Fold ``drop_id`` into ``keep_id`` when both name the same device."""
        keep = self._lights.get(keep_id)
        drop = self._lights.get(drop_id)
        if keep is None or drop is None or keep_id == drop_id:
            return
        if not keep.address and drop.address:
            keep.address = drop.address
            keep.port = drop.port
        if keep.usb_path is None and drop.usb_path is not None:
            keep.usb_path = drop.usb_path
            keep.transport = "usb"
        if not keep.serial and drop.serial:
            keep.serial = drop.serial
        for mapping in (self._by_service, self._by_usb_path, self._by_serial):
            for k, lid in list(mapping.items()):
                if lid == drop_id:
                    mapping[k] = keep_id
        del self._lights[drop_id]
        self.light_removed.emit(drop_id)
        self.light_updated.emit(keep_id)

    # -- worker submission & result routing (main thread) --

    def _submit(self, key: tuple, fn: Callable, *args):
        task = _Task(self, key, fn, *args)
        self._tasks.add(task)
        self._pool.start(task)

    def _on_worker_done(self, task: "_Task", key: tuple, result):
        self._tasks.discard(task)
        kind = key[0]
        if kind == "info":
            self._apply_info(key[1], result)
        elif kind == "state":
            self._apply_state(key[1], result, key[2])
        elif kind == "put":
            self._on_put_done(key[1])
        elif kind == "usbinfo":
            self._apply_usb_info(key[1], result)

    def _on_worker_failed(self, task: "_Task", key: tuple, _exc):
        self._tasks.discard(task)
        kind = key[0]
        if kind == "state":
            self._on_http_fail(key[1])
        elif kind == "put":
            self._on_put_fail(key[1])
        elif kind == "usbinfo":
            # Path stays out of the cache; the next scan retries it.
            self._usb_pending.discard(key[1])
        # "info" failures are ignored: the light keeps its fallback name.

    # -- state fetching --

    def _fetch_info(self, light: KeyLight):
        self._submit(("info", light.id), fetch_accessory_info, light)

    def _apply_info(self, light_id: str, info: dict):
        light = self._lights.get(light_id)
        if light is None:
            return
        name = info.get("displayName") or info.get("productName")
        if name:
            light.name = name
        serial = info.get("serialNumber", "")
        if serial:
            light.serial = serial
            other_id = self._by_serial.get(serial)
            if other_id is not None and other_id != light_id:
                # A USB entry for this device arrived first under a different id;
                # fold this network entry into it so USB stays the transport.
                self._merge(keep_id=other_id, drop_id=light_id)
                return
            self._by_serial[serial] = light_id
        self.light_updated.emit(light_id)

    def _fetch_state(self, light: KeyLight, announce: bool = False):
        self._submit(("state", light.id, announce), fetch_lights, light)

    def _apply_state(self, light_id: str, data: dict, announce: bool):
        light = self._lights.get(light_id)
        if light is None:
            return
        lights = data.get("lights") or []
        light.num_lights = int(data.get("numberOfLights", len(lights) or 1))
        was_online = light.online
        light.fail_count = 0
        light.online = True
        if lights:
            light.state = LightState.from_api(lights[0])
        if announce and not was_online:
            self.light_added.emit(light_id)
        else:
            self.light_updated.emit(light_id)

    def _on_http_fail(self, light_id: str):
        light = self._lights.get(light_id)
        if light is None:
            return
        light.fail_count += 1
        if light.online and light.fail_count >= FAIL_LIMIT:
            light.online = False
            self.light_updated.emit(light_id)

    # -- refresh --

    def refresh_all(self):
        for light in self._lights.values():
            if light.id in self._inflight:
                continue
            self._fetch_state(light)

    def _on_refresh_tick(self):
        self._refresh_ticks += 1
        self._scan_usb()
        for light in self._lights.values():
            if light.id in self._inflight:
                continue
            # Offline lights are probed less often to avoid needless timeouts.
            if not light.online and self._refresh_ticks % 3 != 0:
                continue
            self._fetch_state(light)

    # -- user intents (main thread only) --

    def set_on(self, light_id: str, on: bool):
        light = self._lights.get(light_id)
        if light is None or light.state is None:
            return
        light.state.on = on
        self._flush(light_id)

    def set_brightness(self, light_id: str, value: int):
        light = self._lights.get(light_id)
        if light is None or light.state is None:
            return
        light.state.brightness = clamp(int(value), 0, 100)
        self._schedule(light_id)

    def set_temperature(self, light_id: str, mireds: int):
        light = self._lights.get(light_id)
        if light is None or light.state is None:
            return
        light.state.temperature = clamp(int(mireds), MIRED_MIN, MIRED_MAX)
        self._schedule(light_id)

    def toggle_all(self):
        target_on = not self.any_on()
        for light in self._lights.values():
            if light.online and light.state is not None:
                light.state.on = target_on
                self._flush(light.id)

    def flush_pending(self, light_id: str):
        """Send any debounced change immediately (e.g. on slider release)."""
        timer = self._debounce.get(light_id)
        if timer is not None and timer.isActive():
            timer.stop()
        self._flush(light_id)

    # -- debounce / coalescing --

    def _schedule(self, light_id: str):
        timer = self._debounce.get(light_id)
        if timer is None:
            timer = QTimer(self)
            timer.setSingleShot(True)
            timer.setInterval(DEBOUNCE_MS)
            timer.timeout.connect(lambda lid=light_id: self._flush(lid))
            self._debounce[light_id] = timer
        timer.start()

    def _flush(self, light_id: str):
        light = self._lights.get(light_id)
        if light is None or light.state is None:
            return
        if light_id in self._inflight:
            # A PUT is already running; remember to resend the latest state.
            self._dirty.add(light_id)
            return
        self._inflight.add(light_id)
        # Snapshot so the in-flight PUT reflects the state at send time.
        snapshot = LightState(
            on=light.state.on,
            brightness=light.state.brightness,
            temperature=light.state.temperature,
        )
        self._submit(("put", light_id), put_lights, light, snapshot)

    def _on_put_done(self, light_id: str):
        self._inflight.discard(light_id)
        light = self._lights.get(light_id)
        if light is not None:
            light.fail_count = 0
            if not light.online:
                light.online = True
        self.light_updated.emit(light_id)
        if light_id in self._dirty:
            self._dirty.discard(light_id)
            self._flush(light_id)

    def _on_put_fail(self, light_id: str):
        self._inflight.discard(light_id)
        self._dirty.discard(light_id)
        self._on_http_fail(light_id)


if __name__ == "__main__":
    # Headless smoke test: discover for a few seconds and print what we find.
    import sys

    from PySide6.QtCore import QCoreApplication

    app = QCoreApplication(sys.argv)
    mgr = LightManager()

    def report(light_id: str):
        light = mgr.get(light_id)
        if light is None:
            return
        st = light.state
        detail = (
            f"on={st.on} brightness={st.brightness} temp={kelvin_label(st.temperature)}"
            if st
            else "no state"
        )
        where = (
            f"USB {light.usb_path.decode()}"
            if light.transport == "usb"
            else light.base_url
        )
        print(f"[found] {light.name} via {where} ({light.product}) {detail}")

    mgr.light_added.connect(report)
    mgr.start()
    print(f"Browsing for {MDNS_TYPE} (IPv4 only) for 5s ...")
    QTimer.singleShot(5000, app.quit)
    app.exec()
    found = mgr.lights()
    print(f"\n{len(found)} light(s) discovered.")
    mgr.stop()
