# Jam2 GUI

Running `jam2` with no arguments is the normal way to use Jam2. The GUI and persistent local audio engine live in one application process; starting, joining, leaving, and changing peer membership attach or detach the direct UDP session without reopening an unchanged audio device. The authenticated TCP control connection between GUIs carries shared settings, song grid edits, and looper arrangement sync. Audio travels directly between peers over UDP.

## Start A Jam

On the creator's machine:

1. Open `jam2`.
2. In **Start Jam**, choose the bind host and UDP port. `0.0.0.0:49000` is the usual starting point.
3. Leave STUN enabled for internet testing, or enable **No STUN** for manual/LAN testing.
4. Choose the local audio device and input/output channels.
5. Choose a tuning profile. `fast` is the default, `moderate` is more forgiving, and `safe` is the current Wi-Fi profile.
6. Start the jam and send the generated `jam2://...` URL to the other player.

If session startup fails, Jam2 shows one modal **Start Jam failed** dialog with the detailed in-process network error instead of relying on console output.
On success, the **Jam Ready** invite window is non-modal: logging and session startup continue while it remains open, and closing it does not affect the jam.

The creator's profile selects the session sample rate and frame size. Each
player independently chooses a local profile, audio device, channels, callback
buffer, prefill, and jitter/playout settings.

## Join A Jam

On the joining machine:

1. Open `jam2`.
2. Paste the `jam2://...` URL into **Join Jam**.
3. Choose the local audio device, channels, and Join profile.
4. The sample rate, network frame size, and named session profile are received
   from the creator before network audio starts. Device, channels, levels, and
   local tolerance/tuning controls remain local choices.
5. Connect. Choosing `safe` here changes only this machine's receive/playback
   tolerance; it does not change the creator or the jam packet format.

If the connection fails, Jam2 shows one modal **Join Jam failed** dialog with the detailed in-process network error. Use the connection test tool described in [Connection Test](ConnectionTest.md) if more detail is needed.

## Runtime Controls

The GUI exposes the useful live controls from the engine:

- Toggle the metronome on or off.
- Change BPM.
- Change metronome mode.
- Adjust local metronome level.
- Adjust local remote playback level or mute the remote peer.
- Enable or disable connection diagnostics while testing.

These controls submit fixed-shape typed commands directly to the in-process engine; they do not use a child process, stdin, loopback control socket, binary framing, or JSONL state reconstruction.

## Settings And Device Tests

The cog in the header opens persistent Audio, Create Connection, Create
Defaults, Join Defaults, Logs, and Recording settings. Audio keeps Local Audio
separate from Network Audio. Network Audio uses one shared device and channel
mapping by default; enable **Use different audio devices and channels for
Create and Join** to reveal independent Create Jam Audio and Join Jam Audio
settings. Existing shared preferences populate both roles when this split is
first enabled. Create and Join also keep separate local tuning profiles; the
creator still owns session-wide values such as session sample rate, frame size,
and audio quality. Discovery and public endpoint defaults apply only when
creating a jam.

Local, Start, and Join setup use fixed 44100/48000 sample-rate and
32/64/128/256 buffer-size choices. **Test Device** reports the device's current
rate and which of those combinations can open, using an in-app silent result
dialog. Changing active Local Audio settings restarts the local engine; if the
new configuration cannot start, Jam2 attempts to restore the previous one.
Audio hardware controls are disabled while a jam is active.

The Logs tab selects one folder for GUI event logs and network stats CSV files.
Recording defaults are independent for Perform Input and System Loopback, with
separate output folders and a preferred recording mode.

## Stats

Every two seconds the GUI displays a deliberately small diagnostic set: RTT per
peer, weighted average jitter, interval packet loss, local output underruns,
and one diagnosis hint. When diagnostics are disabled these fields stay at
`-`, and the optional collection/aggregation path is not run. Detailed raw
measurements are written as hidden two-second CSV samples plus a final row for
post-jam analysis; they are not printed periodically to GUI stdout.

For repeatable comparisons, enable stats logging and compare the generated CSV files. See [Diagnosing](Diagnosing.md) for what the common stats mean.

## Shared Song Grid

The song grid lets both players share simple song structure:

- Chords.
- Beat annotations.
- Lyrics.
- Beat divisions.
- Section sizes.

The GUI control plane uses authenticated TCP connections to the creator for
ordering and distribution. It is not a room server and does not relay audio.

## Generated Practice Ideas

The Chord and Beat pages share one coupled practice-idea generator. Key, style,
character, length, and **Complexity 1–8** are selected in the Generate dialog.
Level 4 is the default. The chosen style continues to control the musical form,
vocabulary, groove family, BPM range, and click feel; complexity independently
changes harmonic movement, melody vocabulary, syncopation, subdivisions, and
fills. Raising complexity therefore produces more advanced pop, blues, modal,
jazz, or metal material without changing the selected style or automatically
raising the tempo.

The generated chord and beat section names show separate `H` and `R` levels.
The first UI sets both to the selected complexity value, while the saved
metadata keeps them separate for inspection and future independent controls.
Level 8 uses one coherent advanced harmonic scheme per idea rather than mixing
every available chromatic technique. Irregular rhythmic complexity uses the
current metronome meter and never changes it automatically.

## Track And Looper

The Track tab can:

- Manage four looper banks.
- Add PCM16 WAV lanes to the active bank.
- Add empty lanes and arm a lane for recording. Perform input takes are recorded
  by the persistent engine through typed in-process commands; loopback takes
  are recorded by the GUI.
