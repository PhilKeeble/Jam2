![Jam2gether](assets/banner.png)

# Jam2

Jam2 is a small direct peer-to-peer music jamming tool for two people. It pairs low-latency mono UDP audio with a shared metronome, hard technical stats, and a Qt GUI for starting or joining a jam without adding accounts, rooms, relays, or hosted audio paths.

This `UDP-Multiplex` branch uses one current typed UDP transport for Direct, STUN and Jam2 Server. All participants need a matching current branch build; older TCP/multiplex builds do not interoperate. See [the protocol review](REVIEW-WIRE-FORMAT.md#unified-multiplex-transport-and-mandatory-peer-names).

## Quick Start

1. Build the current branch in Release using [Building](docs/Building.md), and use the same build on each machine.
2. Use the resulting `release` folder on Windows or the normal app bundle on macOS.
3. Run `jam2.exe` on Windows (or `Jam2.app` on macOS). Running `jam2` with no arguments opens the GUI.
4. On the creator's machine, open **Start Jam**, enter your name, choose the local audio device and tuning profile, then create the jam.
5. Send the generated `jam2://...` connection string to the other player.
6. On the joining machine, open **Join Jam**, paste the connection string, enter your name, choose the local audio device and the channels you want to use for input and output, then connect.

Names require 1 to 8 Unicode letters; digits, spaces and symbols are rejected. Duplicate names are allowed, with distinct internal peer IDs. For LAN testing, use Direct and a reachable LAN host/IP. For internet Direct/STUN use, forward the creator UDP port; the creator introduces the other peers for hole punching. Jam2 Server uses external rendezvous for discovery. Audio remains direct peer to peer in every strategy.

JamTaster's model weights are a separate optional download. Open JamTaster and
choose **Download models** if they are missing; it shows the size before approval
and installs them under `components/jamtaster/models/` inside the Jam2 folder.
You can cancel or close the dialog and continue jamming. For manual installation,
extract the pinned release's `Jam2-models.zip` into `components/jamtaster/`.
Normal jamming does not require these models. See
[RELEASE.md](RELEASE.md) for release build scripts and package formats.

## Documentation

| Document | Contents |
| --- | --- |
| [Building](docs/Building.md) | Host-native CMake build instructions for Windows and macOS. |
| [GUI](docs/Gui.md) | How to use the unified `jam2` application to start, join, tune, share songs, and share tracks. |
| [JamTaster](app/jamtaster/README.md) | Bundled native WAV analysis, song-local results, worker protocol, and model notices. |
| [Engine](docs/Engine.md) | `jam2` CLI commands, runtime commands, audio/network tuning flags, and stats logging. |
| [Mesh](docs/Mesh.md) | Experimental direct full-mesh audio usage, GUI launch flow, peer-list behavior, and scaling considerations. |
| [Metronome](docs/Metronome.md) | Metronome modes, timing behavior, levels, and when to compare modes. |
| [Profiles](docs/Profiles.md) | Current tested tuning profiles and what each numeric field does. |
| [Diagnosing](docs/Diagnosing.md) | How to read Jam2 stats and troubleshoot network/audio issues. |
| [Connection Test](docs/ConnectionTest.md) | Pre-flight UDP connectivity checks before running Jam2. |
| [Validation](docs/Validation.md) | Native C++/CTest suites and the full distribution gate. |
| [Stress Tests](docs/StressTests.md) | Native impairment, recovery, timing, and four-peer regression runs. |
| [Benchmark](docs/Benchmark.md) | Two-machine coordinator/agent measurements, artifacts, and analysis. |
| [Licensing](docs/Licensing.md) | Third-party notices and current licensing notes. |
