# Jam2 macOS Test Completion Handoff

Windows completion of the native test initiative does not complete macOS. When
Codex is next run on a macOS endpoint, it must read `AGENTS.md`,
`AGENT-TESTING.md`, `TEST-PLAN.md`, `TEST-LOG.md`, and `TEST-REVIEW.md` completely
before making changes.

## Required macOS completion work

1. Inspect the final Windows test architecture and all platform branches added
   under production and `tests/`; do not redesign shared behavior merely for
   macOS.
2. Configure a test-free Apple build with `bash ./compile.sh` and verify the one
   staged `release/Jam2.app` launches normally.
3. Build and run every non-hardware selected suite, then `--tests-full`, using
   Apple Clang, Qt 6, CoreAudio, CoreMIDI, and the same four-peer topology.
4. Validate both hidden GUI operation and `--show-gui`, including normal
   `MainWindow` construction, dialogs, view snapshots, and clean process exit.
   Export the maintained GUI-control inventory and require the same stable
   contract checks as Windows: every initial widget and custom-painted target
   classified, zero duplicate IDs, and representative BeatGrid, looper,
   Performance Home, and metronome-pattern invocations reflected in both
   control state and the content snapshot. Recheck the final inventory count
   after all transient-dialog work is complete; do not hard-code the interim
   497 count if later Windows slices legitimately add modal targets.
   Run `jam2_four_gui_modal_integration` and confirm the private asynchronous
   opener enters each real native dialog before inventory, then prove Local
   Engine/Start/Join/Jam Sync cancel semantics and clean nested-event-loop
   shutdown under Cocoa. Preserve the strict response-ID ordering that exposed
   Windows BUG-T035.
   Open the extracted `JamSyncDialog` from both a creator and peer 3 in a live
   four-peer jam. Change every field through its real controls, click Apply,
   and require exactly one authoritative revision and exact convergence after
   each proposal. Prove disabling/restoring Track Lanes and Global Playback
   disables/re-enables automatic WAV and recording choices, and that Leader
   Audio disables only metronome-state while Apply remains usable. During the
   later real shared lane-recording workflow, reopen Jam Sync and require all
   six policy fields plus Apply to be disabled while Cancel remains enabled.
   Open Settings and require all 667 currently observed live interactive
   candidates classified with zero duplicate IDs. Edit/cancel and then Save/
   reopen/restore startup BPM, metronome compensation deadband, automatic WAV
   sync, and recording count-in on all four processes. Do not reintroduce the
   generic QListWidget adapter that caused Windows BUG-T040; page-owned fields
   are directly addressable. Run hardware-only audio controls only with the
   explicit Apple hardware profile.
   Switch the metronome to listener-compensated and open Advanced Listener
   Compensation through the extracted `ListenerCompensationDialog`. Require
   its maximum, smoothing, deadband, slew, Apply, and
   Cancel controls to be classified as modal. On four isolated processes prove
   Cancel, Apply, reopen persistence, and restoration. During a running
   four-peer fake-audio jam, change all four values on one listener and require
   the network supervisor's published effective values to change only on that
   peer; restore them before shutdown. Run the pure maximum/smoothing/deadband/
   two-direction-slew boundary and reject NaN/non-finite values. This is Apple
   parity for Windows BUG-P025 through BUG-P027 and BUG-T041.
   Open the extracted Arrangement editor and require its table, Add, Remove,
   Up, Down, Loop, Save, Save + Start/Stop, Cancel, and parameterized row
   editors to be classified as modal. In four isolated GUI processes repeat
   exact Cancel, Save/reopen, Add-then-Remove-without-editing, row ordering,
   Start, Stop, and restoration. Confirm native Cocoa focus/mouse delivery
   selects the owning row for embedded combos/spinboxes; this is parity for
   Windows BUG-P028. In a real four-peer fake-audio jam, save B x2 / C x3 with
   loop off as creator, require all four private content snapshots to converge,
   then clear/restore as peer 4 and require convergence again.
   Open the extracted `LaneRecordingDialog` and require its complete current
   16-control inventory, with no orphaned or hidden `MainWindow` backing
   widgets. Prove Input, Current Jam, and System Loopback retain independent
   output/advanced drafts while switching modes; Cancel must not change the
   engine's manual latency adjustment, capture mode, source, or lane-arm state,
   and accepting Input must update only the initiating process. Repeat the
   Remove Lane/WAV confirmation inventory and disabled Delete-WAV behavior for
   an empty lane. This is Apple parity for Windows BUG-P030, BUG-P031, and
   BUG-T044 through BUG-T046.
   Run the genuine shared-lane recording path with exactly four isolated GUI
   processes and deterministic fake input. Add and identify the actual appended
   lane rather than assuming lane zero or an empty initial arrangement; arm it
   through the real dialog, enter one shared recording group, and require the
   same group/participant/isolation/protection state on all four peers. While
   active, open Jam Sync on a different peer and require all policy fields and
   Apply disabled while Cancel remains enabled. Occupy both bounded file
   workers before stopping, require a visible non-zero import retry count and
   zero import failures, then require the same lane ID, SHA-256, non-header WAV
   bytes, available file, and idle recording state on all four roots. Remove
   the appended lane and restore the exact initial lane-ID set. This is Apple
   parity for Windows BUG-P032, BUG-P033, BUG-P034, and BUG-T047. Exercise real
   CoreAudio input and system-loopback sources only under the explicit hardware
   profile; fake audio remains the deterministic distribution gate.
   Run the painted Add Lane -> WAV drop -> Share Now -> confirmed Remove WAV
   workflow. Require local-only painted availability before share, exact bytes
   and painted availability on all four peers after share, then equal lane
   identity/name/mix state with no hash or painted WAV after removal. This is
   explicit Apple parity for Windows BUG-P022; retain the same-ID/different-WAV
   and local-only/reference merge boundaries.
   Also remove a painted WAV while its sender is paused at outgoing validation.
   Require zero new request-start timeouts, immediate supersession and existing-
   message acknowledgement of the older batch, hash-scoped sender cleanup,
   unchanged post-import lane name/identity, no partial files, and no painted
   WAV on any peer. This is explicit Apple parity for Windows BUG-P023.
   Repeat removal with a receiver paused at incoming finalization after chunk
   writes, requiring deletion of every private partial file. Then replace a
   source WAV with different bytes while its old hash is paused at outgoing
   validation. Require old work to clear within three seconds, the new hash to
   be the sole exact/painted asset on all peers, and preservation when another
   lane still references a hash. This is Apple parity for Windows BUG-P024.
   Force Windows BUG-P029 independently: after same-byte WAV removal, retain
   only the selected source's disposable managed copy, validating each deleted
   non-source path is beneath its isolated temporary peer root with case-
   sensitive Apple path semantics. Arm that source's outgoing-validation pause
   for three seconds, require it to become active, invoke real Share Now, and
   require exact bytes plus idle transfer state on all four peers. This also
   validates the corrected BUG-T043 fixture assumptions; never delete product
   storage or files outside coordinator-created temporary roots.
   Open the extracted `StartJamDialog` and `JoinJamDialog` on four Cocoa GUI
   processes and require their complete live field/action inventories with zero
   duplicates. Mutate Start bind, port, sample rate, create profile, and the
   profile-driven buffer; Cancel and require exact baseline restoration. Mutate
   Join invite, bind, port, join profile, and buffer; Cancel and require only
   the invite retained per REVIEW-001. Then reserve four distinct TCP/UDP
   loopback pairs, start one creator through the real Start button and three
   fake-audio joiners through the real Join button, require three active remotes
   and non-zero callbacks on every process, and leave through the public action.
   Confirm the session page has no hidden modal backing widgets and runtime
   state remains typed. This is Apple parity for Windows BUG-P035 and BUG-T049
   through BUG-T052. Under an explicit CoreAudio hardware profile, separately
   prove an actually unsupported session rate is rejected; headless fake audio
   must never preflight an unrelated physical device.
