# Jam2 Plugin Hosting Idea

Status: initial implementation completed for Windows and implemented in the
cross-platform source/build paths for macOS. Windows has been validated at a
32-frame/48 kHz cadence with all 15 installed test plugins, including Neural
DSP, Equator2, Kontakt, ROLI, TONEX, Melodyne, Helix Native, Supercharger, and
Surge XT modules. A five-second live ASIO test on device 5/input 2 also
completed Nolly's 7,483 deadline-eligible blocks with zero misses. Native macOS
build, signing, device, editor, and installed-plugin validation still requires
a Mac.

The implementation deliberately remains the narrow first version described
here: VST3 only, one isolated plugin per assigned input source, Standard MIDI
or MPE input, canonical mono output, and no Track View plugin subsystem.

## Purpose

Add narrowly scoped third-party audio and instrument plugin support without turning Jam2 into a DAW or changing its direct, low-latency audio and networking model.

The central collaboration rule is:

> Plugins execute only on the machine that owns them. Jam2 converts their result to canonical mono PCM audio for monitoring, recording, sending, and synchronisation; it never shares a dependency on the plugin.

Peers do not need the same plugin, version, preset, licence, MIDI controller, or plugin state. Audio-input and MIDI/MPE performances are recorded directly from the current local source output. Track View receives ordinary completed audio and does not host editable plugins.

## Intended Uses

### Live audio-input effects

A user can place an effect such as a Neural DSP amp plugin on any assigned physical audio source:

```text
audio device input
    -> local plugin
    -> mono My Send bus
       -> local self-monitor
       -> Jam2 network capture
```

Other peers receive the processed audio. They do not load or know about the plugin.

Closing the plugin editor must not stop processing. The controls have distinct meanings:

- **Open Plugin** opens or focuses the plugin's editor.
- **Bypass** switches to dry input without unloading the plugin.
- **Remove** unloads the plugin and restores the normal Jam2 input path.

The engine may have several selected physical input channels. Each selected channel can be an independent mono source, or two explicitly chosen channels can be grouped as one stereo input source when a plugin should process them together. A plugin belongs to the resulting source, not blindly to the whole device and not independently to each channel of a stereo pair. Every source becomes mono at the Jam2 output boundary.

For example:

```text
Input 1 -> mono source -> optional Plugin A --------+
Input 2 -> mono source -> optional Plugin B --------+-> local source mix
Inputs 3+4 -> stereo input group -> Plugin C -------+
```

Jam2's current engine downmixes selected physical inputs before the capture path. Independent per-source plugins require retaining the selected raw channel buffers through source assignment and plugin processing. The final mono downmix therefore moves to the live network boundary:

```text
raw selected channels
    -> explicit mono sources or stereo input groups
    -> each source's plugin
    -> deterministic mono downmix
    -> canonical mono Jam2 source
       -> mono Track View recording
       -> final My Send bus
       -> mono local self-monitor
       -> current mono live network stream
```

Never downmix a stereo input group before its plugin. A stereo processor must receive its left and right input together. The plugin may process and return stereo internally, but Jam2 then converts its main output to mono before monitoring, recording, mixing, or sending it. Jam2's software self-monitor taps that final mono bus so the performer monitors the same source mix sent to peers.

### MIDI input, MPE mode, and instrument plugins

MIDI is used only as live control input to a local instrument plugin:

```text
MIDI or MPE controller
    -> timestamped MIDI events
    -> local instrument plugin
    -> wet PCM audio
    -> deterministic mono downmix
    -> canonical mono Jam2 source
       -> Track View recording
       -> My Send bus -> self-monitor and network capture
```

For Track View, Jam2 records the instrument plugin's downmixed mono output directly into a canonical audio asset. After a successful performance commit, Jam2 does not need to retain the MIDI notes, MPE expression, drum triggers, instrument state, or a MIDI track.

The same path supports conventional MIDI keyboards, MPE controllers, electronic drum kits, pad controllers, and other performance controllers. An electronic drum kit is normally a Standard MIDI source, not a separate Jam2 source type or protocol mode. It can trigger a local drum instrument plugin, with the plugin's wet audio sent live or recorded into Track View. The MIDI path must not discard drum-specific performance messages such as note-off/choke events, velocity, polyphonic or channel pressure, and continuous controller values such as hi-hat position.

Some drum plugins expose several audio output buses for separate kick, snare, overhead, and room channels. The Jam2 scope should use only the plugin's main output bus and convert it to mono. Hosting and routing arbitrary auxiliary plugin outputs would move toward a DAW mixer and can be considered separately.

There is deliberately no initial support for:

- MIDI piano-roll editing.
- MPE expression-lane editing.
- Quantising or regenerating a committed performance.
- Sharing MIDI or plugin state with peers.
- Requiring a peer to load a matching instrument.

Input/instrument recording retains the source's actual output at the time of recording:

| Recording choice | Retained track material |
| --- | --- |
| Audio source with active effect | Processed mono audio output |
| Audio source with bypassed/no effect | Unprocessed mono audio output |
| MIDI instrument, using Standard MIDI or MPE mode | Downmixed mono instrument audio only |
| Combined local inputs | Combined mono My Send mix |

