# DSI2LSL desktop GUI (macOS and Windows)

One native Qt interface built with **PySide6 / Qt for Python**. Retains the upstream GUI's port, stream name, montage, reference, Start/Stop and log, and adds:

* Editable port discovery list (manual ports and `DSISerialPort` still work).
* Startup impedance checkbox, live Impedance on/off buttons, and an analog Reset button with an EEG-disturbance warning.
* Physical-electrode + CMF impedance table, displayed in **kΩ**. LSL continues publishing **ohms**. Vendor contact thresholds: below 1 MΩ good, 1–2 MΩ fair, 2 MΩ or more poor; these are not EEG-quality guarantees.
* API-observed driver/reset state, settling/unavailable status, EEG sample count, and a stale-data warning. Values are hidden after three seconds without new sample telemetry; old values are never presented as live.
* Check libraries without connecting a headset, backend executable selection, verbosity, saved setup fields, and log export (last 2,000 displayed lines).
* Asynchronous graceful shutdown, including closing the window. A stalled shutdown exposes an explicitly confirmed Force stop option; it never silently kills the acquisition process.

**The C backend alone talks to the headset.** The GUI launches it with `QProcess`, sends the existing stdin commands, and receives optional versioned `DSI_GUI` JSON messages on stdout. Diagnostics remain on stderr. It does not create extra LSL streams or access the serial port. It does not record EEG; use LabRecorder and select all three streams.

This is a new front end inspired by the upstream Qt GUI's fields and subprocess architecture, not a patch to its old `.ui` files. Python is used for the UI only; the hardware/LSL bridge remains C. Do not select an old upstream backend: rebuild this folder's matching version so controls and telemetry are available.

## Run from source

Build/rebuild the backend as described in the root `README.md`, then install GUI requirements in a virtual environment.

### macOS

```sh
python3 -m venv .venv-gui
.venv-gui/bin/python -m pip install -r GUI/requirements.txt
.venv-gui/bin/python -m GUI
```

### Windows (PowerShell)

```powershell
py -m venv .venv-gui
.\.venv-gui\Scripts\python.exe -m pip install -r GUI\requirements.txt
.\.venv-gui\Scripts\python.exe -m GUI
```

Run these commands from the project root. The GUI automatically searches `build/Release/`, `build/`, and the application bundle for the backend. Override with `--backend=/path/to/dsi2lsl` or the Browse button. The backend libraries must stay beside its executable.

On the development Mac, the existing `python3` environment already contains PySide6, so **`python3 -m GUI`** is sufficient after rebuilding the C backend.

1. Close DSI-Streamer and other programs using the headset.
2. Select or type the port. Blank uses `DSISerialPort` if configured.
3. Set a non-identifying LSL stream name. Leave montage/reference blank for vendor defaults.
4. Optionally check libraries, then click **Prepare outlets**. A saved port or `--port COM4` on the command line makes this happen automatically on launch. Preparation connects the headset to obtain accurate channel metadata but does not start acquisition or push any LSL samples.
5. Once **Outlets ready** appears, refresh LabRecorder, select all three outlets and start recording, then click **Start streaming**. The GUI shows whether each outlet has an LSL subscriber (not necessarily LabRecorder).
6. Use Impedance on/off during acquisition. Startup impedance is deliberately **off on every GUI launch**, even when other settings are restored.
7. **Stop** pauses acquisition and disables impedance while retaining the same outlets/subscriptions. **Disconnect** or closing the GUI releases the headset and removes the outlets. Allow serial commands to finish; the supplied SDK may take time when the link is lost. Disconnect before changing port, montage, reference or stream identity.

See **[the exact Windows PowerShell commands](../WINDOWS_BUILD.md)** for building and packaging on Windows using a fresh `build-windows` folder.

The table initially shows `—`. It stays unavailable while impedance is off, a reset is active, or the estimate is settling. Reset means the DSI-Streamer-style **analog amplifier reset**, not impedance recalibration. Its request/return and any observed transitions remain recorded in the LSL marker stream.

## Create a double-clickable application

An **Apple Silicon build is already available at `dist/DSI2LSL.app`** in this workspace. Double-click it, or run `open dist/DSI2LSL.app`. It includes Qt, Python, the C backend, and its DSI/LSL libraries; no separate Python installation is needed to run that bundle. This build uses Qt 6.12 and requires **macOS 14 or newer**. It is ad-hoc signed for local use, not Developer ID signed/notarized for general distribution.

Use **PyInstaller on each target OS separately**. A Mac cannot directly produce the Windows executable. Build a backend matching the Python/Qt architecture first, then:

```sh
# Use your GUI virtual-environment Python; Windows uses .venv-gui\Scripts\python.exe
python -m pip install pyinstaller
python tools/package_gui.py --build-dir=build
```

Outputs:

* macOS: `dist/DSI2LSL.app` — the app bundle includes the backend and its libraries.
* Windows: `dist/DSI2LSL/DSI2LSL.exe` — distribute the **whole folder**, including `_internal`, not just the EXE.

Re-running the packaging helper replaces its generated `dist/DSI2LSL`/app output. It is intended for native macOS ARM64/Intel and Windows x64/ARM64 builds with matching SDKs; those combinations require separate testing. Windows may need the Visual C++ runtime. macOS distribution to other machines may require Developer ID signing/notarization; Windows may show SmartScreen warnings for unsigned apps. Third-party/vendor license and redistribution requirements still apply; packaging is not permission to redistribute the SDK.

## Tests

```sh
python -m unittest discover -s tests -p test_gui.py -v
```

Tests run Qt offscreen and a clearly labeled **simulated** subprocess, not a headset. They cover Start/Stop, repeat sessions, on/off/reset commands, real C backend telemetry parsing when built, stale readings, failed start/connection, force termination, library checks and fragmented Unicode output. The C tests separately exercise real LSL outlets/inlets. Physical-headset and Windows behavior are not covered by these tests.

For a safe source/app-bundle startup check, run with `--smoke-test`. It initializes the Qt window, loads the bundled backend's libraries, and exits without connecting to a headset. It uses temporary settings, leaving saved GUI preferences untouched. The macOS app bundle passed this check and ad-hoc signature verification.

## Backend GUI protocol

`--gui-telemetry` enables UTF-8 stdout lines beginning with `DSI_GUI `, followed by JSON with `protocol: 1` and:

* `type: "config"`: stream name, device model/serial, sampling rate, montage channel count, and impedance electrode labels in array order.
* `type: "state"`: observed driver/reset modes, requested driver state, command-pending/readiness flags, sample count, LSL host timestamp, impedance estimate timestamp, and impedance values in **ohms** (`null` for unavailable estimates).
* `type: "event"`: the same command/lifecycle/state event text used by the LSL marker outlet and log.

Telemetry is not another acquisition channel. Its normal periodic updates are approximately 1 Hz while EEG samples arrive; state transitions can also trigger messages. GUI state is not assumed from a click or from successful process launch. A returned serial command is not a sample-exact hardware transition. All timing and signal-contamination limitations in the root README remain applicable.