5. Audit inherited file-descriptor ownership, close-on-exec behavior, process
   launch/termination, signal handling, temporary paths, app-bundle helper
   lookup, and automation disconnect behavior. In particular, prove that each
   of the four coordinator children inherits only its own command-read and
   event-write descriptors and that closing one peer cannot keep another
   peer's pipe alive.
6. Validate the fake audio backend, per-stem capture, WAV assertions, virtual
   network, and real UDP impairment proxy against macOS scheduling and socket
   semantics. Prove the shared four-pair loopback reservation owns distinct
   TCP/UDP ports until child readiness and releases them before create/join.
   Force at least one rejected candidate during an allocator test and prove a
   later distinct candidate is selected rather than retrying one port forever.
   Run all three metronome modes under clean, delay, jitter, loss, duplication,
   reordering, and burst-loss conditions after the live compensation work. Do
   not relax the zero mixer-capacity-drop or 1,024-frame epoch-mapping bounds.
   Windows BUG-T042 retained two red runs where every peer simultaneously
   observed external 400..620 ms proxy/RTT scheduling excursions. The native
   coordinator now records its maximum pump gap on every pass and rejects a
   gap above 50 ms as invalid harness evidence. Confirm this measurement under
   Apple scheduling; if it trips, retain artifacts and implement an appropriate
   native QoS fix before rerunning rather than relaxing product assertions.
   Ensure any product red run prints the detailed authority/revision/mode/
   epoch/mapping/packet/callback/mixer/authentication values. For listener WAV
   alignment, also retain signed median/mean centroid error and final applied
   compensation offset, target, and averaged latency as added for Windows
   BUG-T053. Keep the 10-ms median and 50-ms maximum bounds unchanged; diagnose
   a recurring signed bias before changing either the model or its criterion.
   Also exercise a recovering PeerMixer source and verify that all active
   contributors rebase to the same live tail, re-arm a bounded prefill
   deadline, and do not enter recurring partial-block deadline releases. Check
   how Apple UDP reports destination-unreachable events and preserve their
   separation from genuine proxy receive errors.
   Run both native network validators and the complete real-socket TCP security
   matrix. In particular, open 12 simultaneous unauthenticated clients against
   the cap of eight and require exact high-water eight, active work no greater
   than eight, a non-zero visible pending-cap rejection count (including native
   accepted-socket delivery drops from Windows BUG-P021), and a complete drain.
   Prove authentication/incomplete-frame deadlines, replay, tag corruption,
   oversize, 64-attempt failed-key limiting, paired asset teardown, and
   same-token reauthentication under Apple socket scheduling. Then run the
   native four-peer UDP malformed/replay/corruption/flood matrix with the same
   packet/stat/convergence assertions as Windows.
   Retain the continuous synthetic-input master-output boundary introduced for
   Windows BUG-T029 and prove muted output remains zero before the asynchronous
   master-level command produces a nonzero complete-mix peak.
   Rerun the native WAV-transfer lifecycle matrix and exact four-peer
   shared-content jam as part of this step. Confirm wrong-peer and stale-hash
   frames preserve the current receive, owning-peer cancellation removes every
   `.partial.*` file, and multi-chunk ACK/atomic-commit behavior is identical on
   the Apple filesystem. Repeat the simultaneous same-byte import, three-way
   occupied-lane conflict, and four-way idempotent share scenario. Require one
   lane for independently added identical hashes, exact preservation/order for
   different hashes, zero residual transfer state, distinct canonical paths
   below each of four temporary roots, and exact bytes. Check the path-root
   containment assertion on Apple's case-sensitive `/` paths as explicit
   parity for Windows BUG-T019. Run the four-process interruption test at
   outgoing validation, offer, incoming chunk, and incoming finalization, and
   require exact recovery with no residual or partial files. In particular,
   prove a control disconnect removes its paired asset connection immediately
   and that rejoining with the same peer token can authenticate a fresh asset
   stream; this is Apple parity for Windows BUG-P018. Also verify the 10-second
   request-to-start deadline, same-hash source preservation, and fresh retry
   budget after source handoff. Run the expanded eleven-WAV version: removal
   during paused validation with zero stale start deadlines, removal after
   chunk writes with zero partials, different-byte replacement that promptly
   cancels old validation, one bounded dropped start, a sender leaving with at
   least two outgoing batches, all four processes leaving and creating a fresh
   jam with zeroed revision state, and source A consuming three start failures
   before source B consumes one and succeeds with a fresh retry budget. Confirm
   the request-generation guard prevents old timers from affecting retries or
   the new session, and retain exact bytes, four isolated roots, and zero
   retry-owner/partial state.
7. Do not collect or gate source coverage on macOS. `compile.sh --tests-full`
   builds and runs the complete normal Release test catalogue only; it must
   never invoke the Windows PowerShell coverage scripts or an Apple LLVM
   coverage alternative.
8. If a macOS hardware profile is available, run the explicit CoreAudio,
   CoreMIDI, and plugin extension. Never treat unavailable hardware as a pass.
9. For every failure, perform the same implement, test, self-review, gap-fix,
   and retest loop used on Windows. Append all iterations to `TEST-LOG.md` and
   add judgment-dependent items to `TEST-REVIEW.md`.

## Iteration 11 parity additions

- Build `SettingsDialog.cpp` with Apple Clang and run the complete four-process
  Settings workflow. Require semantic profile activation, dependency state,
  Cancel isolation, Save/reopen persistence, restoration, and Settings Cancel
  during a live four-peer fake-audio jam. Confirm the extracted dialog owns its
  transient controls while `MainWindow` retains persistence and live audio
  apply/rollback.
- Require each Cocoa GUI-agent process to keep its preferences and GUI logs
  below its coordinator-created storage root. Verify a Cancel-only workflow
  does not falsely require persistence and no real Application Support
  preference or configured external log folder is read or modified.
- Repeat BUG-P036 timing proof using the remaining idle interval for incoming
  and outgoing Track Sync batches under Cocoa timer scheduling.
- Repeat BUG-P037 directly and through the four-peer WAV interruption case: a
  receiver must supersede an active stream, re-request the same hash/source,
  receive a new start and chunk zero, converge on exact bytes/model/views, and
  leave no batch, worker, retry, or partial-file residue. Keep the existing
  timeout values and all wire messages unchanged.
