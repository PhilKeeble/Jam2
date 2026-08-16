# Jam2 Automated Test and Refactor Plan

## Goal

Build a Windows-complete, cross-platform C++/CTest validation system for Jam2.
The normal build remains test-free. Selected suites and a full pre-distribution
gate build their private test binaries on demand and test the one staged
`release/jam2.exe`.

Automated real-jam coverage always uses four Jam2 peers. Cross-machine and soak
testing are deliberately outside this initiative; action-driven virtual-time
and bounded real-process tests cover the automatable behavior instead.

## Non-negotiable rules

- Preserve every visible product feature.
- Refactor only around meaningful responsibility, ownership, lifecycle,
  dependency, real-time, or testability boundaries; never split by file size.
- `PracticeIdeaGenerator.cpp` remains cohesive unless a concrete independent
  responsibility is found.
- Decompose `MainWindow` where covered workflows have clear owned boundaries,
  including settings/preferences, jam lifecycle, shared content/WAVs,
  metronome/listener compensation, and automation support when appropriate.
  Retain cross-workflow window orchestration in `MainWindow`; do not extract a
  section merely to reduce its line count.
- Every slice follows implement, test, self-review, gap-fix, and retest before
  the next slice starts.
- While slice 7 work remains, use only the smallest relevant selected suite or
  exact CTest for each component cluster. Do not rerun the full instrumented
  and optimized catalogue after every cluster. Run one final two-pass Windows
  distribution gate only after the planned coverage and review work is done.
- Every discovered defect is entered in the structured bug register in
  `TEST-LOG.md`, including its observed symptom, root cause, production or test
  change, regression proof, and remaining test need. Unresolved behavior or a
  residual risk requiring user judgment is also entered in `TEST-REVIEW.md`.
- Do not change a wire-protocol field, version, payload shape, encoding,
  decoder, authentication rule, or compatibility behavior without first
  describing the proposed change to the user and receiving approval.
- Fix safely isolated bugs found during the work. Record ambiguous changes,
  functionality risks, manual-review needs, and possible feature removals in
  `TEST-REVIEW.md` without removing the feature.
- Accept one current format at each persistence/protocol boundary. Remove
  genuine obsolete compatibility, but retain current version checks needed for
  safety.
- Keep test implementation under `tests/`; never stage test binaries into
  `release/`.

## Slice status

| Slice | Status | Exit criteria |
|---|---|---|
| 1. Test/build foundation | Complete | Opt-in top-level tests, quick builds omit tests, existing native tests pass, tracking files reviewed |
| 2. Test-control foundation | Complete | Typed action/control inventory, state/view snapshots, inherited-handle GUI agent, four-peer coordinator |
| 3. Jam Sync | Complete | Native policy/revision/reconciliation component and four-peer sync workflows pass review |
| 4. Shared content baseline | Complete | Songs, views, ideas, tracks, WAVs/assets, stale completions, and convergence baseline covered |
| 5. Performance | Complete | Transport, recording, fake audio injection/capture, timing/content, and all metronome modes/epochs under deterministic delay, jitter, loss, duplication, reordering, and bounded burst loss covered |
| 5B. WAV-sharing hardening | Complete | Exactly four isolated peers pass concurrent same/different offers, reconnect/retry, policy transitions, partial/stale transfers, conflicts, deduplication, source handoff, session reset, and exact-byte convergence |
| 6. Network/security | Complete | Native UDP impairment, authentication, malformed input, and bounded resource stress covered |
| 7. Remaining app audit | Complete | Every control/view/action/function inventoried; justified refactors and current-format/dependency cleanup complete |
| 8. Distribution gate | In progress | Windows two-pass coverage and normal Release full suite pass; superseded Python validation/stress removed |

## WAV-sharing hardening matrix

The completed shared-content slice is the baseline, not the final robustness
claim. Slice 5B must use exactly four isolated GUI peers and cover at least:

- simultaneous imports of identical bytes and different bytes from multiple
  peers, with stable contribution IDs, deduplication, and deterministic lane
  ordering;
- conflicting occupied lanes versus matching empty lanes, proving unrelated
  local content is preserved and the offered lane is reconciled safely;
- auto-share off/on transitions and explicit Share Tracks while transfers are
  pending, without a policy toggle silently validating a missing file;
- sender or receiver disconnect/rejoin during validation, offer, chunking, and
  final commit, followed by bounded retry and exact SHA-256/byte convergence;
- duplicate, reordered, stale-generation, wrong-source, truncated, and
  hash-mismatched transfer events, with no partial file becoming visible;
- repeated sharing after convergence, proving idempotence and no lane/file
  multiplication; and
- independent per-peer storage roots throughout, so same-machine paths cannot
  create a false positive.

Slice 5B is closed in four reviewed iterations rather than one broad test:

1. Harden the native asset-transfer state machine. Prove malformed, duplicate,
   reordered, stale-hash, wrong-source, truncated, hash-mismatched, invalid-WAV,
   cancellation, worker-failure, idempotent-existing-file, acknowledgement, and
   queue-deduplication behavior. No partial file may become visible or remain
   abandoned on disk.
2. Expand the real four-peer GUI jam to simultaneous identical and different
   imports, occupied/empty-lane conflicts, repeated offers, policy transitions,
   and deterministic exact-byte/model convergence with four isolated roots.
3. Add bounded real-process interruption and rejoin cases at validation, offer,
   chunking, and finalization boundaries, proving retry ownership and eventual
   exact convergence without a soak or cross-machine campaign.
4. Self-review the complete WAV path, close any uncovered state/view/action
   gaps, rerun the focused suite, then rerun the full Windows catalogue before
   declaring Slice 5B complete.

Iteration status: all four iterations are complete. Iteration 4 adds direct
dropped-start recovery, pure same-hash ownership units, a two-batch leave,
complete new-session revision reset, and forced three-failure source-A to
one-failure source-B handoff across seven exact 800,044-byte WAVs and exactly
four isolated GUI peers. The final focused test passed three consecutive runs,
the shared-content gate passed 4/4, and the authoritative full Windows catalogue
passed 33/33 in 418.61 s. Slice 6 network/security is next.

## Network/security matrix

Slice 6 is split at the real transport boundary so each part can be reviewed
and stabilized independently:

1. Register Jam2's native boundary and controller-lifecycle validators as
   first-class CTests. Exercise real TCP sockets for the unauthenticated cap,
   authentication and incomplete-frame deadlines, replay/tag/oversize rejection,
   the bounded failed-key budget, paired control/asset teardown, and same-token
   reauthentication. Require hard bounded-work and disconnect statistics.
2. Add native UDP capture and injection around exactly four Jam2 processes.
   Prove short/malformed, replayed, corrupted, duplicated, reordered, delayed,
   and bounded-flood traffic is rejected or tolerated while the real jam stays
   converged and all four peers continue exchanging authenticated audio/events.
3. Compare this native matrix with the Python validation/stress inventory,
   close any parity gaps, self-review both TCP and UDP resource ownership, then
   run the selected network gate and full Windows catalogue. Python validation
   or stress code is removed only after this evidence exists; analysis and
   benchmarking tools remain.

