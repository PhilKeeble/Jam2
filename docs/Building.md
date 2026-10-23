# Building Jam2

Jam2 uses host-native CMake builds. Docker is not part of the normal development workflow because the audio paths must be validated against the host ASIO or CoreAudio driver stack.

## Updating the Source Checkout

Jam2 tracks its model and ONNX Runtime binaries with Git LFS. Before building
an existing checkout, update the current branch and download its LFS objects
with the platform script:

```powershell
.\update.ps1
```

```bash
bash ./update.sh
```

The scripts install or configure Git LFS when needed, use a fast-forward-only
Git pull, download and verify the current LFS objects, and preserve existing
local changes. They stop with an error instead of automatically merging a
diverged branch or discarding local work.

## Release packages

For distribution, use `build-release.ps1 --version 0.8.1` on Windows or
`bash build-release.sh --version 0.8.1` on macOS. Add `--include-models` to also
create the separate model archive. Outputs go to `dist/`. Add `--publish` with
an explicit version to create/reuse the GitHub releases and upload the assets;
this requires GitHub CLI and a one-time `gh auth login` on each machine.
See [RELEASE.md](../RELEASE.md) for the ZIP layouts, metadata, universal Mac
dependencies and optional signing. The release version override does not edit
the source checkout. Ordinary compile commands retain the development version.

## Windows

Required tools:

- Visual Studio 2022 or Visual Studio 2022 Build Tools with the **Desktop development with C++** workload and a Windows SDK.
- CMake 3.24 or newer.
- Ninja.
- Qt 6 with an MSVC 64-bit desktop kit. Jam2 uses the Core, Gui, Widgets, and Network packages.
- The Steinberg ASIO SDK.

The simplest Windows build is:

```powershell
.\compile.cmd
```

The script opens an MSVC build console, checks every prerequisite before
configuring, finds standard Qt and ASIO SDK installations, and explains how to
fix anything that is missing. It searches for Qt under `C:\Qt` and for the ASIO
SDK under `C:\Tools` when Windows is installed on `C:`.

Install the common build dependencies from an Administrator PowerShell with:

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e
```

Install Qt with the [Qt Online Installer](https://doc.qt.io/qt-6/get-and-install-qt.html).
Choose a Qt 6 **MSVC 2022 64-bit** desktop kit, not a MinGW-only kit.

Download the [Steinberg ASIO SDK](https://www.steinberg.net/developers/asiosdk-open/)
and extract it. Placing the extracted package under `C:\Tools` lets
`compile.cmd` find a layout such as `C:\Tools\ASIO-SDK-version\ASIOSDK`
automatically. The ZIP file by itself is not sufficient.

For installations elsewhere, set these environment variables in the shell
that launches `compile.cmd`:

```powershell
$env:QT_DIR = 'D:\SDKs\Qt\6.x\msvc2022_64'
$env:ASIO_SDK_DIR = 'D:\SDKs\ASIO-SDK\ASIOSDK'
.\compile.cmd
```

`QT_DIR` must name the Qt kit directory containing
`lib\cmake\Qt6\Qt6Config.cmake`. `ASIO_SDK_DIR` must name the SDK directory
containing `common\asio.h`. An invalid explicit path is reported rather than
silently replaced with an automatically discovered installation.

Developers who need to invoke CMake directly can use the same paths:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$env:QT_DIR" -DASIO_SDK_DIR="$env:ASIO_SDK_DIR"
cmake --build build
```

Real ASIO validation must happen on a Windows host with the target audio device and ASIO driver installed. ASIO4ALL can be useful for early enumeration and rough tests, but a vendor ASIO driver is preferred for latency and stability validation.

## macOS

Required tools:

- Xcode Command Line Tools.
- Homebrew.
- CMake 3.24 or newer.
- Ninja.
- Qt 6 with Core, Gui, Widgets, and Network modules for the unified `jam2` application.

The simplest macOS build is:

```bash
bash ./compile.sh
```

The script verifies the Apple developer tools, installs Homebrew from its
official installer when necessary, and installs missing CMake, Ninja, and
Qt base packages with Homebrew before building. Apple requires its Command Line
Tools to be installed through the macOS installer; if they are missing, the
script opens that installer and asks you to rerun `compile.sh` when it finishes.

Homebrew Qt is detected automatically. To use another Qt installation, set
`QT_DIR` to its prefix:

```bash
QT_DIR="$HOME/Qt/6.x/macos" bash ./compile.sh
```

macOS release packages require Qt libraries containing both `arm64` and
`x86_64`. Homebrew's native Qt installation is suitable for development on
this Mac but cannot supply both architectures for a universal release. Install
the macOS desktop kit with the [Qt Online Installer](https://doc.qt.io/qt-6/macos-installation.html)
and select its prefix explicitly:

```bash
QT_DIR="$HOME/Qt/6.x/macos" bash ./build-release.sh --version 0.1.1 --publish
```

Replace `6.x` with the installed version. When switching an existing build
between Homebrew and another Qt installation, clear CMake's cached Qt selection
once with `cmake --fresh -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.x/macos"`
before running the release script.

Developers who need to invoke CMake directly can use the Homebrew Qt prefix:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build build
```

Real CoreAudio validation must happen on a macOS host with the target audio device attached.

## Build Notes

- CMake is the single source of truth for Windows and macOS builds.
- The required DaisySP source subset is vendored under
  `libs/third_party/DaisySP`; normal builds do not download dependencies.
- Jam2 builds and tests only optimized Release. Configure fresh build directories with `-DCMAKE_BUILD_TYPE=Release`.
- Add or update relevant native CTests with each feature or fix and run only cases directly relevant to the change. Run the full suite only on explicit user instruction; report its failures without automatic repairs. See [Validation.md](Validation.md).
- The unified GUI/CLI `jam2` target is enabled by default for new CMake build directories.
- Built app binaries are staged to the repo-root `release` directory.
- Intermediate build products stay in the selected CMake build directory.
- Shared protocol, STUN, stats, and timing code should remain platform-neutral.
- ASIO and CoreAudio code should stay isolated behind platform-specific source files.