- Verify the `jam2_tests_gui` aggregate target rebuilds every executable selected
  by the GUI CTest label so a changed source cannot execute a stale harness.

## Iteration 12 parity additions

- Build the owned `LocalEngineDialog` in `SessionStartupDialogs` with Apple
  Clang. On four Cocoa GUI processes, change sample rate, buffer size, input and
  output channels, check Save as Local defaults, Cancel, then prove Settings
  still presents every exact original value.
- With an explicit CoreAudio hardware profile, exercise Test Device and an
  accepted Local Engine result through the real controls. Require exact device
  preference matching, typed runtime values, persistence only when requested,
  and successful launch/callbacks for a supported profile.
- Exercise the no-device branch explicitly: Start closes the editor, presents
  the maintained `Perform` warning, and changes neither runtime nor persisted
  state. This is parity for self-review item BUG-T060.

## Iteration 13 parity additions

- Build `InputSourceDialogs.cpp` and the new router diagnostics with Apple
  Clang. Run the exact four-Cocoa-process Start/Join workflow with two selected
  fake inputs per peer: inventory Audio/MIDI/Plugin modals, edit include/level,
  reopen, group and ungroup, and require exact 2 -> 1 -> 2 source-slot topology,
  non-zero routed blocks/signal, and zero invalid/renderer failures on all four
  peers.
- Exercise the CoreAudio test-input callback introduced for BUG-P038 and prove
  its generated buffer passes through `InputSourceRouter` with no callback
  allocation, lock, logging, exception, or blocking. Repeat disable and 50%
  source-level checks from the native core regression.
- Under an explicit hardware profile, repeat Audio Inputs include, level,
  stereo grouping, ungrouping, and recording-lock behavior with real CoreAudio
  channels. Exercise real CoreMIDI discovery/removal and plugin editor/worker
  lifecycle separately; unavailable hardware or plugins must be reported as
  gated, never counted as a pass.
- Recheck BUG-P039 publication ordering under Apple callback scheduling while
  topology changes. Confirm the extracted dialog retains Cocoa layout,
  confirmation, modality, and control availability behavior. Do not expand the
  extraction into MIDI/plugin ownership unless their device/worker lifetime is
  first covered and the boundary remains cohesive.

## Iteration 14 parity additions

- Build `MidiInputBackend.cpp` and the MIDI portion of
  `InputSourceDialogs.cpp` with Apple Clang. Confirm the default application
  constructor still selects the system CoreMIDI backend and the private GUI
  agent alone receives the deterministic two-device synthetic backend.
- Run the exact four-Cocoa-process MIDI workflow used on Windows. On every
  peer, prove discovery Cancel suppresses late completion, configuration Cancel
  creates no source, the selected device and MPE mode reach slot 2, Standard
  mode/include/37% level persist across close/reopen, Plugins exposes the
  matching MIDI tile, removal clears all source/router state, and the other
  fake device safely reuses slot 2 before final cleanup.
- Run `jam2_midi_input_backend_units` under Apple sanitizers where available.
  Retain the exclusive-open, event validation, close/reopen, full bounded
  queue, false-on-drop, and drop-counter assertions from BUG-T067. Synthetic
  delay and injection must remain outside the real-time callback and outside
  the production CoreMIDI path.
- Verify the inherited-handle GUI channel's exact-control snapshot returns one
  classified match or explicit absence in a single Cocoa event-loop turn,
  rejects mixed cursor/control requests, and never masks duplicate IDs. Keep
  complete paginated inventory audits strict at stable UI points.
- With an explicit CoreMIDI hardware profile, repeat discovery, assignment,
  removal, and device close/reopen against a real controller. Opening still
  occurs only when an instrument is attached; complete that device/plugin
  branch with the later plugin-lifecycle slice rather than counting unavailable
  hardware or a missing VST3 as a pass.
- Rerun the complete GUI catalogue and every metronome/epoch impairment cell.
  No Iteration 14 network or metronome behavior changed, so all existing bounds
  and protocol shapes remain exact.

## Iteration 15 parity additions

- Build `InputPluginBackend.cpp` and the Plugins portion of
  `InputSourceDialogs.cpp` with Apple Clang. Confirm normal startup selects the
  system backend and retains the existing macOS VST3 directory chooser,
  isolated bounded probe/class selection, worker process, editor, renderer,
  diagnostics, and non-blocking retirement. The synthetic backend and
  `midi.inject` action must remain private to GUI automation.
- Run `jam2_input_plugin_backend_units` with the Cocoa platform plugin. Preserve
  mono/stereo deterministic audio, bypass, MIDI note/mute/reset and consumed-
  event diagnostics, editor/health/retirement, valid request-shape checks, and
  owner-destruction cancellation. Verify the Qt runtime is resolved by the
  normal macOS test environment and that both unit/plugin/GUI aggregate build
  graphs include the target.
- Repeat the exact four-process plugin workflow: audio Add/Open/Bypass/Replace,
  concurrent-load rejection, MIDI instrument Add/Open/inject/mute/Replace,
  global BYPASS, audio/MIDI Remove, router/device cleanup, and final source-slot
  reuse on every peer. Require the same raw plugin and input-router state as
  Windows, not only visible button changes.
- Repeat both delayed-completion races under Cocoa scheduling. Regrouping two
  audio inputs during a load must retire the late host without attachment;
  removing a MIDI source during instrument load/device open must leave no host,
  device, renderer, queue, or router state. Close the Plugins dialog while both
  operations are pending to prove callbacks do not dereference destroyed UI.
- Reproduce the BUG-T071 callback-order scenario with Apple Clang and confirm
  both progress copies remain valid regardless of permitted argument evaluation
  order. Exercise successful MIDI device-open attachment and every rejection
  branch; no exception may escape a queued GUI callback.
- Reproduce BUG-T072 under Cocoa deferred-delete scheduling. Exact-control
  snapshots during repeated audio and MIDI replacement must always see zero or
  one matching control, never two. Retired panels must detach synchronously and
  destruct safely after button-signal delivery.
- With an explicit real VST3/CoreMIDI profile, load one audio effect and one
  instrument, open each editor, render/inject audio or MIDI, bypass, replace,
  remove, and verify worker/device cleanup. Unavailable plugins or hardware are
  gated evidence, never automatic passes. Do not change probe limits, plugin
  formats, or add a non-isolated plugin path for parity convenience.
- Rerun the complete GUI catalogue and every unchanged-threshold metronome/
  epoch impairment cell. No Iteration 15 network or metronome behavior changed;
  protocol fields, payloads, parser/version, authentication, epoch rules, and
  all existing acceptance bounds remain exact.

## Iteration 16 artifact and focused-test parity

- Verify that CTest supplies `JAM2_TEST_ARTIFACT_ROOT`, `TEMP`, `TMP`, and
  `TMPDIR` beneath the configured `build/test-artifacts` directory to the test
  process and all four child Jam2 processes. `QTemporaryDir`, Qt's temporary
  path, and `std::filesystem::temp_directory_path()` must all resolve beneath
  this root during automated tests; normal application startup remains
  unaffected.
