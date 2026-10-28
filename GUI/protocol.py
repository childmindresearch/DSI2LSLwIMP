"""Arguments, backend discovery, and the versioned stdout protocol (no Qt dependency)."""
from dataclasses import dataclass
import json
import math
from pathlib import Path
import sys

PREFIX = "DSI_GUI "
RESET_PHASES = ("idle", "holding", "releasing", "failed")


@dataclass
class Options:
    port: str = ""
    stream_name: str = "WS-default"
    montage: str = ""
    reference: str = ""
    impedance_on: bool = False
    settle_seconds: float = 2.0
    verbosity: int = 2

    def arguments(self) -> list[str]:
        name = self.stream_name.strip()
        if not name or len(name.encode("utf-8")) > 180:
            raise ValueError("Enter a stream name of 1–180 UTF-8 bytes.")
        for value in (self.port, name, self.montage, self.reference):
            if any(c in value for c in "\r\n\0"):
                raise ValueError("Configuration fields cannot contain newlines or NUL characters.")
        if not math.isfinite(self.settle_seconds) or self.settle_seconds < 0:
            raise ValueError("Settling time must be a finite, nonnegative number.")
        args = [f"--lsl-stream-name={name}", "--gui-telemetry",
                f"--impedance={'on' if self.impedance_on else 'off'}",
                f"--impedance-settle={self.settle_seconds:g}", f"--verbosity={self.verbosity}"]
        for key, value in (("port", self.port), ("montage", self.montage), ("reference", self.reference)):
            if value.strip():
                args.append(f"--{key}={value.strip()}")
        return args  # Passed directly to QProcess: no shell quoting or shell execution.


def find_backend() -> Path | None:
    filename = "dsi2lsl.exe" if sys.platform == "win32" else "dsi2lsl"
    root = Path(__file__).resolve().parent.parent
    candidates = []
    if getattr(sys, "frozen", False):
        candidates.append(Path(sys._MEIPASS) / "backend" / filename)
        candidates.append(Path(sys.executable).parent / filename)
    candidates.extend(root / folder / filename for folder in ("build-windows/Release", "build/Release", "build", "", "backend"))
    return next((p for p in candidates if p.is_file()), None)


def decode_record(line: str) -> dict | None:
    if not line.startswith(PREFIX):
        return None

    def reject_constant(value):
        raise ValueError(f"Non-JSON numeric constant: {value}")

    record = json.loads(line[len(PREFIX):], parse_constant=reject_constant)
    if not isinstance(record, dict) or record.get("protocol") != 1:
        raise ValueError("Unsupported GUI telemetry protocol; rebuild the matching backend.")
    kind = record.get("type")
    if kind == "config":
        labels = record.get("labels")
        if not isinstance(labels, list) or not labels or not all(isinstance(x, str) for x in labels):
            raise ValueError("Invalid electrode labels in telemetry.")
        if not isinstance(record.get("stream_name"), str):
            raise ValueError("Missing stream name in telemetry.")
    elif kind == "state":
        for key in ("driver_mode", "reset_mode", "requested_on", "command_pending", "impedance_ready", "sample_count"):
            if type(record.get(key)) is not int:
                raise ValueError(f"Invalid state field: {key}")
        # Older backends omit reset_phase; treat as idle but reject unknown values.
        if record.setdefault("reset_phase", "idle") not in RESET_PHASES:
            raise ValueError("Invalid state field: reset_phase")
        if record["sample_count"] < 0:
            raise ValueError("Invalid sample count.")
        values = record.get("impedance")
        if not isinstance(values, list) or not all(
                v is None or (type(v) in (int, float) and math.isfinite(v) and v >= 0) for v in values):
            raise ValueError("Invalid impedance values in telemetry.")
    elif kind == "event":
        if not isinstance(record.get("event"), str):
            raise ValueError("Invalid event in telemetry.")
    else:
        raise ValueError("Unknown telemetry record type.")
    return record
