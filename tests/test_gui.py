"""Headless Qt tests using a real subprocess but no real headset."""
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication, QMessageBox
from GUI.controller import BackendController
from GUI.main import MainWindow
from GUI.protocol import Options, decode_record


def wait_until(predicate, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        QApplication.processEvents()
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("Timed out waiting for Qt/subprocess state")


class ProtocolTests(unittest.TestCase):
    def test_actual_c_backend_telemetry_with_mock_headset(self):
        root = Path(__file__).resolve().parent.parent
        candidates = [root / "build" / "test_impedance", root / "build-windows" / "Release" / "test_impedance.exe",
                      root / "build" / "Release" / "test_impedance.exe"]
        backend = next((p for p in candidates if p.is_file()), None)
        if backend is None:
            self.skipTest("Build the C tests first to validate real backend JSON.")
        result = subprocess.run([str(backend)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        records = [decode_record(line) for line in result.stdout.splitlines() if line.startswith("DSI_GUI ")]
        self.assertEqual(records[0]["type"], "config")
        self.assertTrue(any(r.get("impedance") == [123000, 456000] for r in records))
        self.assertTrue(any(r.get("event") == "impedance_command_failed" for r in records))

    def test_arguments_preserve_spaces_without_shell(self):
        args = Options(port="/dev/cu.My DSI", stream_name="My device", montage="F3,F4").arguments()
        self.assertIn("--port=/dev/cu.My DSI", args)
        self.assertIn("--lsl-stream-name=My device", args)
        self.assertIn("--impedance=off", args)
        self.assertIn("--gui-telemetry", args)
        self.assertNotIn("--reference=", args)

    def test_invalid_configuration(self):
        for options in (Options(stream_name=""), Options(port="COM4\nquit"), Options(settle_seconds=float("nan")),
                        Options(stream_name="é" * 100)):
            with self.assertRaises(ValueError):
                options.arguments()

    def test_malformed_and_nonfinite_telemetry(self):
        self.assertIsNone(decode_record("diagnostic log"))
        for line in ('DSI_GUI {', 'DSI_GUI {"protocol":2}',
                     'DSI_GUI {"protocol":1,"type":"event","event":NaN}'):
            with self.assertRaises(ValueError):
                decode_record(line)


class QtTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setQuitOnLastWindowClosed(False)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        settings = QSettings(str(Path(self.temp.name) / "settings.ini"), QSettings.Format.IniFormat)
        fake = str(Path(__file__).with_name("fake_gui_backend.py").resolve())
        self.window = MainWindow(backend=sys.executable, settings=settings, launch_prefix=[fake])
        self.events = []
        self.window.controller.event_received.connect(self.events.append)

    def tearDown(self):
        if self.window.controller.busy:
            self.window.controller.force_stop()
            wait_until(lambda: not self.window.controller.busy)
        self.window.watchdog.stop()
        self.window.close()
        self.window.deleteLater()
        QApplication.processEvents()
        self.temp.cleanup()

    def start_fake(self):
        if not self.window.controller.busy:
            self.window.prepare_outlets()
        wait_until(lambda: self.window.controller.stage == "ready")
        self.window.start_streaming()
        wait_until(lambda: self.window.controller.last_data is not None)

    def test_streaming_on_off_reset_and_graceful_stop(self):
        self.assertFalse(self.window.startup_impedance.isChecked())
        self.start_fake()
        c = self.window.controller
        self.assertEqual(self.window.table.rowCount(), 2)
        self.assertEqual(self.window.table.item(0, 1).text(), "—")
        self.assertFalse(self.window.configuration.isEnabled())
        self.window.imp_on.click()
        wait_until(lambda: c.state["driver_mode"] == 1 and c.pending_command is None)
        self.assertEqual(self.window.table.item(0, 1).text(), "123.0")
        self.assertEqual(self.window.table.item(0, 2).text(), "Good contact")
        count = c.state["sample_count"]
        self.assertTrue(c.send("reset"))  # Button confirmation is handled separately by the UI.
        wait_until(lambda: "analog_reset_command_returned" in self.events)
        wait_until(lambda: c.state["sample_count"] > count)
        self.window.imp_off.click()
        wait_until(lambda: c.state["driver_mode"] == 0 and c.pending_command is None)
        self.assertEqual(self.window.table.item(0, 1).text(), "—")
        self.window.stop.click()
        wait_until(lambda: c.stage == "ready")
        self.assertIn("acquisition_stopped", self.events)
        self.assertTrue(c.busy)
        self.assertTrue(self.window.start.isEnabled())
        self.window.disconnect.click()
        wait_until(lambda: not c.busy)

    def test_stale_data_is_not_displayed_as_live(self):
        self.window.startup_impedance.setChecked(True)
        self.start_fake()
        self.window.controller.last_data = time.monotonic() - 4
        self.window._render()
        self.assertEqual(self.window.table.item(0, 1).text(), "—")
        self.assertEqual(self.window.table.item(0, 2).text(), "Stale")
        self.assertIn("No fresh", self.window.acquisition_status.text())
        self.assertTrue(self.window.imp_off.isEnabled())

    def test_repeated_start_stop_does_not_duplicate_handlers(self):
        for _ in range(2):
            self.start_fake()
            self.window.controller.stop()
            wait_until(lambda: not self.window.controller.busy)
        self.assertEqual(self.events.count("acquisition_started"), 2)
        self.assertEqual(self.events.count("acquisition_stopped"), 2)

    def test_close_requires_confirmation_and_waits_for_shutdown(self):
        self.window.show()
        self.start_fake()
        with patch("GUI.main.QMessageBox.question", return_value=QMessageBox.StandardButton.Cancel):
            self.window.close()
        self.assertTrue(self.window.isVisible())
        self.assertTrue(self.window.controller.busy)
        with patch("GUI.main.QMessageBox.question", return_value=QMessageBox.StandardButton.Yes):
            self.window.close()
        wait_until(lambda: not self.window.controller.busy)
        self.assertFalse(self.window.isVisible())
        self.assertIn("acquisition_stopped", self.events)

    def test_reset_requires_confirmation(self):
        self.start_fake()
        with patch("GUI.main.QMessageBox.warning", return_value=QMessageBox.StandardButton.Cancel):
            self.window.reset.click()
        self.assertNotIn("analog_reset_requested", self.events)
        with patch("GUI.main.QMessageBox.warning", return_value=QMessageBox.StandardButton.Yes):
            self.window.reset.click()
        wait_until(lambda: "analog_reset_command_returned" in self.events)

    def test_failed_start_and_connection_error(self):
        c = self.window.controller
        c.start(str(Path(self.temp.name) / "missing-program"), [], self.temp.name)
        wait_until(lambda: not c.busy)
        self.assertEqual(c.stage, "error")
        self.assertTrue(self.window.prepare.isEnabled())
        self.window.port.setEditText("SIM_FAIL")
        self.window.prepare_outlets()
        wait_until(lambda: not c.busy)
        self.assertEqual(c.stage, "error")
        self.assertIn("Simulated connection failure", self.window.console.toPlainText())

    def test_library_check_and_force_stop(self):
        self.window.check_libraries()
        wait_until(lambda: not self.window.controller.busy)
        self.assertIn("Library check passed", self.window.console.toPlainText())
        self.window.port.setEditText("SIM_HANG")
        self.start_fake()
        self.window.controller.stop()
        self.assertEqual(self.window.controller.stage, "stopping")
        self.window.controller.force_stop()
        wait_until(lambda: not self.window.controller.busy)
        self.assertIn("FORCED STOP", self.window.console.toPlainText())

    def test_prepared_outlets_wait_for_start_and_survive_stop(self):
        self.window.prepare_outlets()
        c = self.window.controller
        wait_until(lambda: c.stage == "ready" and c.state is not None)
        pid = c.process.processId()
        self.assertEqual(c.state["sample_count"], 0)
        self.assertNotIn("acquisition_started", self.events)
        self.assertTrue(self.window.start.isEnabled())
        self.start_fake()
        c.pause()
        wait_until(lambda: c.stage == "ready")
        self.assertEqual(c.process.processId(), pid)
        count = c.state["sample_count"]
        self.start_fake()
        wait_until(lambda: c.state["sample_count"] > count)
        self.assertEqual(c.process.processId(), pid)

    def test_launch_prepares_saved_port_without_acquiring(self):
        self.window.port.setEditText("SIM_PORT")
        self.window._auto_prepare()
        wait_until(lambda: self.window.controller.stage == "ready")
        self.assertIsNone(self.window.controller.last_data)
        self.assertNotIn("acquisition_started", self.events)

    def test_fragmented_utf8_and_bad_protocol(self):
        c = BackendController()
        records, logs = [], []
        c.event_received.connect(records.append)
        c.log.connect(logs.append)
        line = ("DSI_GUI " + json.dumps({"protocol": 1, "type": "event", "event": "électrode"}, ensure_ascii=False) + "\n").encode()
        split = line.index("é".encode()) + 1
        c._consume("stdout", line[:split])
        self.assertEqual(records, [])
        c._consume("stdout", line[split:])
        self.assertEqual(records, ["électrode"])
        c._consume("stdout", b'DSI_GUI {"protocol":9}\n')
        self.assertIn("Telemetry ignored", logs[-1])


if __name__ == "__main__":
    unittest.main()
