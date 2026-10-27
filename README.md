# dsi2lsl with electrode impedance and markers

Cross-platform C backend and Qt desktop GUI extending [App-WearableSensing](https://github.com/labstreaminglayer/App-WearableSensing). Uses the provided DSI C API and **liblsl 1.17.7**. No DSI-Streamer process is required; close it before connecting because the headset serial port cannot be shared.

**Status:** built and tested on macOS Apple Silicon with simulated DSI function pointers and real LSL transport.

## Installation

### 1. What you need

* A DSI headset.
* Windows 10/11 x64 or macOS 14+ (Apple Silicon or Intel).
* Internet access for the first build.
* [LabRecorder](https://github.com/labstreaminglayer/App-LabRecorder/releases) to record the LSL streams.

### 2. Get this project

Copy or clone the project to a local folder, for example `C:\projects\dsi2lslwimp` on Windows. Do not copy another machine's `build*`, `dist`, or `.venv-*` folders.

### 3. Download and install the DSI API

Get the SDK from the [Wearable Sensing Downloads page](https://support.wearablesensing.com/help/downloads/index.html). If needed, request the unlock password from Wearable Sensing support. Unlock and download the DSI-API zip, extract it, rename the extracted folder to `DSI_API`, and move it into the project root beside `CMakeLists.txt`:

```text
project-root/
├── CMakeLists.txt
└── DSI_API/
    ├── DSI.h
    ├── DSI_API_Loader.c
    ├── libDSI-Windows-x64.dll
    ├── libDSI-Darwin-arm64.dylib
    ├── libDSI-Darwin-x86_64.dylib
    └── ...
```

The SDK is licensed, is not redistributed with this project, and is gitignored. Each user must download their own copy. To keep it elsewhere, configure with `-DDSI_API_DIR=<path>`.

### 4. Install drivers and pair the headset

For USB on Windows, install the CP210x VCP driver from the same Downloads page. For Bluetooth, pair the headset in the operating system's Bluetooth settings. On Windows, note its COM port in Device Manager under **Ports (COM & LPT)** or **Bluetooth**; on macOS, note its `/dev/cu.*` port. Close DSI-Streamer before connecting; it cannot share the headset serial port.

### 5. Build

On Windows, follow the exact PowerShell commands in **[WINDOWS_BUILD.md](WINDOWS_BUILD.md)**. On macOS, use the commands in [Build](#build). Both expect the SDK at `DSI_API/` unless you set `-DDSI_API_DIR=<path>`.

### 6. First run

Open the GUI, select the headset port, and click **Prepare outlets**. In LabRecorder, refresh the stream list, select all three streams, and start recording. Then click **Start streaming**. Click **Stop** to pause acquisition or **Disconnect** to release the headset and remove the outlets.

### 7. Troubleshooting

* Configure error about missing DSI SDK: check that the folder is named `DSI_API` and is in the project root.
* “Cannot load DSI library”: check that the SDK library exists and matches the build architecture.
* Port in use: close DSI-Streamer and other programs using the headset.
* Streams not visible in LabRecorder: check the firewall and ensure both apps are on the same network.

## Desktop GUI

The new **[Qt GUI](GUI/README.md)** works from the same source on macOS and Windows. It retains the original setup fields and adds live impedance on/off, analog reset, electrode/CMF readings, observed-state indicators, library checking and graceful shutdown.

After building the backend, install `GUI/requirements.txt` in your Python environment and run **`python -m GUI`** from this folder. On the development Mac, PySide6 is already available, so `python3 -m GUI` works. See `GUI/README.md` for virtual-environment instructions and creating a double-clickable `.app` or Windows executable folder.

**Outlets and acquisition are separate:** the GUI automatically prepares/advertises all three outlets on launch when a port is saved or supplied. First-run users choose a port and click **Prepare outlets**. Hardware connection is required to obtain accurate channel metadata; no fabricated placeholder stream is created before that. Start LabRecorder recording/subscriptions, then click **Start streaming**. No EEG, impedance or marker sample is pushed before the first Start. **Stop** pauses acquisition while keeping the same outlets alive; **Disconnect** removes them. Configuration is locked while outlets are prepared to preserve subscribed stream identity.

After the first Start, lifecycle/status markers can continue while paused, but EEG and impedance samples stop. Before the first Start, standby state is sent only to the GUI/log, not to any LSL outlet.

For exact Windows PowerShell install/build/test/package commands, see **[WINDOWS_BUILD.md](WINDOWS_BUILD.md)**.

## Streams

All three outlets are created at startup, regardless of whether impedance is enabled:

| Name (default) | Type / format | Contents |
| --- | --- | --- |
| `WS-default` | EEG / float32, headset sampling rate | Existing configured montage, including triggers/auxiliary channels if selected. No new filtering or impedance subtraction. |
| `WS-default-Impedance` | Impedance / double64, irregular | One value per physical referential EEG source, excluding the virtual factory-reference source, followed by `CMF=<factory reference>`. Electrode estimates are ohms; CMF is a unitless vendor metric. Polled approximately once per second while EEG samples arrive. |
| `WS-default-ImpedanceMarkers` | Markers / one string channel, irregular | Command requests, command returns/failures, API-observed mode changes, acquisition lifecycle, and state snapshots approximately once per second. |

The impedance layout deliberately does **not** mirror the EEG montage: `F3-F4` has no single physical-electrode impedance. Linked ears/mastoids are included when the API exposes them as referential sources. CMF is read through the vendor's separate `DSI_Headset_GetImpedanceCMF()` getter and is a unitless common-mode-fault metric, not an impedance in ohms. This matches the selection logic in `DSI_API/demo.c` and the vendor's C/Python impedance tutorials.

Impedance values are `NaN` when the driver is off/unknown, during an analog reset, during the configurable settling period, while a command is executing, or for negative/nonfinite API estimates. They are **not** filled with stale last-known impedances or misleading zeroes. NaN does not distinguish these causes; use the marker stream. Finite estimates are not a guarantee of good contact or of a newly recomputed hardware measurement.

The API returns physical-electrode estimates in ohms. The GUI displays these in kΩ and rates contact using the vendor tutorial thresholds: below 1 MΩ Good, 1–2 MΩ Fair, and 2 MΩ or more Poor. Wait approximately 2 seconds after enabling impedance or releasing reset before reading estimates. The separate CMF getter is reported as a unitless metric, not ohms; the vendor tutorials print it as `Common-mode fault: %.1f`. These are contact indicators, not EEG-quality guarantees. See the vendor [C](https://support.wearablesensing.com/api/getting_started/start_c.html#task-check-impedances) and [Python](https://support.wearablesensing.com/api/getting_started/start_python.html) tutorials.

XML metadata includes physical channel labels, units, hardware/firmware/API identity, settling time, timestamp method, and a shared `stream_group`. Stream source IDs are based on device serial number and stream name; use different stream names for simultaneous devices if serial identity is unavailable. Do not put participant-identifying information in stream names. EEG labels now use the API channel name and preserve the full montage expression in metadata without modifying API-owned strings.

## Build

Requirements: CMake **3.24+**, a C/C++ compiler, supplied DSI SDK, and internet access for the first build (liblsl and its dependency). The root `lsl_c.h` is kept untouched but **not used**: the build uses the modern header matching the linked library. The liblsl source archive is pinned by version and SHA-256.

### macOS (Apple Silicon or Intel)

Install Xcode Command Line Tools and CMake if needed, then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/dsi2lsl --check-libraries
./build/dsi2lsl --port=/dev/cu.YOUR_DSI_PORT --lsl-stream-name=DSI --impedance=off
```

Build natively on each architecture, or explicitly set `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64`. The vendor libraries are separate architecture builds, not a universal library. For this development session, CMake is available at `.venv-build/bin/cmake` and CTest at `.venv-build/bin/ctest`.

### Windows (Visual Studio 2022, x64)

Install CMake and Visual Studio's **Desktop development with C++** workload:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\dsi2lsl.exe --check-libraries
.\build\Release\dsi2lsl.exe --port=COM4 --lsl-stream-name=DSI --impedance=off
```

`-A ARM64` selects the corresponding SDK library; that configuration remains untested. Matching application, DSI and liblsl architectures are required. Windows may also need the Visual C++ runtime and headset USB/Bluetooth drivers.

The build copies the target DSI library and liblsl runtime beside the executable. On macOS/Linux, the copied liblsl uses its SONAME (for example `liblsl.2.dylib`). Keep these files together when moving the program; Windows Debug/Release folders are separate. Other liblsl source-build artifacts are not required at runtime. Redistribution of the vendor SDK is subject to its `LICENSE.md`; retain relevant third-party licenses when distributing binaries.

To use an installed modern liblsl instead of downloading/building it:

```sh
cmake -S . -B build -DDSI_FETCH_LIBLSL=OFF -DCMAKE_PREFIX_PATH=/path/to/liblsl
```

That installation must provide the `LSL` CMake package and a shared library. `DSI_API_DIR` selects another vendor SDK; `DSI_LIBRARY` overrides the target vendor binary during the build. `--dsi-library=/absolute/path/to/library` overrides the DSI runtime location.

### SDK runtime version

Check the runtime version logged by the app. The **1.21.3 package tested here reported 1.20.4** at runtime. SDK package metadata, header, and binary versions can differ; nothing in the SDK was changed by this implementation. Do not assume a package's timeout/link-loss fixes are present based only on its package version. Other platform binaries need their own runtime-version check.

## Controls

Default startup driver state is **off**, explicitly sent to the headset. `--impedance=on` enables continuous checks when acquisition starts, not during outlet preparation. The CLI also advertises outlets first and waits for `start`; `--auto-start` explicitly restores immediate acquisition if desired.

Type a command followed by **Enter** while streaming:

| Command | Action |
| --- | --- |
| `start` | Begin/resume acquisition on the already-advertised outlets. |
| `stop` | Disable impedance and pause acquisition; keep all outlets and subscriptions alive. |
| `on` or `i` | Send `DSI_Headset_StartImpedanceDriver()`. EEG acquisition continues. |
| `off` or `o` | Send `DSI_Headset_StopImpedanceDriver()`. EEG acquisition continues; impedance becomes NaN. |
| `reset` or `r` | Start the analog-reset sequence: call `DSI_Headset_StartAnalogReset()`, hold, then call `DSI_Headset_ReleaseAnalogReset()`. This resets the analog amplifier, **not** an impedance calibration routine. |
| `status` or `s` | Publish/print the cached API-observed state. |
| `quit` or `q`, Ctrl+C | Disable impedance, stop acquisition and release resources. |

`--impedance-settle=2` is the default estimate-suppression period after commands and observed state changes, including reset release. It is a conservative starting value from the vendor tutorial, not a validated universal recovery time. It does not mask/drop EEG samples or certify EEG as clean after two seconds. Repeated `on` restarts the suppression period. To cycle just the impedance driver, issue `off`, check `status`, then `on`; this is distinct from analog reset. A command failure stops the acquisition loop and triggers best-effort shutdown.

### Analog reset

`reset` starts the SDK analog-reset sequence; it is not an impedance calibration. The backend emits `analog_reset_requested`, calls `DSI_Headset_StartAnalogReset()` and emits `analog_reset_started` when the call returns successfully. It holds reset for `--reset-hold=SECONDS` (default 1.0), then calls `DSI_Headset_ReleaseAnalogReset()` and emits `analog_reset_released`. The API-observed `reset_mode` is 1 while reset is held and 0 when released. While `reset_mode` is nonzero, impedance is unavailable (`NaN`); the state and periodic `state ... reset_phase=` markers report `holding`, `releasing`, `failed`, or `idle`.

After release the backend polls `DSI_Headset_GetAnalogResetMode()` for up to 3 seconds. A return to mode 0 completes the reset and restarts the 2-second impedance settling period. If mode remains nonzero, the backend emits `analog_reset_release_failed`, marks the phase failed, and continues acquisition; reconnect to recover, or the backend returns to idle if the mode later becomes 0. On connect, the backend calls Release once (`analog_reset_cleared_on_connect`). If reset is unexpectedly held while idle, it emits `analog_reset_unexpectedly_held` once and makes one release attempt. `LockAnalogReset` is never called.

These semantics follow the supplied SDK's `DSI.h:204-206` and changelog: version 1.3.0 describes Lock/Release as the start/end of a contact-optimization session during which signals are invalid, and version 1.6.2 notes analog-reset locks are released during initialization. The [vendor C](https://support.wearablesensing.com/api/getting_started/start_c.html) and [Python](https://support.wearablesensing.com/api/getting_started/start_python.html) tutorials do not cover analog reset.

Commands and acquisition run on the same thread; serial commands are never issued from the sample callback or a competing hardware-control thread. Stdin is nonblocking: terminals and subprocess pipes work on both macOS and Windows. Closing stdin does not stop streaming. In the Windows console the app collects characters until Enter; redirected disk files are not supported as a control channel. The new Qt GUI in `GUI/` drives these commands through the subprocess's stdin and uses optional `--gui-telemetry` JSON stdout records to display state and impedance without accessing the headset itself.

Existing options remain: `--port`, `--montage`, `--reference`, `--verbosity`, `--lsl-stream-name`, `--help`. Options use `--NAME=VALUE` (or legacy `/NAME:VALUE`), not separate-value syntax. `-m` is reserved for montage, fixing the original conflict with the stream name. `--help` and invalid-option-value checks do not connect to the headset. `--check-libraries` loads libraries without connecting.

## What continuous impedance does to EEG

### Verified vendor behavior

* The supplied vendor demo explicitly states that impedance measurement **injects currents at 110 Hz and 130 Hz** (`demo.c`, lines 741–744).
* The vendor signal-quality guide states that enabling impedance **injects a test signal into the data**. This is not just additional passive telemetry.
* Impedances are associated with physical sources, not re-referenced montage channels. Their API estimates are derived while signal acquisition is running.
* The demo waits one second before reading; the public API quick reference/tutorial recommends roughly **two seconds**. The demo also says to plot readings and verify they reach a plateau before averaging.
* The SDK warns that **locking analog reset invalidates signal data**. This program does not lock it. The Start→hold→Release sequence can still create a disturbed interval; see [Analog reset](#analog-reset).

### Analysis implications (recommendations, not a vendor guarantee)

110/130 Hz components lie within the recorded bandwidth at the common 300 Hz sampling rate. Their amplitudes depend on electrode contact and the acquisition configuration. They can affect high-frequency EEG, spectra and broadband metrics; turning the driver on/off can create transients. Reference subtraction can redistribute contamination between montage channels. Poor contact or large artifacts can lead to nonlinear behavior/clipping that filtering cannot undo.

For low-frequency EEG/ERP work, a suitably designed low-pass filter with sufficient attenuation at both test frequencies may remove much of the periodic contamination. If higher frequencies matter, narrowly targeted notch filters or sinusoidal regression may help, but they also remove/alter genuine activity around the same frequencies. **Always anti-alias filter before downsampling**, or the test tones can fold into the band of interest. Avoid filtering through a reset/gap as though it were normal continuous EEG; inspect and annotate first, then use an appropriate segmented processing strategy with edge padding.

The impedance values alone lack the exact injected-current waveform phase and cannot be used to reconstruct and subtract a guaranteed clean EEG signal. Markers provide annotations, **not** automatic repair. Recommended default: checks before/between experimental blocks, driver off during sensitive recordings, and exclude/flag transition/reset intervals with empirically chosen padding. For long continuous monitoring, validate spectra and clipping with on/off recordings before relying on filtered data.

### Marker timing and interpretation

Examples:

```text
impedance_on_requested
impedance_on_command_returned
impedance_state mode=1
analog_reset_requested
analog_reset_started
analog_reset_state mode=1
analog_reset_released
analog_reset_state mode=0
state driver_mode=1 reset_mode=0 reset_phase=idle requested_on=1 command_pending=0 impedance_ready=1
```

`*_requested` is emitted **before** the serial API call; `analog_reset_started` and `analog_reset_released` record successful command returns, not sample-exact hardware edges. `*_state` comes from changes in the API getters. Normal mode conventions are 0=off and 1=on; raw other modes are recorded and treated conservatively as not ready. Very short reset states can occur entirely within a blocking command, so an observed on/off pair is not guaranteed. The request and command markers still record the operation. Confirm conventions on the target firmware. Reset release failure and recovery markers are described above.

### EEG timestamps

EEG timestamps use `nominal_rate_anchored_to_packet_time`: each sample is placed at `offset + base_packet_time + (k−base_k)/fs`, using the vendor packet timestamp and nominal sampling rate. The LSL offset is the minimum `receipt_lsl − packet_time` observed per packet over approximately 10-second windows, slewed by at most 0.05 ms per packet. A packet-time jump larger than 1.5 packet durations emits `sample_gap` and re-anchors. Non-monotonic or non-finite packet timestamps emit `timestamp_fallback` once and use receipt timestamps thereafter. Impedance and marker streams continue to use `host_callback_receipt_lsl_local_clock`; impedance samples use the receipt time of the triggering EEG callback. This method does not correct drift between nominal sampling rate and the device clock.

Markers use the LSL host clock. Bluetooth buffering, hardware anti-alias filter delay, synchronous serial commands, and the 10 ms acquisition-loop polling interval prevent sample-exact alignment between a command and affected EEG. Offline clock synchronization cannot remove those device/transport delays. Use request/return and state markers plus a measured safety margin when annotating; do not interpret `impedance_ready` as EEG artifact-free. Periodic snapshots let a recorder started late learn the current state, but do not recover earlier transitions.

### Legacy recordings

Load XDF recordings with `pyxdf.load_xdf(..., dejitter_timestamps=True)` to apply pyxdf's timestamp dejittering. Recordings made before these fixes have Trigger-typed EEG metadata (all EEG channels were typed as Trigger), impedance stuck at NaN after any reset, and CMF previously mislabeled as ohms. Interpret their channel types and CMF values accordingly; these metadata/value issues cannot be repaired merely by loading with dejittering.

In LabRecorder, record **all three streams** into the same XDF. Confirm the impedance and marker streams contain samples (not just names) before the experiment. A final shutdown marker is best-effort; network delivery after immediately destroying outlets is not guaranteed.

## Tests

Tests in `tests/test_impedance.c` use fake DSI getters/commands with **real liblsl stream discovery and EEG/impedance/marker sample transfer**. They cover source selection, ohm metadata, trigger preservation, off/settling/reset NaNs, command/observed-state markers, command rejection and option aliases. Separate tests check help and loading the supplied DSI library without connecting. A relocated macOS bundle also passed library-loading validation. These tests do not establish electrical behavior, firmware timing, or headset compatibility.

## Research sources

* [Upstream App-WearableSensing](https://github.com/labstreaminglayer/App-WearableSensing) — existing EEG bridge and exclusive serial-port access.
* Supplied `DSI_API/demo.c`, sections 2.3.1, 2.4 and `ImpedanceDemo`; supplied `DSI.h` analog-reset functions — physical source selection, callback behavior, 110/130 Hz injection and reset warning.
* [Vendor API quick reference](https://support.wearablesensing.com/api/quick_reference.html), [C tutorial, Check Impedances](https://support.wearablesensing.com/api/getting_started/start_c.html#task-check-impedances), and [Python tutorial](https://support.wearablesensing.com/api/getting_started/start_python.html) — controls, packet timestamps, impedance ohms/kilohms and thresholds, settling guidance, and unitless CMF. The tutorials do not document analog reset.
* [Vendor signal-quality guide](https://support.wearablesensing.com/faq/software/questions/signal-quality-dsi-streamer.html) — injected signal and dry-electrode impedance guidance.
* [Vendor LSL GUI documentation](https://support.wearablesensing.com/examples/lsl/gui.html) — Reset is an analog amplifier reset; recent vendor GUI versions already offer an impedance startup checkbox, but this does not establish a separate impedance-values/markers stream.
* [liblsl v1.17.7](https://github.com/sccn/liblsl/releases/tag/v1.17.7) — current latest stable release checked during this work (2026-10-14).