- Verify `compile.sh --tests <suite>` and `--tests-full` clear only the exact
  `build/test-artifacts` tree before testing, remove it on complete success,
  and retain it on any CTest failure. Retained artifacts must be
  removed by the next test invocation. Do not use or clean the user's global
  macOS temporary directory as part of this lifecycle.
- Confirm an exact `--test-name` remains a selector within its requested suite
  and does not accidentally intersect the suite's CTest label expression.
- Build and run `jam2_core_boundary_units` with Apple Clang. Preserve the local
  UDP/STUN-only fixtures, moved-resource checks, exact replacement-endpoint
  fan-out, ring/MIDI/downmix boundaries, and session close behavior. Resolve
  platform-specific socket error differences in the fixture without weakening
  the production error classification or changing any network protocol shape.

## Iteration 17 CLI parity

- Build and run `jam2_cli_boundary_units` with Apple Clang and the staged app
  bundle executable. Preserve the direct help/dispatch, channel-selection,
  key-redaction, stats-copy/format, platform-error, and detached-playback-sink
  contracts.
- Run the 150-ms synthetic local-audio recording path without CoreAudio
  hardware. Require a normal exit, nonzero written frames, valid JSON, the
  expected WAV format and test-input metadata, and build-local temporary state.
  Resolve the bundle executable path through CMake; do not create a second
  Jam2 binary or bundle for this test.
- Keep `CliRuntimeSupport.hpp` platform-neutral and verify detached calls stay
  no-ops after the engine lifetime boundary. This test must not weaken or mock
  the actual CoreAudio hardware-profile tests required later.

## Iteration 18 application-infrastructure parity

- Run `jam2_application_boundary_units` under Apple Clang. Its POSIX pipe path
  must prove the same automation queue capacity/high-water/rejection,
  non-draining cleanup, malformed-frame count, and one-shot disconnect report
  as the Windows HANDLE path.
- Repeat numeric loopback TCP reservation, collision, error, close/destructor,
  and listener ephemeral-port publication. Account for POSIX bind semantics in
  the fixture without weakening exclusive reservation ownership.
- Preserve the real synthetic Engine start/reuse/restart proof and fake only
  the bounded network worker used to expose peer-gain queue validation. This
  unit is not a substitute for the full real four-peer Cocoa runtime cases.
- Require fixed 32-hex-character peer tokens that decode to nonzero typed peer
  IDs; do not change token generation or authentication protocol for parity.

## Iteration 19 diagnostic and corpus parity

- Run `jam2_debug_entrypoint_units` against the staged bundle executable. All
  three help surfaces, five invalid parser inputs, five encoder-produced valid
  inputs, and unknown-target rejection must match Windows classifications.
- Repeat the real synthetic local-network-local lifecycle. Require one Engine
  start, zero restarts, two reuses, and a strictly advancing frame clock across
  both transitions; this opens no CoreAudio hardware.
- Run the fixed four-bar, two-samples-per-cell `funk_static_pocket` corpus case.
  Require six samples, two audio mixes, parsed corpus JSON, and at least the
  expected full-mix/drum WAV pair per rendered sample. Keep the generator
  cohesive; do not split it because this diagnostic exercises a large file.
- Confirm all temporary fuzz inputs, manifests, corpus JSON, and rendered WAVs
  stay under `build/test-artifacts` and are retained only on failure.

## Iteration 20 GUI model/presentation parity

- Build and run `jam2_gui_model_boundary_units` with Apple Clang and Qt's
  offscreen platform first, then repeat once with Cocoa if clipboard/window
  activation semantics require native confirmation. Preserve the exact four-
  section, subdivision, section-identity/capacity, generated-section, current-
  schema persistence, mixer diagnosis/meter, focus/capture, path, and invite
  contracts proved on Windows.
- Confirm the nonmodal Jam Ready dialog copies the complete invitation on open
  and via its Copy URL action, presents one read-only URL editor, closes safely,
  and never leaves an unlaid-out child. Platform warnings about raise or font
  discovery must not be converted into product assertions.
- Confirm `setAppReleaseRootForTesting` keeps every created songs/captures path
  beneath `build/test-artifacts`; do not write into the application bundle or
  the user's global temporary directory. This focused parity run opens no
  CoreAudio device and launches no peer process.

## Iteration 21 GUI widget parity

- Build and run `jam2_gui_widget_boundary_units` first under Qt's offscreen
  platform. Preserve all model sizes, virtual control identities, painted mouse
  targets, drag modes, local-file drop boundaries, and wide/narrow Performance
  renderers proved on Windows.
- Repeat with Cocoa only if native event routing differs for drag/drop, wheel,
  focus, clipboard, or key delivery. Platform-coordinate adjustments belong in
  the fixture; do not weaken the product's control identity, bounds, protected
  interaction, or one-local-file contract.
- Confirm the active looper edit preview survives a same-ID lane refresh and
  commits identical frame bounds under Apple Clang. Keep the explicit reset of
  the simulated live-recording extent before deterministic crop-coordinate
  checks.
- Inspect `REVIEW-002` on macOS as source ownership only. Do not remove the
  dormant bank callback or placeholder-lane branch as a parity workaround
  without the user's behavior decision.
- Require all temporary state beneath `build/test-artifacts`; this test opens
  no CoreAudio device and launches no peer process.

## Iteration 22 JamTaster native analysis/export parity

- Run the strengthened `jamtaster_native_units` under Apple Clang and preserve
  JSON, mix/crop, chroma, chord/bass/drum context, section-choice, RMS-energy,
  hash, current JamJar schema, owned-asset cleanup, and failure contracts.
- Require the nonuniform eight-beat case to take the anchored stretch path and
  emit four 8 kHz, 32,000-frame WAVs. Small floating-point analysis differences
  may not weaken exact file shape, frame count, bounds, ownership, or schema.
- Confirm the unrelated user WAV survives JamTaster regeneration while only
  stale `jamtaster-*.wav` assets are removed. All stems, reports, partial files,
  JamJars, and rendered WAVs must remain beneath `build/test-artifacts`.
- This parity item runs no ONNX inference and opens no CoreAudio device. Model,
  worker, and Qt service parity belong to their later focused slices.

## Iteration 23 staged JamTaster model parity

- Build and run `jam2_jamtaster_model_units` against the staged bundle model
  directory. Require actual Beat This, Basic Pitch, ADTOF, and ChordMini CPU
  inference with the same tensor counts, finite outputs, nonempty musical
  events, bounds, sorting, and invalid-range/shape rejection as Windows.
- Confirm the test executable resolves the imported ONNX Runtime 1.23 dylib via
  its rpath. Do not copy or select a system ONNX Runtime, and do not weaken the
  API-version proof to accommodate an older library.
- Preserve the missing/empty Demucs model cleanup boundary, but do not claim
  full Demucs inference here; ensemble separation has its own later parity
  item. This test opens no CoreAudio device and no Jam2 peers.
- Keep any test-local runtime staging and temporary Demucs path under `build`.
  The single public Jam2 bundle and its staged worker/runtime layout must remain
  unchanged.

## Iteration 24 Demucs ensemble and pipeline parity

