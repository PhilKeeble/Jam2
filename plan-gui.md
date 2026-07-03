# Jam2 GUI Plan

## Summary

Build `jam2-gui` as a separate controller process. The GUI owns the TCP control plane, shared beat view, lead cues, and process orchestration. The existing `jam2` CLI remains the audio engine and keeps the UDP audio path isolated.

Use C++20 and Qt 6 Widgets so the GUI builds as a native desktop app on Windows, macOS, and Linux with the same CMake-based workflow as the rest of the project.

Process boundaries:

```text
jam2-gui <-> jam2-gui        TCP control/document plane
jam2 CLI <-> jam2 CLI        UDP audio plane
jam2-gui -> jam2 CLI         local stdin runtime commands
jam2 CLI -> jam2-gui         stdout/stats/status parsing
```

## Architecture

- `jam2-gui` starts and stops a local `jam2` child process for listen/connect audio streaming.
- `jam2-gui` sends runtime commands to `jam2` through stdin rather than sharing sockets or audio state.
- `jam2-gui` reads machine-readable startup/status output from `jam2` for connection state, selected device details, and stats.
- UDP remains audio-only inside `jam2`; TCP never touches the audio hot path.
- TCP control-plane handling must run in the GUI process and never block the audio engine.
- Headless `jam2` must continue to work without the GUI.

## Tech Stack

Use Qt 6 Widgets rather than QML for the first GUI. The app is a practical control surface with sliders, selectors, stats, process control, and an editable beat grid; Qt Widgets keeps that straightforward and native.

Dependencies:

- C++20.
- CMake.
- Qt 6 Widgets for the desktop UI.
- Qt Core for application state, timers, and `QProcess`.
- Qt Network for `QTcpServer` and `QTcpSocket`.
- Qt JSON classes for framed control-plane messages.
- Signalsmith Stretch for future pitch-preserving practice playback.
- Essentia for future offline WAV analysis: tempo/BPM, key, and chord suggestions.

Avoid for the first version:

- QML, unless the UI later needs heavy animation or touch-style composition.
- Embedding audio/network engine logic in the GUI.
- TCP or GUI code inside the `jam2` audio process.
- Running track analysis or chord detection in the live UDP audio path.

Suggested source layout:

```text
apps/jam2-gui/
  main.cpp
  MainWindow.*
  SessionController.*
  Jam2Process.*
  ControlServer.*
  ControlClient.*
  BeatGridModel.*
  BeatGridWidget.*
  LeadCueModel.*
  SharedTrackModel.*
  SharedTrackController.*
```

Shared helpers can be extracted later if needed:

```text
src/common/
  jam_url parsing/generation
  session id/key formatting
  framed JSON message helpers
```

## TCP Control Plane

Use an authenticated TCP connection between GUI instances for non-realtime session state.

Port model:

- The listener GUI opens a TCP listener on the same numeric port as the UDP audio port.
- Users can port-forward both TCP and UDP for that port.
- The `jam2://` connection string can continue to carry one endpoint, session id, and key.

Session flow:

- Listener GUI generates the session id/key.
- Listener GUI starts `jam2 listen` with the generated session id/key and selected local audio settings.
- Listener GUI displays the `jam2://` string using the same session id/key.
- Connector GUI accepts the pasted `jam2://` string and connects to the listener GUI over TCP first.
- TCP auth uses the session id/key from the URL.
- Listener GUI sends shared audio/session config over TCP.
- Connector user selects only local settings such as device and input/output channels.
- Connector GUI starts `jam2 connect` with the URL, shared config, and local device/channel settings.
- If TCP control fails, the GUI should report that shared GUI features are unavailable; UDP audio may still be possible through headless/manual flow.

Control messages:

- Use a simple framed JSON protocol initially because it is outside the audio path and easier to debug.
- Carry GUI/session messages such as beat grid edits, lead cues, shared audio settings, metronome settings, and peer state.
- Do not send audio over TCP.
- Do not rely on TCP for low-latency metronome click timing.

Example messages:

```json
{"type":"session.config","sample_rate":44100,"frame_size":128,"playback_prefill_frames":1536}
{"type":"beat.set","revision":12,"beat":3,"text":"D"}
{"type":"grid.resize","revision":13,"beats":12}
{"type":"lead.change_pending","revision":14,"target":"student","bars":4}
{"type":"track.offer","track_id":"abc","name":"backing.wav","bytes":12345678,"sha256":"..."}
{"type":"track.play_pending","track_id":"abc","start_frame":0,"countdown_bars":4}
```

## Future Multi-Peer Control Topology

If Jam2 later supports 3 or 4 people with direct P2P UDP audio, keep the GUI/control plane host-authoritative rather than full mesh.

Topology:

```text
UDP audio:        peer-to-peer mesh
TCP GUI/control: host-authoritative star
```

Example:

```text
B GUI -> A GUI TCP
C GUI -> A GUI TCP
D GUI -> A GUI TCP
A GUI broadcasts ordered control/document events to everyone
```

Rules:

- The host/leader GUI accepts TCP connections from each participant.
- The host GUI orders shared document edits and assigns revisions.
- Clients send proposed edits with their current base revision.
- The host accepts/rejects/orders edits, then broadcasts the authoritative update.
- Last accepted host revision can win for same-cell conflicts in the first version.
- TCP control star must not relay audio; audio remains direct UDP between peers.
- Shared song document, arrangement, lead cues, track metadata, track playback commands, and synced track-processing state should flow through the host GUI.
- Remote peer playback levels should remain local per listener, and in multi-peer sessions should likely become per-peer controls.
- Outgoing send gain remains local to the sender.
- Metronome level can remain local unless an explicit sync option is added.

Reason:

- Full-mesh TCP document/control sync would require multi-path conflict resolution and duplicate event handling.
- Host-authoritative control keeps shared GUI state simple while preserving the no-relay-audio product constraint.

## GUI Controls

First useful GUI surface:

- Listen/connect setup.
- Device selection.
- Input/output channel selection.
- Metronome on/off.
- BPM.
- Metronome level slider.
- Remote peer playback level slider.
- Stats strip for RTT, jitter, packet loss, playback depth, underruns, and drift.
- Start/stop/quit controls for the local `jam2` child process.

Level controls:

- Metronome level slider maps to the `jam2` runtime metronome level command.
- Remote peer playback level slider maps to the `jam2` runtime remote level command.
- Shared track level slider maps to the `jam2` runtime track level command once engine-side track mixing exists.
- Remote playback level is local-only and should not alter what the peer hears.
- Outgoing send gain can be considered later, but should be a separate control from local playback level.

Rough layout:

```text
+--------------------------------------------------------------------------------+
| Jam2                                    Connected: Alex    RTT 18ms  Loss 0.0% |
+--------------------------------------------------------------------------------+
| [Listen] [Connect]  Device: Fireface USB  In: 1  Out: 1,2  [Start Jam] [Stop] |
+--------------------------------------------------------------------------------+
| Metronome [On]  BPM 120  Level [======----]   Remote Alex [========--]        |
+--------------------------------------------------------------------------------+
| Song: Untitled Jam                         Section: Verse A      Lead: Teacher |
| [Chord View] [Beat View] [Lyrics] [Arrangement] [Track]    Lead swap: [4 bars]|
+--------------------------------------------------------------------------------+
| Active tab content                                                              |
+--------------------------------------------------------------------------------+
| Stats: depth 35ms | underruns 0 | drift +12ppm | callback max 3.2ms           |
+--------------------------------------------------------------------------------+
```

## Shared Song Document

Build the central GUI panel around a shared song document. The document should remain a lightweight jam scratchpad, not a DAW or notation editor.

Document model:

- A song contains ordered sections such as Intro, Verse, Chorus, Bridge, and Outro.
- Sections can have short labels such as `A`, `B`, or `C`.
- Each section has a beat/bar length and can be expanded in increments of 4 beats.
- Each section stores parallel editable lanes for chords, beat/pattern notes, lyrics, and general notes.
- Shared tracks can store project metadata such as detected BPM, user-corrected BPM, loop points, pitch offset, and preferred playback speed.
- Shared tracks can store practice processing metadata such as EQ/frequency-focus settings.
- Edits sync over the GUI TCP control plane.
- Save/load local song documents later as JSON once the shared in-session model is stable. Corrected track BPM and other track metadata should persist when the project is reloaded.

Tabbed views:

- `Chord View`: chord progressions by section, bar, and beat.
- `Beat View`: drum patterns, rhythmic hits, sequence notes, stops, builds, and cue markers.
- `Lyric View`: lyric fragments aligned to sections, bars, or beats.
- `Arrangement View`: song structure and section order, such as `Intro -> Verse A -> Chorus B -> Verse A -> Bridge C -> Chorus B`.
- `Track View`: shared/solo track loading, waveform, loop controls, speed/pitch, EQ/frequency focus, analysis, and synced track-processing state.

First useful behavior:

- Default to one `A` section with 8 beats.
- Add, duplicate, rename, reorder, and delete sections.
- Expand a section by 4 beats.
- Let both users edit chord/beat/lyric cells.
- Keep text cells free-form so rough song ideas are fast to capture.
- Make the current section and nearby sections easy to scan without a busy interface.

Practice/playback behavior:

- Arrangement View should allow users to rehearse a complete song flow, not just isolated loops.
- When a practice/playback mode is started, the GUI can auto-scroll through the arrangement and show the active section/bar/beat.
- Chord, beat, and lyric views should be able to follow the current arrangement position so users see the relevant content as the song moves.
- First version can use a local GUI timer/countdown.
- Later, tie the playhead and auto-scroll to the shared-grid metronome epoch so both users see the same position.
- Shared backing tracks should later align to the same song/beat grid, so track playback, metronome, and arrangement position can start together and stay visually coherent.

Optional tracker:

- When metronome is enabled, optionally show a local playhead/arrow above the current beat.
- The tracker can be local-only at first.
- Later, tie the tracker to the shared-grid metronome epoch when that engine mode exists.

Chord View rough layout:

```text
+--------------------------------------------------------------------------------+
| Chord View                                                                      |
|                                                                                |
|   Verse A                                                                      |
|   +---------+---------+---------+---------+---------+---------+---------+-----+|
|   | Beat 1  | Beat 2  | Beat 3  | Beat 4  | Beat 5  | Beat 6  | Beat 7  |  8  ||
|   |   D     |   A     |   F#m   |   G     |   D     |   A     |   G     |  A  ||
|   +---------+---------+---------+---------+---------+---------+---------+-----+|
|                                                                                |
|   Chorus B                                                                     |
|   +---------+---------+---------+---------+---------+---------+---------+-----+|
|   |   Bm    |   G     |   D     |   A     |   Bm    |   G     |   A     |  A  ||
|   +---------+---------+---------+---------+---------+---------+---------+-----+|
+--------------------------------------------------------------------------------+
```

Beat/drum transcription view:

- Make Beat View useful for quickly transcribing drum patterns by ear.
- Provide common lanes such as kick, snare, hi-hat, ride, toms, crash, and free-form cue lanes.
- Allow simple step-grid entry across beats/subdivisions.
- Support common subdivisions such as quarter notes, eighth notes, sixteenth notes, and triplets.
- Let users mark hits, rests, accents, ghost notes, and open/closed hi-hat textually or with simple symbols.
- Keep entry lightweight enough for rough jam notes rather than full drum notation.
- Allow a backing-track loop to play while users mark the pattern.
- Later, let the metronome/rhythm-training features use the same grid for accents or practice patterns.

Beat View rough layout:

