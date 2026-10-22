"""Nonblocking subprocess lifecycle; the GUI never accesses the headset serial port."""
import time
from PySide6.QtCore import QObject, QProcess, QTimer, Signal
from .protocol import decode_record


class BackendController(QObject):
    log = Signal(str)
    config_received = Signal(dict)
    state_received = Signal(dict)
    event_received = Signal(str)
    changed = Signal()
    completed = Signal(int, bool)
    slow_stop = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.process = QProcess(self)
        self.process.setProcessChannelMode(QProcess.ProcessChannelMode.SeparateChannels)
        self.process.started.connect(self._started)
        self.process.readyReadStandardOutput.connect(self._stdout)
        self.process.readyReadStandardError.connect(self._stderr)
        self.process.finished.connect(self._finished)
        self.process.errorOccurred.connect(self._error)
        self.stop_timer = QTimer(self)
        self.stop_timer.setSingleShot(True)
        self.stop_timer.timeout.connect(self.slow_stop.emit)
        self.stage = "idle"
        self.probing = False
        self.pending_command = None
        self.config = None
        self.state = None
        self.last_data = None
        self._count = 0
        self._buffers = {"stdout": b"", "stderr": b""}
        self._done = True
        self._quit_sent = False

    @property
    def busy(self):
        return not self._done

    def start(self, program, args, working_directory, *, probing=False):
        if self.busy:
            return False
        self.probing = probing
        self.stage = "checking" if probing else "connecting"
        self.pending_command = None
        self.config = self.state = self.last_data = None
        self._count = 0
        self._done = False
        self._quit_sent = False
        self._buffers = {"stdout": b"", "stderr": b""}
        self.process.setWorkingDirectory(str(working_directory))
        self.changed.emit()
        self.process.start(str(program), args)
        return True

    def _started(self):
        if self.stage == "stopping":
            self._quit()

    def send(self, command):
        allowed = (command == "start" and self.stage == "ready") or (
            command in ("stop", "on", "off", "reset") and self.stage == "acquiring")
        if not allowed or self.pending_command:
            return False
        data = (command + "\n").encode("ascii")
        if self.process.write(data) != len(data):
            self.log.emit("Could not queue the hardware command.")
            return False
        self.pending_command = command
        self.changed.emit()
        return True

    def pause(self):
        return self.send("stop")

    def stop(self):
        if not self.busy or self.stage == "stopping":
            return
        self.stage = "stopping"
        self.pending_command = None
        self.stop_timer.start(5000)
        self.changed.emit()
        self._quit()

    def _quit(self):
        if self._quit_sent or self.process.state() != QProcess.ProcessState.Running:
            return
        if self.probing:
            return  # Library check normally exits on its own, without a headset.
        self._quit_sent = True
        self.process.write(b"quit\n")
        self.process.closeWriteChannel()  # Qt drains queued bytes before closing stdin.
        self.log.emit("Stop requested; waiting for the backend to disable impedance and disconnect.")

    def force_stop(self):
        if self.busy:
            self.log.emit("FORCED STOP: hardware shutdown/markers may be incomplete. Check or power-cycle the headset.")
            self.process.kill()

    def _stdout(self):
        self._consume("stdout", bytes(self.process.readAllStandardOutput()))

    def _stderr(self):
        self._consume("stderr", bytes(self.process.readAllStandardError()))

    def _consume(self, channel, data, final=False):
        buffer = self._buffers[channel] + data
        lines = buffer.split(b"\n")
        self._buffers[channel] = b"" if final else lines.pop()
        for line in lines:
            text = line.rstrip(b"\r").decode("utf-8", errors="replace")
            if not text:
                continue
            if channel == "stderr":
                self.log.emit(text)
                continue
            try:
                record = decode_record(text)
            except (ValueError, TypeError) as error:
                self.log.emit(f"Telemetry ignored: {error}")
                continue
            if record is None:
                self.log.emit(text)
            elif record["type"] == "config":
                self.config = record
                self.config_received.emit(record)
            elif record["type"] == "state":
                if self.config is None or len(record["impedance"]) != len(self.config["labels"]):
                    self.log.emit("Telemetry ignored: electrode metadata missing or value count mismatch.")
                    continue
                self.state = record
                if record["sample_count"] > self._count:
                    self.last_data = time.monotonic()
                self._count = record["sample_count"]
                self.state_received.emit(record)
            else:
                event = record["event"]
                if event == "outlets_ready" and self.stage != "stopping":
                    self.stage = "ready"
                if event == "acquisition_started" and self.stage != "stopping":
                    self.stage = "acquiring"
                    self.last_data = None
                if event == "acquisition_stopped" and self.stage != "stopping":
                    self.stage = "ready"
                    self.last_data = None
                returned = {"on": "impedance_on_command_returned", "off": "impedance_off_command_returned",
                            "reset": "analog_reset_command_returned", "start": "acquisition_started",
                            "stop": "acquisition_stopped"}
                if event == returned.get(self.pending_command) or event.endswith("_command_failed"):
                    self.pending_command = None
                self.event_received.emit(event)
            self.changed.emit()
        # Protect the UI from an incompatible/noisy backend that never terminates a line.
        if len(self._buffers[channel]) > 1024 * 1024:
            self._buffers[channel] = b""
            self.log.emit("Discarded an oversized backend output line.")

    def _error(self, error):
        self.log.emit(f"Backend: {self.process.errorString()}")
        if error == QProcess.ProcessError.FailedToStart:
            self._complete(-1, True)

    def _finished(self, code, status):
        self._stdout()
        self._stderr()
        self._consume("stdout", b"", final=True)
        self._consume("stderr", b"", final=True)
        self._complete(code, status == QProcess.ExitStatus.CrashExit)

    def _complete(self, code, crashed):
        if self._done:
            return
        self._done = True
        self.stop_timer.stop()
        self.stage = "idle" if code == 0 and not crashed else "error"
        self.pending_command = None
        self.last_data = None
        self.changed.emit()
        self.completed.emit(code, crashed)
