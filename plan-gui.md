# Jam2 GUI Roadmap

## Current State

`jam2-gui` exists as a Qt 6 Widgets controller app in `apps/jam2-gui`.

Implemented first pass:

- Builds as C++20/CMake target `jam2-gui`.
- Uses Qt Widgets, Qt Core, Qt Network, Qt Multimedia, and Qt JSON.
- Writes the executable to repo-root `release`.
- Deploys Qt runtime files on Windows through `windeployqt`.
- Uses a dark 1920x1080-oriented control-surface layout.
- Starts and stops the existing `jam2` CLI process.
- Keeps live UDP audio inside the `jam2` CLI; the GUI does not own the audio path.
- Sends runtime stdin commands for metronome on/off, BPM, metronome level, and remote level.
- Reads `jam2` JSONL status output for RTT, jitter, packet loss, playback depth, underrun/missing-frame, and drift display.
- Lists audio devices through `jam2 list-devices` and passes selected device IDs to `--audio-device`.
- Supports listen/connect setup, generated `jam2://` URLs, explicit session id/key, bind/public endpoint, local device, channels, sample rate, buffer, frame size, and prefill controls.
- Opens a simple authenticated TCP JSON control connection between GUI instances using the session id/key.
- Sends basic shared messages for beat-grid edits, grid resize, lead-change cue, track metadata, track-processing state, track playback commands, and chunked WAV transfer.
- Provides Chord View, Beat View, Lyrics, Arrangement, Track, and Stats tabs.
- Uses one shared song document model for chord, beat, lyric, and arrangement views.
- Supports local song title, new/open/save JSON song files, section add/duplicate/rename/delete, 4-beat resize, arrangement reorder, and persisted track metadata.
- Provides Track tab capture controls, loopback-source refresh, captured WAV import, sidecar metadata loading, local WAV playback, track level/position controls, and chunked WAV sharing over the TCP control plane.
- Keeps speed/pitch controls as synced metadata/simple playback-rate controls until Signalsmith is added.

The existing process boundary remains:

```text
jam2-gui <-> jam2-gui        TCP control/document plane
jam2 CLI <-> jam2 CLI        UDP audio plane
jam2-gui -> jam2 CLI         local stdin runtime commands
jam2 CLI -> jam2-gui         stdout/stats/status parsing
```

## Near-Term GUI Work

Tighten the first pass into a dependable two-person control surface.

- Validate listen/connect flow between two machines on the same LAN and over the normal port-forwarded path.
- Make TCP control failure non-fatal and clearly show when shared GUI features are unavailable.
- Send host session config over TCP before the connector starts `jam2 connect`.
- Lock connector-side shared audio settings to host-provided values while keeping local device/channel choices editable.
- Show selected device backend/name in active session status after startup.
- Improve process error surfacing for invalid devices, bad URLs, missing `jam2`, blocked ports, and failed STUN/public endpoint setup.
- Add a compact machine-readable startup parser for URL, selected device details, and stream settings.
- Keep all stats as raw technical data. Do not add playability scores or inferred recommendations.

## Shared Song Document

Turn the current lightweight grid into a useful jam scratchpad.

- Extend the current document model to include lead state.
- Sync add, duplicate, rename, reorder, delete, and resize operations over TCP.
- Add revision handling for accepted/rejected remote edits rather than last-writer local mutation only.
- Make Beat View a real drum/rhythm grid with lanes such as kick, snare, hat, ride, crash, toms, and cue notes.
- Add subdivisions: quarter, eighth, sixteenth, and triplets.
- Add revision handling for track metadata changes where needed.

## Arrangement And Lead Cues

- Add arrangement blocks such as Intro, Verse, Chorus, Bridge, and Outro.
- Add start/stop practice playback for the arrangement using a local GUI timer first.
- Auto-scroll Chord, Beat, and Lyric views to the active section/bar/beat during practice playback.
- Add an optional local playhead when the metronome is enabled.
- Later, tie playhead, auto-scroll, and lead-change countdowns to the shared-grid metronome epoch from the engine.
- Keep lead cues as TCP control messages only; they must not affect audio.

## Shared Track Playback

Add shared backing-track practice without sending live playback audio over TCP or UDP.

Rules:

- The shared track is local playback on each machine.
- TCP carries metadata, file readiness, optional file transfer, and playback commands.
- The live UDP stream remains only for live peer audio.
- TCP jitter must not affect track playback once the file is available locally.

Needed work:

