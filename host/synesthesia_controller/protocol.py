"""Shared validation and serial message formatting."""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, asdict


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def hue_from_request(value: object, mode: str = "raw") -> tuple[float, float]:
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError("hue value must be numeric") from exc
    if mode == "raw":
        raw = clamp(number, 0.0, 2.0)
    elif mode == "normalized":
        raw = clamp(number, 0.0, 1.0) * 2.0
    else:
        raise ValueError("mode must be 'raw' or 'normalized'")
    return raw, raw / 2.0


def hue_message(raw: float, normalized: float) -> bytes:
    payload = {"type": "hue", "raw": round(raw, 6), "normalized": round(normalized, 6)}
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")


def osc_float_message(address: str, value: float) -> bytes:
    """Build a minimal OSC 1.0 message with one 32-bit float argument."""
    if not address.startswith("/"):
        raise ValueError("OSC address must start with '/'")

    def padded(value: bytes) -> bytes:
        return value + b"\0" * ((4 - len(value) % 4) % 4)

    return padded(address.encode("utf-8")) + padded(b",f") + struct.pack(">f", float(value))


@dataclass
class ControllerConfig:
    deadband: float = 0.02
    hold_timeout_ms: int = 400
    pullaway_speed_mm_s: int = 800
    active_min_mm: int = 50
    active_max_mm: int = 500

    def validate(self) -> "ControllerConfig":
        if not 0 <= self.deadband <= 1:
            raise ValueError("deadband must be between 0 and 1")
        if self.hold_timeout_ms < 0:
            raise ValueError("hold_timeout_ms must not be negative")
        if self.pullaway_speed_mm_s <= 0:
            raise ValueError("pullaway_speed_mm_s must be positive")
        if not 0 <= self.active_min_mm < self.active_max_mm:
            raise ValueError("active distance range is invalid")
        return self

    def update(self, values: dict) -> "ControllerConfig":
        candidate = asdict(self)
        for key in candidate:
            if key in values:
                candidate[key] = values[key]
        return ControllerConfig(**candidate).validate()
