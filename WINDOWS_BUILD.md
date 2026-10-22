# Windows x64: exact PowerShell build commands

These instructions build **this modified project**, not an unmodified checkout of App-WearableSensing. Windows 10/11 x64, Python 3.11 x64 and Visual Studio 2022 Build Tools are assumed. Internet access is required for Python packages and liblsl's first build. Windows ARM64 needs a separate matching toolchain/SDK/Python configuration.

## 1. Copy the source to Windows

Copy the project to, for example, `C:\projects\dsi2lslwimp`. Include:

```text
CMakeLists.txt
dsi2lsl.c
README.md
WINDOWS_BUILD.md
GUI\
tests\
tools\
```

Follow the [README Installation instructions](README.md#installation) to download the DSI API, rename its extracted folder to `DSI_API`, and place it in the project root. Do **not** copy another machine's `build*`, `dist`, or `.venv-*` folders. This uses a fresh `build-windows` folder so another machine's CMake cache cannot interfere.

## 2. Install prerequisites once

Run in PowerShell if these prerequisites are not already installed. Accept Windows elevation prompts for Build Tools and wait for its installer to finish.

```powershell
winget install --exact --id Python.Python.3.11 --accept-package-agreements --accept-source-agreements
winget install --exact --id Microsoft.VisualStudio.2022.BuildTools --accept-package-agreements --accept-source-agreements --override '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
```

Then **close PowerShell and open a new PowerShell window** so the Python launcher is available. A normal PowerShell window is sufficient; the Visual Studio CMake generator finds its toolchain automatically. CMake itself is installed in the local Python environment below, avoiding system PATH changes or execution-policy changes.

## 3. Build and test

Change the first line to your actual project directory. Run the lines in order; **stop if a command reports an error**.

```powershell
Set-Location 'C:\projects\dsi2lslwimp'

py -3.11 -m venv .venv-gui
$Python = (Resolve-Path '.\.venv-gui\Scripts\python.exe').Path
& $Python -m pip install --upgrade pip
& $Python -m pip install -r '.\GUI\requirements.txt' pyinstaller cmake

$CMake = (Resolve-Path '.\.venv-gui\Scripts\cmake.exe').Path
$CTest = (Resolve-Path '.\.venv-gui\Scripts\ctest.exe').Path
& $CMake -S . -B build-windows -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON
& $CMake --build build-windows --config Release --parallel
& $CTest --test-dir build-windows -C Release --output-on-failure
& '.\build-windows\Release\dsi2lsl.exe' --check-libraries
& $Python -m unittest discover -s tests -p test_gui.py -v
```

The C/GUI tests use simulated headset functions or a simulated subprocess; the library check does not connect to a headset. The application/DSI/liblsl libraries all need to be x64. The supplied SDK's runtime version may differ from its folder name; read the logged version.

## 4. Run directly from source (optional)

```powershell
& $Python -m GUI --backend '.\build-windows\Release\dsi2lsl.exe'
```

On the first run, select/type the headset COM port and click **Prepare outlets**. Once the GUI says **Outlets ready**, select all three streams in LabRecorder and start LabRecorder recording, then click **Start streaming**. No LSL sample is pushed before Start. On subsequent GUI launches, the saved port is used to prepare/advertise the outlets automatically, still without starting acquisition.

To specify the port on the very first launch and prepare automatically (replace COM4):

```powershell
& $Python -m GUI --backend '.\build-windows\Release\dsi2lsl.exe' --port COM4
```

**Stop** pauses EEG and disables impedance but keeps the same three outlets alive, preserving recorder subscriptions. **Disconnect** or closing the app shuts down the backend and removes its outlets. To change montage/stream identity, disconnect first, then edit and prepare again; refresh/reselect streams in LabRecorder after such a change.

## 5. Package a double-clickable GUI

Close the source GUI before packaging:

```powershell
& $Python '.\tools\package_gui.py' --build-dir build-windows
& '.\dist\DSI2LSL\DSI2LSL.exe' --smoke-test
Start-Process '.\dist\DSI2LSL\DSI2LSL.exe'
```

Distribute the **entire `dist\DSI2LSL` folder**, including `_internal`, not just the EXE. It includes Python, Qt, the backend, and DSI/LSL runtime libraries; Python is not required on the receiving machine. Vendor/third-party redistribution terms still apply. The helper replaces its generated package on rebuild.

The smoke test starts Qt, checks backend library loading and exits without connecting a headset or modifying saved preferences. The subsequent normal launch may connect to a saved headset port, but does not acquire/publish EEG until Start.

If a receiving computer reports a missing Visual C++ runtime, install the official runtime:

```powershell
winget install --exact --id Microsoft.VCRedist.2015+.x64 --accept-package-agreements --accept-source-agreements
```

Allow the application's LSL traffic on the appropriate trusted network if Windows Firewall prompts. Subscription indicators mean **any LSL client** has connected, not proof that LabRecorder is recording. Physically pair the headset/install its serial drivers as required, and close DSI-Streamer before using this app.