Iteration 1 registered the two existing native validators and completed the
TCP/security portion. Iteration 2 added two exact four-process UDP gates. One
proves bidirectional corruption, the full malformed corpus, delayed replay, and
an 8,192-datagram short flood with bounded receive batches; the other proves
authenticated 32-bit sequence wrap, excessive forward-gap rejection, extreme
sample-time rejection, and exact loss attribution. Both passed three
consecutive focused Windows runs, and the unchanged clean shared-grid case
also passed. After BUG-T029 was closed, the selected network gate passed 4/4
in 128.01 s and the complete Windows catalogue passed 37/37 in 549.51 s. Slice
6 is complete; Slice 7's remaining app/control/view/function audit is active.

## Remaining application audit matrix

Slice 7 treats “every button” as a maintained behavioral contract rather than
an unreviewed click sweep. A generated beat cell and a Start Jam button both
need coverage, but the former belongs to a parameterized family while the
latter needs a distinct lifecycle workflow and modal-state assertions.

1. Establish a GUI-control contract and deterministic inventory. Exclude Qt's
   internal editor/popup children, require stable metadata for every semantic
   control, expose conditional availability, and fail on duplicate or
   unclassified controls. Keep the diagnostic inventory export optional.
2. Cover the unique application, session, navigation, data, and performance
   controls through their real widget signals, including modal open/accept/
   cancel behavior and state/result assertions.
3. Cover looper, metronome, mixer, transport, and recording controls against
   fake audio and the existing exact four-peer jam. WAV user flows must prove
   model, visible-state, ownership, transfer, interruption/retry, and exact-byte
   convergence rather than merely proving that a button emitted a signal.
4. Register and cover BeatGrid's generated chord, beat, lyric, section,
   pagination, and editing families by coordinates and family contracts. Add
   explicit virtual entries for custom-painted Performance Home hit targets.
5. Cover settings, dialogs, device selectors, plugins, persistence, and file
   choices. Hardware-only behavior stays gated by the explicit hardware
   profile; deterministic validation uses fake devices and injected audio.
6. Audit maintained actions, views, message handlers, functions, formats, and
   dependencies. Refactor only when this exposes a concrete ownership,
   lifecycle, duplication, or testability boundary. In particular, move
   covered `MainWindow` workflows into their relevant owned sections where
   that removes mixed ownership while leaving its coordinating role intact.
   Emit reviewed coverage exemptions for genuinely unreachable/platform-gated
   code and rerun the focused and complete Windows catalogues before closing
   the slice.

Iteration 1's initial-window contract is complete. The first diagnostic pass
found 472 candidates after excluding combo/spin implementation children.
Self-review removed read-only views and explicitly accounted hidden state
mirrors, leaving 460 real semantic Qt controls. Coordinate-based BeatGrid
families and painted looper/metronome/Performance Home providers add 37 virtual
targets, producing a maintained `497 / 497 classified / 0 unclassified / 0
duplicates` inventory. A four-process workflow invokes representative chord,
musical, beat, lyric, looper, home, and metronome targets and passed three
consecutive focused Windows runs. Iteration 2 adds a real four-instance modal
workflow for Local Engine, Start, Join, and Jam Sync; it inventories each live
dialog and proves cancellation/state ownership, passing twice after its red/
green review. Iteration 3 binds painted add/drop and the real Share Now/removal
confirmation to exact four-peer bytes and views. It found and fixed BUG-P022,
where removal cloned the old shared WAV back into the arrangement. Settings and
specialist dialogs remain. Same-byte remove/re-import/re-share is green in two
repetitions. Removal during paused outgoing validation is also green twice and
found/fixed BUG-P023: a superseded Track Sync batch left four obsolete start
retries and sender work alive. The gate now requires zero new timeout/retry,
queue, worker, partial-file, hash, identity, or painted-view residue. Removal
after all chunks were privately written is green, and different-byte
replacement during paused sender validation found/fixed BUG-P024 before
passing twice. The identified WAV removal/re-import/replacement race set is
closed. Settings now has a complete live `667 / 667 classified / 0
unclassified / 0 duplicate` inventory and four representative preference
groups prove Cancel, Save, reopen persistence, and restoration across four
processes. Hardware audio actions remain explicitly gated. Specialist dialogs
and the remaining app/function audit are the active Slice 7 work. Iteration 5
closes the Listener Compensation specialist dialog: its six transient controls
are classified, Cancel/Apply/reopen/restoration run on four processes, and one
listener changes all four effective runtime tuning values inside an active
four-peer jam without changing the other peers. The runtime now consumes the
configured deadband, applies maximum/smoothing/slew through one directly tested
pure step boundary, and rejects non-finite CLI tuning. The first complete
performance rerun passed 20/21 metronome impairment cases; the sole red run
contained a simultaneous approximately 620 ms host/proxy scheduling excursion
on all four peers and correctly failed strict mapping/capacity checks. The exact
listener-compensated/loss case then passed twice with thresholds unchanged. A
second pre-fix full run exposed the same common proxy-pump starvation under
reordering, so the Windows coordinator now matches the four high-priority Jam2
children and rejects any run with a proxy pump gap above 50 ms as invalid test
evidence. After that harness-only fix, reordering passed twice, loss passed,
and the authoritative complete performance gate passed 23/23 in 367.87 s.
Iteration 5 is closed. No wire protocol field, payload, parser, version,
message, or ordering changed.

Iteration 6 applies the responsibility-driven `MainWindow` split to the
Arrangement editor. `ArrangementEditorDialog` now owns its modal controls,
row operations, validation, and typed result; `MainWindow` retains arrangement
application, sharing, and start/stop orchestration. Four offline GUI processes
prove Cancel/Save/Start/Stop plus all row actions, and an authenticated four-
peer fake-audio jam proves creator and joiner edits converge exactly. Native
model boundaries reject invalid rows without mutation. The slice found and
fixed BUG-P028 (embedded editors did not own table-row selection) and BUG-P029
(newly accepted manual WAV bytes could miss full-mesh fan-out when metadata was
unchanged). The deterministic WAV regression also corrected BUG-T043 in its
own stale-copy assumptions. The forced race passed twice, and the complete
shared-content catalogue passed 4/4 in 112.10 s. The authoritative GUI-labelled
catalogue then passed 8/8 in 218.01 s, closing Iteration 6 on Windows. No network
protocol field, payload, parser, version, message, or ordering changed.

Iteration 7 makes the second responsibility-driven `MainWindow` split:
`ListenerCompensationDialog` now owns the covered six-control modal editor and
returns the existing typed local settings value. `MainWindow` keeps preference
persistence, active-runtime submission, and diagnostic logging. The focused
four-process modal workflow passed in 68.24 s, the active four-peer fake-audio
runtime workflow passed in 4.22 s, and the complete GUI catalogue passed 8/8 in
158.65 s. No product bug, behavior change, parallel settings schema, or network
protocol change was introduced. The remaining app/control/function audit now
selects further ownership boundaries by cohesion and coverage, never file size.

