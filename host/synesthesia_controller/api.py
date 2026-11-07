#!/usr/bin/env python3
"""Development HTTP API and serial transport for the hue controller."""

from __future__ import annotations

import argparse
import json
import socket
import threading
import urllib.request
import urllib.error
from queue import Queue, Full
from dataclasses import asdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional

from protocol import ControllerConfig, hue_from_request, hue_message, osc_float_message


ROOT = Path(__file__).parent


class Transport:
    def __init__(self, serial_path: Optional[str], osc_host: Optional[str], osc_port: int, osc_address: str):
        self.serial_path = serial_path
        self.osc_target = (osc_host, osc_port) if osc_host else None
        self.osc_address = osc_address
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if self.osc_target else None
        self.last_message: Optional[bytes] = None
        self._serial = None
        self._serial_error = OSError
        self._serial_queue = Queue(maxsize=8)
        if serial_path:
            try:
                import serial  # type: ignore
            except ImportError as exc:
                raise SystemExit("Install pyserial or use --dry-run") from exc
            self._serial_error = serial.SerialException
            self._serial = serial.Serial(serial_path, 115200, timeout=0.2, write_timeout=0.2)
            threading.Thread(target=self._write_loop, name="esp32-serial-writer", daemon=True).start()

    def set_osc(self, host: str, port: int, address: str):
        if not host:
            raise ValueError("osc_host must not be empty")
        if not 1 <= int(port) <= 65535:
            raise ValueError("osc_port must be between 1 and 65535")
        if not address.startswith("/"):
            raise ValueError("osc_address must start with '/'")
        old = self._udp
        self.osc_target = (host, int(port))
        self.osc_address = address
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if old:
            old.close()

    def _write_loop(self):
        while True:
            message = self._serial_queue.get()
            try:
                self._serial.write(message)
                self._serial.flush()
            except (self._serial_error, TimeoutError) as exc:
                print(f"Serial write unavailable: {exc}")
            finally:
                self._serial_queue.task_done()

    def send_osc(self, raw: float, normalized: float) -> None:
        if self._udp:
            self._udp.sendto(osc_float_message(self.osc_address, normalized), self.osc_target)

    def send(self, message: bytes) -> None:
        self.last_message = message
        if self._serial:
            try:
                self._serial_queue.put_nowait(message)
            except Full:
                print("Serial output queue full; dropping message")

        payload = json.loads(message.decode("utf-8"))
        self.send_osc(payload["raw"], payload["normalized"])

    def send_serial(self, message: bytes) -> None:
        if not self._serial:
            return
        try:
            self._serial_queue.put_nowait(message)
        except Full:
            print("Serial output queue full; dropping message")

    def send_network_osc_config(self, device_ip: str, host: str, port: int, address: str) -> None:
        if not device_ip or device_ip == "0.0.0.0":
            return
        payload = json.dumps({"host": host, "port": port, "address": address}).encode("utf-8")
        request = urllib.request.Request(f"http://{device_ip}/api/osc", data=payload, headers={"content-type": "application/json"}, method="POST")
        try:
            urllib.request.urlopen(request, timeout=0.75).read()
        except (OSError, urllib.error.URLError):
            pass

    def start_reader(self, callback):
        if not self._serial:
            return

        def read_loop():
            while True:
                try:
                    line = self._serial.readline()
                    if not line:
                        continue
                    callback(json.loads(line.decode("utf-8")))
                except OSError as exc:
                    print(f"Serial reader stopped: {exc}")
                    return
                except (json.JSONDecodeError, UnicodeDecodeError, ValueError) as exc:
                    print(f"Ignoring serial input: {exc}")

        threading.Thread(target=read_loop, name="esp32-serial-reader", daemon=True).start()


class State:
    def __init__(self, transport: Transport):
        self.transport = transport
        self.config = ControllerConfig()
        self.raw = 0.0
        self.normalized = 0.0
        self.lock = threading.Lock()
        self.wifi = {"ssid": "", "secret": ""}
        self.wifi_status = {"state": "not_configured", "ssid": "", "ip": "0.0.0.0"}
        self.device_host = "huecontroller.local"

    def apply_hue(self, raw: float, normalized: float):
        with self.lock:
            self.raw, self.normalized = raw, normalized


