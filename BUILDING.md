# Building Jam2

Jam2 uses host-native CMake builds. Docker is not part of the normal development workflow because the real audio paths must be tested against host ASIO and CoreAudio drivers.

## Windows

Required tools:

- Visual Studio Build Tools with the C++ workload.
- CMake.
- Ninja.
- Qt 6 with Widgets and Network modules when building `jam2-gui`.
- ASIO SDK available locally when the ASIO backend is implemented.

Local SDK path for this workspace:

```text
C:\Tools\ASIO-SDK_2.3.4\ASIOSDK
```

When the ASIO backend is added, CMake should default to this path if `ASIO_SDK_DIR` is not set.

Recommended build commands from a Developer PowerShell or Developer Command Prompt:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Real ASIO validation must happen on a Windows host with the target audio device and ASIO driver installed. MinGW may be useful for quick experiments, but MSVC is the expected Windows toolchain.

Local Windows audio test setup:

- ASIO4ALL is installed and can be used for initial ASIO enumeration and rough streaming tests.
- A vendor ASIO driver for the target audio interface is still preferred for serious latency and stability testing.

## macOS

Required tools:

- Xcode Command Line Tools.
- CMake.
- Ninja.
- Qt 6 with Widgets and Network modules when building `jam2-gui`.

Build commands:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Real CoreAudio validation must happen on a macOS host with the target audio device attached.

## Notes

- CMake is the single source of truth for builds on Windows and macOS.
- The repository is split into `libs/jam2-core`, `apps/jam2-cli`, `apps/jam2-gui`, `apps/jam2-capture`, and `tests` so additional app targets can share the same core library.
- Distributable app executables are written to the repo-root `release` directory. Tests and intermediate build products stay in the selected CMake build directory.
- Shared protocol, STUN, stats, and timing code should remain platform-neutral.
- ASIO and CoreAudio code should stay isolated behind platform-specific source files.
- The real-time audio callback path must not allocate, log, lock, throw exceptions, or block.