MIDI/controller events and separate before/after-effect copies are not retained. The user chooses the desired sound using the source's **Open Plugin** and **Bypass** controls before recording.

## One Plugin per Input Source

Each assigned input source may own at most one loaded plugin instance. This is a deliberate product and safety constraint, not merely a first-version GUI limitation:

- A physical mono source or stereo input group can have zero or one effect plugin.
- A MIDI source, configured as Standard MIDI or MPE, can have zero or one instrument plugin; without an enabled instrument it produces no audio.
- A plugin output cannot feed another plugin.
- There are no serial chains, parallel chains, send effects, sidechains, or plugin-to-plugin routing.
- Replacing a plugin keeps the current instance active while the candidate is
  scanned and initialized, then atomically swaps to the completed replacement.
- Bypass keeps the same plugin instance but routes latency-aligned dry audio through the same canonical mono output boundary.
- Muting or disabling a MIDI instrument source produces silence and resets its active notes; it does not expose raw MIDI as another Jam2 source.

This keeps each source understandable from one compact card, bounds the number of worker processes and shared-memory paths, and makes crash attribution and recovery unambiguous. A source's worker identity, state, latency, failures, and output layout always refer to that source's one plugin. Supporting plugin chains would be a separate product decision requiring cumulative latency accounting, chain-wide failure policy, layout negotiation between every stage, and substantially more routing UI.

## MPE Scope

Supporting an MPE controller such as a ROLI Seaboard with Equator2 is principally transparent, timestamped MIDI pass-through. Jam2 Core owns the generic MIDI/MPE input contract, but does not need to turn performances into editable `MPENote` objects unless a future visualiser requires it. The plugin worker performs the plugin-specific conversion from those preserved events to VST3 events, note IDs, and note-expression parameters.

The MIDI path must preserve:

- All MIDI channels rather than folding input onto channel 1.
- Note-on velocity and note-off/release velocity.
- Per-note pitch bend.
- Per-note channel pressure.
- CC74 timbre/slide messages.
- Master-channel controls such as sustain.
- MPE zone configuration messages and pitch-bend ranges where applicable.
- Event ordering and sample offsets within each audio block.

MIDI events arrive outside the audio callback and must cross into it using a bounded, preallocated real-time-safe queue. Jam2 should send all-notes-off/reset messages to the relevant channels when stopping, disconnecting a controller, replacing a plugin, or recovering from an overflow. Queue overflow and dropped MIDI events must be visible in stats.

## Core and Application Source Layout

MIDI input, MPE input, physical-input grouping, deterministic mono conversion, and canonical mono track audio are engine capabilities. Their reusable types and real-time behaviour belong in `libs/jam2-core`, not in a plugin-specific application subsystem. Stereo is retained only where needed to feed or receive a plugin bus before the canonical mono boundary.

A proposed source layout is:

```text
libs/jam2-core/
  include/jam2/
    input_source.hpp
    audio_source_router.hpp
    rendered_input_source.hpp
    midi_device.hpp
    midi_event.hpp
    midi_event_queue.hpp
    midi_clock_mapper.hpp
    midi_source.hpp
    mpe.hpp
    track_take_recorder.hpp
    prepared_track_source.hpp
    pcm16_wav.hpp

  src/
    audio_source_router.cpp
    rendered_input_source.cpp
    midi_device_windows.cpp
    midi_device_macos.mm
    midi_event_queue.cpp
    midi_clock_mapper.cpp
    midi_source.cpp
    mpe.cpp
    track_take_recorder.cpp
    prepared_track_source.cpp
    pcm16_wav.cpp

app/
  application/
    InputSourceController.hpp/.cpp

  gui/
    AudioSourcesDialog.hpp/.cpp
    MidiSourcesDialog.hpp/.cpp
    PluginSelectorDialog.hpp/.cpp

  pluginhost/
    PluginProtocol.hpp
    PluginSharedMemory.hpp/.cpp
    PluginHostService.hpp/.cpp
    PluginAudioBridge.hpp/.cpp
    PluginRegistry.hpp/.cpp
    PluginWorkerMain.cpp
    Vst3Host.hpp/.cpp
    Vst3Instance.hpp/.cpp
    Vst3MidiMapper.hpp/.cpp
    Vst3MpeMapper.hpp/.cpp
    Vst3Scanner.hpp/.cpp
    Vst3EditorWindow.hpp
    Vst3EditorWindowWindows.cpp
    Vst3EditorWindowMac.mm
```

The names are illustrative, but the dependency boundary is important.

Jam2 Core owns:

- Physical input-channel identity and the selected engine device's channel set.
- Explicit mono sources and ordered left/right stereo input groups.
- Enforcement that a physical channel belongs to at most one active group.
- Input-group metadata, pre-plugin channel routing, and deterministic post-plugin mono conversion.
- The real-time canonical mono source mixer shared by monitoring, recording, and My Send.
- MIDI device discovery, opening, timestamp capture, and stable device identity.
- Fixed-size MIDI events, bounded queues, device-clock to audio-frame mapping, and sample offsets within a block.
- Generic Standard MIDI and MPE configuration, including zones, member channels, bend ranges, validation, reset behaviour, and diagnostics.
- A plugin-independent rendered-source contract that converts an external renderer's main bus into canonical mono PCM.
- Mono take recording, WAV reading/writing, prepared-track playback, mixing, and track metadata.