Iteration 8 extracts `JamSyncDialog` after first closing a real behavioral
coverage gap: existing modal coverage proved Cancel, but live four-peer policy
changes bypassed Apply. The native integration now changes every field through
the real dialog as peer 3 and as creator, requiring exactly one authoritative
revision per Apply and exact convergence on all four peers. It also proves WAV/
recording dependency disabling and re-enabling plus Leader Audio's selective
metronome-state lock. The complete Jam Sync selector passed 2/2 in 5.86 s and
the GUI catalogue passed 8/8 in 191.85 s. `MainWindow` retains authority,
message routing, route effects, content publication, and recording protection.
The one remaining dialog branch—full lock during an active shared lane take—is
tied to the inline Arm Lane Recording workflow and is explicitly part of that
next recording coverage/extraction slice. No product or network protocol change
was made.

Iteration 9 closes the Arm Lane Recording ownership and four-peer recording
slice. `LaneRecordingDialog` owns the 16-control modal editor, three capture-
mode drafts, mode-dependent presentation, browsing/source refresh, validation,
and typed accepted result. Eight obsolete permanently hidden Qt widgets used
only as storage were replaced by typed runtime state. `MainWindow` retains the
cross-workflow responsibilities: resolving a current lane identity, configuring
the engine/source router, coordinating the shared recording group, staging the
take, publishing arrangement/asset work, and updating visible recording state.
The real four-peer fake-audio regression now adds and arms the dynamically
identified lane, proves the genuine group/isolation state on all peers, opens
Jam Sync during the take and proves its complete policy lock, injects bounded
file-worker congestion, stops the take, and requires one exact non-empty WAV by
lane ID/SHA-256/byte count on all four isolated peers before removing the lane
and restoring the exact baseline IDs. It found and fixed BUG-P030 through
BUG-P034 and corrected BUG-T044 through BUG-T048. Final Windows evidence is the
23/23 performance selector in 377.12 s, focused modal proof in 79.27 s, and the
complete GUI catalogue at 8/8 in 220.68 s. No network message, field, payload,
parser, version, authentication, authorization, metronome epoch rule, or
ordering changed. The remaining app/control/function audit now selects the next
cohesive ownership boundary; it is not a size-driven `MainWindow` breakup.

Iteration 10 applies that same responsibility rule to Start Jam and Join Jam.
`StartJamDialog` and `JoinJamDialog` now own every visible connection, device,
profile, tuning, diagnostics, and lifecycle control plus profile-driven draft
behavior and typed results. `MainWindow` retains credential generation,
preference persistence, device testing, session authority, runtime launch, and
mesh orchestration. Self-review removed the former second set of roughly forty
permanently hidden Qt controls from `MainWindowPages` and replaced them with a
typed `SessionRuntimeDraft`; the page now constructs only controls it actually
owns. The pre-refactor real-dialog workflow starts one creator and three
joiners with headless fake audio, requires a full four-peer mesh and callbacks,
then leaves through the public action. The expanded modal workflow proves that
Start Cancel discards connection/profile/tuning drafts and Join Cancel retains
only the documented invite draft. It found/fixed BUG-P035 and BUG-T049 through
BUG-T053. Final Windows evidence is the focused real Start/Join gate in 2.43 s,
expanded modal proof in 72.75 s, the complete GUI catalogue at 9/9 in 216.24 s,
and the unchanged-threshold performance/metronome catalogue at 23/23 in
376.29 s. A single earlier listener-compensated/burst-loss run crossed the
10-ms median WAV bound by 24.42 frames; its artifacts were retained, signed
timing/state diagnostics were added, and eight exact reruns plus the complete
matrix passed without changing product behavior or an acceptance bound. No
network protocol message, field, payload, parser, version, authentication rule,
metronome epoch behavior, or ordering changed.

Iteration 11 extracts the Settings workflow as the next genuine `MainWindow`
ownership seam. `SettingsDialog` now owns the transient multi-page editor,
control dependencies, local drafts, validation, and typed accepted result;
`MainWindow` retains device discovery/testing, preference persistence, live
audio application/rollback, and application-wide effects. This removed roughly
1,600 lines of modal construction/orchestration from `MainWindow` without
splitting cohesive code merely for size. Four GUI processes prove representative
controls and dependencies, profile activation, Cancel isolation, Save/reopen
persistence, restoration, and active-jam Settings behavior.

The slice also turned an intermittent four-peer WAV cleanup red into two
product fixes. Track Sync batch expiry now honors the remaining 30-second idle
window rather than restarting a fresh window after late progress. More
importantly, a receiver re-request for the same active `(hash, peer)` restarts
that abandoned stream from byte zero; previously the sender discarded the
recovery request as a duplicate and timed out against a stream the receiver had
superseded. A direct unit regression and repeated real four-peer interruption
runs prove exact bytes, model/view convergence, and zero residual transfer
state. Automated GUI processes now isolate preferences and logs below their
per-peer roots, and the `gui` build target explicitly depends on every test its
CTest label can select. Final Windows evidence is 9/9 GUI tests in 192.89 s and
23/23 performance tests in 375.78 s, including all 21 metronome/epoch mode-by-
impairment cells. No network message, field, header/payload shape, parser,
version, authentication rule, metronome model/epoch rule, or ordering changed.
The next `MainWindow` extraction will again be chosen by responsibility and
ownership, not line count; `PracticeIdeaGenerator` remains cohesive.

Iteration 12 continues the same ownership-driven split with the Local Engine
startup editor. `LocalEngineDialog`, alongside Start/Join in
`SessionStartupDialogs`, now owns its transient hardware/sample/buffer/channel
controls, stable identities, device-test callback, and typed result.
`MainWindow` retains device discovery/preference matching, persistence, warning
lifecycle, and runtime launch. A strengthened four-process pre-refactor test
edits every non-hardware draft plus Save Defaults, cancels, and proves the
independent Settings editor still reads the exact original values. It passed
before extraction in 94.46 s and after extraction/self-review in 94.78 and
93.79 s. The final GUI catalogue passed 9/9 in 177.90 s. No engine, audio-path,
network, metronome, epoch, or protocol behavior changed. The larger input/
MIDI/plugin area remains an audit candidate, but its live host/router/worker
ownership must be designed before any split; size alone is not justification.

Iteration 13 establishes the next responsibility boundary around Audio Inputs.
Before extraction, four real Start/Join processes now select two fake input
channels, inventory the Audio Inputs, MIDI Inputs, and Plugins modals, mutate
include/level state, create and undo a stereo pair, and prove the exact live
`InputSourceRouter` topology and signal on every peer. That proof found a real
automation/product-path defect: synthetic input bypassed source routing, so UI
source controls could not affect fake audio. Headless, Windows test-input, and
macOS test-input paths now feed their preallocated synthetic buffers through
the same lock-free router used by physical input. `AudioInputSourcesDialog`
owns the transient editor, rebuilding, confirmations, stable control IDs, and
typed callbacks; `MainWindow` retains the live source graph, plugin lifecycle,
router attachment, and recording locks. Final Windows evidence is 9/9 GUI
tests in 189.12 s and 23/23 performance tests in 376.44 s, including every
four-peer metronome/epoch impairment cell. MIDI and plugin extraction remains
deferred until an equally coherent lifecycle seam is proven. No network
message, field, payload, parser, version, authentication rule, metronome model,
epoch rule, or ordering changed.

