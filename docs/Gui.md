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
separate output folders and a preferred recording mode. Finite recording limits
are entered as bars and use the current BPM and beats-per-bar setting.

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

The Chord and Beat pages share one coupled practice-idea generator. The Generate
dialog selects key, style, internal profile, profile-native form, meter,
compatible scale/mode override, optional production family, and
**Complexity 1–8**. Auto choices use the profile catalog; there is no global
length whitelist or Character control.

Complexity is a shared cumulative curriculum whose concrete realization is
profile-dependent. It progresses through core grammar, voicing and connection,
directed colour, rhythmic development, expanded tonal or riff vocabulary,
independent dialogue and form, large-scale tonal or metric tools, and integrated
use. A level unlocks compatible tools without forcing them or applying a Jazz
substitution vocabulary to static/modal profiles.

Drum complexity uses probability bands rather than locking ordinary drummer
techniques to individual levels. Levels 1-4 can all use kick alternatives,
ghosts, cymbal articulation, pickups, and phrase-end fills; their occurrence
chances rise gradually with the selected level. Levels 5-8 additionally unlock
advanced syncopation and subdivision cells. Triplet, sixteenth, hat-roll, and
three-over-two cells span multiple beats and repeat or answer within a phrase;
the generator never replaces one isolated random beat. Each family begins with
an authored two-bar A/A' pocket and retains protected kick/backbeat anchors.
Overall hit growth remains bounded to 35% over the core groove.

Chord, melody, bass, and supporting-line events share an editable per-beat
subdivision named like the drum grid: Quarter, Eighth, 16th, or Triplet. Their
timing remains independent of the drum division. In the Chord page, `~` holds
the previous event and `-` rests. The six rows are Subdivision, Chords, Chord
Tones, Melody, Bass, and Supporting Line.

Generate uses 13 researched public styles and 26 compact internal profiles.
The selected profile controls compatible scale/mode choices, native form,
meter, progression and groove families, bass grammar, supporting roles, and
sound targets. Lo-Fi and Synthwave are compatible production families rather
than duplicate styles. Modern Progressive Metalcore is available separately as
an experimental sound-design profile.

There is no global Mood or Character generator. A seed-derived,
profile-compatible variation plan changes bounded density, register,
articulation, brightness, space, and timing axes while preserving the selected
style grammar. The shared eight-level complexity ladder unlocks musical tools;
an unlocked tool is not forced into every generation.
Melodies are chord-aware vocal-like instrumental phrases where the profile is
vocal-centred. Bass and support remain independent editable parts rather than
slash-chord or melody metadata.

The performance view plus both Chord and Beat pages expose **Continue Idea...**.
It defaults from the viewed bank to the next empty bank, analyses the source
harmony, tonal centre, groove anchors, and melodic rhythm, then selects a
related-but-contrasting continuation while preserving the source bank, tempo,
meter, and mode. The two-bank dialog can target any other bank. The selected
relationship and raw chord/groove/melody similarity measurements are retained
in Idea Details and the diagnostic log.

Both Chord and Beat pages also expose **Generate...**, **Generate Reference WAVs...**,
and **Idea Details...**. Idea Details opens in a practical teaching view that
explains the style, form, groove, part relationships, complexity choices, and
ways to try the idea in a jam. **Detailed Analysis** switches to the complete
seed, recipe, native-form sections, theory decisions, role events,
articulations, per-lane timing, automation, and numeric synthesis parameters.
It warns when the current grid differs from the stored generated result.
Generated recipes use the research-based version-7 schema exclusively. Earlier
generated recipe versions are rejected; there is no Mood/Character migration or
fallback generation path. Manual songs without generated metadata remain valid.

Reference WAVs can render five separate layers: chords, drums, melody, bass,
and supporting line. Rendering uses the recipe's deterministic synthesis,
articulation, per-lane timing, automation, meter denominator, and style kit.
The Beat grid adds dedicated Cross-stick / Rim
lanes to Kick, Snare, Closed/Open HH, Ride, Crash, and Tom. This lets Bossa and
Reggae expose characteristic percussion relationships instead of hiding them
behind a generic kit label. Render diagnostics report profile and patch IDs,
event counts, timing values, elapsed time, and output peak.

Generating an idea applies its researched BPM, explicit meter numerator and
denominator, tempo pulse, subdivision click, and accent grouping. Compound
meters use a dotted-quarter tempo pulse spanning three written eighths; simple
and odd meters use one written unit per tempo pulse. The user can change the
written beat unit, tempo pulse, metronome division, play mask, and accents
afterward.
Pattern headings use beat/subdivision labels:
Quarter shows `1.1 2.1 3.1 4.1`, Eighth shows `1.1 1.3 2.1 2.3`,
16th shows `1.1 1.2 1.3 1.4`, and Triplet shows `1.1 1.2 1.3` per beat.

## Track And Looper

The Track tab can:

- Manage the fixed Banks A-D. Chord, beat, lyric, and looper editors all show
  one locally selected bank at a time without changing the live bank.
- Add PCM16 WAV lanes to the selected bank.
- Add empty lanes and arm a lane for recording. Perform input takes are recorded
  by the persistent engine through typed in-process commands; loopback takes
  are recorded by the GUI.
- Use a stacked lane editor with inline mute, solo, record-arm, gain, rename, remove, drag, and edge-crop controls.
- Read bar numbers at the top of the lane timeline while retaining a vertical grid line and snap point for every beat.
- Render each bank to its own exact-section-length mono PCM16 cache.
- In Perform mode, load that prepared cache into the engine and control play/stop/level there.
- Sync collaborative arrangement snapshots and missing managed WAV assets by content hash when Track Sync is enabled.
- Use **Share Tracks** to explicitly reconcile all asset-backed local lanes with the jam.