The application and GUI own:

- Audio and MIDI source-assignment dialogs.
- User choices such as grouping Inputs 3 and 4, choosing Standard MIDI or MPE, selecting a plugin, and including a source in My Send.
- Persistence and restoration of those choices.
- Coordinating source changes with recording and session state.
- Displaying source layouts, meters, plugin state, and errors.

The plugin-host subsystem owns only VST3-specific behaviour:

- Scanning, loading, unloading, blacklisting, and restoring local VST3 instances.
- The private worker process and fixed-capacity shared-memory protocol.
- Converting Jam2 Core MIDI/MPE events into VST3 events, note IDs, and supported note-expression parameters.
- Plugin bus negotiation, processing, state, editor windows, deadline handling, and crash recovery.
- Publishing the worker's completed main output bus through the bridge, which converts it into a canonical mono source understood by Jam2 Core.

This produces one common engine model:

```text
physical channel(s) -> optional effect worker -----+
                                                   |
MIDI/MPE device -> instrument worker -> PCM -------+-> rendered input sources
                                                   -> canonical mono conversion
                                                   -> record / My Send / monitor
unprocessed physical source -----------------------+
```

Jam2 Core must not include Steinberg SDK types or know that a rendered source came from VST3. Conversely, the worker must not own physical MIDI devices or Jam2's audio device. The main process keeps those devices open, timestamps events against Jam2's audio timeline, and can restart a failed instrument worker without disconnecting the controller.

Track View remains mono in this plan. Its storage, processing, recording, playback, cache, and synchronisation paths continue using the existing canonical mono asset model. Preserved stereo tracks are documented later only as a separate possible extension and are not a dependency of plugin support.

## GUI and Source Assignment Model

The GUI should treat local sound as a small set of assigned **Audio** and **MIDI** sources that are mixed into the existing single outgoing **My Send** stream. It should not make the user choose Audio or MIDI as one mutually exclusive global input.

The compact performance control remains simple:

```text
YOU / MY SEND

Inputs: [Audio - 4] [MIDI - 2]
[combined send meter]

Send       ---------------- 0.0 dB
Monitor    ---------------- 0.0 dB
```

The Audio and MIDI buttons open separate assignment popups or drawers. Per-source controls live in these popups, while the existing performance controls remain the master send and monitor levels. The monitor is not an independent source mix: it taps the final mono My Send signal and applies only a monitor-local enable/level after the tap, without changing what peers receive.

### Audio popup

The Audio popup shows the one audio device already opened by Jam2 and lists every input channel selected in the engine's Audio settings. It does not open an unrelated second audio device. Each selected channel must be assigned as a mono source or grouped explicitly with another selected channel as a stereo input group for joint plugin processing. Both produce a mono Jam2 source.

```text
AUDIO INPUTS

Device: Focusrite USB ASIO
Engine channels: 1, 2, 3, 4                 [Audio Settings]

Input 1       Mono    No effect
Input 2       Mono    Neural DSP
Inputs 3 + 4  Stereo input  Stereo Effect -> Mono

[Group inputs as stereo...]
```

Each source exposes:

- Its authoritative physical input number, or ordered left/right input numbers for a stereo input group.
- The negotiated plugin layout, such as `Mono -> Mono`, `Mono -> Stereo`, `Duplicated mono -> Stereo`, or `Stereo -> Stereo`.
- The fixed `Jam2 output: Mono` conversion shown whenever the plugin bus itself is stereo.
- Zero or one effect slot, enforcing the one-plugin-per-source constraint, with **Add**, **Open**, **Bypass**, **Replace**, and **Remove** actions.
- A numeric source level and meter.
- An **Include in My Send** toggle.

Jam2 must not guess semantic names such as `Microphone`, `Guitar`, or `Keyboard`; it only knows the selected physical input numbers. An optional user-created alias could be considered later, but routing, persistence, and recording selection must work entirely with labels such as `Input 1` and `Inputs 3 + 4`.

There is no per-source **Monitor locally** toggle. Enabling Jam2's master self-monitor monitors every source currently included in My Send, after the final mono downmix. Disabling it changes only local playback and does not mute the peer send. Hardware direct monitoring remains an interface-level choice outside Jam2 and may cause the user to hear an additional dry signal that Jam2 cannot control.

Selecting **Group inputs as stereo** opens a focused popup containing left and right selectors populated from the engine's selected channels:

```text
CREATE STEREO INPUT

Left channel:  [Input 3]
Right channel: [Input 4]

                         [Cancel] [Create]
```

The grouping rules are:

- A physical channel belongs to at most one source group.
- A mono source contains one physical input channel.
- A stereo input group contains an explicitly ordered left/right pair.
- Pairing removes the two independent mono assignments.
- Unpairing restores two unassigned mono channels.
- Left and right can be swapped explicitly.
- One plugin instance processes the stereo input group; Jam2 must not instantiate it independently for each side.