- Use a stacked lane editor with inline mute, solo, record-arm, gain, rename, remove, drag, and edge-crop controls.
- Render the active bank to a prepared mono PCM16 cache.
- In Perform mode, load that prepared cache into the engine and control play/stop/level there.
- Sync collaborative arrangement snapshots and missing managed WAV assets by content hash when Track Sync is enabled.
- Use **Share Tracks** to explicitly reconcile all asset-backed local lanes with the jam.

Perform prepared-cache playback uses the engine's ASIO/CoreAudio output path. Prepared caches must match the active engine sample rate; offline resampling is deferred.

Generated practice reference WAVs remain local after rendering, even during an
active jam. They are saved with the local project and omitted from arrangement
sync until **Share Tracks** is clicked. Share Tracks promotes those lanes and
publishes them through the same content-hash transfer used for other WAVs.
When a peer already has a valid WAV with the requested hash, Jam2 reuses it
instead of transferring another copy. Missing WAVs are requested one at a time;
the next request starts only after the receiver has validated and committed the
previous file.

A newly selected WAV with a different sample rate is rejected before it changes
the track, looper bank, prepared mix, or current playback, and the dialog shows
both expected and actual rates. If an existing local lane is incompatible with
a jam being joined, Jam2 leaves it visible but quarantines it from playback and
Track Sync until it is unloaded or replaced.

Any authenticated peer may originate shared arrangement edits or prepared-track Play, Stop, Restart, or Record Start while Track Sync is enabled. The creator validates each full-snapshot arrangement proposal, assigns the next ordered revision, and rebroadcasts it; this sequencing role does not make the creator the sole editor. Disabling Track Sync keeps that peer's controls local, prevents it from proposing shared edits or track actions, and makes it disregard incoming ones. The setting is peer-local and is not loaded from project or shared-arrangement snapshots. Source event IDs persist across a leave/rejoin of the network worker so replay protection does not discard the first actions after reconnection.

Loading or recording a WAV while Track Sync is enabled automatically offers that local lane to the jam. Existing asset-backed lanes are also offered when a peer joins or re-enables Sync. Offers use stable contribution IDs and content hashes: a matching empty lane may be filled, while a conflicting occupied lane is preserved and the offered lane is appended. The creator requests the other peer's tracks before publishing its snapshot, so peers that built separate track sets with Sync off converge to the additive union regardless of which side re-enables Sync first. **Share Tracks** retries this reconciliation explicitly; it never replaces a different existing lane.

Lane recording is local. The first version records one clip per lane, stages the recorded WAV, inserts it at timeline frame 0, and lets the user adjust the lane region afterward. The selected lane region can be moved by dragging the clip body and cropped by dragging either edge; numeric frame controls remain available for exact edits.

Perform recording waits for a safe next whole bar, performs the grid-aligned count-in, and starts on a shared Track Sync boundary. On that boundary all opted-in peers return to `1.1` and restart prepared tracks, keeping their markers and backing tracks aligned with the take. A manual Stop finishes at the next whole bar so the imported take remains bar-aligned. The waveform and Looper position markers follow the continuous engine transport and the configured beats per bar.

## Track Recording From The GUI

The GUI records Perform Input takes through the already-loaded local or network
engine and records System Loopback takes internally, then imports the resulting
WAV into the armed Track lane. Perform Input therefore has no separate device,
channel, sample-rate, or buffer selector. It exposes duration/stop behavior,
count-in and metronome behavior, and manual latency adjustment. System Loopback
instead exposes its preferred source, duration/stop behavior, trigger,
threshold, hold, pre-roll, tail, and trim controls; it has no Perform Input
count-in, metronome, latency, or ASIO controls. Arm-dialog changes apply to that
take only and do not overwrite the saved defaults.

Perform Input uses the engine's smoothed weighted mono fold-down. Each selected
channel learns its own peak-noise floor only while closed, so microphone or
interface noise is not promoted when another channel's note decays. Activity
has hysteresis and each channel weight is ramped per sample with a 4 ms attack
and 45 ms release, so a quiet or noisy selected input cannot switch the
averaging divisor at callback boundaries. A single sounding channel retains
unity gain even when four inputs are selected; multiple sounding channels form
a bounded weighted average. CSV diagnostics record the effective channel
weight, normalization gain, transition count, maximum block gain change, and
the first four channel weights and learned noise floors.

All popup dialogs use a shared compact-window policy. Custom dialogs, messages,
progress windows, and the non-native Open, Save, and folder pickers are capped
below the available desktop size, restored if the window manager tries to
maximize them, and centred over the Jam2 window.

System Loopback recording uses its own block active-channel fold-down and logs
the endpoint format, channel mask, active-channel range, and recorded peak.

Recordings always target the active jam contract rate, or the running local
engine rate outside a jam. Perform Input writes at that engine rate. System
Loopback may be supplied by Windows at a different shared-mode endpoint rate;
Jam2 applies offline band-limited resampling before writing the WAV, so a
48 kHz Windows endpoint still produces a 44.1 kHz WAV for a 44.1 kHz engine.
The completion log reports both rates, frame counts, and the conversion ratio.
If a jam contract and its engine rate ever disagree, recording is refused
instead of creating a WAV that the active project cannot load.

The Arm dialog identifies its target by stable bank and lane IDs. If a synchronized arrangement update removes that lane or switches the active bank while the dialog is open, arming is cancelled with a warning instead of using stale lane storage.

The network button is **End Jam** for the creator and **Leave Jam** for every
other participant. End Jam returns connected GUIs to Local immediately; Leave
Jam affects only that participant.