Perform prepared-cache playback uses the engine's ASIO/CoreAudio output path. Prepared caches must match the active engine sample rate; offline resampling is deferred.

The Performance bank strip launches a bank. While Global Play is running, a
shared launch first prepares the target on every participating peer, then the
creator publishes one absolute beat from the continuous UDP grid. All peers
start that bank from source `1.1` on the same safe bar without replacing the
grid epoch; an empty bank switches the shared visual position and contributes
silence. Track Sync disabled makes bank launches and arrangements local. A peer
with Track Sync disabled acknowledges but does not join a shared transition, so
it cannot hold the other peers behind the 30-second preparation deadline.

The Looper Arrangement dialog stores up to 64 Bank/Repeat rows plus an optional
loop. Save updates the project definition; Start begins at row one, Stop returns
the dialog to editing, and manual bank launch stops the running arrangement.
Transitions use section lengths and exact shared-grid boundaries rather than a
wall-clock timer. Performance shows LIVE, NEXT, and current arrangement state.

Generated practice reference WAVs remain local after rendering, even during an
active jam. They are saved with the local project and omitted from arrangement
sync until **Share Tracks** is clicked. Share Tracks promotes those lanes and
publishes them through the same content-hash transfer used for other WAVs.
When a peer already has a valid WAV with the requested hash, Jam2 reuses it
instead of transferring another copy. Share Tracks sends one bounded batch
manifest, requests missing WAVs one at a time, stages the complete additive
union, and publishes one authoritative arrangement only after every advertised
asset is validated. The sender holds incoming arrangement UI changes until the
batch completion message while audio and the UDP scheduling grid continue.

A newly selected WAV with a different sample rate is rejected before it changes
the track, looper bank, prepared mix, or current playback, and the dialog shows
both expected and actual rates. If an existing local lane is incompatible with
a jam being joined, Jam2 leaves it visible but quarantines it from playback and
Track Sync until it is unloaded or replaced.

Any authenticated peer may originate shared arrangement edits or prepared-track Play, Stop, Restart, or Record Start while Track Sync is enabled. The creator validates each full-snapshot arrangement proposal, assigns the next ordered revision, and rebroadcasts it; this sequencing role does not make the creator the sole editor. Disabling Track Sync keeps that peer's controls local, prevents it from proposing shared edits or track actions, and makes it disregard incoming ones. The setting is peer-local and is not loaded from project or shared-arrangement snapshots. Source event IDs persist across a leave/rejoin of the network worker so replay protection does not discard the first actions after reconnection.

Loading or recording a WAV while Track Sync is enabled automatically reconciles
that local set with the jam. Offers use stable contribution IDs and content
hashes: a matching empty lane may be filled, while a conflicting occupied lane
is preserved and the offered lane is appended. **Share Tracks** retries the
atomic additive reconciliation explicitly; it never publishes intermediate
one-track snapshots or replaces a different existing lane.

Lane recording is local. The first version records one clip per lane, stages the recorded WAV, inserts it at timeline frame 0, and lets the user adjust the lane region afterward. The selected lane region can be moved by dragging the clip body and cropped by dragging either edge; numeric frame controls remain available for exact edits.

Perform recording waits for the next safe beat of the continuous epoch, performs
the requested count-in bars, and then starts the prepared backing, recording,
and track-relative song markers together at `1.1`. The UDP epoch is not reset.
A manual Stop finishes at the next whole bar so the imported take remains
bar-aligned. Global Play starts the track-relative song markers on the next
beat even when no prepared WAV exists. A prepared WAV is optional audio: if it
becomes available while Global Play is running, it joins on a later beat at the
corresponding song position without resetting `1.1` or the metronome accent.
The scheduling grid continues silently underneath while Global Play is stopped.

## Track Recording From The GUI

The GUI records Perform Input takes through the already-loaded local or network
engine and records System Loopback takes internally, then imports the resulting
WAV into the armed Track lane. **Current Jam** records local input plus received
peer audio at the local endpoint; backing audio and the local metronome are
optional and disabled by default. A leader-audio click embedded in received
peer audio cannot be separated and is identified in the recording dialog.
Perform Input therefore has no separate device,
channel, sample-rate, or buffer selector. It exposes bar-limit/stop behavior,
count-in and metronome behavior, and manual latency adjustment. System Loopback
instead exposes its preferred source, bar-limit/stop behavior, a shared silence
threshold, tail duration, and leading/trailing trim controls; it has no Perform Input
count-in, metronome, latency, or ASIO controls. Arm-dialog changes apply to that
take only and do not overwrite the saved defaults.

Finite Perform Input takes schedule their stop at an exact engine frame measured
from the scheduled recording start, after any count-in. Finite System Loopback
takes convert the chosen bar count to source frames and begin capturing immediately.
The trim settings are applied after capture and can shorten the saved WAV without
changing the recording start or its duration limit.

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

Every new jam receives a two-word display name. Managed paths replace spaces
with underscores: an unsaved `Purple Orbit` jam writes beneath
`release/tracks/Purple_Orbit`, with generated, received, imported, recorded,
prepared, and jam-recording subfolders. Saving moves that workspace to
`release/songs/Purple_Orbit/Purple_Orbit.jamjar`; JamJar files retain the same
JSON project representation and use relative paths to their adjacent assets.
The Performance people rail exposes **Record Jam**, whose default folders are
`Take-1`, `Take-2`, and so on.

The Arm dialog identifies its target by stable bank and lane IDs. If a synchronized arrangement update removes that lane or switches the active bank while the dialog is open, arming is cancelled with a warning instead of using stale lane storage.

The network button is **End Jam** for the creator and **Leave Jam** for every
other participant. End Jam returns connected GUIs to Local immediately; Leave
Jam affects only that participant.