This supports arbitrary selected-channel sets, for example inputs 1 and 2 as separate mono sources while inputs 3 and 4 form one stereo plugin input. All three resulting Jam2 sources are mono after processing.

### Plugin bus-layout negotiation and mono boundary

The source first declares its physical input layout, then Jam2 uses the VST3 bus-arrangement interfaces to ask the plugin which main input/output layouts it supports and applies one supported configuration. This negotiation exists solely to make the plugin operate correctly; it never changes the canonical mono Jam2 source or recording format.

For a mono physical source, the outcomes are:

| Supported plugin route | Jam2 route | Jam2 source and recording |
| --- | --- | --- |
| Mono input -> mono output | Pass one input channel directly | Mono directly |
| Mono input -> stereo output | Pass one input channel, then downmix plugin L/R | Mono |
| Stereo input -> stereo output only | Duplicate the mono source into plugin L/R, then downmix | Mono |

For a mono physical source, **Automatic** should prefer a direct mono-input/mono-output arrangement when the plugin supports it, then mono-input/stereo-output, then duplicated-mono stereo input as a compatibility fallback. A stereo input group should prefer a stereo-input arrangement. The source card shows the selected plugin route and its fixed mono Jam2 output so no stereo result is implied.

```text
Input 2
Effect: Neural DSP                    [Open] [Bypass]
Plugin I/O: Automatic - Mono -> Stereo
Jam2 output: Mono
```

Once chosen, this is a known routing configuration rather than runtime signal detection. With no plugin, a mono physical source passes directly while a stereo input group is averaged to mono. Instrument plugins normally have no audio input; Jam2 uses their main mono output directly or deterministically downmixes their main stereo output.

### MIDI popup

The MIDI popup lists MIDI devices independently from physical audio channels. Each assignment chooses Standard MIDI or MPE and an instrument plugin:

```text
MIDI INPUTS

ROLI Seaboard   MPE            Equator2        [Open] [Mute]
Roland e-kit    Standard MIDI  Drum instrument [Open] [Mute]

[Add MIDI input...]
```

Each MIDI source exposes:

- MIDI device.
- Standard MIDI or MPE mode, remembered per assignment.
- Instrument plugin.
- Source level and output meter.
- **Include in My Send** toggle.
- Instrument **Open**, **Mute/Enable**, **Replace**, and **Remove** actions.

An expandable MPE section may expose lower/upper zone, global channel, member channels, and pitch-bend range. Normal use should preserve the controller and plugin's configuration without requiring those controls.

Audio and MIDI devices operate concurrently. For example, two independent audio inputs and an electronic drum kit can all be active and mixed locally into the same outgoing peer stream.

### Recording GUI

Keep the existing top-level recording-source choice:

```text
[Input] [Jam Mix] [System Loopback]
```

When **Input** is selected, reveal an additional **Input source** tile rather than adding a top-level tile for every device:

```text
Input source: [Input 2 - Neural DSP]
```

The selector lists assigned sources rather than raw devices:

- Each mono audio source.
- Each stereo input group, represented by its resulting mono Jam2 source.
- Each MIDI instrument output, whether its source uses Standard MIDI or MPE.
- The combined final mono My Send mix.

Every selector entry records canonical mono audio, so the UI does not ask the user to choose a recording layout:

```text
Input 2 - Neural DSP
Input 1 - effect bypassed
Inputs 3+4 - Stereo Effect -> Mono
Equator2 - ROLI Seaboard
```

The plugin bus can still be mono or stereo internally, but Jam2 downmixes it before the recording tap. The Track View WAV, self-monitor, and peer send therefore use the same mono source signal.

Recording always captures the source output exactly as it is currently configured and heard. There is no separate dry/wet recording switch. To record dry, bypass or remove the source effect before starting; to record wet, leave it active. The Audio popup must therefore keep **Open Plugin** and **Bypass** quick and visible, and the MIDI popup must keep **Open Plugin** and **Mute/Enable** equally accessible.

Bypass may remain available during recording because both wet and dry paths end at the same mono boundary. Source regrouping and plugin replacement should still be disabled until the take stops so the identity and processing path of the recorded source cannot change mid-take.

MIDI instrument sources expose only their rendered audio because Jam2 does not retain their event stream. The term **Input source** is preferable to **Device** here because the audio device is selected globally and a MIDI assignment includes both a controller and instrument plugin.

## Hosting-Layer Boundary

Plugin hosting is implemented directly against the maintained Steinberg VST3
SDK sources. JUCE is not part of Jam2's audio engine or plugin boundary.

Jam2 continues to own:

- Audio-device and ASIO setup.
- Device callback and block size.
- Audio clocks and timing.
- MIDI-device capture, timestamping, Standard MIDI/MPE source configuration, and bounded event queues.
- Mono sources, stereo pre-plugin input grouping, deterministic mono conversion, and rendered-source routing.
- Capture and playback ring buffers.
- Resampling and drift correction.
- Network packetisation and direct-mesh transport.
- Canonical mono track recording and playback, with application code coordinating caching, hashing, and synchronisation.

The narrow native VST3 hosting layer provides the plugin-facing facilities:

- VST3 discovery and instantiation.
- Plugin processor/instance hosting.
- Audio/MIDI block delivery.
- Plugin parameter and state access when needed locally.
- Plugin editor creation.
- Plugin scanning and known-plugin/blacklist support.
- Translation from Jam2 Core MIDI/MPE events into VST3 events and note expression.

Jam2 continues to use its own existing audio-device callback and does not adopt
another framework's device manager merely to host plugins.

The live path should remain conceptually:

```text
Jam2 device callback
    -> Jam2 Core source router
    -> publish a preallocated block to the plugin worker
    -> consume the worker's previously completed block
    -> existing Jam2 monitor/network/record path
```

When no plugin is active, the callback should take the existing direct path without passing through the hosting layer. The selected SDK may affect build time and executable size, but inactive plugin support should have effectively no runtime audio-path cost.

### Required desktop platforms and plugin formats

Plugin support must ship on both Windows and macOS. It should not be designed as a Windows implementation with macOS deferred indefinitely.

VST3 is the sole planned plugin format because it is common on both platforms and covers the named Neural DSP and Equator2 use cases. Jam2 does not plan to host Audio Unit, VST2, AAX, LV2, LADSPA, or ARA plugins. A single VST3 implementation keeps discovery, validation, processing, MIDI/MPE translation, state, safety policy, and diagnostics behind one boundary.

The same plugin-facing model still requires platform-specific module loading and editor-window attachment:

- Windows loads the matching VST3 DLL/bundle architecture and attaches the plugin editor to a Win32 window owned from the Qt application.
- macOS loads the VST3 Mach-O bundle, calls the required bundle entry/exit lifecycle, and attaches the plugin editor to a Cocoa window owned from the Qt application.
- macOS builds must support the architectures Jam2 distributes, with Apple Silicon required and Intel considered according to Jam2's supported Mac builds. Each helper process can load only a compatible plugin binary architecture.
- Plugin scanning, editor resizing, DPI/Retina scaling, focus, modal windows, and shutdown order require validation on both platforms.

Supporting one format strengthens the case for a focused native implementation using Steinberg's cross-platform hosting examples. Jam2 must maintain the Win32 and Cocoa editor bridges and platform lifecycle differences, but avoids carrying a multi-format abstraction primarily intended to normalise formats that Jam2 does not support.

On macOS, a signed/notarized process that loads third-party plugins from other developers may require the Hardened Runtime `com.apple.security.cs.disable-library-validation` entitlement. Prefer placing that entitlement on a dedicated plugin-host helper rather than the main Jam2 process, subject to validation against Apple's signing and notarization requirements.

## Buffers, Channels, and Latency

The hosting layer does not choose Jam2's device buffer size or inherently add an audio block of buffering. Jam2 supplies the existing callback block directly to the plugin.

All bridge buffers must be allocated when configuring the device or plugin. The audio callback must not allocate, log, lock, throw, scan plugins, open windows, or perform file operations.

Prefer plugins that accept Jam2's actual small and potentially varying callback sizes. Do not silently collect 32- or 64-sample device callbacks into a large 256- or 512-sample block merely to accommodate a plugin, because that would add substantial one-way latency. A plugin that cannot operate with the live block contract should be reported as incompatible with Jam2's live-source use.

At 48 kHz, one block represents approximately:

| Samples | Duration |
| ---: | ---: |
| 32 | 0.67 ms |
| 64 | 1.33 ms |
| 128 | 2.67 ms |
| 256 | 5.33 ms |
| 512 | 10.67 ms |

Plugins can add their own reported latency through lookahead, oversampling, convolution, or internal buffering. Jam2 cannot compensate for live input latency by sending audio earlier. The software self-monitor and network sender consume the same final mono My Send buffer so the performer and peers hear the same source mix and downmix. A separate monitor level may be applied only after this shared tap and must not affect the network send.

Many instrument, drum, and guitar plugins produce stereo output while Jam2's current live network audio is mono. The processing policy should be:

- Accept mono physical input where appropriate.
- Preserve an explicitly grouped stereo physical input until after its plugin.
- Accept a plugin's main mono or stereo output bus.
- Convert stereo plugin output deterministically with `mono = (left + right) / 2`.
- Use that canonical mono result for self-monitoring, recording, My Send, Track View, and Track Sync.
- Apply the same mono boundary to instrument plugins and bypassed stereo input groups.

The averaging rule avoids a correlated stereo signal doubling in amplitude. Very wide or polarity-opposed presets can partially cancel during mono conversion, but the performer hears that exact result through the shared self-monitor before sending or recording it. The source card should expose the plugin bus route and `Jam2 output: Mono` so the conversion is visible rather than hidden.

### Canonical mono recording

All initial recording paths produce mono PCM16 at the active session sample rate:

```text
physical mono input --------------------------+
stereo input group -> optional plugin --------+
MIDI/MPE -> stereo instrument plugin ---------+-> canonical mono -> PCM16 WAV
System Loopback ------------------------------+
```

The rules are:

- Physical audio recording uses the assigned source's post-plugin canonical mono output.
- MIDI instrument recording, in Standard MIDI or MPE mode, uses the instrument plugin's downmixed mono main output.
- Combined local-input recording uses the final mono My Send mix.
- System Loopback downmixes the endpoint's active channels to mono using an explicit bounded policy.
- Imported stereo WAVs are converted to canonical mono while being staged; Track View does not retain their stereo layout.
- Endpoints or files with more than two channels require an explicit speaker-mask-aware downmix or are rejected; Jam2 must not guess a pair from whichever channels contain signal.

This preserves the current mono Track View playback, asset hashing, cache, and WAV-sharing contract. There is no automatic mono/stereo recording-layout choice because the Jam2 recording result is always mono.

### Separate future consideration: preserved stereo tracks

Preserving stereo is explicitly outside the plugin implementation plan. It can be revisited later if real use demonstrates that stereo backing tracks, spatial instrument presets, room microphones, or stereo loopback recordings provide enough value to justify the additional model and UI complexity.

That would be a separate feature rather than an automatic consequence of adding plugins. A future stereo proposal would need to address all of the following together:

- Mono/stereo track metadata and a versioned Track Sync manifest.
- Validation of declared channel count against received WAV content.
- Interleaved stereo import, resampling, recording, caching, hashing, and file-size limits.
- Left/right prepared-track storage and mixing alongside mono tracks.
- Stereo waveform presentation, meters, and main-device output.
- A clear rule for whether source recording taps stereo before the canonical live-send downmix.
- Stereo System Loopback and an explicit multichannel endpoint downmix policy.
- Approximately doubled storage and Track Sync transfer cost for PCM16 stereo assets.
- Compatibility behaviour for older mono-only Jam2 peers and saved projects.

Until that separate proposal is deliberately selected and implemented end-to-end, plugin output, input recording, Track View, Track Sync, imports, and System Loopback remain canonical mono. Plugin workers may still use stereo buses internally because many VST3 plugins require them; that internal compatibility does not imply stereo Jam2 assets.

## Plugin Editors

Users should be able to click **Open Plugin** and use the plugin's normal interface, including a Neural DSP or Equator2 editor, presets, licence dialogs, and sound controls.

The plugin host should open the plugin's native VST3 editor as a separate top-level window rather than embedding it into a Qt page. If runtime processing is isolated in a helper process, that helper also owns the plugin editor window:

```text
Jam2 Qt window
    -> Open Plugin command to helper

helper-owned plugin editor window
    -> plugin's complete native VST3 interface
```

The host must manage window ownership, editor creation/destruction, resizing, DPI/Retina scaling, focus, auxiliary/modal windows, and shutdown order on Win32 and Cocoa. All editor operations occur on the helper's GUI/message thread and remain separate from real-time audio processing.

Plugin state is retained locally only to restore configured Audio and MIDI sources. Track View contains completed audio and does not retain or load a track plugin state. Plugin state is never loaded automatically from peer-controlled data.

## Safety, Isolation, and Real-Time Reliability

### Asynchronous application workflow

Only user choices and final atomic source attachment run on the Qt GUI thread.
MIDI device enumeration, MIDI device opening, isolated VST3 probing, component
initialization, and worker startup run through background jobs. Removing or
replacing a plugin immediately detaches/signals its worker and uses timed,
asynchronous terminate/kill escalation for a hung process; no interactive
action waits for third-party code to exit.

Inside each helper, plugin audio processing runs on its own high-priority
thread. Native editor creation and message pumping stay on the helper's main
thread, so opening an editor cannot make native window traffic stall the audio
transport. The main Jam2 process never loads a plugin module.

The bridge consumes only the response belonging to the immediately expected
audio block. A late response is discarded rather than inserted later in the
stream. Audio effects use latency-aligned dry fallback, instruments use
silence, and transitions are briefly ramped to avoid a discontinuity when a
worker misses a deadline.

A loaded VST3 plugin is third-party native code. It can allocate, lock, block, produce invalid samples, overrun the callback deadline, hang, corrupt memory, or crash its host process. Using only VST3 gives Jam2 one boundary to harden, but the format itself does not provide a sandbox.

Ordinary C++ `try`/`catch` handles only C++ exceptions thrown across the call. It does not safely contain access violations, signals, stack corruption, deadlocks, or prior memory corruption. Windows structured exception handling can observe some hardware faults, but continuing the main process after an arbitrary plugin fault is not a reliable safety boundary; macOS signal recovery has the same fundamental problem. Exception handling is useful for reporting a cooperative plugin failure, not for guaranteeing Jam2 survives native-code failure.

Use two levels of process separation:

1. **Scanner process:** plugin discovery and validation always occur outside Jam2. A scan crash or hang records the exact plugin as failed/blacklisted and cannot stop a jam.
2. **Runtime plugin helper:** every active plugin loads and processes outside the main Jam2 GUI/network/audio-device process.

The runtime helper uses preallocated shared memory with fixed-capacity audio and MIDI slots, monotonic sequence counters, and no serialization or heap allocation in either real-time callback. Its two-block pipeline produces block `N` for consumption during callback `N+2`; this gives Windows enough scheduling margin at a 32-frame callback without making the Jam2 callback wait:

| Device block at 48 kHz | Isolation latency |
| ---: | ---: |
| 32 samples | 1.33 ms |
| 64 samples | 2.67 ms |
| 128 samples | 5.33 ms |