def make_handler(state: State):
    class Handler(BaseHTTPRequestHandler):
        def _json(self, status: int, payload: dict):
            data = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _body(self) -> dict:
            length = int(self.headers.get("content-length", "0"))
            try:
                value = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError as exc:
                raise ValueError("request body must be JSON") from exc
            if not isinstance(value, dict):
                raise ValueError("request body must be an object")
            return value

        def do_GET(self):
            if self.path == "/":
                data = (ROOT / "configurator.html").read_bytes()
                self.send_response(200)
                self.send_header("content-type", "text/html; charset=utf-8")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            with state.lock:
                if self.path == "/api/config":
                    result = asdict(state.config)
                    result.update({"osc_host": state.transport.osc_target[0], "osc_port": state.transport.osc_target[1], "osc_address": state.transport.osc_address})
                    return self._json(200, result)
                if self.path == "/api/status":
                    return self._json(200, {"raw": state.raw, "normalized": state.normalized, "serial": state.transport.serial_path, "osc": state.transport.osc_target, "osc_address": state.transport.osc_address, "device_host": state.device_host, "wifi": state.wifi_status})
                if self.path == "/api/wifi":
                    return self._json(200, {"ssid": state.wifi["ssid"], "secret_set": bool(state.wifi["secret"])})
            self._json(404, {"error": "not found"})

        def do_POST(self):
            if self.path != "/api/hue":
                return self._json(404, {"error": "not found"})
            try:
                body = self._body()
                raw, normalized = hue_from_request(body.get("value"), body.get("mode", "raw"))
            except ValueError as exc:
                return self._json(400, {"error": str(exc)})
            with state.lock:
                state.raw, state.normalized = raw, normalized
                state.transport.send(hue_message(raw, normalized))
                return self._json(200, {"raw": raw, "normalized": normalized})

        def do_PUT(self):
            if self.path == "/api/wifi":
                try:
                    body = self._body()
                    ssid = str(body.get("ssid", "")).strip()
                    if not ssid:
                        raise ValueError("ssid must not be empty")
                    with state.lock:
                        state.wifi["ssid"] = ssid
                        if "secret" in body and body["secret"]:
                            state.wifi["secret"] = str(body["secret"])
                        state.transport.send_serial((json.dumps({"type": "wifi", "ssid": state.wifi["ssid"], "secret": state.wifi["secret"]}, separators=(",", ":")) + "\n").encode("utf-8"))
                        return self._json(200, {"ssid": state.wifi["ssid"], "secret_set": bool(state.wifi["secret"])})
                except (ValueError, TypeError) as exc:
                    return self._json(400, {"error": str(exc)})
            if self.path != "/api/config":
                return self._json(404, {"error": "not found"})
            try:
                body = self._body()
                with state.lock:
                    state.config = state.config.update(body)
                    if "device_host" in body:
                        state.device_host = str(body["device_host"]).strip()
                    if any(key in body for key in ("osc_host", "osc_port", "osc_address")):
                        current_host, current_port = state.transport.osc_target
                        state.transport.set_osc(body.get("osc_host", current_host), body.get("osc_port", current_port), body.get("osc_address", state.transport.osc_address))
                        state.transport.send_serial((json.dumps({"type": "osc", "host": state.transport.osc_target[0], "port": state.transport.osc_target[1], "address": state.transport.osc_address}, separators=(",", ":")) + "\n").encode("utf-8"))
                        state.transport.send_network_osc_config(state.device_host, state.transport.osc_target[0], state.transport.osc_target[1], state.transport.osc_address)
                    result = asdict(state.config)
                    result.update({"osc_host": state.transport.osc_target[0], "osc_port": state.transport.osc_target[1], "osc_address": state.transport.osc_address, "device_host": state.device_host})
                    return self._json(200, result)
            except (ValueError, TypeError) as exc:
                return self._json(400, {"error": str(exc)})

        def log_message(self, *_args):
            return

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", help="serial device, for example /dev/cu.usbmodem101")
    parser.add_argument("--dry-run", action="store_true", help="do not open a serial device")
    parser.add_argument("--no-osc", action="store_true", help="do not send OSC to Synesthesia")
    parser.add_argument("--osc-host", default="127.0.0.1")
    parser.add_argument("--osc-port", type=int, default=6000)
    parser.add_argument("--osc-address", default="/controls/meta/hue")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if args.serial and args.dry_run:
        parser.error("choose --serial or --dry-run")
    transport = Transport(None if args.dry_run else args.serial, None if args.no_osc else args.osc_host, args.osc_port, args.osc_address)
    state = State(transport)

    def handle_device_message(message):
        if message.get("type") == "wifi_status":
            state.wifi_status = {"state": message.get("state", "unknown"), "ssid": message.get("ssid", ""), "ip": message.get("ip", "0.0.0.0")}
            return
        if message.get("type") != "hue":
            return
        if "raw" in message:
            raw, normalized = hue_from_request(message["raw"], "raw")
        elif "normalized" in message:
            raw, normalized = hue_from_request(message["normalized"], "normalized")
        else:
            return
        state.apply_hue(raw, normalized)
        transport.send_osc(raw, normalized)

    transport.start_reader(handle_device_message)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(state))
    print(f"Hue API listening on http://{args.host}:{args.port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