- Add peer file-ready state before enabling synced playback.
- Add restart, count-in, loop enable, loop start/end controls.
- Add synchronized start commands using countdown first; current play commands start immediately.
- Later, start on a declared shared metronome beat/bar epoch.
- Add engine-side local track mix support so jam-mode track playback can use the same ASIO/CoreAudio output path as remote peer audio and metronome.
- Keep track level independent from metronome level and remote peer level. Current Qt Multimedia playback already has a local independent level slider.
- Do not add a mode that sends the backing track as live peer audio unless explicitly requested later; current WAV sharing is file transfer before local playback.

## Signalsmith Stretch Pass

Add this as a targeted dependency pass after the Signalsmith source is added to the repo.

Scope:

- Use Signalsmith Stretch for pitch-preserving speed changes and small pitch shifts.
- Apply it only to shared/solo backing-track practice playback.
- Do not process live UDP peer audio with Signalsmith.
- Run stretch/pitch processing outside the UDP audio path.
- Apply pitch-preserving speed and pitch cents/semitones to playback. The current GUI only stores/syncs these values and uses Qt playback-rate for simple speed.
- Sync speed, pitch, loop region, and playback commands over TCP when `sync track controls` is enabled.

Fallback before this pass:

- Track UI keeps speed/pitch visible as synced metadata; speed is also mapped to Qt playback rate as a temporary local playback behavior.

## Track EQ And Frequency Focus

Add local practice EQ for backing tracks only.

- Scope EQ/focus to shared or solo backing-track playback.
- Do not EQ live peer audio by default.
- Do not alter the live UDP stream.
- Add focus frequency, Q/bandwidth, gain boost/cut, high-pass, and low-pass controls.
- Consider low/mid/high gain only if quick adjustment needs it.
- Store EQ/focus settings per track in the song/project document.
- Sync EQ/focus settings over TCP when `sync track controls` is enabled.
- Use simple local DSP such as biquad filters.
- Do not plan stem isolation or machine-learning source separation for this feature.

Example future synced track-processing message:

```json
{
  "type": "track.processing",
  "track_id": "abc",
  "speed": 0.75,
  "pitch_cents": 0,
  "loop_enabled": true,
  "loop_start_frame": 441000,
  "loop_end_frame": 882000,
  "eq_enabled": true,
  "focus_frequency_hz": 120,
  "focus_q": 1.4,
  "focus_gain_db": 8,
  "highpass_hz": 40,
  "lowpass_hz": 400
}
```

## Future Multi-Peer Control Topology

Only revisit this if Jam2 later supports three or four people with direct peer-to-peer UDP audio.

Preferred topology:

```text
UDP audio:        peer-to-peer mesh
TCP GUI/control: host-authoritative star
```

Rules:

- The host GUI accepts TCP connections from each participant.
- The host GUI orders shared document edits and assigns revisions.
- Clients send proposed edits with their current base revision.
- The host accepts/rejects/orders edits, then broadcasts authoritative updates.
- TCP control must not relay audio.
- Remote playback levels remain local per listener and may become per-peer controls.
- Outgoing send gain remains local to the sender.

## Engine Work Needed By Future GUI Features

Track in `PLAN.md` or implementation issues as needed.

- Machine-readable startup output should include stable selected-device and stream-config fields.
- Runtime stdin commands should grow to cover future track level/play/stop/seek once engine-side local track mixing exists.
- Engine-side local file/track playback source is needed for jam-mode backing tracks through the same ASIO/CoreAudio output path.
- Shared-grid metronome epoch data should be exposed clearly enough for GUI playhead, arrangement follow, track count-in, and lead-change timing.
- Future runtime metronome mode commands can be added when needed.

## Test Checklist

- Listener GUI starts `jam2 listen`, opens TCP control, and displays a usable connection string.
- Connector GUI pastes the URL, authenticates TCP, receives host session config, and starts `jam2 connect`.
- Both users can select local device and channels independently.
- Device refresh works when `jam2-gui.exe` and `jam2.exe` are both in `release`.
- Metronome level slider changes local click level during a jam.
- Remote playback level slider changes local peer volume without affecting outgoing audio.
- Stats strip updates with RTT, jitter, packet loss, depth, underrun/missing-frame, and drift values.
- Beat/chord edits sync reliably between two GUI instances.
- Grid resize and section operations sync reliably once implemented.
- Lead-change cue is visible on both GUIs.
- Shared track metadata syncs reliably over TCP.
- Shared WAV transfer writes verified files under `received_tracks/`.
- Qt Multimedia local track playback works for imported/captured/received WAVs.
- Current synced track playback commands start/stop both local players; future countdown/beat-epoch start improves alignment.
- Audio remains stable while TCP messages, beat edits, and GUI rendering are active.
