# Jam2 GUI

`jam2-gui` is the normal way to run Jam2. It controls a local `jam2` audio engine process, shows live stats, and opens a TCP control connection to the other GUI for shared settings, song grid edits, and track sharing. Audio still travels directly between the two `jam2` engine processes over UDP.

## Start A Jam

On the host machine:

1. Open `jam2-gui`.
2. In **Start Jam**, choose the bind host and UDP port. `0.0.0.0:49000` is the usual starting point.
3. Leave STUN enabled for internet testing, or enable **No STUN** for manual/LAN testing.
4. Choose the local audio device and input/output channels.
5. Choose a tuning profile. `fast` is the default, `moderate` is more forgiving, and `safe` is the current Wi-Fi profile.
6. Start the jam and send the generated `jam2://...` URL to the other player.

Use the same profile, sample rate, and frame size on both machines. Each player still chooses their own local device and channel numbers.

## Join A Jam

On the joining machine:

1. Open `jam2-gui`.
2. Paste the `jam2://...` URL into **Join Jam**.
3. Choose the local audio device and channels.
4. The sample rate, tuning profile, and detailed engine settings are received from the host before the local engine starts.
5. Connect.

If the connection fails, first run the connection test tool described in [Connection Test](ConnectionTest.md).

## Runtime Controls

The GUI exposes the useful live controls from the engine:

- Toggle the metronome on or off.
- Change BPM.
- Change metronome mode.
- Adjust local metronome level.
- Adjust local remote playback level or mute the remote peer.
- Enable stats and CSV logging while testing.

These controls affect the local engine through stdin runtime commands.

## Stats

The GUI displays hard measurements from the engine. The most useful fields while tuning are packet loss, jitter, RTT, playback depth, underruns, overruns, drift ppm, resampler ratio, receive loop stalls, and callback gaps.

For repeatable comparisons, enable stats logging and compare the generated CSV files. See [Diagnosing](Diagnosing.md) for what the common stats mean.

## Shared Song Grid

The song grid lets both players share simple song structure:

- Chords.
- Beat annotations.
- Lyrics.
- Beat divisions.
- Section sizes.

The current GUI control plane is a direct single-peer TCP connection authenticated with the session id and key. It is not a room server and does not relay audio.

## Shared Tracks

The Track tab can:

- Load a local PCM16 WAV.
- Display the waveform.
- Play, stop, loop, and seek locally.
- Adjust track gain.
- Change speed and pitch with Signalsmith processing.
- Share a WAV to the authenticated peer over the GUI TCP connection.
- Sync basic play/stop controls when track sync is enabled.

Track playback is currently GUI-local through Qt audio output. It is separate from the engine's ASIO/CoreAudio output path and is not sent over the live UDP audio stream.

## Capture From The GUI

The GUI can launch `jam2-capture` for input or loopback recording and import the resulting WAV metadata into the Track tab. Capture behavior is documented in [Capture](Capture.md).