Do not make the Jam2 audio callback synchronously wait on an IPC round trip; that would convert helper scheduling jitter into callback underruns. The helper should receive appropriate real-time scheduling priority, but Jam2 still consumes only output already marked complete.

Failure behaviour is source-specific:

- An audio-effect helper crash or missed output deadline switches to a dry path delayed by the same two-block isolation latency, avoiding a timing jump.
- An instrument helper crash produces silence for that instrument source, sends note resets when restarted, and leaves other physical and MIDI sources active.
- Jam2 records the failure, disables automatic immediate reload loops, and offers an explicit restart.
- Invalid NaN/infinite or runaway output is rejected before it enters monitoring, recording, or network capture.

A helper per active source plugin is the selected runtime model: because a source can own at most one plugin, a worker maps directly to one source and one plugin instance. One plugin failure therefore affects only its source. Do not place every plugin in one shared helper, because one faulty plugin could then stop all plugin sources. Jam2 expects only a small number of local sources, so the stronger per-plugin containment is worth the additional process overhead.

Do not include an in-process mode in the initial implementation. It could remove the two-block isolation latency, but a plugin fault could terminate Jam2 and exception handlers cannot make that boundary safe. Reconsider it only if measured playing tests demonstrate that the deterministic isolated latency is materially problematic; any later option must be labelled explicitly as lower isolation.

Additional safeguards should include:

- Scan plugins outside the main Jam2 process without exception.
- Maintain a known-plugin list and crash/scan blacklist.
- Provide a startup safe mode that skips plugin loading.
- Keep a prepared, latency-aligned dry bypass path for live effects.
- Validate channel counts and reported layouts before activation.
- Sanitize NaN and infinite output and prevent runaway levels.
- Detect processing deadline misses and report them outside the callback.
- Avoid automatically loading a plugin or state supplied by a peer.


## Required Measurements

Plugin support must preserve Jam2's rule that tuning and failure data are inspectable. At minimum expose:

- Plugin name, format, and active/bypassed state.
- Plugin-reported latency in samples and milliseconds.
- Current, average, and maximum plugin processing time.
- Percentage of the callback deadline consumed.
- Plugin processing deadline misses/overruns.
- Automatic bypass or invalid-output count.
- Helper process restarts/crashes and last failing plugin identity.
- Shared-memory input/output sequence depth and missed helper blocks.
- Fixed isolation latency in samples and milliseconds.
- MIDI/MPE queue depth, capacity, and dropped-event count.
- Per-source physical input assignment, plugin bus layout, and canonical mono output state.
- Per-source and combined wet peak levels.
- Source grouping and the stereo-to-mono conversion point.
- Any adapter buffer depth or added latency; normally this should be zero.

Collection must remain real-time-safe, with formatting and logging performed outside the callback.

## Initial Product Scope

The smallest useful first version should favour:

- Windows and macOS VST3 hosting as one required cross-platform feature.
- Apple Silicon support and the currently supported Windows x64 build; other architectures follow Jam2's declared platform support.
- No AU, VST2, AAX, LV2, LADSPA, or ARA hosting.
- A focused Steinberg VST3 SDK implementation first; use a broader wrapper only if the cross-platform prototype demonstrates a concrete compatibility problem it solves.
- An internal plugin-host helper component with no separate public Jam2 executable or public plugin-host command.
- One isolated helper process per active plugin; no in-process plugin execution in the initial implementation.
- Separate Audio and MIDI source-assignment popups feeding one existing **My Send** stream.
- At most one live effect plugin per assigned mono source or stereo input group.
- At most one instrument plugin per MIDI source, configured as either Standard MIDI or MPE.
- One worker process per active source plugin, with no plugin chains or plugin-to-plugin routing.
- Conventional MIDI keyboards, MPE controllers, and electronic drum kits through the same bounded MIDI path; electronic drums normally use Standard MIDI mode.
- The main mono or stereo output bus of an instrument plugin, converted to canonical mono rather than routing arbitrary auxiliary buses.
- A separate native plugin editor window.
- Completed audio as the only plugin-produced artifact placed in Track View or required by peers.
- Fast, visible **Open Plugin** plus audio-effect **Bypass** or instrument **Mute/Enable** controls.
- Recording of the selected source's current post-plugin/bypass output without a separate dry/wet tap selector.
- Canonical mono recording for every Audio, MIDI instrument, combined-input, imported-WAV, and System Loopback path.
- Wet-audio-only recording for MIDI instrument performances in either Standard MIDI or MPE mode.
- Existing mono Track View assets and Track Sync, without making preserved stereo tracks a dependency of plugin hosting.

Explicitly avoid initially:

- Any serial or parallel plugin chain, plugin graph, send effect, or plugin-to-plugin routing.
- Sidechains and multiple plugin buses.
- Routing separate kick, snare, room, or other auxiliary outputs from a drum plugin.
- Plugins attached directly to Track View tracks.
- Track plugin chains, states, previews, offline renders, or wet-render revision subsystems.
- Plugin automation lanes.
- Editable MIDI or MPE clips.
- Cross-peer plugin or preset synchronisation.
- Automatic acquisition or installation of plugins.
- Loading a peer-selected plugin or untrusted state blob.
- Replacing Jam2's audio-device engine with JUCE.
- Treating C++ exceptions, Windows structured exceptions, or macOS signals as a substitute for process isolation.
- Preserved stereo Track View assets, stereo Track Sync, or stereo System Loopback; these require a separately approved end-to-end feature.