Iteration 14 establishes and extracts the MIDI Inputs responsibility. A small
`MidiInputBackend` seam preserves the real system CoreMIDI/Windows MIDI path
while giving the private GUI agent two deterministic synthetic devices. A
native backend unit proves inventory filtering, exclusive open ownership,
complete channel-voice delivery, validation, close/reopen, bounded queue
capacity, and visible drops. The four-process live-jam workflow proves
discovery Cancel with no late completion, configuration Cancel isolation,
device selection, Standard/MPE mode, include and level edits, close/reopen
persistence, Plugins-view exposure, removal, exact router cleanup, assignment
of the other device into the reclaimed slot, and final teardown on every peer.

`MidiInputSourcesDialog` now owns the transient device panels, asynchronous
discovery progress, nested configuration editor, filtering, stable control
contracts, warnings, and rebuilding. `MainWindow` retains the live source,
device, plugin, queue, router, worker, recording-lock, and retirement state
behind typed callbacks. Transient modal waits use a bounded exact-control
snapshot in one GUI turn; complete paginated inventories remain strict at
stable points and continue failing duplicate or unclassified controls. Final
Windows evidence is 10/10 GUI tests in 227.41 s and 23/23 performance tests in
376.03 s, including WAV interruption/recovery and all 21 metronome/epoch
mode-by-impairment cells. The plugin scanner/loader/editor lifecycle remains
the next candidate and will be extracted only after its asynchronous ownership
has equally direct coverage. No network message, field, payload, parser,
version, authentication rule, metronome model/epoch rule, or ordering changed.

Iteration 15 establishes the isolated input-plugin boundary and completes the
next responsibility-driven `MainWindow` split. `InputPluginBackend` preserves
the existing system VST3 chooser, bounded isolated probe, worker process, and
real-time bridge behind a neutral owned host interface. The private GUI agent
uses a deterministic allocation-free synthetic effect/instrument instead, with
observable audio transform/bypass, MIDI event consumption/mute/reset, editor,
health, retirement, and raw diagnostics. Native units prove valid and invalid
audio/MIDI source shapes, mono and stereo injection, bounded rendering,
diagnostics, editor/bypass/reset/retire behavior, and owner-destruction
cancellation.

The four-process real Start/Join workflow now exercises every audio and MIDI
plugin action on every peer: Add, Open, Bypass, Replace, Remove, global BYPASS,
concurrent-load rejection, MIDI device opening and injected event consumption,
router attachment/cleanup, and source-slot reuse. Delayed loads also race a
stereo regroup and MIDI-source removal, and must retire without attaching to a
changed or removed source. `InputPluginsDialog` owns the transient panels,
progress/busy state, button refresh, stable control contracts, and rebuilding;
`MainWindow` retains hosts, MIDI devices/queues, source routing, worker-pool
dispatch, recording guards, and retirement behind typed callbacks. Roughly 500
obsolete inline UI lines were removed rather than disabled. The strengthened
tests caught and fixed an indeterminately moved callback that terminated MIDI
attachment and a deferred-widget retirement window that briefly duplicated
control IDs. Two consecutive focused four-peer runs and the complete GUI gate
prove the fixes. Final Windows evidence is plugin 2/2 in 0.22 s and GUI 11/11
in 253.37 s. The prior Iteration 14 performance gate remains the current
unchanged-threshold 23/23 metronome/epoch proof because this slice changed no
engine, network, metronome, epoch, wire, authentication, or ordering behavior.

Iteration 16 establishes the first authoritative Windows coverage inventory
and the workflow for closing it without repeatedly paying the full-suite cost.
The latest `--tests-full` command took 2331.6 seconds (38:51.6): its
instrumented pass was behaviorally green 43/43 in 1486.29 seconds, and the
rebuilt normal Release pass was green 43/43 in 759.75 seconds. All 21
metronome/epoch mode-by-impairment cells, both UDP security cases, every
four-peer GUI flow, and the native boundary case passed in both passes. The
command correctly remained red only because the coverage inventory is not yet
closed.

The canonical baseline contains 3009 maintained functions: 782 fully covered,
1479 partially covered, 36 explicitly reviewed exemptions, 712 wholly
uncovered, and 16 functions the Windows collector skipped. All 116 maintained
source files are either observed or explicitly accounted for, with no
unreviewed missing source file. The earlier provisional figure of 804 was not
the final baseline; completing the previously time-limited boundary case
exercised another 92 functions, leaving 712 to classify and close.

All automated temporary state is now rooted at `build/test-artifacts` through
CTest's `JAM2_TEST_ARTIFACT_ROOT`, `TEMP`, `TMP`, and `TMPDIR` environment. A
selected or full test command clears that exact directory before execution,
removes it after complete success, and retains it after failure. This applies
to the CTest process and every child Jam2 peer on Windows and macOS. Focused
coverage outputs are also isolated from the canonical full reports, although
dynamic Windows collection is deliberately not used per component because its
collector startup cost and reliability overwhelm sub-second tests.

The first post-baseline cluster is `jam2_core_boundary_units`. It exercises
previously untouched ring/MIDI reset and diagnostics, downmix helpers,
prepared-source abandonment, recorder statistics, protocol diagnostics and
replay reset, peer-stream/mixer ownership, UDP errors and buffer controls, a
local fake STUN exchange, session access/control/move/close behavior, and an
actual packet rerouted only to a promoted replacement endpoint. Its exact
network-selected test passed in 0.03 seconds after self-review, then the entire
unit aggregate passed 12/12 in 2.18 seconds. No production implementation or
wire behavior changed in this cluster.

Iteration 17 closes the 23-function CLI boundary identified by the canonical
inventory. `jam2_cli_boundary_units` directly proves every help/dispatch path,
channel-list validity boundary, session-key redaction form, peer-stream stats
copy, periodic raw diagnostics, platform error text, and the detach-safe CLI
playback adapter. It also launches the one staged `release/jam2.exe` for a
hardware-free device-argument rejection and a real 150-ms headless local run,
then validates recorded frames and the parsed `recording.json` sidecar. The
playback adapter and OS diagnostic formatter moved unchanged into the cohesive
`CliRuntimeSupport.hpp` seam so their lifetime/error contracts are directly
testable. The exact test passed in 0.33 seconds and the unit aggregate passed
13/13 in 2.49 seconds. No network protocol or product behavior changed.

Iteration 18 closes the small application-infrastructure boundary with
`jam2_application_boundary_units`. Native inherited pipes prove the automation
event queue's exact 128-entry capacity, high-water/rejection counters,
non-draining stop accounting, malformed-frame count, and one-shot disconnect
report. Native TCP loopback proves reservation construction/bind/collision/
error/close/destruction and listener port publication/close. A real headless
Engine proves ApplicationRuntime start/reuse/restart counters; a bounded fake
network worker isolates valid and invalid peer-gain conversion/queueing without
network traffic. Random peer tokens are fixed-shape, distinct, and decode to
usable identities. The exact test passed in 0.05 seconds and the unit aggregate
passed 14/14 in 2.53 seconds. No implementation behavior changed.