```text
+--------------------------------------------------------------------------------+
| Beat View                                                    subdivision: 16ths |
|                                                                                |
|   Verse A                                                                      |
|        1 e + a  2 e + a  3 e + a  4 e + a                                     |
|   Kick X . . .  . . X .  X . . .  . . X .                                     |
|   Snare. . . .  X . . .  . . . .  X . . .                                     |
|   Hat  x x x x  x x x x  x x x x  x x x x                                     |
|   Cue  . . . .  . . . .  stop . .  build .                                    |
+--------------------------------------------------------------------------------+
```

Chord practice and randomization:

- Add optional random chord tools for jams, ear training, and solo practice.
- Allow random chords within a selected key/scale, simple diatonic sets, or a user-defined chord pool.
- Allow random chord changes by beat, bar, or section.
- Let users freeze/edit generated chords after generation so random ideas become normal song document content.
- Keep random chord generation deterministic when shared over TCP by syncing the generated result, not relying on each peer's random generator.

Arrangement View rough layout:

```text
+--------------------------------------------------------------------------------+
| Arrangement                                                                     |
|                                                                                |
| [Intro] -> [Verse A] -> [Chorus B] -> [Verse A] -> [Bridge C] -> [Chorus B]   |
|                                                                                |
| Current: Verse A, bar 2                         [Auto Scroll] [Start Practice] |
+--------------------------------------------------------------------------------+
```

## Lead Cues

Add a lightweight shared lead indicator and lead-change cue system.

Behavior:

- Show the current lead clearly, such as `Teacher leads` or `Student leads`.
- Provide a keybind/button to request a lead swap.
- A lead swap should notify both users with a countdown, for example `Student leads in 4 bars`.
- After the countdown, update the shared lead label.
- Lead cues are TCP control messages and do not affect audio.

Timing:

- First version can use a simple GUI countdown.
- Later, use the shared-grid metronome beat/bar timeline for accurate bar-counted lead changes.

## Shared Track Playback

Add shared backing-track playback for practice/jam sessions. TCP should handle file metadata, optional file transfer, and playback commands; the audio file should play locally on both machines rather than being streamed in real time over TCP.

Track View should be a first-class tab because track playback needs more room than a compact global control strip.

Track View rough layout:

```text
+--------------------------------------------------------------------------------+
| Track View                                                                      |
+--------------------------------------------------------------------------------+
| Track: backing.wav        BPM: 118.7 [accept 120]       Key: D major           |
| [Load] [Share] [Analyze] [Sync Controls: On]                                   |
+--------------------------------------------------------------------------------+
| Waveform                                                                        |
| 00:00     00:10     00:20     00:30     00:40                                  |
| |---------|---------|---------|---------|---------|                             |
|       [ Loop Start ]======== selected loop =======[ Loop End ]                 |
+--------------------------------------------------------------------------------+
| Playback                                                                        |
| [Play] [Stop] [Restart] [Count-in 4 bars]                                      |
| Speed: 0.75x  [----------|------]     Pitch: +0 cents [----|----]              |
| Loop: [On]  Start 00:12.4  End 00:20.1  [Loop 4 beats] [Loop 8 beats]          |
+--------------------------------------------------------------------------------+
| EQ / Focus                                                                      |
| Focus Freq: 120 Hz  [----|----------------]  Gain: +8 dB  Q: 1.4              |
| High-pass: 40 Hz    Low-pass: 400 Hz                                           |
| [Bass Focus] [Vocals Range] [Guitar Range] [Reset EQ]                          |
+--------------------------------------------------------------------------------+
| Analysis                                                                        |
| Detected BPM: 118.7   Accepted BPM: 120     Key: D major                       |
| Suggested chords: D | A | F#m | G     [Apply to Chord View]                   |
+--------------------------------------------------------------------------------+
```

Modes:

- `Solo practice`: no peer, TCP connection, or UDP audio stream is required. The GUI can load, manipulate, and play audio locally through whatever output path the user's system preferences provide.
- `Jam mode`: both GUIs coordinate track readiness, playback, and optional track-processing control state over TCP, and the track is mixed locally with the live jam monitoring path when engine-side track mixing exists.

Target listening behavior:

- In solo practice, the user hears the manipulated audio file locally without starting a UDP stream.
- In jam mode, each user hears the shared audio file mixed with the remote peer audio through the same selected ASIO/CoreAudio output device used by `jam2`.
- The shared track should not be sent over the live UDP audio path as peer audio.
- TCP jitter should not affect playback once the file is available locally.

File/session behavior:

- A user can load a local audio file in `jam2-gui`.
- The GUI sends file metadata over TCP: name, size, duration if known, format, and hash.
- When a WAV is loaded, the GUI should make an approximate BPM guess and show it as editable metadata.
- The user can correct the guessed BPM; the corrected value becomes the track BPM stored in the song/project document.
- If the initial guess is right, the guessed value can be accepted and persisted as the track BPM.
- If the peer does not already have the file cached, the GUI can transfer it over TCP before playback.
- Playback controls stay in the GUI: play, stop, seek/start position, track level, and optional countdown.
- First useful format can be WAV for simplicity; MP3/FLAC can be added later if decoding support is chosen.

Synchronization behavior:

- First version can use a TCP playback command with a countdown, such as `track starts in 4 bars`, and start locally on both machines.
- Once shared-grid metronome timing exists, playback should optionally start on a declared beat/bar epoch.
- Each side should play the file locally from the same start frame/time.
- The GUI should show whether both peers have the file ready before enabling synchronized play.
- Track processing controls such as speed, pitch, loop region, and EQ/frequency focus should be syncable over TCP so both users can hear the same practice state during a connected session.
- Add a `sync track controls` option so users can choose between shared processing controls and local-only processing.

Mixing model:

- Solo practice option: GUI plays the manipulated track through the system/default output without starting `jam2` or requiring UDP/TCP session state.
- Jam-mode target option: `jam2` exposes a local file/track playback source that `jam2-gui` controls, and `jam2` mixes that track into local output alongside remote peer audio and metronome.
- The track mix should be local-only by default. It should not change what the peer receives unless a separate future "send backing track" mode is explicitly added.
- Track level should be independent from remote peer level and metronome level.

Potential future practice controls:

- Add a track speed control for targeted practice, such as `50%` to `100%`.
- Start with simple resample-speed behavior if implemented first; this slows the track and changes pitch.
- Use Signalsmith Stretch for pitch-preserving time stretch and small pitch adjustments once a DSP dependency is justified.
- Keep Signalsmith processing scoped to shared track/practice playback. It should not process live UDP peer audio.
- Run time-stretch/pitch processing outside the UDP audio path; the GUI/shared-track layer controls playback and only the resulting local track mix is heard by each user.
- Add loop controls with loop start/end markers.
- Add track EQ/frequency-focus controls for ear training and part learning.
- Add quick actions such as loop current `4` or `8` beats once beat-grid or track-grid timing exists.
- Add count-in before loop restart.
- Add manual "this region is 4/8/16 bars" BPM calculation as a more reliable correction path than fully automatic tempo detection.
- Offer actions to set song/session BPM from the accepted track BPM or fit the track to the current song BPM.
- Sync selected loop region, speed, pitch, and EQ/focus settings over TCP when `sync track controls` is enabled so both users practice the same section with the same processing.
- Keep these as GUI/shared-track future work until the base shared playback path is stable.

Signalsmith Stretch notes:

- Prefer Signalsmith Stretch because it is C++, permissively licensed, and more than sufficient for slowing down practice sections and making small pitch adjustments.
- Use it for solo practice, ear-training, demonstration, and shared backing-track practice.
- Solo practice should work entirely inside `jam2-gui` without starting a live jam or UDP stream.
- Keep the live UDP stream independent: UDP remains only for live peer audio.
- If the target output is the same ASIO/CoreAudio device as the jam, the processed track output should be handed to the local track mix path, not sent over the network as live audio.
- First useful controls: speed, pitch in semitones/cents, loop start/end, play/stop, and track level.

Tempo metadata:

- Imported WAV files should get an approximate BPM guess on load.
- The GUI must make the guessed BPM easy to edit because automatic tempo detection can be wrong for sparse, rubato, or pickup-heavy material.
- Store both the guessed BPM and accepted/corrected BPM where useful, but use the accepted/corrected BPM for project reloads and playback alignment.
- Track BPM should live in the shared song/project document, not only in transient UI state.

Offline track analysis with Essentia:

- Use Essentia in `jam2-gui` for offline analysis of imported WAV files.
- Analyze tempo/BPM, key, and suggested chord changes for a loaded track.
- Present all detected values as editable suggestions, not as authoritative truth.
- Let users accept or correct detected BPM/key/chords.
- Store accepted/corrected BPM, key, and chord content in the shared song/project document.
- Chord suggestions should populate Chord View by section/bar/beat where possible.
- Users must be able to edit generated chord cells directly in Chord View.
- Keep original analysis output available as metadata if useful, but corrected user edits are the source of truth after acceptance.
- Run analysis as a background GUI task so the interface stays responsive.
- Do not run Essentia analysis on live UDP peer audio.
- Credit Essentia and any other analysis libraries in the GUI about/license view.

Essentia integration investigation:

- Download and inspect the Essentia source before committing to a full library dependency.
- Identify the smallest practical subset needed for WAV tempo/BPM, key, chroma, and chord suggestions.
- Prefer a lightweight integration over linking all Essentia features and optional dependencies.
- Avoid unused features such as Python bindings, TensorFlow models, broad file/metadata loading, fingerprinting, and unrelated MIR algorithms unless a future use case requires them.
- Consider extracting or wrapping only the relevant algorithms if licensing and maintenance cost are acceptable.
- Consider a separate analysis helper executable if direct Qt GUI linking makes the GUI build too heavy or awkward on Windows/macOS.
- `jam2-gui` should be able to consume analysis metadata generated elsewhere, not only analysis it runs itself.

Track EQ and frequency focus:

- Add EQ controls for solo practice and shared backing-track playback so users can hunt for parts by ear while slowing or looping the track.
- Scope EQ to the backing/shared track only; do not EQ live peer audio by default and do not alter the live UDP stream.
- First useful controls should include a movable focus frequency, bandwidth/Q value, and gain boost/cut.
- Add simple high-pass and low-pass filters so users can narrow the audible frequency range.
- Add optional low/mid/high gain controls if they help quick adjustment.
- Store EQ/focus settings per track in the song/project document.
- Sync EQ/focus settings over TCP when `sync track controls` is enabled so a teacher can adjust the backing track and both users hear the same local processing state.
- Use straightforward local DSP such as biquad filters; do not plan stem isolation or machine-learning source separation for this feature.

Example synced track-processing state:

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

## Engine Requirements

`jam2-gui` depends on small audio-engine additions, tracked in `PLAN.md`:

- `jam2 listen` can accept GUI-provided session id/key.
- Headless `jam2 listen` still generates a session id/key when none is provided.
- Machine-readable startup/status output is available.
- Runtime stdin commands exist for metronome level and remote playback level.
- Runtime stdin commands and local mix support exist for shared track playback level/play/stop once target shared playback is implemented.
- Future runtime metronome mode commands can be added when metronome modes exist.

## Things To Test

- Listener GUI starts `jam2 listen`, opens TCP control, and displays a usable connection string.
- Connector GUI pastes the URL, authenticates TCP, receives shared audio config, and starts `jam2 connect`.
- Both users can select local device and channels independently.
- Metronome level slider changes local click level during a jam.
- Remote playback level slider changes local peer volume during a jam without affecting outgoing audio.
- Beat grid edits sync reliably between both GUIs.
- Grid resize by 4-beat increments syncs reliably.
- Lead-change cue and countdown are visible to both users.
- Shared track metadata and file-ready state sync reliably over TCP.
- Shared track playback can start on both machines from the same start point after a countdown.
- In the target implementation, shared track audio is heard through the same ASIO/CoreAudio output as remote peer audio and metronome.
- Audio remains stable while TCP messages, beat edits, and GUI rendering are active.