## Suggested Implementation Order

1. Build equivalent Windows and macOS prototypes directly with the Steinberg VST3 SDK for discovery, loading, editor display, state, MIDI/MPE, binary size, build time, and signing/notarization impact. Consider JUCE only if this exposes a concrete compatibility or maintenance problem.
2. Implement the selected per-plugin runtime helper model using a two-block shared-memory pipeline and validate scheduling reliability and isolation latency at 32/64/128 samples. Do not add in-process execution unless later playing tests establish a concrete need.
3. Add the plugin-independent source model to Jam2 Core: independent mono inputs, explicit stereo pre-plugin input grouping, deterministic post-plugin mono conversion, canonical mono rendered-source routing, and the existing direct no-plugin path.
4. Add the Audio popup with independent mono sources, explicit stereo input grouping, one effect slot per source, visible plugin bus routing, and fixed `Jam2 output: Mono` behaviour.
5. Add latency/CPU/IPC measurements, helper failure recovery, and automatic latency-aligned dry bypass safeguards for live source effects.
6. Add MIDI device capture, bounded event queues, clock mapping, Standard MIDI/MPE configuration, resets, and diagnostics to Jam2 Core; add application/GUI assignment separately.
7. Deliver Core's preserved channels and expression messages through the VST3 worker and validate MPE performance with Equator2.
8. Validate electronic drum input including velocity, hi-hat controllers, choke/pressure messages, and a drum plugin's main output.
9. Downmix Standard MIDI and MPE instrument output to canonical mono and send it through the existing shared monitor/network path.
10. Add assigned-source selection under the existing Input recording mode, with every choice recording the same canonical mono signal used by Jam2.
11. Validate canonical mono input recording, imported-WAV conversion, System Loopback, Track View playback, cache, and Track Sync without adding a stereo asset path.

Live input plugins should be activated only after the source router, mono conversion, block adapter, latency reporting, helper failure path, and latency-aligned bypass have been exercised. Preserved stereo tracks may be reconsidered independently later and must not be bundled into plugin implementation.

## Licensing and Distribution

Use the current MIT-licensed Steinberg VST3 SDK and retain its required licence and copyright notices. If a broader wrapper such as JUCE is reconsidered later, verify its licence against Jam2's GPLv3 distribution model before adoption. Do not redistribute third-party plugins; Jam2 discovers plugins installed and licensed by the user.

Relevant upstream references:

- [Steinberg VST3 MPE host interface](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IVst3WrapperMPESupport.html)
- [Steinberg VST3 EditorHost](https://steinbergmedia.github.io/vst3_dev_portal/pages/What%2Bis%2Bthe%2BVST%2B3%2BSDK/EditorHost.html)
- [Steinberg VST3 plugin locations](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Plugin%2BLocations.html)
- [Apple disable-library-validation entitlement](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.cs.disable-library-validation)
- [ROLI MPE explanation](https://support.roli.com/en/support/solutions/articles/36000027933)
- [ROLI Equator2](https://roli.com/product/equator2)

## Summary Decision

Plugin support should be an optional local processing layer at Jam2's input boundary. The engine's selected physical channels become independent mono sources or explicit stereo pre-plugin input groups. MIDI devices become instrument sources configured in either Standard MIDI or MPE mode; electronic drums are ordinarily Standard MIDI devices rather than a third source category. A plugin may use a mono or stereo main bus internally, but every result is deterministically converted to canonical mono before entering Jam2 monitoring, recording, Track View, Track Sync, or **My Send**.

The feature must behave consistently on Windows and macOS, using VST3 as the sole plugin format and the Steinberg SDK as the initial implementation path. AU is deliberately out of scope.

Recording captures the current canonical mono output of the selected assigned source. The user chooses the source and never chooses mono or stereo. Fast **Open Plugin**, audio-effect **Bypass**, and instrument **Mute/Enable** controls let the user establish the desired sound before recording. MIDI/controller data and alternate dry/wet versions are not kept.

Track View does not host plugins or plugin state. It receives and synchronises completed canonical mono audio using the existing mono asset model. Preserved stereo Track View, Track Sync, imports, and System Loopback are a separate future consideration that must be deliberately justified and implemented end-to-end; they are not part of this plugin plan. Jam2 remains responsible for all device, buffering, resampling, networking, recording, cache, and synchronisation behaviour.

Safety comes from a narrow VST3-only contract plus process separation, not from exception handling alone. Each active plugin runs in its own internal helper and exchanges fixed audio/MIDI blocks through bounded shared memory. This adds two known device blocks of latency, giving the worker a reliable scheduling deadline at a 32-frame Windows callback, while preventing an ordinary plugin-process crash or hang from taking down the main Jam2 jam/network process. In-process plugin execution is deliberately excluded unless later measured playing tests justify accepting its lower isolation.