Iteration 19 registers the previously unowned private diagnostic entrypoints.
`jam2_debug_entrypoint_units` proves all debug help, accepted and rejected
control/PCM16/PCM24/asset/WAV fuzz inputs, unknown-target rejection, and a real
headless local-network-local lifecycle that starts one Engine and reuses it
twice. `jam2_music_corpus_units` generates one fixed four-bar Funk profile with
six deterministic structural samples, two rendered full mixes, and their drum
WAVs, then parses both manifest and corpus. Exact tests took 2.89 and 1.70
seconds; the reviewed aggregate passed 16/16 in 7.12 seconds. The only product
edit replaces stale help text naming `jam2_test.py` with the native automated
fuzz/replay owner; parser and generated-audio behavior are unchanged.

Iteration 20 adds the standalone GUI model/presentation boundary without a
multi-process launch. `jam2_gui_model_boundary_units` proves BeatGrid editing,
musical/drum subdivision conversion, section ownership and capacity, generated
section replacement, current persistence, mixer diagnostic priority and meter
decay, focus/capture helpers, release-path isolation, and the real nonmodal
invite dialog including clipboard behavior. The reviewed exact test passed in
0.04 seconds and the unit aggregate passed 17/17 in 7.09 seconds. No product or
wire behavior changed. The next focused cluster is synthetic-event coverage of
the standalone BeatGrid, Performance, waveform, and looper-lane widgets.

Iteration 21 closes that standalone widget cluster with
`jam2_gui_widget_boundary_units`. Synthetic Qt input now proves the complete
BeatGrid, Performance Home, waveform, meter, metronome, and looper-track
interaction surfaces, including painted WAV actions, all looper clip drag
modes, stable-ID refresh during a drag, protected interaction, and local-file
drop rejection boundaries. The reviewed exact test passed in 4.04 seconds and
the unit aggregate passed 18/18 in 11.02 seconds. This gives a direct native
owner to all 91 wholly-uncovered functions selected from the canonical widget
inventory. No product or wire behavior changed; two ambiguous dormant looper
ownership paths are recorded in `TEST-REVIEW.md` rather than altered. The next
cluster will be selected from the frozen canonical inventory; no new full or
coverage run is permitted until the remaining planned component work closes.

Iteration 22 closes the 38-function pure-native JamTaster analysis/export
cluster by strengthening `jamtaster_native_units`. It now owns JSON write and
failure paths, WAV mix/crop boundaries, chroma/chord/bass/drum post-processing,
section choice and stem-energy inference, analysis serialization, file hashing,
and complete v3 JamJar creation with four real stem WAVs. Self-review added a
nonuniform eight-beat export that forces bar-anchored stretching and exact
32,000-frame outputs. The reviewed exact test passed in 0.20 seconds and the
unit aggregate passed 18/18 in 11.25 seconds. ONNX model execution, the worker
protocol, and Qt JamTaster service lifecycle remain separate clusters. No
product or wire behavior changed, and no full or coverage run occurred.

Iteration 23 adds a focused staged-model boundary for 37 canonical gaps. Real
CPU inference now proves the ONNX wrapper plus Beat This, Basic Pitch, ADTOF,
and ChordMini tensor, timing, ordering, range, and nonempty musical-output
contracts in under one second. The Demucs entry owns conversion/model-load
failure only; real separation remains with the pipeline/separation slice. The
reviewed exact test passed in 0.87 seconds and the unit aggregate passed 19/19
in 12.16 seconds. Windows initially selected an older System32 ONNX Runtime;
the test now colocates the CMake-imported 1.23 DLL, matching the already-safe
public staged worker layout. No product, model, or wire behavior changed.

Iteration 24 completes real JamTaster separation and pipeline ownership. A
17.90-second exact test loads all four staged Demucs ensemble members and
requires deterministic shifts, precise progress, four finite nonzero aligned
stems, and positive reconstructed-mixture correlation. A separate 2.97-second
pipeline test reuses those proven stems while running every other real model,
every checkpoint/report/export helper, current cache reuse, and malformed-cache
refresh. It directly owns all 13 remaining `Pipeline.cpp` baseline gaps and
upgrades Demucs from load-boundary to real behavior proof. The combined unit
aggregate passed 21/21 in 32.54 seconds. No product, model, or wire behavior
changed, and no full or coverage run occurred.

Iteration 25 closes the 43-function private-process layer. The staged worker
test performs real tempo and complete stem-reusing analysis, cache and alias
flows, index repair, every progress mapping, and structured request/model/ONNX
failures. The Qt service test injects local bundle paths while production path
resolution delegates unchanged, then proves bundle validation, request
ownership, observers, output parsing, success/failure/failed-start/cancel/
destruction, and post-cancel reuse. Self-review found and fixed Windows
`taskkill` output inheritance (`BUG-P040`) without changing termination state.
The unit aggregate passed 23/23 in 35.34 seconds. No public protocol, model,
network, metronome/epoch, or shared-WAV behavior changed.

Iteration 26 closes the 15-row JamTaster dialog boundary with an offscreen
native test that operates every button and result selector and proves chooser,
saved-data, quick/full apply, create continuation, progress, failure, busy,
cancellation, and close behavior through the real Qt service lifecycle. Self-
review found and fixed cross-WAV retained/live result adoption (`BUG-P041`),
incomplete exports being treated as usable converted songs (`BUG-P042`), and a
failed create-after-analysis continuation leaking into a later job
(`BUG-P043`). The final exact dialog test passed in 0.68 seconds, its shared
service regression in 0.34 seconds, and the unit aggregate passed 24/24 in
36.15 seconds. No full or coverage run occurred, and no public worker, network,
metronome/epoch, shared-WAV, audio-device, or four-peer contract changed.

Iteration 27 closes the contained Practice Idea/Research Drum core without
splitting the large generator. One focused native target owns all selector
catalog APIs, deterministic/random generation wrappers, continuation evidence,
every legacy researched-drum source/transient/texture/tail family, embedded kit
lookup, fixed synth-voice allocation, detail banks, determinism, diagnostics,
and bus processing. Of 29 canonical gaps, 27 now have direct behavior and two
provably unreachable fallbacks were removed; three unused internal parameters
and one shadowed secondary-voice name were also cleaned without changing
content or render logic. The reviewed exact test passed in 0.39 seconds and the
unit aggregate passed 25/25 in 36.64 seconds. No full or coverage run occurred,
and no protocol, network, metronome/epoch, shared-WAV, plugin, or four-peer
contract changed.

Iteration 28 closes the Practice Idea modal/controller follow-on in the same
focused target. Offscreen native interaction now proves Generate, Continue,
Reference WAV, and Idea Details cancellation/acceptance, every selector and
availability dependency, teaching/technical toggling, and exact returned
settings. `PracticeIdeaController::generatedSection` has direct exact-kind,
combined-practice, and missing-result proof. Self-review removed one shadowed
drum phrase-plan local and the genuinely unused research-drum sample lane
parameter; generated content and render math are unchanged. The reviewed exact
test passed in 0.41 seconds and the short unit aggregate passed 25/25 in 37.49
seconds. No full or coverage run occurred, and no protocol, network,
metronome/epoch, shared-WAV, plugin, audio-device, or four-peer contract
changed. The remaining Slice 7 function inventory is active.

