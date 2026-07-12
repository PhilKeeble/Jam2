# Jam2 GUI

`jam2-gui` is the normal way to run Jam2. It controls a local `jam2` audio engine process, shows live stats, and opens a TCP control connection to the other GUI for shared settings, song grid edits, and looper arrangement sync. Audio still travels directly between the two `jam2` engine processes over UDP.

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

## Track And Looper

The Track tab can:

- Manage four looper banks.
- Add PCM16 WAV lanes to the active bank.
- Add empty lanes and arm a lane for recording. Perform input takes are recorded by the running engine over the local GUI-control socket; loopback takes are recorded by the GUI.
- Use a stacked lane editor with inline mute, solo, record-arm, gain, rename, remove, drag, and edge-crop controls.
- Render the active bank to a prepared mono PCM16 cache.
- In Perform mode, load that prepared cache into the engine and control play/stop/level there.
- Sync host-authoritative arrangement snapshots and missing managed WAV assets by content hash when Track Sync is enabled.

Perform prepared-cache playback uses the engine's ASIO/CoreAudio output path. Prepared caches must match the active engine sample rate; offline resampling is deferred.

Lane recording is local. The first version records one clip per lane, stages the recorded WAV, inserts it at timeline frame 0, and lets the user adjust the lane region afterward. The selected lane region can be moved by dragging the clip body and cropped by dragging either edge; numeric frame controls remain available for exact edits.

## Track Recording From The GUI

The GUI records Perform input takes through the running engine and records loopback takes internally, then imports the resulting WAV into the armed Track lane.