- Run `jam2_jamtaster_demucs_units` with all four staged macOS ensemble models.
  Require deterministic member-specific shifts, exact quarter-step progress,
  ordered finite nonzero stems, the source's 22.05 kHz/5,513-frame shape after
  conversion, and positive source-to-summed-stem correlation.
- Run `jam2_jamtaster_pipeline_units` with independently seeded cached stems.
  Preserve every invalid-input/model/name/channel boundary, monotonic stage,
  current report/manifest/progress/JamJar format, timing diagnostic, one-step
  cache result, and malformed-cache full refresh proved on Windows.
- Use the imported ONNX Runtime 1.23 dylib through test rpaths. Do not load a
  system runtime or change model bytes, output thresholds, sample alignment,
  cache acceptance, or public bundle layout for parity.
- All source/stem/report/JamJar state must remain under
  `build/test-artifacts`. These cases open no CoreAudio device or Jam2 peer.

## Iteration 25 worker protocol and service lifecycle parity

- Run `jam2_jamtaster_worker_protocol_units` against the staged bundle helper.
  Preserve real tempo/full stem-reusing analysis, cached tempo/convert/split,
  every progress stage, repaired index, and request/protocol/action/missing-
  model/invalid-ONNX/malformed-JSON exit classifications.
- Build and run `jam2_jamtaster_service_units` using injected test bundle and
  worker paths. Confirm the existing production constructor still resolves
  `Contents/Resources/jamtaster` and `Contents/Helpers/jamtaster-worker` while
  the test seam creates no alternate public bundle.
- Preserve observer removal, normalized stderr/unstructured stdout, incompatible
  event handling, progress clamp, atomic request cleanup, structured/no-result/
  failed-start errors, cancellation idempotence, reuse, and destruction with an
  active helper. macOS has no `taskkill`; ordinary terminate plus the timed kill
  fallback must yield the same final state without inherited helper output.
- All requests, indexes, reports, stems, and fake bundle content must remain
  beneath `build/test-artifacts`. No CoreAudio device or Jam2 peer is opened.

## Iteration 26 JamTaster dialog parity

- Build and run `jam2_jamtaster_dialog_units` with Qt's offscreen Cocoa path and
  the injected private test worker. Preserve every Windows button, selector,
  chooser, modal response, progress, result-label, busy, failure, cancellation,
  close, quick-apply, converted-apply, and create-continuation assertion.
- Prove retained and live results are adopted only when the service task input
  is the dialog's current WAV. Repeat matching and mismatching paths through
  macOS path normalization, including a helper continuing after its first
  dialog closes (`BUG-P041`).
- Require a usable converted result to have one `.jamjar` in its converted
  directory plus a valid analysis object. Incomplete export must retain safe
  tempo/stem quick apply while disabling chord/drum/bass/section converted
  application (`BUG-P042`).
- Reproduce the two-job incomplete-create case and require the continuation to
  be one-shot under Cocoa nested message loops (`BUG-P043`). Confirm ordinary
  analysis after an incomplete create request never invokes creation.
- Keep the fake bundle, sparse oversized JSON fixture, requests, reports, and
  converted folders under `build/test-artifacts`. This test opens no CoreAudio
  device, network socket, or Jam2 peer and creates no alternate app bundle.

## Iteration 27 Practice Idea and Research Drum core parity

- Build and run `jam2_practice_idea_boundary_units` with the maintained
  `Jam2Resources.qrc` compiled into the test target. Require the same complete
  style/profile/form/meter/bar/production/mode matrix and deterministic/random
  generation and continuation invariants as Windows.
- Preserve the generator as one contained implementation. Confirm removal of
  the two unreachable fallbacks and unused parameters compiles cleanly with
  Apple Clang without changing generated fingerprints for explicit seeds.
- Repeat every researched source, transient, texture, blend, synth, tail, kit,
  and lane-support case. Require the 40-hit fixed-subvoice render and detail
  banks to be finite, nonzero, and byte-shape deterministic; timing values need
  only remain nonnegative because wall-clock microseconds are platform-specific.
- Apply the same short-room-send bus case and invalid frame/sample-rate guards.
  Do not open CoreAudio: all render buffers are offline and must remain owned by
  the test process. No peers, network sockets, or alternate app bundles are
  involved.

## Iteration 28 Practice Idea dialog/controller parity

- Build and run `jam2_practice_idea_boundary_units` with Apple Clang, Qt
  Widgets, the maintained resource manifest, and Qt's offscreen platform.
  Preserve Generate, Continue, Generate Reference WAVs, and Idea Details modal
  Cancel/accept behavior, every form selector/dependency, exact target-bank BPM
  ownership, current meter/length presentation, no-layer rejection, and the
  teaching/technical toggle proved on Windows.
- Repeat under Cocoa only if native modal activation, standard-button routing,
  font/layout sizing, or nested event-loop behavior differs. Fixture timing or
  widget discovery may be adapted for Cocoa, but the exact returned request,
  layer availability, button enablement, and ownership contracts must not be
  weakened.
- Confirm Apple Clang accepts the renamed drum phrase-plan local and the removal
  of the unused research-drum sample lane parameter without changing explicit-
  seed generator output or offline render values. Preserve exact generated-kind,
  combined-practice fallback, and missing-section lookup behavior.
- This focused parity item opens no CoreAudio device, network socket, Jam2 peer,
  or alternate app bundle. Any temporary Qt state must remain beneath
  `build/test-artifacts` and be absent after success.

## Iteration 29 project persistence and transient WAV parity

- Build and run `jam2_project_persistence_units` with Apple Clang and Qt Core.
  Require every fixture, partial, JamJar, and transient WAV to remain beneath
  `build/test-artifacts`; no CoreAudio device, GUI, socket, or peer is involved.
- Preserve exact abandoned-transfer filename cleanup, canonical alias
  ownership, active/deferred/persistent transitions, workspace relocation,
  unowned/non-WAV/directory refusal, asynchronous WAV-only deletion, and empty
  managed-folder pruning. Confirm APFS path case behavior; do not weaken path
  ownership if the endpoint uses a case-sensitive volume.
- Prove ordinary atomic writes preserve exact bytes and malformed, nonobject,
  missing, unavailable, and directory reads/writes fail. Reproduce `BUG-P044`:
  a 4 MiB plus one byte save must fail before creating a file, while the same
  existing input remains rejected by the unchanged reader limit. Also confirm
  an unavailable app-bundle-adjacent destination fails without residue.
- Confirm removal of the duplicate project-file state and unused working-folder
  helper does not affect bundle save/open behavior; `JamStorage` remains the
  project-file authority and the coordinator retains project/workspace folder
  ownership only.

## Iteration 30 fake-injected loopback capture parity

- Build and run `jam2_gui_loopback_recorder_units` with Apple Clang and Qt
  Core. The injected backend must drive the same recorder thread, accumulator,
  decoder, resampler/trimmer, completion boundary, and WAV writer without
  opening CoreAudio or creating an alternate app bundle.
- Preserve exact PCM16/24/32/Float32 decode, sign extension, clipping,
  non-finite-to-silence behavior, duration/peak counters, trim qualification,
  identity/down/up sample shapes, concurrent-start rejection, stop, destructor
  join, backend/observer exception containment, and post-failure reuse.