Iteration 29 closes the project-persistence/transient-WAV ownership cluster.
One offline native test exercises every live coordinator method across project
state, canonical relocation, active/deferred/persistent asset ownership, safe
discard, asynchronous WAV-only cleanup, empty-workspace pruning, and JSON/file
boundaries. It found and fixed `BUG-P044`: Jam2 could atomically save a file
larger than its existing 4 MiB read limit and therefore create a project it
could not reopen. The write boundary is now symmetric. Two duplicate/dead
project-path helpers were deleted because `JamStorage` is already authoritative.
Nine of 11 canonical gaps now have direct behavior and the other two no longer
exist. The exact test passed in 0.05 seconds and the short unit aggregate passed
26/26 in 36.45 seconds. No full or coverage run occurred, and no format,
protocol, network, metronome/epoch, shared-WAV wire, audio, plugin, or four-peer
contract changed. The adjacent recording/capture ownership cluster is next.

Iteration 30 closes the 14-row GUI loopback capture cluster with a real
dependency seam for injected audio. The same recorder thread, accumulator,
format decoder, resampler/trimmer, diagnostics result, and PCM16 WAV writer used
by Windows capture now run deterministically without a physical device. Exact
PCM16/24/32/Float32, non-finite, trim, duration/peak, lifecycle, stop, reuse,
failure, and exception contracts are covered. Self-review fixed completion-
observer process termination (`BUG-P045`), non-atomic/narrow-path WAV output
(`BUG-P046`), and unsafe non-finite integer conversion (`BUG-P047`). The exact
test passed in 0.07 seconds and the short unit aggregate passed 27/27 in 36.60
seconds. Real WASAPI endpoint capture remains hardware-profile work; the
existing empty-WAV success policy is unchanged and listed as `REVIEW-003`. No
full or coverage run occurred, and no ASIO, protocol, network, metronome/epoch,
shared-WAV wire, plugin, or four-peer contract changed. Track recording state/
scheduling is the next adjacent cluster.

Iteration 31 closes the 14-row Track Recording Workflow cluster. A narrow
submit/snapshot seam now drives exact command, timing, transport, capture,
lane, transient-WAV, completion, rejection, and jam-recorder lifecycle tests,
plus one real Headless Engine tone-to-WAV completion under
`build/test-artifacts`. Self-review fixed duration-overflow ordering, owned
asynchronous rejection rollback, stale take-completion correlation, partial-
start WAV ownership, numeric timing boundaries, serialized jam transitions,
and blank destination validation (`BUG-P048` through `BUG-P054`). The final
exact case passed in 0.61 seconds and the short unit aggregate passed 28/28 in
38.06 seconds. The take ID is local engine/GUI state only; no wire protocol,
network, metronome/epoch, shared-WAV, device, plugin, or four-peer contract
changed. The remaining Slice 7 canonical function inventory is active.

Iteration 32 closes the platform-neutral audio callback-processing cluster.
One allocation-free `noexcept` seam is now used by Windows ASIO, macOS
CoreAudio, and the Headless fake device for callback timing, peaks, gains,
monitoring, prepared-source mixing, resampled playback, synthetic injection,
metronome rendering, and output clipping. The new focused native target covers
the math and state boundaries directly and drives a real threaded Headless
Engine through input, monitoring, network playback, gain, mixing, and timing.
Self-review fixed signed interpolation overflow, unsafe extreme metronome frame
math, invalid synthetic-render conversions, final-frame beat-index wrap,
unrepresentable interval thresholds, and Headless/physical gain-and-meter
divergence (`BUG-P055` through `BUG-P060`). The final exact target passed 1/1 in
0.60 seconds and the short unit aggregate passed 29/29 in 38.52 seconds. The
frozen inventory's 47 Windows audio-device rows mix callback processing with
hardware-only ASIO ownership; the common behavior is now directly covered,
while device enumeration/open/format/lifecycle remains explicit hardware-
profile work and is not falsely counted as closed. No protocol, network,
metronome model/epoch rule, shared-WAV wire, plugin, or four-peer contract
changed. The remaining Slice 7 canonical function inventory is active.

Iteration 33 closes the SessionController reactive-command and transport-grid
cluster. One checked core seam now owns raw/musical conversion and next-bar
count-in schedules, replacing unsafe duplicated arithmetic and an unused CLI
scheduler. A new direct unit target covers ordinary, offset, epoch, meter,
count-in, invalid, and exhausted frame domains. A new integration target drives
exactly four staged Jam2 processes through a real direct mesh with fake audio,
correlated invalid/gain/delayed-snapshot commands, advance publication of a
future `RecordStart`, exact one-bar schedule adoption, active count-in on all
four peers, bounded queues, manifest validation, and clean reactive shutdown.
Self-review fixed collapsed countdown/target ownership, extreme arithmetic,
delay addition overflow, rejected top-level scheduled snapshots, and transport
publication with no advance notice (`BUG-P061` through `BUG-P065`). The final
exact four-peer test passed in 5.50 seconds; focused transport, Track Recording,
and audio/metronome regressions passed; and the short unit aggregate passed
30/30 in 38.25 seconds. `REVIEW-004` records the separate fixed-raw-target versus
later render-offset-slew product question. No wire field, header, payload,
parser, version, authentication rule, or compatibility behavior changed. No
full or coverage run occurred. The remaining Slice 7 canonical function
inventory is active.

Iteration 34 closes the live JamStorage/shared-track workspace-state cluster.
The new `jam2_workspace_state_units` target owns exact build-local tests for
new/renamed/saved/external jam roots, all asset folders, artifact state, take
naming, recursive empty-workspace pruning, safe discard, exact opened-project
path retention, managed project/file renames, collision refusal, every live
shared-track playback phase/status transition, and looper-lane rename bounds.
It found and fixed `BUG-P066` through `BUG-P068`: a selected JamJar path could
be replaced by an inferred title path, changing an externally opened title
could attempt to move the complete user-selected parent folder, and a managed
rename could leave the old JamJar filename behind. Storage now owns the exact
selected project file, treats only the canonical Jam2 songs root as movable,
and moves a managed folder and project filename together. Path identity is
case-insensitive on Windows and case-sensitive elsewhere. One unused worker
wait wrapper was removed; `REVIEW-005` records two unreferenced legacy
`track.processing` helpers instead of removing a possible protocol surface
without approval. The final focused test passed in 0.04 seconds and the short
unit aggregate passed 31/31 in 38.67 seconds. No full or coverage run, JamJar
JSON change, network message/field/parser/version change, metronome/epoch
change, or shared-WAV wire change occurred. The remaining Slice 7 canonical
function inventory is active.

Iteration 35 closes the GUI metronome transport/controller clock boundary.
The new `jam2_metronome_transport_controller_units` target directly owns tap
tempo reset/median/range/extreme timestamps, PlaybackGrid pattern/rate/engine
state/interpolation/clear behavior, controller submit/rejection/exception
containment, local/remote mutation gating, recording-schedule revision
ownership, checked render-offset projection, invalid rates, and engine-reset
reuse. `BUG-P069` through `BUG-P072` fix the last controller copy of unsafe
`INT64_MIN`/raw-frame offset math, signed tap timestamp subtraction, running
grid frame wrap, and non-finite sample-rate conversion. The controller now
uses the same checked transport conversion as the engine/session paths;
PlaybackGrid clamps to the maintained metronome/rate domain and saturates
elapsed frame advancement. A narrow injectable submit callback makes the
non-real-time controller testable while the production constructor still
delegates to `ApplicationRuntime`. The reviewed exact case passed in 0.04
seconds, transport timing passed in 0.02 seconds, Track Recording passed in
0.61 seconds, and the short unit aggregate passed 32/32 in 37.59 seconds. No
full or coverage run, protocol change, metronome model/epoch-rule change, or
network behavior change occurred. The remaining Slice 7 canonical function
inventory is active.

