# Building Jam2

Jam2 uses host-native CMake builds. Docker is not part of the normal development workflow because the audio paths must be validated against the host ASIO or CoreAudio driver stack.

## Windows

Required tools:

- Visual Studio Build Tools with the C++ workload.
- CMake.
- Ninja.
- Qt 6 with Widgets, Network, and Multimedia modules for `jam2-gui`.
- ASIO SDK for Windows ASIO builds.

Install the common build dependencies from an Administrator PowerShell with:

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100"
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e
winget install --id Python.Python.3.12 -e
```

Install Qt with `aqtinstall`:

```powershell
py -m pip install --user aqtinstall
py -m aqt install-qt windows desktop 6.7.3 win64_msvc2019_64 -O C:\Qt
```

The ASIO SDK is not normally available through `winget`. Download it from Steinberg, extract it locally, then set `ASIO_SDK_DIR` if it is not in the default path.

Local SDK path used in this workspace:

```text
C:\Tools\ASIO-SDK_2.3.4\ASIOSDK
```

Recommended build commands from a Developer PowerShell or Developer Command Prompt:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

If CMake cannot find Qt automatically, pass its install prefix:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64
```

If the ASIO SDK is in a different folder, pass it explicitly:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64 -DASIO_SDK_DIR=C:/Tools/ASIO-SDK_2.3.4/ASIOSDK
```

Real ASIO validation must happen on a Windows host with the target audio device and ASIO driver installed. ASIO4ALL can be useful for early enumeration and rough tests, but a vendor ASIO driver is preferred for latency and stability validation.

## macOS

Required tools:

- Xcode Command Line Tools.
- CMake.
- Ninja.
- Qt 6 with Widgets, Network, and Multimedia modules for `jam2-gui`.

Install the common build dependencies with:

```bash
xcode-select --install
brew update
brew install cmake ninja qt
```

Recommended build commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

If CMake cannot find Homebrew Qt automatically, pass the Qt prefix:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
```

Real CoreAudio validation must happen on a macOS host with the target audio device attached.

## Build Notes

- CMake is the single source of truth for Windows and macOS builds.
- Jam2 currently expects Release builds. Configure fresh build directories with `-DCMAKE_BUILD_TYPE=Release`.
- `jam2-gui` and `jam2-capture` are enabled by default for new CMake build directories.
- Built app binaries are staged to the repo-root `release` directory.
- Tests and intermediate build products stay in the selected CMake build directory.
- Shared protocol, STUN, stats, and timing code should remain platform-neutral.
- ASIO and CoreAudio code should stay isolated behind platform-specific source files.