- Write and parse the exact mono PCM16 RIFF under a nested Unicode APFS path.
  Confirm `QSaveFile` leaves no destination or temporary residue for blank,
  directory, unavailable-volume, or interrupted commit targets (`BUG-P046`).
  Every fixture must remain under `build/test-artifacts`.
- macOS currently reports the existing explicit unsupported result for the
  default system-loopback backend and source enumeration; verify that behavior
  remains clean and non-destructive. Do not introduce a broad CoreAudio device
  abstraction as a parity workaround. If native system-output capture is later
  requested as a macOS feature, add it behind the same narrow capture backend
  and retain all processing/lifecycle contracts.
- Revisit `REVIEW-003` only after the user chooses whether a valid zero-frame or
  fully trimmed silent WAV should remain a successful capture. The parity test
  must preserve current behavior until that decision is made.

## Iteration 31 track-recording workflow and local-event parity

- Build and run `jam2_track_recording_workflow_units` with Apple Clang and Qt
  Core. Preserve every pure clock/grid/latency/transport boundary, exact local
  command type/cookie/frame/flag contract, all six partial-submission failures,
  lane/capture ownership, transient cleanup, stale completion, command
  rejection, and jam-recorder confirmation/retry case proved on Windows.
- Require NaN, infinity, exhausted beat/frame ranges, duration overflow, and
  the minimum signed render offset to fail or saturate exactly as specified.
  Ordinary 48 kHz half-second beat and render-offset targets must remain exact;
  do not weaken them for floating-point tolerance unless Apple Clang produces a
  documented representation difference outside an integer frame boundary.
- Run the real Headless Engine tone-to-WAV case. It must propagate the exact
  recorder take ID through the in-process `EngineEvent`, report 48 kHz and a
  nonzero frame count, and create its WAV only beneath
  `build/test-artifacts`. This test opens no CoreAudio device or network socket
  and launches no Jam2 peer.
- Confirm partially accepted arm sequences remain cleanup-owned across APFS
  file creation/close timing, and that start/stop command rejection restores
  the last engine-confirmed jam state. All fixture directories and canceled
  WAVs must be absent after success.
- `EngineEvent::id` is local engine/GUI correlation only. Do not add it to any
  network packet, control JSON, protocol header, payload, parser, version, or
  compatibility path while performing macOS parity.

## Iteration 32 shared audio-device processing parity

- Build and run `jam2_audio_device_processing_units` with Apple Clang. Preserve
  every direct callback interval, peak, gain, saturation, local-monitor,
  prepared-source, output-clipping, ring/resampler, synthetic input, and
  metronome branch proved on Windows, including opposite PCM rails and the
  final representable frame/beat boundaries.
- Run its real threaded Headless Engine case and require input, monitor,
  output, callback timing, network attachment, and post-gain remote peak
  diagnostics to become nonzero within the existing bounded waits. No
  CoreAudio device or Jam2 peer is opened by this focused case, and it must
  leave `build/test-artifacts` absent.
- Compile the migrated `audio_device_macos.cpp` against CoreAudio and compare
  its callback order with Windows and Headless: capture/routing/peaks, network
  playback and remote gain, remote peak, local monitor, prepared source,
  metronome, output gain/clipping/peak, then recorder stems. Keep every helper
  `noexcept`, allocation-free, lock-free, logging-free, and nonblocking in the
  real-time callback.
- With an explicit macOS hardware profile, exercise CoreAudio enumeration,
  stream open/start/stop, device configuration change, negotiated format,
  callback conversion, and RAII teardown. These platform lifecycle functions
  are intentionally not claimed by the offline shared-processing test.
- Reproduce `BUG-P055` through `BUG-P060` under Apple Clang: interpolation
  across opposite PCM rails, minimum signed render offset at the maximum raw
  frame, non-finite/excessive render parameters, saturated beat diagnostics,
  unrepresentable callback thresholds, and Headless post-gain meter/recording
  parity. Ordinary 48 kHz samples and metronome epochs must remain exact.
- This refactor changes no packet, header, payload, parser, protocol version,
  authentication rule, network ordering, metronome model, or epoch rule. Do
  not modify any of those contracts while completing macOS parity.

## Iteration 33 reactive transport timing and four-peer count-in parity

- Build and run `jam2_transport_timing_units` with Apple Clang. Preserve exact
  raw/musical conversion and saturation for zero, positive, negative,
  `INT64_MIN`, and `UINT64_MAX` boundaries; exact ordinary 48 kHz schedules;
  the 200 ms next-bar lead; positive/negative render offsets; nonzero epoch;
  three-beat subdivided eight-bar count-ins; and fail-closed invalid/exhausted
  ranges.
- Build and run `jam2_four_session_command_integration` against the one staged
  `release/Jam2.app` executable. It must launch exactly four direct-mesh peers
  with Headless fake audio and isolated roots, reject the correlated invalid
  command, apply all four gains, honor a 512-frame delayed snapshot, publish
  `RecordStart` before countdown, and expose the exact 96,000-frame one-bar
  future schedule and then active count-in on all four peers before clean
  reactive shutdown and valid manifests.
- Confirm the inherited automation pipes, Qt controller timer, and absolute
  `apply_frame` snapshot work under POSIX descriptors/Cocoa event dispatch.
  Fixture timing may retain the bounded ten-second wall allowance, but the
  authoritative checks remain audio-frame based; do not replace them with
  sleeps or poll counts.
- Reproduce `BUG-P061` through `BUG-P065` and `BUG-T089`/`BUG-T090`: distinct
  countdown/target, bounded frame math, saturating delay addition, accepted
  top-level scheduled snapshot, immediate publication of generated future
  transport, correlated rejection diagnostics, and stable stored musical-phase
  assertions while render compensation moves.
- Revisit `REVIEW-004` only after the user decides whether adopted raw
  transport targets should remain fixed or follow later render-offset slew.
  Preserve current fixed-target behavior for parity until that decision is
  made. Continue to use the existing 21 clean/impaired all-mode matrix as the
  independent live metronome/epoch proof.
- No network field, header, payload, parser, version, authentication rule, or
  compatibility path changed on Windows. Do not change any during parity work
  without prior user approval.

## Iteration 34 workspace storage and shared-track state parity

- Build and run `jam2_workspace_state_units` with Apple Clang and Qt Core/
  Widgets offscreen. Require its release-root override, external project,
  JamJar, WAV, take, and pruning fixtures to remain beneath
  `build/test-artifacts`; it opens no CoreAudio device, socket, peer, or app
  bundle.
- Preserve exact new/rename/save/discard, every asset folder, artifact state,
  take collision, recursive empty-workspace, nested-artifact retention,
  looper-lane rename, and shared-track phase/status behavior proved on Windows.
- Reproduce `BUG-P066` through `BUG-P068`: an opened filename that differs
  from the internal title must remain the exact save target; changing an
  externally opened display title must preserve its parent and unrelated
  sibling; a canonical managed rename must move its directory and `.jamjar`
  filename together while retaining adjacent asset bytes.