Iteration 36 closes the TrackWorkspaceSupport WAV-staging and lane-merge
boundary. The new `jam2_track_workspace_support_units` target directly proves
strict Unicode-path PCM16 metadata, exact and rate-converted atomic staging,
SHA-named repair/idempotence, already-owned recordings, the 8--384 kHz
endpoints, invalid destination/rate rejection, synchronized conflict and
authoritative-removal behavior, and repeated hashless local-only merges.
`BUG-P073` through `BUG-P076` reject unsupported requested rates and unsafe
staging destinations, propagate a mid-hash file read failure, and prevent a
path-only local WAV from multiplying on every arrangement merge. Self-review
added both valid rate endpoints and the zero-rate source-preservation
contract. The reviewed exact case passed in 0.06 seconds and the short unit
aggregate passed 33/33 in 38.04 seconds. No full or coverage run, wire/schema
change, shared-WAV transfer change, metronome/epoch change, audio callback
change, or network behavior change occurred. The remaining Slice 7 canonical
function inventory is active.

Iteration 37 closes the shared Section-bank preparation barrier and quantized
launch-clock ownership cluster. `SharedBankLaunchCoordinator` replaces five
interdependent readiness fields in `MainWindow`, snapshots exactly the peers
that received a prepare, removes departures explicitly, and immediately
cancels a pending switch if a new peer authenticates before content/WAV
synchronization can be proved. Its direct native target covers all readiness
orders, duplicates, stale/self/nonmember input, four-peer completion,
departure, cancellation, next-beat/bar bounds, valid/invalid clock domains,
epoch projection, and saturation. The real four-peer fake-audio performance
test now queues Section B from a joiner and Section A from the creator while
global playback runs; all four peers converge and retain their exact epoch
frames. `BUG-P077` through `BUG-P079` close unstable barrier membership,
unchecked frame arithmetic, and invalid-clock quantization. The reviewed exact
unit passed in 0.04 seconds, the reviewed four-peer case passed in 15.21
seconds, and the short unit aggregate passed 34/34 in 37.83 seconds. No full or
coverage run and no `bank.*` message name, field, shape, validator, parser,
version, authentication, or other wire change occurred. `REVIEW-006` and
`REVIEW-007` retain protocol-sensitive validation/commit questions for user
approval. The remaining Slice 7 canonical function inventory is active.

Iteration 38 closes the LooperProject lane-edit and Section-shortening
ownership cluster. Local lane creation, gain, mute, solo, rename, region, WAV
clear, timeline-end, and destructive crop operations now pass through checked
model methods; Section shortening stages the complete project mutation before
commit and preserves a looped lane's source loop while shortening only its
timeline placement. Removed placements report their content identity so an
unreferenced in-flight WAV transfer is cancelled after the atomic commit.
Direct model and Section boundary tests cover invalid/non-finite/extreme state,
rate conversion, crop/clear/unchanged outcomes, serialization, and saturation.
The real four-peer GUI smoke covers gain/mute/solo visible state, and the modal
matrix covers Rename Lane Cancel and Save on every peer. `BUG-P080` through
`BUG-P084` and `BUG-T091` are closed. The reviewed exact model passed in 0.03
seconds, Section/widget boundaries in 4.05 seconds, the final modal workflow in
99.70 seconds, and the short unit aggregate passed 34/34 in 37.85 seconds.
`REVIEW-002` records three additional dormant MainWindow wrappers, while
`REVIEW-008` records the maintained Section-duration versus prepared-renderer
limit mismatch. No full/coverage run, persistence parser/schema change,
network message/field/parser/version change, shared-WAV wire change, or
metronome/epoch change occurred. Prepared-mix/project-lifecycle ownership is
the next cluster.

Iteration 39 closes prepared-mix request, generation, cache, playback-intent,
and derived-file ownership. The non-widget `PreparedMixLifecycle`, owned by
`TrackWorkspaceController`, replaces eleven aliased/cache fields spread across
`MainWindow` and rejects completions that no longer own the current project
generation. It also owns exact coalescing priority, per-bank cache identity,
active adoption, failure counters, one-shot play intent, managed relocation,
and retryable obsolete paths. `BUG-P085` through `BUG-P088` close stale PCM
adoption after project replacement, inactive/missing cache leaks, invalid worker
result bank/metadata acceptance, and forgotten cleanup failures. The reviewed
exact lifecycle target passed in 0.03 seconds, the real renderer/application
boundary in 0.03 seconds, the exact four-peer prepared-bank workflow in 15.05
seconds, and the short native unit aggregate passed 35/35 in 38.21 seconds.
No full/coverage run, renderer duration-limit change, wire/schema change,
network behavior change, or metronome/epoch change occurred. The remaining
live application/action tail audit is next.

Iteration 40 closes the shared-track loop and project-state ownership
subcluster. `SharedTrackController` now owns bounded start/end edits,
whole-track/reset/disable transitions, overflow- and non-finite-safe effective
frame conversion, the unchanged persisted track shape, and atomic checked
project decoding. Opening a JamJar validates the candidate track before the old
workspace is destroyed; a missing track object now installs clean state instead
of retaining the previous project. Automation exposes exact model, effective
loop, prepared-cache, and engine prepared-source data. The exactly-four-peer
fake-audio workflow stops and restarts real prepared WAV playback, then drives
Clear, Start, End, disable, re-enable, and final Clear on every process.
`BUG-P089` through `BUG-P092` and `BUG-T092` are closed. The reviewed exact
workspace test passed in 0.06 seconds, the corrected four-peer test passed in
18.89 seconds, and the short native unit aggregate passed 35/35 in 38.40
seconds. No full/coverage run, persistence field/shape change, network message/
field/header/payload/parser/version/authentication change, shared-WAV wire
change, metronome-model change, or epoch-rule change occurred. The remaining
file/action tail audit is active; one planned implementation cluster remains
before the final gate, subject to any coherent gap found by that audit.

Iteration 41 closes that final file/action and JamJar-asset cluster.
`LooperAssetMaterializer` owns validated, collision-safe, rollback-capable WAV
materialization without changing the persisted project shape. Checked lane
replacement now gives asynchronous import, recording, and compatibility
conversion one atomic model boundary and explicit superseded-file ownership.
Save verifies the complete candidate before promoting an unsaved workspace and
clears storage artifact state only after acceptance. The exactly-four-peer
shared-content workflow saves and reparses all four isolated WAV-heavy JamJars,
requiring relative paths and exact hashes; the GUI smoke and modal workflows
close live Section add/remove and Export-scope Cancel behavior. `BUG-P093`
through `BUG-P100` and `BUG-T093` through `BUG-T096` are closed. The reviewed
workspace target passed in 0.08 seconds, LooperProject in 0.03 seconds, GUI
smoke in 5.12 seconds, GUI modal in 113.58 seconds, shared content in 19.81
seconds, and the unit aggregate passed 35/35 in 38.05 seconds. `REVIEW-002`
records the final dormant-wrapper classification. No protocol, persistence
shape, shared-WAV wire, metronome-model, or epoch-rule change occurred. Slice 7
is complete; the single final two-pass Windows distribution gate is now the
only planned cluster, with a repair iteration added only if that gate exposes a
real regression or an unreviewed maintained-code gap.

