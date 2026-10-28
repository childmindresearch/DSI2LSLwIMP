"""Explicitly simulated backend for GUI tests: no headset access and no LSL outlets."""
import json
import queue
import sys
import threading
import time

HOLD_SECONDS, RELEASE_SECONDS, SETTLE_SECONDS = 0.3, 0.2, 0.2


def emit(kind, **fields):
    print("DSI_GUI " + json.dumps({"protocol": 1, "type": kind, **fields}), flush=True)


def main():
    print("SIMULATED GUI TEST BACKEND — no headset connection or LSL streams", file=sys.stderr, flush=True)
    if "--check-libraries" in sys.argv:
        print("Simulated library check", file=sys.stderr, flush=True)
        return 0
    options = dict(arg[2:].split("=", 1) for arg in sys.argv[1:] if arg.startswith("--") and "=" in arg)
    if options.get("port") == "SIM_FAIL":
        print("Simulated connection failure", file=sys.stderr, flush=True)
        return 1
    emit("config", stream_name=options.get("lsl-stream-name", "WS-default"), model="SIMULATED DSI",
         serial=123, sampling_rate=300, eeg_channels=2, labels=["F3", "CMF=Pz"])
    emit("event", event="outlets_ready")
    commands = queue.Queue()

    def read_commands():
        for line in sys.stdin:
            commands.put(line.strip())

    threading.Thread(target=read_commands, daemon=True).start()
    driver = 0
    acquiring = False
    count = 0
    started = time.monotonic()
    next_update = 0.0
    reset_mode, phase, phase_until, settled = 0, "idle", 0.0, 0.0
    while True:
        try:
            command = commands.get(timeout=0.02)
        except queue.Empty:
            command = None
        if command == "quit" and options.get("port") != "SIM_HANG":
            emit("event", event="impedance_off_requested")
            emit("event", event="impedance_off_command_returned")
            if acquiring:
                emit("event", event="acquisition_stopped")
            emit("event", event="backend_disconnected")
            return 0
        if command == "start":
            acquiring = True
            driver = int(options.get("impedance") == "on")
            emit("event", event="acquisition_started")
        if command == "stop":
            acquiring = False
            driver = 0
            emit("event", event="acquisition_stopped")
        if command in ("on", "off"):
            emit("event", event=f"impedance_{command}_requested")
            driver = int(command == "on")
            emit("event", event=f"impedance_{command}_command_returned")
        now = time.monotonic()
        if command == "reset":
            # Mirror the real backend: hold, release, confirm mode 0 (or fail when stuck), then settle.
            emit("event", event="analog_reset_requested")
            reset_mode, phase, phase_until = 1, "holding", now + HOLD_SECONDS
            emit("event", event="analog_reset_started")
            emit("event", event="analog_reset_state mode=1")
            next_update = 0.0
        elif phase == "holding" and now >= phase_until:
            phase, phase_until = "releasing", now + RELEASE_SECONDS
            emit("event", event="analog_reset_released")
            next_update = 0.0
        elif phase == "releasing" and now >= phase_until:
            if options.get("port") == "SIM_RESET_STUCK":
                phase = "failed"
                emit("event", event="analog_reset_release_failed")
            else:
                reset_mode, phase, settled = 0, "idle", now + SETTLE_SECONDS
                emit("event", event="analog_reset_state mode=0")
            next_update = 0.0
        if now >= next_update:
            if acquiring:
                count += 30
            ready = int(acquiring and driver and phase == "idle" and now >= settled)
            emit("state", timestamp=now, driver_mode=driver, reset_mode=reset_mode, reset_phase=phase,
                 requested_on=driver, command_pending=0, impedance_ready=ready, sample_count=count,
                 acquiring=int(acquiring), eeg_subscribers=0, impedance_subscribers=0, marker_subscribers=0,
                 impedance_timestamp=now, impedance=[123000, 456000] if ready else [None, None])
            next_update = now + 0.1
        if options.get("port") == "SIM_EXIT" and now - started > 0.3:
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
