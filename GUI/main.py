"""Qt desktop UI for macOS and Windows. Run with `python -m GUI`."""
import argparse
import os
from pathlib import Path
import sys
import time
import tempfile

from PySide6.QtCore import QSettings, Qt, QTimer
from PySide6.QtGui import QColor, QCloseEvent
from PySide6.QtSerialPort import QSerialPortInfo
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog,
    QGridLayout, QGroupBox, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMainWindow,
    QMessageBox, QPlainTextEdit, QPushButton, QSpinBox, QSplitter, QTableWidget,
    QTableWidgetItem, QVBoxLayout, QWidget,
)
from .controller import BackendController
from .protocol import Options, find_backend


class MainWindow(QMainWindow):
    def __init__(self, *, backend=None, settings=None, launch_prefix=None, auto_prepare=False, port=None):
        super().__init__()
        self.settings = settings if settings is not None else QSettings("DSI2LSL", "ImpedanceGUI")
        self.launch_prefix = launch_prefix or []  # Injection point for hardware-free subprocess tests.
        self.controller = BackendController(self)
        self.close_after_stop = False
        self.setWindowTitle("DSI2LSL — EEG & impedance")
        self.resize(1050, 780)
        self.setMinimumSize(800, 660)
        self._build_ui()
        self._restore_settings(backend)
        if port is not None:
            self.port.setEditText(port)
        self.controller.log.connect(self.console.appendPlainText)
        self.controller.changed.connect(self._render)
        self.controller.config_received.connect(self._configure_table)
        self.controller.completed.connect(self._completed)
        self.controller.slow_stop.connect(self._slow_stop)
        self.watchdog = QTimer(self)
        self.watchdog.timeout.connect(self._render)
        self.watchdog.start(1000)
        self._render()
        if auto_prepare:
            QTimer.singleShot(0, self._auto_prepare)

    def _build_ui(self):
        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(18, 16, 18, 16)
        title = QLabel("DSI → LSL")
        title.setStyleSheet("font-size: 24px; font-weight: 600;")
        layout.addWidget(title)
        layout.addWidget(QLabel("Stream EEG, electrode impedance, and impedance event markers together."))

        self.configuration = QGroupBox("Connection / streams")
        grid = QGridLayout(self.configuration)
        self.port = QComboBox()
        self.port.setEditable(True)
        self.port.setToolTip("COM4 on Windows; /dev/cu.* on macOS. Blank uses DSISerialPort.")
        self.refresh = QPushButton("Refresh ports")
        self.refresh.clicked.connect(self.refresh_ports)
        port_row = QHBoxLayout()
        port_row.addWidget(self.port, 1)
        port_row.addWidget(self.refresh)
        self.stream_name = QLineEdit("WS-default")
        self.stream_name.setToolTip("Public LSL stream name. Do not include participant-identifying information.")
        self.stream_name.textChanged.connect(self._update_stream_names)
        self.montage = QLineEdit()
        self.montage.setPlaceholderText("Vendor default — or F3,F4,C3,C4")
        self.montage.setToolTip("DSI montage expression, e.g. F3,F4 or F3-F4. Leave blank for vendor defaults.")
        self.reference = QLineEdit()
        self.reference.setPlaceholderText("Vendor default — or A1/2+A2/2")
        self.reference.setToolTip("DSI reference expression; blank uses the vendor-selected reference.")
        self.startup_impedance = QCheckBox("Enable impedance at startup")
        self.startup_impedance.setToolTip("Off by default. Impedance injects 110/130 Hz test signals into EEG.")
        self.settle = QDoubleSpinBox()
        self.settle.setRange(0, 120)
        self.settle.setValue(2)
        self.settle.setSuffix(" s")
        self.settle.setToolTip("Suppress impedance estimates after state changes. This does NOT guarantee clean EEG.")
        for row, label, widget in ((0, "Port", None), (1, "Montage", self.montage)):
            grid.addWidget(QLabel(label), row, 0)
            if widget:
                grid.addWidget(widget, row, 1)
        grid.addLayout(port_row, 0, 1)
        grid.addWidget(QLabel("LSL stream name"), 0, 2)
        grid.addWidget(self.stream_name, 0, 3)
        grid.addWidget(QLabel("Reference"), 1, 2)
        grid.addWidget(self.reference, 1, 3)
        grid.addWidget(self.startup_impedance, 2, 1)
        grid.addWidget(QLabel("Impedance settling"), 2, 2)
        grid.addWidget(self.settle, 2, 3)
        grid.setColumnStretch(1, 1)
        grid.setColumnStretch(3, 1)
        layout.addWidget(self.configuration)

        self.advanced = QGroupBox("Backend")
        backend_layout = QHBoxLayout(self.advanced)
        self.backend_path = QLineEdit()
        self.backend_path.setPlaceholderText("Build dsi2lsl, or select the executable…")
        self.backend_path.setToolTip("Use this project's updated backend. The original backend has no GUI telemetry or runtime impedance controls.")
        self.browse = QPushButton("Browse…")
        self.browse.clicked.connect(self._browse_backend)
        self.check = QPushButton("Check libraries")
        self.check.clicked.connect(self.check_libraries)
        self.verbosity = QSpinBox()
        self.verbosity.setRange(0, 10)
        self.verbosity.setValue(2)
        backend_layout.addWidget(self.backend_path, 1)
        backend_layout.addWidget(self.browse)
        backend_layout.addWidget(self.check)
        backend_layout.addWidget(QLabel("Verbosity"))
        backend_layout.addWidget(self.verbosity)
        layout.addWidget(self.advanced)
        self.streams = QLabel()
        self.streams.setWordWrap(True)
        self.streams.setTextFormat(Qt.TextFormat.PlainText)
        layout.addWidget(self.streams)

        actions = QHBoxLayout()
        self.prepare = QPushButton("Prepare outlets")
        self.prepare.clicked.connect(self.prepare_outlets)
        self.start = QPushButton("Start streaming")
        self.start.setStyleSheet("font-weight: 600; padding: 6px 14px;")
        self.start.clicked.connect(self.start_streaming)
        self.stop = QPushButton("Stop")
        self.stop.clicked.connect(self.controller.pause)
        self.disconnect = QPushButton("Disconnect")
        self.disconnect.clicked.connect(self.controller.stop)
        self.imp_on = QPushButton("Impedance on")
        self.imp_on.clicked.connect(lambda: self.controller.send("on"))
        self.imp_off = QPushButton("Impedance off")
        self.imp_off.clicked.connect(lambda: self.controller.send("off"))
        self.reset = QPushButton("Analog reset…")
        self.reset.clicked.connect(self._confirm_reset)
        self.force = QPushButton("Force stop…")
        self.force.setVisible(False)
        self.force.clicked.connect(self._confirm_force)
        for button in (self.prepare, self.start, self.stop, self.disconnect, self.imp_on, self.imp_off, self.reset, self.force):
            actions.addWidget(button)
        actions.addStretch()
        layout.addLayout(actions)
        status = QHBoxLayout()
        self.acquisition_status = QLabel("Idle")
        self.acquisition_status.setStyleSheet("font-weight: 600;")
        self.impedance_status = QLabel("Impedance: unknown")
        self.reset_status = QLabel("Reset: unknown")
        for label in (self.acquisition_status, self.impedance_status, self.reset_status):
            status.addWidget(label)
        status.addStretch()
        layout.addLayout(status)
        self.device_status = QLabel("No headset connected")
        self.device_status.setTextFormat(Qt.TextFormat.PlainText)
        layout.addWidget(self.device_status)
        self.subscribers = QLabel("Outlets not yet advertised")
        layout.addWidget(self.subscribers)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        table_box = QGroupBox("Electrode contact estimates")
        table_layout = QVBoxLayout(table_box)
        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["Electrode", "Impedance (kΩ)", "Contact"])
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.verticalHeader().hide()
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        table_layout.addWidget(self.table)
        hint = QLabel("— = off, resetting, settling, unavailable, or stale.\nContact ratings are not an EEG quality guarantee.")
        hint.setWordWrap(True)
        table_layout.addWidget(hint)
        console_box = QGroupBox("Backend log")
        console_layout = QVBoxLayout(console_box)
        self.console = QPlainTextEdit()
        self.console.setReadOnly(True)
        self.console.setMaximumBlockCount(2000)
        console_layout.addWidget(self.console)
        log_buttons = QHBoxLayout()
        save = QPushButton("Save log…")
        save.clicked.connect(self._save_log)
        clear = QPushButton("Clear")
        clear.clicked.connect(self.console.clear)
        log_buttons.addWidget(save)
        log_buttons.addWidget(clear)
        log_buttons.addStretch()
        console_layout.addLayout(log_buttons)
        splitter.addWidget(table_box)
        splitter.addWidget(console_box)
        splitter.setSizes([430, 570])
        layout.addWidget(splitter, 1)
        warning = QLabel("Impedance adds 110/130 Hz test signals to EEG. Reset can disturb EEG. Record all three streams in LabRecorder.")
        warning.setWordWrap(True)
        layout.addWidget(warning)
        self.setCentralWidget(central)

    def refresh_ports(self):
        selected = self.port.currentText()
        ports = {}
        for port in QSerialPortInfo.availablePorts():
            address = port.portName() if sys.platform == "win32" else port.systemLocation()
            if sys.platform == "darwin" and ("/tty." in address or address.endswith("Bluetooth-Incoming-Port")):
                continue
            ports[address] = port.description()
        if sys.platform == "darwin":
            for path in Path("/dev").glob("cu.*"):
                if not path.name.endswith("Bluetooth-Incoming-Port"):
                    ports.setdefault(str(path), "")
        self.port.clear()
        self.port.addItem("")
        for address, description in sorted(ports.items()):
            self.port.addItem(address)
            self.port.setItemData(self.port.count() - 1, description, Qt.ItemDataRole.ToolTipRole)
        self.port.setEditText(selected)  # Discovery is optional: preserve manually supplied ports.

    def _restore_settings(self, backend):
        self.refresh_ports()
        for key, widget in (("stream_name", self.stream_name), ("montage", self.montage), ("reference", self.reference)):
            widget.setText(str(self.settings.value(key, widget.text())))
        self.port.setEditText(str(self.settings.value("port", "")) or os.environ.get("DSISerialPort", ""))
        self.settle.setValue(float(self.settings.value("settle", 2)))
        self.verbosity.setValue(int(self.settings.value("verbosity", 2)))
        selected = backend or (find_backend() if getattr(sys, "frozen", False) else self.settings.value("backend", ""))
        if not selected or not Path(selected).is_file():
            selected = backend or find_backend() or ""
        self.backend_path.setText(str(selected))
        geometry = self.settings.value("geometry")
        if geometry:
            self.restoreGeometry(geometry)
        # Deliberately do not restore auto-on: startup impedance is off on every GUI launch.
        self._update_stream_names()

    def _save_settings(self):
        for key, value in (("port", self.port.currentText()), ("stream_name", self.stream_name.text()),
                           ("montage", self.montage.text()), ("reference", self.reference.text()),
                           ("backend", self.backend_path.text()), ("settle", self.settle.value()),
                           ("verbosity", self.verbosity.value()), ("geometry", self.saveGeometry())):
            self.settings.setValue(key, value)

    def _update_stream_names(self):
        name = self.stream_name.text().strip() or "WS-default"
        self.streams.setText(f"LSL outlets: {name}  ·  {name}-Impedance  ·  {name}-ImpedanceMarkers")

    def _browse_backend(self):
        filename, _ = QFileDialog.getOpenFileName(self, "Select updated dsi2lsl executable", self.backend_path.text())
        if filename:
            self.backend_path.setText(filename)

    def _program(self):
        path = Path(self.backend_path.text().strip()).expanduser().resolve()
        if not path.is_file() or (sys.platform != "win32" and not os.access(path, os.X_OK)):
            raise ValueError("Select an executable dsi2lsl backend, or build it first (see README.md).")
        return path

    def start_streaming(self):
        self.controller.send("start")

    def _auto_prepare(self):
        if not self.port.currentText().strip():
            self.console.appendPlainText("First run: choose a port, then Prepare outlets. No EEG is sent until Start streaming.")
            return
        self.prepare_outlets(automatic=True)

    def prepare_outlets(self, *, automatic=False):
        try:
            program = self._program()
            arguments = Options(self.port.currentText(), self.stream_name.text(), self.montage.text(),
                                self.reference.text(), self.startup_impedance.isChecked(), self.settle.value(),
                                self.verbosity.value()).arguments()
        except ValueError as error:
            if automatic:
                self.console.appendPlainText(f"Cannot prepare outlets: {error}")
            else:
                QMessageBox.warning(self, "Cannot prepare outlets", str(error))
            return
        self._save_settings()
        self.table.setRowCount(0)
        self.force.hide()
        self.console.appendPlainText("Connecting and advertising outlets; EEG will wait for Start streaming…")
        self.controller.start(program, self.launch_prefix + arguments, program.parent)

    def check_libraries(self):
        try:
            program = self._program()
        except ValueError as error:
            QMessageBox.warning(self, "Cannot check libraries", str(error))
            return
        self.controller.start(program, self.launch_prefix + ["--check-libraries"], program.parent, probing=True)

    def _configure_table(self, record):
        self.table.setRowCount(len(record["labels"]))
        for row, label in enumerate(record["labels"]):
            self.table.setItem(row, 0, QTableWidgetItem(label))
            self.table.setItem(row, 1, QTableWidgetItem("—"))
            self.table.setItem(row, 2, QTableWidgetItem("Unavailable"))

    def _render(self):
        c = self.controller
        busy = c.busy
        self.configuration.setEnabled(not busy)
        self.advanced.setEnabled(not busy)
        self.prepare.setEnabled(not busy)
        self.start.setEnabled(c.stage == "ready" and not c.pending_command)
        self.stop.setEnabled(c.stage == "acquiring" and not c.pending_command)
        self.disconnect.setEnabled(busy and c.stage != "stopping")
        state = c.state or {}
        runtime = c.stage == "acquiring" and not c.pending_command and not state.get("command_pending")
        self.imp_on.setEnabled(runtime and state.get("driver_mode") != 1)
        self.imp_off.setEnabled(runtime)  # Always allow an explicit safety-off command.
        self.reset.setEnabled(runtime and state.get("reset_mode") == 0)
        stale = c.last_data is None or time.monotonic() - c.last_data > 3
        if c.stage == "acquiring":
            if c.last_data is None:
                label = "Acquiring — waiting for EEG samples"
            elif stale:
                label = "No fresh EEG telemetry (>3 s) — check connection/log"
            else:
                label = f"Streaming · {state.get('sample_count', 0):,} EEG samples received"
        else:
            label = {"idle": "Select port → Prepare outlets", "ready": "Outlets ready — start LabRecorder, then Start streaming",
                     "error": "Backend failed — see log", "checking": "Checking libraries (no headset connection)",
                     "connecting": "Connecting / initializing headset…", "stopping": "Stopping / disconnecting…"}[c.stage]
        self.acquisition_status.setText(label)
        if busy and c.config:
            names = (("EEG", "eeg_subscribers"), ("Impedance", "impedance_subscribers"), ("Markers", "marker_subscribers"))
            self.subscribers.setText("LSL subscribers: " + " · ".join(
                f"{name}: {'connected' if state.get(key) else 'waiting'}" for name, key in names) +
                " (any LSL client, not necessarily LabRecorder)")
        else:
            self.subscribers.setText("Outlets not yet advertised")
        if c.pending_command:
            self.impedance_status.setText(f"Command pending: {c.pending_command}")
        elif busy and state:
            mode = state["driver_mode"]
            imp = "Off" if mode == 0 else ("On" if mode == 1 else f"Unknown (mode {mode})")
            if mode == 1 and not state["impedance_ready"]:
                imp += " — settling / unavailable"
            if stale:
                imp += " (last reported)"
            self.impedance_status.setText(f"Impedance: {imp}")
        else:
            self.impedance_status.setText("Impedance: unknown / disconnected")
        if busy and state:
            reset = state["reset_mode"]
            self.reset_status.setText("Reset: " + ("inactive" if reset == 0 else f"active/unknown (mode {reset})") +
                                      (" (last reported)" if stale else ""))
        else:
            self.reset_status.setText("Reset: unknown")
        if busy and c.config:
            config = c.config
            self.device_status.setText(f"{config.get('model', 'DSI')} · serial {config.get('serial', '?')} · "
                                       f"{config.get('eeg_channels', '?')} EEG/montage channels · {config.get('sampling_rate', '?')} Hz")
        elif not busy:
            self.device_status.setText("No headset connected")
        unavailable = (c.stage != "acquiring" or stale or bool(c.pending_command) or
                       not state.get("impedance_ready"))
        values = state.get("impedance", [])
        for row in range(self.table.rowCount()):
            value = values[row] if row < len(values) and not unavailable else None
            rating = "Stale" if busy and stale and c.last_data is not None else "Unavailable"
            color = self.palette().text().color()
            if value is not None:
                rating = "Good contact" if value < 1_000_000 else ("Fair contact" if value < 2_000_000 else "Poor contact")
                color = QColor("#278347" if value < 1_000_000 else "#a16b00" if value < 2_000_000 else "#c0392b")
            self.table.item(row, 1).setText("—" if value is None else f"{value / 1000:,.1f}")
            self.table.item(row, 2).setText(rating)
            self.table.item(row, 2).setForeground(color)

    def _confirm_reset(self):
        answer = QMessageBox.warning(self, "Reset analog amplifier?",
            "This can disturb EEG and invalidate an interval of data. The backend will emit reset markers.\n\n"
            "This is an analog amplifier reset, not impedance recalibration. Continue?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel, QMessageBox.StandardButton.Cancel)
        if answer == QMessageBox.StandardButton.Yes:
            self.controller.send("reset")

    def _slow_stop(self):
        self.force.show()
        self.console.appendPlainText("Shutdown is taking longer than 5 seconds. Waiting is safest; Force stop is available if necessary.")

    def _confirm_force(self):
        answer = QMessageBox.warning(self, "Force termination?",
            "This skips normal hardware shutdown and may leave impedance enabled or acquisition running. "
            "You may need to power-cycle the headset. Force stop?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel, QMessageBox.StandardButton.Cancel)
        if answer == QMessageBox.StandardButton.Yes:
            self.controller.force_stop()

    def _completed(self, code, crashed):
        self.force.hide()
        if self.controller.probing:
            text = "Library check passed." if code == 0 and not crashed else "Library check failed — see log."
        else:
            text = "Backend disconnected normally." if code == 0 and not crashed else f"Backend exited with error (code {code}); check the log and headset."
        self.console.appendPlainText(text)
        if self.close_after_stop:
            self.close()

    def _save_log(self):
        filename, _ = QFileDialog.getSaveFileName(self, "Save displayed log", "dsi2lsl-log.txt", "Text files (*.txt)")
        if filename:
            try:
                Path(filename).write_text(self.console.toPlainText(), encoding="utf-8")
            except OSError as error:
                QMessageBox.warning(self, "Cannot save log", str(error))

    def closeEvent(self, event: QCloseEvent):
        if self.controller.busy:
            event.ignore()
            if not self.close_after_stop:
                answer = QMessageBox.question(self, "Stop and close?",
                    "Stop the backend and disconnect the headset before closing?",
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel, QMessageBox.StandardButton.Cancel)
                if answer == QMessageBox.StandardButton.Yes:
                    self.close_after_stop = True
                    self.controller.stop()
        else:
            self._save_settings()
            event.accept()


def main():
    parser = argparse.ArgumentParser(description="Cross-platform DSI2LSL desktop GUI")
    parser.add_argument("--backend", help="Path to the matching dsi2lsl executable")
    parser.add_argument("--port", help="Headset port for automatic outlet preparation on launch")
    parser.add_argument("--smoke-test", action="store_true", help="Check Qt startup and backend libraries, then exit without connecting a headset")
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setApplicationName("DSI2LSL")
    app.setOrganizationName("DSI2LSL")
    temporary_settings = tempfile.TemporaryDirectory() if args.smoke_test else None
    selected_backend = args.backend or (find_backend() if args.smoke_test else None)
    if args.smoke_test and (not selected_backend or not Path(selected_backend).is_file()):
        temporary_settings.cleanup()
        return 1
    settings = (QSettings(str(Path(temporary_settings.name) / "settings.ini"), QSettings.Format.IniFormat)
                if temporary_settings else None)
    window = MainWindow(backend=selected_backend, settings=settings, port=args.port, auto_prepare=not args.smoke_test)
    window.show()
    if args.smoke_test:
        window.controller.completed.connect(lambda code, crashed: app.exit(0 if code == 0 and not crashed else 1))
        QTimer.singleShot(0, window.check_libraries)
    result = app.exec()
    if temporary_settings:
        temporary_settings.cleanup()
    return result


if __name__ == "__main__":
    raise SystemExit(main())