Slice 8's pre-gate audit found two close-out mismatches before an authoritative
pass could be claimed. The old Python `validate` and `stress` command families
were still publicly dispatched despite native parity, and the documented
`--hardware-profile` option had no build-script/CMake implementation even
though a real-device plugin executable existed. The superseded Python runners,
their private scenario/verdict/policy stack, and their tests are being retired
while benchmark, analysis, connectivity, fuzz, and reusable impairment tooling
remain. The optional hardware command surface will be wired only after the
currently running build exits so an edited batch/CMake graph cannot invalidate
that process. `BUG-T097` records the separate interrupted-gate file lock.

Iteration 42 completes the repair work exposed by that first gate. The explicit
hardware profile now drives the production VST3 backend/worker/editor and ASIO
device 5, including invalid-channel rejection followed by a clean reopen. The
hardware test passes. Production MIDI, controller binary/rejection, practice,
preference, CLI routing, and extracted DetailSectionEdit boundaries pass their
focused tests. The exactly-four-peer active workflow now covers the remaining
generated-idea, destructive Section, reference-WAV sharing, looper-region,
New Jam, and dirty-window close interactions and passes in 93.19 seconds.
Collector-skipped, dormant, and external-hardware functions have exact reviewed
exemptions; no live source directory is broadly exempted. The only planned work
left is the replacement two-pass gate and a narrowly scoped repair iteration if
its fresh report identifies a genuinely unreviewed function.

Iteration 43 owns the 60-function tail reported by the replacement gate. Real
four-peer GUI actions now cover the remaining MainWindow coordination paths,
including deterministic fake-device startup/restart and a real build-local WAV
through an injected loopback backend. Named helpers own device UI, connection
guidance, interaction policy, sidecar parsing, and JamTaster section creation.
The four-peer native session test covers both requested runtime completion and
the existing coordinator `session.error` path. Exact residual exemptions are
limited to reviewed dormant UI, private catalogue invariants, and unavailable
physical/driver callbacks. Focused tests pass. One fresh Windows instrumented
catalogue plus the checker is now the only coverage task; its report must show
zero unreviewed missing, wholly uncovered, and collector-skipped functions
before the final full distribution command can be expected to succeed.

The fresh instrumented catalogue passed all 76 behavioral cases in 2,019.95
seconds and reduced the maintained inventory to one reported uncovered
function with no missing or collector-skipped source. That final row is the
MSVC-emitted out-of-line copy of the implicit-inline
`NetworkCommandController::handleRuntimeFinished(int)` callback. The same body
is measured at its requested-shutdown call site and is proved observably by
the exactly-four-peer test's three clean manifests and one propagated exit-code
4 manifest, so it has one exact compiler-shape exemption rather than a source,
network, or controller exemption. Rechecking the existing XML passed with zero
unreviewed missing, wholly uncovered, or collector-skipped functions.

Iteration 44 follows up the distribution run's four-performance failure. The
retained peer logs proved a late prepared-mix replacement race rather than a
slow timeout: two peers committed global Play while their replacement WAV was
loaded unscheduled, leaving transport playing with the prepared source stopped.
Prepared-attach planning now belongs to `TrackRecordingWorkflow` and keys from
the typed pending/running global-transport state, not the one-shot render UI
intent. Focused pending/running ordering tests and the exact four-peer
performance integration pass; the existing timeout remains strict.

Iteration 45 closes two later metronome-matrix failures without changing the
metronome or epoch models. Impaired-network mixer recovery is now judged by an
exact, bounded output-equivalent rate rather than requiring a designed recovery
counter to stay at zero; clean networking remains zero-drop. Windows loopback
reservations now select a UDP-valid ephemeral port before proving the same
number for TCP, avoiding protocol-specific excluded ranges. The dual-protocol
support unit and both exact failed four-peer cases pass.

Iteration 46 makes hidden GUI-agent mode process-wide. Every Qt top-level
window is marked `WA_DontShowOnScreen` at polish/show time and native file
dialogs are disabled, while all GUI integration launchers now propagate the
existing `--show-gui` opt-in consistently. No automated desktop-visibility
assertion was added at the user's request; the existing real four-peer modal
workflow passes and final on-screen/off-screen behavior is manually verified.

Iteration 47 extends that same hidden-presentation boundary to platform alert
audio. Hidden GUI-agent message boxes retain their text, controls, modality,
and response behavior but clear the alert icon before Qt requests its system
sound. Explicit `--show-gui` runs retain normal production icons and sounds.

## Intended command surface

```text
compile.cmd
compile.sh
compile.cmd --tests <suite>
compile.sh --tests <suite>
compile.cmd --tests <suite> --test-name <exact-ctest-name>
compile.sh --tests <suite> --test-name <exact-ctest-name>
compile.cmd --tests-full [--show-gui] [--hardware-profile <path>]
compile.sh --tests-full [--show-gui] [--hardware-profile <path>]
```

Implemented suite names are `unit`, `plugin`, `gui`, `jam-sync`,
`shared-content`, `performance`, `network`, and `full`. The `network` selector
uses an exact CTest label and therefore does not accidentally run the separate
`network-impairment` metronome matrix.

The ordinary commands configure `BUILD_TESTING=OFF`. Test commands configure it
on and build only the selected suite target. GUI tests are hidden by default;
`--show-gui` makes the same tests visible. Hardware is included only when an
explicit profile is supplied.

## Coverage and completion

The replacement full Windows gate will run one instrumented pass and then rebuild and
rerun against the normal staged Release executable. Maintained functions,
controls, actions, views, messages, and critical transitions must be covered or
appear in an explicitly reviewed exemption. Every uncovered item is emitted
for review; new unreviewed gaps fail the gate. Run from the repository root:

```text
compile.cmd --in-dev-shell --tests-full --hardware-profile build\hardware-profile.json
```

Its reports supersede the 172-uncovered/42-skipped baseline. A passing gate
must report zero unreviewed missing sources, wholly uncovered functions, and
collector-skipped functions, then pass the optimized Release catalogue with
the same hardware profile.

Windows completion does not claim macOS completion. The exact later macOS work
is maintained in `TEST-MACOS.md`. Source coverage is a Windows-only
distribution gate: macOS `compile.sh --tests-full` runs the normal Release test
catalogue without PowerShell, instrumentation, or an Apple coverage substitute.

## Iteration 48 overview

All previously answered review items are now either implemented or retained as
directed, and `TEST-REVIEW.md` contains only `REVIEW-008`. Approved dead looper,
track-processing, and session-default paths were removed. Empty/silence-only
loopback capture now fails visibly without creating/importing an asset or
leaving a completed-capture path. The
approved bank request bound and exact prepared-bank commit match are enforced
without changing emitted protocol shape. Focused units, the exactly-four-peer
performance transition, and the MSVC Release build pass. The remaining design
work is the unified Section/recording/render duration and memory-allocation
contract; no limit was changed in this iteration.