- Run the external-root cases on the endpoint's ordinary APFS volume and, if
  available, a case-sensitive APFS fixture. The ownership comparison is
  intentionally case-sensitive outside Windows. Include a Unicode JamJar
  filename and confirm a read-only rename reports an error without moving
  unrelated files.
- Confirm removal of the unused `waitForWorkers` wrapper compiles cleanly.
  Leave the unreferenced `track.processing` helpers unchanged until the user
  resolves `REVIEW-005`; do not remove or revive a message/schema path during
  parity work without approval.
- Windows changed no JamJar JSON shape, network message, field, payload,
  parser, protocol version, authentication rule, metronome/epoch behavior, or
  shared-WAV transfer contract. Preserve that scope on macOS.

## Iteration 35 metronome transport controller parity

- Build and run `jam2_metronome_transport_controller_units` with Apple Clang
  and Qt Core. Preserve every tap-tempo median/reset/range/backward/extreme
  case, PlaybackGrid pattern/rate/anchor/interpolation/clear case, submit
  boundary, mutation gate, schedule revision, render-offset projection, and
  invalid-rate case proved on Windows. It opens no CoreAudio device, socket,
  peer, GUI window, or alternate app bundle.
- Reproduce `BUG-P069` through `BUG-P072`: `INT64_MIN` and positive-overflow
  raw-to-musical projection, complete signed tap timestamps, final-frame
  QElapsedTimer interpolation, and NaN/infinite/near-integer sample rates.
  Exact integer outputs and saturation must match Windows; ordinary timer
  advancement need only satisfy its bounded monotonic state contract.
- Confirm the production `ApplicationRuntime` constructor and injected
  `CommandSubmitter` build without warnings and that exception containment is
  outside any real-time callback. Preserve finite 7999.6-to-8000 rounding and
  the maintained 8--384 kHz limits.
- Run the existing `jam2_transport_timing_units` and
  `jam2_track_recording_workflow_units` alongside this focused target. The
  final macOS distribution gate remains responsible for the complete
  four-peer all-mode clean/impaired metronome and dynamic-epoch matrix.
- No metronome model, epoch proposal/adoption, listener compensation,
  transport message, packet field, payload, parser, protocol version,
  authentication rule, or network behavior changed on Windows. Preserve that
  scope during parity.

## Iteration 36 WAV staging and lane-merge parity

- Build and run `jam2_track_workspace_support_units` with Apple Clang and Qt
  Core. Keep every fixture beneath `build/test-artifacts`; this target opens no
  CoreAudio device, network socket, Jam2 peer, GUI window, or alternate app
  bundle.
- Preserve strict Unicode-path PCM16 metadata, exact-byte same-rate staging,
  SHA-named corrupt-destination repair, zero requested-rate behavior, and the
  exact 441-frame stereo 44.1-to-48 kHz conversion. Require 8 kHz and 384 kHz
  endpoint conversions to pass and -1, 1, 7999, and 384001 to reject before
  filesystem mutation.
- Reproduce `BUG-P073` through `BUG-P076`: unsupported conversion rates,
  blank/relative staging roots and unsafe asset components, hash read-error
  propagation, and repeated hashless local-only lane preservation. APFS path
  identity is intentionally case-sensitive; test an ordinary APFS fixture and,
  if available, a separately configured case-sensitive volume.
- Preserve synchronized merge behavior for same-hash local mix/path state,
  same-ID/different-WAV re-keying, authoritative lane omission, and removal of
  WAV content without resurrection. Practice references, legacy managed
  references, non-local lanes, and empty paths must remain excluded from the
  local-only preservation pass.
- Run `jam2_looper_project_units`, `jam2_gui_loopback_recorder_units`, and the
  short unit aggregate alongside the focused target. Successful cleanup must
  leave `build/test-artifacts` absent.
- No network message, field, header, payload, encoding, parser, version,
  authentication rule, ordering, shared-WAV transfer state machine,
  metronome/epoch behavior, or real-time callback changed on Windows. Preserve
  that scope during parity.

## Iteration 37 shared Section-bank barrier and launch-clock parity

- Build and run `jam2_shared_bank_launch_units` with Apple Clang and Qt Core.
  Preserve the exact creator-plus-three-peer barrier, stale/self/nonmember/
  duplicate handling, explicit departure, cancellation, replacement, and
  consume-on-commit behavior. This pure target opens no CoreAudio device,
  network socket, peer, or GUI window.
- Reproduce `BUG-P077` through `BUG-P079`: exact prepare membership, immediate
  cancellation if a new peer authenticates mid-prepare, explicit removal of a
  departed original peer, full-domain beat/frame/epoch saturation, and
  fail-closed invalid transport clocks. Run the numeric cases under Apple
  Clang/UndefinedBehaviorSanitizer where available.
- Run `jam2_four_performance_integration` against exactly four direct-mesh
  Headless fake-audio app instances. A joiner must queue Section B and the
  creator must queue Section A while shared-grid transport remains running.
  Require active/pending/barrier state and original lane identity to converge
  on every peer, with each peer's exact metronome epoch frame unchanged across
  both transitions.
- Confirm POSIX/Cocoa timing and control delivery preserve the same prepare/
  ready/switch ordering. The new barrier diagnostics must be present in
  automation snapshots and all artifacts must remain under, then cleanly
  remove, `build/test-artifacts`.
- Preserve `REVIEW-006` and `REVIEW-007` without changing validation or adding
  a commit acknowledgement until the user makes those protocol decisions.
  Windows changed no `bank.*` name, field, JSON shape, validator, parser,
  version, authentication rule, or encoding; macOS parity must not either.

## Iteration 38 LooperProject editing and Section-shortening parity

- Build and run `jam2_looper_project_units` and
  `jam2_gui_widget_boundary_units` with Apple Clang and Qt. Preserve lane
  creation/identity/name/rate/frame/crop bounds, atomic rejected edits,
  gain/mute/solo, WAV clearing, 44.1-to-48 kHz timeline conversion, natural and
  explicit endpoints, loop-preserving crop, placement clearing, and saturated
  extreme arithmetic. Run the numeric cases under UndefinedBehaviorSanitizer
  where available.
- Run `jam2_four_gui_agent_smoke` and
  `jam2_four_gui_modal_integration` against exactly four staged Jam2 app
  instances with fake audio. Require gain, mute, and solo visible state on all
  four peers and exercise Rename Lane change/Cancel and change/Save through the
  explicit owned modal controls on each process.
- Reproduce `BUG-P080` through `BUG-P084` and `BUG-T091`: reject invalid local
  lane state before mutation, keep timeline math defined at numeric limits,
  preserve loop source coordinates while shortening placement output, cancel
  an unreferenced removed-WAV transfer, reject a zero-length explicit stop,
  and keep every rename child classified under Cocoa event dispatch.
- Exercise Section shortening with a placement wholly beyond the new end and
  prove its managed original file is preserved while any unreferenced transfer
  is cancelled. Repeat with two lanes sharing a hash to prove the transfer is
  retained until the final reference is removed.
- Keep artifacts beneath `build/test-artifacts` and require successful cleanup.
  Preserve `REVIEW-002` and `REVIEW-008`; do not delete dormant wrappers or
  change the Section/renderer limits before the user decides their intended
  behavior.
