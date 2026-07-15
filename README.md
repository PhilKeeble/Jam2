![Jam2](logo.png)

# Jam2

Jam2 is a small direct peer-to-peer music jamming tool for two people. It pairs low-latency mono UDP audio with a shared metronome, hard technical stats, and a Qt GUI for starting or joining a jam without adding accounts, rooms, relays, or hosted audio paths.

## Quick Start

1. Download the latest Jam2 release from the GitHub Releases page for this repository.
2. Extract the release zip.
3. Run `jam2.exe` on Windows (or `Jam2.app` on macOS). Running `jam2` with no arguments opens the GUI.
4. On the creator's machine, open **Start Jam**, choose the local audio device and tuning profile, then create the jam.
5. Send the generated `jam2://...` connection string to the other player.
6. On the joining machine, open **Join Jam**, paste the connection string, choose the local audio device and the channels you want to use for input and output, then connect.

For LAN testing, use **No STUN** and a reachable LAN host/IP. For internet testing, leave STUN enabled unless you already know the public endpoint or port-forwarded address to use.

## Documentation

| Document | Contents |
| --- | --- |
| [Building](docs/Building.md) | Host-native CMake build instructions for Windows and macOS. |
| [GUI](docs/Gui.md) | How to use the unified `jam2` application to start, join, tune, share songs, and share tracks. |
| [Engine](docs/Engine.md) | `jam2` CLI commands, runtime commands, audio/network tuning flags, and stats logging. |
| [Mesh](docs/Mesh.md) | Experimental direct full-mesh audio usage, GUI launch flow, peer-list behavior, and scaling considerations. |
| [Metronome](docs/Metronome.md) | Metronome modes, timing behavior, levels, and when to compare modes. |
| [Profiles](docs/Profiles.md) | Current tested tuning profiles and what each numeric field does. |
| [Diagnosing](docs/Diagnosing.md) | How to read Jam2 stats and troubleshoot network/audio issues. |
| [Connection Test](docs/ConnectionTest.md) | Pre-flight UDP connectivity checks before running Jam2. |
| [Validation](docs/Validation.md) | Deterministic framework and native post-build checks. |
| [Stress Tests](docs/StressTests.md) | Targeted impairment, recovery, timing, and full-mesh regression runs. |
| [Benchmark](docs/Benchmark.md) | Two-machine coordinator/agent measurements, artifacts, and analysis. |
| [Licensing](docs/Licensing.md) | Third-party notices and current licensing notes. |