- Windows changed no persistence parser/schema, network message, field,
  payload, encoding, validator, version, authentication rule, shared-WAV wire
  behavior, metronome model, or epoch rule. Preserve that scope during parity.

## Iteration 39 prepared-mix lifecycle parity

- Build and run `jam2_prepared_mix_lifecycle_units` with Apple Clang and Qt
  Core. Preserve exact request/revision counters, invalid-bank rejection,
  priority-bank coalescing, wrong-generation rejection, rerun consumption,
  stale completion disposal, result validation, active/inactive cache identity,
  one-shot playback intent, path relocation, and retryable obsolete-path
  ownership.
- Reproduce `BUG-P085` through `BUG-P088`: replace a project while an old-bank
  render is active and prove the old output cannot apply; retire an inactive
  cache and an active cache displaced by an empty/missing bank; reject invalid
  worker bank/metadata rather than clamping; and retain a managed deletion
  failure for retry.
- Run `jam2_application_boundary_units` for the real PCM renderer and
  `jam2_four_performance_integration` against exactly four fake-audio Jam2 app
  instances. Both Section B and Section A prepared-bank transitions must
  converge with exact epoch preservation and no pending lifecycle state.
- Confirm managed-path relocation remains case-sensitive on macOS where the
  project root requires it. Keep every derived WAV under
  `build/test-artifacts`/the managed workspace, and require successful cleanup
  to remove retained transient paths.
- Preserve `REVIEW-008`: this ownership refactor deliberately does not change
  the five-minute renderer bound or the longer maintained Section model.
  Windows changed no persistence shape/parser, network message/field/payload,
  protocol validator/version/authentication rule, metronome model, or epoch
  behavior; macOS parity must not either.

## Iteration 40 shared-track loop/project-state parity

- Build and run `jam2_workspace_state_units` with Apple Clang and Qt Core.
  Preserve checked start/end conflict handling, final-millisecond clamping,
  whole-track/reset/disable transitions, finite and open-ended effective frame
  conversion, and safe NaN/infinity recovery.
- Reproduce `BUG-P089` through `BUG-P092`: contradictory loop edits must
  canonicalize; opening a project with no track must not retain the previous
  track; malformed/non-finite track state must fail before project teardown;
  and a nonempty persisted path must receive deterministic local compatibility
  audit ownership.
- Run `jam2_four_performance_integration` against exactly four Cocoa Jam2
  GUI-agent instances with fake audio. Require the explicit Stop/Play fixture
  to put all four real prepared sources into playing, then exercise Clear,
  Start, End, disable, re-enable, and final Clear on every process with ordered
  effective frames and zero prepared failure/busy counters.
- Use the native Open dialog to select a missing-track project, a malformed
  track project, and a mismatched-rate WAV. Confirm rejected input preserves
  the current project and the existing compatibility warning/conversion path
  remains visible for valid input.
- Confirm successful cleanup removes all paths beneath
  `build/test-artifacts`. Windows changed no persisted field/shape and no
  network message/field/header/payload/parser/version/authentication behavior,
  shared-WAV wire behavior, metronome model, or epoch rule; preserve that scope
  during Cocoa parity.

## Iteration 41 transactional JamJar asset and live-action parity

- Build and run `jam2_track_workspace_support_units` and
  `jam2_looper_project_units` with Apple Clang and Qt Core. Preserve complete
  WAV metadata/SHA validation, atomic candidate replacement with stable lane
  identity, exact-byte deduplication, portable name sanitization, collision
  suffixing, rollback, source-mutation rejection, and first/reused/repaired
  staged-file ownership.
- Reproduce `BUG-P093` through `BUG-P099` on the distribution filesystem.
  Check APFS case-sensitive and case-insensitive configurations where
  available, canonical paths through symlinks, `QIODevice::NewOnly` exclusive
  creation, `QSaveFile` replacement/commit semantics, rollback during a real
  sharing violation, and retry after a stale compatibility-audit completion.
  A user's external source WAV must never enter managed deletion ownership.
- Run `jam2_four_shared_content_integration` against exactly four isolated
  Cocoa Jam2 GUI-agent processes. Save all four converged WAV-heavy projects,
  require clean saved state and distinct roots, parse every JamJar, resolve
  every relative WAV, and verify exact SHA-256 identity for the six expected
  contents with no partial files left behind.
- Run `jam2_four_gui_agent_integration` and
  `jam2_four_gui_modal_integration`. Add/remove one Section on all four peers,
  then open the Export scope modal, verify the default scope and owned controls,
  and Cancel without opening a native chooser. This reproduces `BUG-P100` and
  exercises Cocoa ownership/event timing without creating an export file.
- Use native Open/Save/drop/recording workflows for one valid external WAV,
  one hash-invalid/missing asset, one already-saved second Save, and one
  mismatched-rate compatibility conversion. Confirm failed Save preserves the
  unsaved root, successful repeated Save is clean, lane replacement is atomic,
  and superseded managed WAVs are retired without deleting external sources.
- Successful tests must remove everything beneath `build/test-artifacts` and
  leave only the one staged Jam2 app bundle. Windows changed no persistence
  field/shape, network message/field/header/payload/encoding/validator/parser/
  version/authentication behavior, accepted network input, shared-WAV transfer
  state machine, metronome model, epoch rule, audio algorithm, or real-time
  callback; macOS parity must not either.

## Iteration 42 Windows closure to reproduce on macOS

- Build and run the direct `DetailSectionEdit` test from its new owned source
  files. Verify Cocoa focus-out, Return, Escape, and double-click event ordering
  matches Windows before accepting the extraction.
- Run the final `jam2_four_session_dialog_integration` with exactly four Cocoa
  GUI-agent processes. It must report no duplicate IDs while repeatedly
  rebuilding BeatGrid views and must pass generated idea, details, continuation,
  curated preview, Section Trim/shrink, reference-WAV sharing, lane region,
  Clear Idea, New Jam Cancel, and dirty window-close Cancel. This specifically
  reproduces `BUG-P101`, `BUG-P102`, and the Cocoa close-event boundary.
- Configure an explicit macOS plug-in path and CoreAudio device in the macOS
  hardware-profile equivalent. Exercise probe/probe-all/probe-file/self-test,
  production host status/error/stats, MIDI controls, native editor open/close,
  invalid channel rejection with clean recovery, and five seconds of callbacks.
  Do not measure plug-in deadlines until the editor has fully closed.
- Do not carry Windows WinMM exemptions over to CoreMIDI. With a physical MIDI
  device, validate open, supported short-message routing, unsupported/SysEx
  counters, close, and reopen. If no device is available, record the exact
  hardware limitation under `REVIEW-011` rather than manufacturing a pass.
- Re-run the production controller binary/rejection case and staged public
  network create/join help checks. No protocol acceptance or emitted bytes may
  be changed to obtain macOS parity.

## macOS acceptance

- Quick builds omit all test targets.
- Selected suites build only their required targets.
- The normal Release app is built into the single staged bundle and passes the
  complete four-peer suite through `compile.sh --tests-full`.
- No source-coverage collector or coverage gate runs on macOS.
- No alternate Jam2 executable or app bundle is created or tested.
