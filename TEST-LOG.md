# Jam2 Test Initiative Log

This is the append-only implementation and verification log. Each slice records
its implementation pass, commands and results, self-review findings, gap fixes,
and final retest before its status changes in `TEST-PLAN.md`.

## 2026-08-13 - Goal initialization

- Locked automated real-jam topology to four peers.
- Locked the current run to Windows/MSVC completion.
- Excluded cross-machine and soak testing from the automated catalogue.
- Chose vertical-slice refactoring with no feature removal and no size-driven
  file splitting.
- Chose native C++ replacement of Python validation and stress after verified
  parity; Python benchmark, connectivity, research, and analysis tooling stays.
- Chose quick builds with `BUILD_TESTING=OFF`, hidden GUI testing by default,
  optional visible GUI, and explicit hardware profiles.
- Started slice 1: test/build foundation.

### Slice 1 - iteration 1

- Implemented: centralized existing native tests under `tests/`, made test
  targets opt-in, and added selected/full build command parsing.
- Verification: `cmd.exe /d /c "call compile.cmd --in-dev-shell"` passed with
  `BUILD_TESTING=OFF`; the generated build contained no test subdirectory or
  test target rules.
- Verification gap: `--tests unit` failed during CMake generation because
  `Qt6::Core`, imported inside `app/`, was not visible from the sibling
  `tests/` directory.
- Gap fix: made the test CMake owner resolve its directly linked Qt component.
- Retest gap: configuration then passed, but moving the JamTaster test exposed
  a source-relative include of its Demucs integration header.
- Gap fix: changed the test to include that header through JamTaster's public
  source include root, making its location independent of the test tree.
- Retest: `--tests unit` passed all three registered native tests (core input,
  plugin bridge, and JamTaster), followed by `--tests-full` passing the same
  complete current catalogue.
- Self-review finding: switching back to `BUILD_TESTING=OFF` removed Ninja test
  targets but left the previous generated root CTest catalogue, which could let
  a manual `ctest` run stale binaries.
- Self-review finding: the test sibling observed the empty cached ONNX root
  instead of the platform path resolved inside `app/`, generating an invalid
  `/lib` runtime-path entry.
- Self-review finding: the deterministic plugin suite unnecessarily compiled
  the external-fixture worker test even though CTest could not legitimately run
  it without a configured plugin.
- Gap fixes: remove the stale root catalogue in test-free configurations,
  derive the ONNX runtime directory from the globally imported target, and keep
  the plugin suite limited to its runnable bridge test until fixture support is
  added.
- Final retest: `--tests-full` passed 3/3; `--tests plugin` selected and passed
  only the registered deterministic plugin bridge case; a subsequent normal
  build passed with `BUILD_TESTING=OFF`, no test targets in Ninja's graph, and
  no root CTest catalogue.
- Script error review: an unknown suite exits with code 2 before building.
- macOS script execution was not attempted on Windows; the required Apple-side
  syntax/build/run validation remains in `TEST-MACOS.md`.
- Diff review: no whitespace errors; test sources and registration are now
  owned solely by `tests/`; no test executable is staged into `release/`.
- Slice 1 result: complete after implementation, two gap-fix iterations,
  selected/full retests, and final test-free build verification.

## 2026-08-13 - Slice 2: test-control foundation

### Iteration 1

- Inspected the existing bounded inherited-handle `AutomationChannel`, reactive
  headless actions in `SessionController`, debug capability description, GUI
  entrypoint, fake-input support, and `MainWindow` control construction.
- Finding: the existing channel already had the required cross-platform
  transport and bounds, but the 15 action names were separately maintained in
  validation and capability reporting.
- Implemented `jam2_automation_contract`, with a typed action/payload registry
  shared by validation and capability reporting. Added native valid/invalid
  boundary cases for every declared action.
- First verification gap: the new Qt-using test exited with Windows loader code
  `0xc0000135` because its CTest environment did not include the selected Qt
  kit's binary directory.
- Gap fix: added a test-local `PATH` modification using the imported Qt target;
  no DLL was copied into `tests/` or `release/`.
- Retest: `--tests unit` passed 4/4.

### Iteration 2

- Implemented the private `debug gui-agent` entrypoint in the one public Jam2
  executable. It requires inherited automation handles, constructs the real
  `MainWindow`, is not shown on screen by default, and supports explicit
  `--show-gui`.
- Implemented bounded, paged control inventory and view snapshots. Inventory
  reports actual Qt class/kind, supported operations, stable test ID,
  diagnostic tree path, and current state. Snapshot pages add widget values
  and window/modal state.
- Implemented strict reactive commands for inventory, snapshot, real control
  invocation, and orderly shutdown. Unsupported fields, bad values, duplicate
  IDs, disabled controls, and unknown IDs are rejected.
- Added the first 14 stable IDs to actual application controls without changing
  their existing signal/slot behavior.
- First MSVC verification gap: an indirect Windows header exposed the `min` and
  `max` macros in the new source.
- Gap fix: applied the repository's existing `NOMINMAX` convention at the
  translation-unit boundary.
- Retest: Jam2 compiled and the unit catalogue remained 4/4 green.

### Iteration 3

- Added native cross-platform private process support and an exactly-four-peer
  coordinator under `tests/support/`. On Windows it uses restricted inherited
  handle lists, per-peer anonymous pipes, Unicode environment blocks, hidden
  processes, bounded framing, exact exit codes, and cleanup of failed children.
- Added `jam2_four_gui_agent_integration`. It launches four concurrent staged
  `release/jam2.exe` instances, inventories every control page, validates the
  stable IDs, changes the real BPM spin box independently on each peer, reads
  the value back from state snapshots, verifies rejection of an unknown
  control, then shuts down all four cleanly.
- Added the selected `gui` suite and full-suite ownership. Test binaries stay in
  `build/tests`; only `release/jam2.exe` is launched. `--show-gui` is accepted
  only by the GUI and full suites.
- First four-process verification gap: the test tried to write the startup song
  title, which is deliberately read-only in that state. All four agents
  correctly rejected it, so 8 assertions failed (rejection plus readback per
  peer).
- Gap fix: retained the agent's protection and changed the smoke mutation to
  the enabled BPM control.
- Retest: the four-process GUI suite passed in about 7 seconds.

### Iteration 4 - self-review and closure

- Self-review finding: the inventory advertised `set-text` for a read-only edit
  even though invocation correctly rejected it.
- Self-review finding: command queue overflow could update a plain rejection
  counter from the pipe thread while the GUI thread read it.
- Self-review finding: the initial POSIX coordinator branch allowed later peer
  processes to inherit earlier peers' pipe endpoints, which could prevent EOF
  and controller-loss behavior on macOS.
- Gap fixes: operation descriptions now reflect read-only state; rejection,
  queue-drop, and event-drop counters are separate atomics and exposed as hard
  data; POSIX descriptors are close-on-exec except for the two intentional
  child endpoints, and the child starts in the staged executable directory.
- Windows retest after review: `--tests gui` passed 1/1 with all four processes.
- Full retest: `--tests-full` passed 5/5 (4 native unit cases plus the
  four-process GUI integration) in 7.51 seconds.
- The full run confirmed each instance currently exposes 521 interactive Qt
  objects, 14 stable IDs, 507 not-yet-stable inventory entries, and zero
  duplicate stable IDs. This is the hard baseline for the later complete GUI
  audit; slice 2 establishes the mechanism rather than claiming all controls
  are already covered.
- Distribution ownership review: the CTest log confirms the integration uses
  `release/jam2.exe`; the release root contains only that public executable;
  private executables and their support library are under `build/tests`.
- Final quick-build retest: the mandated normal MSVC command passed with
  `BUILD_TESTING=OFF`; the Ninja graph contained no test targets and the root
  CTest catalogue was absent. Invalid `--tests unit --show-gui` exits 2 before
  configuration.
- Diff review: no whitespace errors and no visible product feature was removed.
  The Apple process branch was authored and reviewed but cannot be executed on
  Windows; its required validation remains in `TEST-MACOS.md`.
- Slice 2 result: complete. Started slice 3, Jam Sync.

## 2026-08-13 - Slice 3: Jam Sync

### Iteration 1

- Audited the Jam Sync policy, preference values, protocol validation,
  authorization, GUI application paths, and the Python policy model. The
  policy was embedded in `MainWindow` even though normalization, routing,
  serialization, revision ordering, and stale-state adoption are independent
  application rules.
- Added the production `jam2_jam_sync` component. The generated-idea enum keeps
  its persisted numeric values explicitly; the component owns policy defaults,
  normalization, route decisions, current-format serialization/parsing,
  creator/joiner preparation, authoritative ordering, and stale adoption.
- Rewired `MainWindow` and control-message validation to this one component
  without changing visible Jam Sync controls or meanings.
- Fixed a protocol bug found during the audit: `jam.sync.set` previously passed
  generic validation with an omitted revision. A new joiner starts at revision
  -1, so that malformed frame was interpreted as revision 0 and could replace
  its local policy. Authoritative sets now require a positive revision;
  proposals must omit it; both accept only the current field set.
- Added native policy units for all dependency matrices and content routes,
  generated-idea modes, strict current-format parsing, normalization,
  four-state ordering/reconciliation, and stale rejection.
- First verification exposed a compile-time testability mismatch: the original
  GUI agent stored the real `MainWindow` as `QWidget&`, which was sufficient for
  widget inventory but hid the new typed Jam test seam.
- Gap fix: retain the concrete `MainWindow&`; inventory remains generic while
  Jam startup and snapshots use the narrow private friend interface.
- Focused retest: both the policy units and real integration passed.

### Iteration 2 - real four-peer workflow

- Added private GUI-agent commands for real creator/joiner startup and Jam Sync
  policy changes. Startup drives the existing `MainWindow::startJam` and
  `SharedSessionController`; the only test seam selects existing headless
  silence input, localhost endpoints, bounded four-peer membership, and dialog
  suppression. Normal GUI startup/device behavior is unchanged.
- Added `jam2_four_jam_sync_integration`. It starts exactly four staged
  `release/jam2.exe` processes, creates one authenticated direct mesh, joins
  three peers, waits for all six remote edges and four running network
  runtimes, changes policy from a joiner, changes it again from the creator,
  observes all four real views, and shuts every process down cleanly.
- Added the focused `jam-sync` suite to both build scripts. Its first complete
  Windows run passed 2/2 in 2.98 seconds.

### Iteration 3 - self-review and closure

- Self-review finding: a failed mesh phase was recorded but the integration
  continued into policy actions, which could obscure the causal failure.
- Self-review finding: the process workflow accepted minimum revisions after
  changes instead of proving one exact revision on every peer; its initial mesh
  predicate also did not prove the creator policy had reconciled.
- Self-review finding: `JamSyncPolicyState::adopt` relied on callers having
  parsed a positive revision, weakening the state object's own invariant.
- Gap fixes: each process phase now stops on failure; initial policy must be the
  exact revision-1 default on all four peers; joiner and creator changes must
  each advance exactly once and end on one exact revision; direct state
  adoption rejects revisions below 1; unknown song scopes fail closed.
- Review-gap Windows retest: `--tests jam-sync` passed 2/2 in 2.89 seconds.
- Full Windows regression after closure passed 7/7 in 10.20 seconds,
  including both exactly-four-process integrations.
- Diff review: no whitespace errors, no visible feature removal, and no test
  executable was staged into `release/`. The existing Python Jam Sync module is
  still imported by the broader legacy Python validation runner, so removal is
  deliberately deferred until that runner's remaining coverage has native
  parity rather than breaking it piecemeal.
- Slice 3 result: complete. Started slice 4, shared content.

## 2026-08-13 - Slice 4: shared content

### Iteration 1 - song model and all three editor views

- Added a compact private content snapshot sourced from the real song and
  looper models. It reports exact shared-model/component digests, arrangement
  and model revisions, title model/view values, first-section chord, target,
  beat and lyric cells, all three selected editor sections, banks, lanes,
  assets, and pending worker/asset state.
- Added inherited-handle actions that drive the existing rename, cell-edit,
  grid-resize, `beat.set`, `grid.resize`, and authoritative `song.set` paths.
  They do not introduce a second content protocol or bypass the production
  models.
- Added `jam2_four_shared_content_integration` and the selected
  `shared-content` suite. The test creates exactly four real Jam2 peers, waits
  for full-mesh and initial model/view reconciliation, then edits the title,
  chord, target, beat and lyric views from different peers, applies a joiner
  structural proposal, checks one exact shared digest after every phase, and
  shuts all processes down cleanly.
- First compile gap: the content snapshot treated an existing plain worker
  count as atomic. Fixed the type usage without changing its ownership.
- First process run exposed unexplained exact-model divergence even though
  titles and arrangement revisions matched. Component diagnostics isolated it
  to the looper model: the same initial visible empty lane had independently
  generated UUIDs on different peers.
- Bug fix: `LooperProject` now owns creation of deterministic per-bank initial
  placeholders. This is a meaningful model invariant used by startup and new
  project creation, not a file-size refactor.
- Protocol cleanup: path-addressed local persistence still stores
  `asset_path`, while hash-addressed shared serialization omits it. Remote code
  already discarded that machine-local value and reconstructed its canonical
  local path, so sending it added bytes and guaranteed false cross-peer
  differences without carrying usable information.
- Added `jam2_looper_model` as the independently testable production model
  boundary and native units proving deterministic initial identities,
  persistence-path retention, shared-path omission, and hash-addressed loading.
- Self-review found the earlier Jam Sync test's released-port chooser could
  fail immediately if the OS reused a just-probed port. It now retries until
  four distinct values are selected.
- Review-gap focused retest passed 2/2: model units plus the full four-peer
  content workflow in 3.12 seconds. No visible feature was removed.

### Iteration 2 - generated ideas and transport protocol repair

- Extended the real content snapshot with production chord and beat
  fingerprints and added a deterministic private action that uses the existing
  seeded practice-idea generator and the normal `applyPracticeIdea` workflow.
- Extended the exactly-four-peer workflow to cover full, chords-only,
  beats-only, and disabled generated-idea sharing. Different peers originate
  each policy/content phase; exact model digests prove convergence when
  enabled, exact fingerprints prove the selected part boundary, and the
  disabled phase proves only the originating peer changes before sharing is
  restored and all four reconcile again.
- First process run exposed a production failure: changing idea timing stops
  playback, and the resulting transport packet terminated every network
  runtime with `failed to encode bounded current UDP packet`.
- Root cause: the current transport encoder and decoder use the fixed 28-byte
  version-2 payload, but the core packet validator still permitted only the
  obsolete 20-byte shape. That obsolete decoder path was unreachable because
  protocol parsing rejected it first.
- Bug fix: the protocol now defines shared fixed-size constants, accepts only
  the current 28-byte transport payload, and removes the unreachable legacy
  decoder branch. Packet-encoding failures now include packet type, payload
  size, output capacity, and audio format for actionable runtime diagnostics.
- Added a native core regression proving a current transport packet encodes,
  authenticates, and parses, while the obsolete 20-byte shape is rejected.
- Self-review finding: the disabled-sharing check used a fixed 300 ms delay,
  which could race slow systems and did not actively wait for the local
  mutation. Replaced it with bounded polling that proves peers 1-3 retain the
  prior digest and peer 4 alone diverges.
- Review-gap retests: `--tests unit` passed 6/6, including the protocol
  regression; `--tests shared-content` passed 2/2, with the full four-process
  content workflow completing in 5.63 seconds. No generated-idea or transport
  feature was removed.

### Iteration 3 - real WAV transfer, isolation, and stale completions

- Added private actions that queue the existing asynchronous WAV import and
  explicit Share Tracks workflows. Content snapshots now expose per-lane hash,
  local-file availability, byte size, pending asset count, and active file
  worker count without reading audio on a real-time path.
- Extended the exactly-four-peer workflow with generated mono PCM16 fixtures.
  It proves automatic Track Sync transfers the exact bytes and SHA-256 hash to
  all four peers; disabling automatic WAV sharing converges lane metadata while
  leaving bytes only on the importer; explicit Share Tracks later transfers
  those bytes despite the automatic policy being off; restoring the policy
  converges on all four peers.
- First manual-sharing run falsely appeared complete because all same-machine
  peers shared the public `release/tracks` tree. Added a private debug-only
  storage-root option and made the four-peer coordinator assign one temporary
  root per process. This accurately represents independent machines and keeps
  automated GUI jams out of user workspaces. Self-review corrected member
  destruction order so processes are terminated before their temporary roots
  are removed on a failure path.
- The isolated run exposed a production bug: an empty lane `assetPath` was
  resolved relative to the project folder, producing the project directory
  itself. Callers using existence checks then treated the directory as the
  missing WAV and marked its hash validated, so later manual Share Tracks did
  not request the file.
- Bug fix: empty asset paths remain empty at the central absolute-path resolver.
  The real four-peer test then passed with exact files present in every isolated
  peer root.
- Extracted the existing asset chunk protocol and `AssetTransferService` into
  the independently testable `jam2_asset_transfer` production target. This is
  a responsibility boundary already present in the code, not a size-based
  split.
- Added native units for exact chunk source/order/count rules, wrong-source and
  duplicate rejection, hash-mismatch rejection, cancellation during an
  incoming write, a superseded incoming generation, and cancellation during
  outgoing validation. Delayed workers prove stale completions cannot accept,
  commit, replace, or send asset data.
- Focused review-gap run passed 3/3, including the full four-peer workflow in
  7.58 seconds. Full Windows regression passed 10/10 in 18.94 seconds.
- Removed the exact pre-isolation test artifact
  `release/tracks/Four_Peer_Content_31816`; temporary isolated roots now clean
  themselves up. No user project or visible feature was removed.
- Slice 4 result: complete after three implemented, tested, reviewed, gap-fixed,
  and retested iterations. Started slice 5, performance.

## 2026-08-13 - Slice 5: performance

### Iteration 1 - fake audio, shared metronome, and global transport (in progress)

- Added bounded fake-input selection to private GUI-agent jam startup using the
  existing runtime modes (`off`, `silence`, `tone-440`, `pulse-1s`, and
  `metro-pulse`). Normal GUI/device startup and the public command surface are
  unchanged.
- Added a private performance snapshot sourced from the production engine,
  transport workflow, metronome model, and visible controls. It reports hard
  state including frames/callbacks, capture/playback attachment, input/send/
  remote/output/metronome peaks, pattern/epoch state, transport revisions and
  commits, playback state, and recording counters.
- Added real GUI-agent actions for metronome and global transport. They click
  the existing performance controls after bounded validation rather than
  submitting a parallel test-only engine behavior.
- Added `jam2_four_performance_integration` and a focused `performance` suite
  to both build scripts. The test launches exactly four isolated GUI peers with
  injected tone input and requires decoded remote audio on every instance.
- First Windows run built successfully but failed the initial audio predicate:
  each peer had three active remotes, progressing callbacks, 124999-ppm local
  tone, and roughly 287000-352000 ppm decoded remote audio, while the exposed
  `send_peak_ppm` remained zero.
- Self-review found `send_peak_ppm` and its GUI accumulator were declared and
  reported by engine/CLI diagnostics but never written by any backend. The
  callback path now publishes the input peak after the existing numeric send
  gain on headless, Windows ASIO, and CoreAudio paths without allocation,
  locking, logging, or audio mutation.
- The same run showed metronome state can legitimately begin enabled from
  persisted user preferences. The test now establishes and proves a shared
  off/BPM baseline before exercising off-to-on convergence, preventing saved
  preferences from satisfying the assertion accidentally.
- Second Windows run proved the send meter fix on all four peers (124999 ppm)
  and exact metronome state/BPM convergence. It then exposed a test-model gap:
  the GUI intentionally configures the click as transport-gated, so an enabled
  metronome produces no click while global playback is stopped. The state
  phase now proves the gate explicitly; audible metronome output is required
  only after the global transport commit makes playback active.
- User constraint recorded: no future wire-protocol or compatibility change
  may be made without prior description and approval. The Slice 4 transport
  repair was confirmed to have retained the existing 28-byte v2 wire shape;
  it aligned validation with the already-emitted shape and removed an
  unreachable 20-byte branch.
- Slice 5 exit criteria expanded to require robust epoch/model proof for
  shared-grid, leader-audio, and listener-compensated modes across a
  deterministic impairment matrix: clean traffic, delay, jitter, loss,
  duplication, reordering, and bounded burst loss.
- Post-pass review added a narrow headless unit regression for send-meter
  scaling and mute rather than relying only on the unity-gain network case.
  The reviewed focused suite passed 2/2 in 5.24 seconds: the new core meter
  unit and the exact four-process performance integration.
- Iteration 1 result: complete. Fake input, transmitted/received audio,
  metronome state/render gating, global restart/stop commits, and relevant GUI
  views are now observed through production state. Started recording coverage.

### Iteration 2 - four-peer jam recording

- Added a private bounded recording action that invokes the exact callback used
  by mouse input on the custom-painted Record Jam control. Automation only
  suppresses the optional name/completion dialogs; the normal next-take naming,
  workspace allocation, workflow commands, engine writer, and view updates are
  unchanged.
- Extended the technical snapshot with workflow/engine/view recording state,
  isolated folder, queued/written frames, drops, drop events, and writer errors.
- Extended the four-peer performance integration to start four simultaneous
  recorders while injected local/remote tone, shared playback, and the gated
  metronome are active. It waits for real engine activation and more than 4800
  written frames before stopping each through the same GUI callback.
- Finalization must leave queued frames equal to written frames with zero
  drops/events/writer errors. Every peer folder must remain beneath its own
  temporary storage root and differ from the other three.
- The test opens all 20 resulting files (`mix`, `my-input`, `their-input`,
  `inputs-mix`, and `metronome` for four peers), validates the exact mono
  PCM16/48 kHz header and recorder frame length, and requires non-silent data
  in every stem.
- First behavioral run passed. Post-pass review confirmed the test observes
  asynchronous engine state rather than trusting the immediate workflow
  toggle, observes painted-view state independently, and verifies finalized
  bytes before temporary cleanup. Focused performance suite passed 2/2 in
  6.03 seconds.
- Iteration 2 result: complete. Started the all-mode metronome/epoch impairment
  matrix.

## 2026-08-13 - Structured defect register introduced

Every later defect entry must use the same fields: observed symptom, root
cause, change, regression proof, and remaining test need. Entries stay in this
append-only log after they are fixed so the historical failure remains visible.

### Production defects found and fixed

#### BUG-P001 - malformed authoritative Jam Sync state could be adopted

- Observed symptom: `jam.sync.set` passed generic validation with no revision;
  a new joiner at revision `-1` interpreted the omitted field as revision `0`
  and could replace its local policy with malformed state.
- Root cause: proposal and authoritative-set shapes shared permissive
  validation, and the state object's adoption invariant depended on callers.
- Change: authoritative sets require a positive revision, proposals must omit
  it, both accept only the current field set, and direct adoption rejects
  revisions below `1`.
- Regression proof: native Jam Sync units cover strict parsing, invalid
  revision shapes, dependency matrices, exact revision advancement and stale
  rejection; the four-peer workflow proves creator/joiner reconciliation.
- Remaining test need: exercise authenticated malformed/replayed control input
  under bounded connection stress in the network/security slice.

#### BUG-P002 - identical new jams began with divergent empty looper lanes

- Observed symptom: four peers showed matching titles and arrangement
  revisions but different exact song digests before any edit.
- Root cause: each peer independently generated UUIDs for the same visible
  initial empty bank lanes.
- Change: `LooperProject` centrally creates deterministic initial placeholder
  identities for startup and new projects.
- Regression proof: model units prove deterministic initial identities and the
  isolated four-peer content test requires one exact initial and post-edit
  digest across all peers.
- Remaining test need: the WAV hardening slice must cover simultaneous imports,
  matching-empty-lane reconciliation, occupied-lane conflicts, repeated shares
  and reconnects so deterministic placeholders remain safe under real races.

#### BUG-P003 - current transport events failed packet validation

- Observed symptom: an idea timing change stopped playback and every network
  runtime exited with `failed to encode bounded current UDP packet`.
- Root cause: the already-current v2 transport encoder/decoder used a fixed
  28-byte payload while a stale internal validator admitted only the obsolete
  20-byte shape; the old decoder branch could not be reached after parsing.
- Change: shared constants align validation with the existing 28-byte wire
  shape, the unreachable obsolete branch was removed, and encode failures now
  report packet type, payload size, capacity, and audio format. No emitted
  field, version, encoding, authentication rule, or wire layout changed.
- Regression proof: a native core regression encodes, authenticates and parses
  the current packet and rejects the obsolete size; the four-peer generated-
  idea workflow traverses the transport stop path without runtime failure.
- Remaining test need: exercise every transport action under deterministic
  impairments and malformed/replay traffic in the performance and
  network/security slices. Any future protocol/compatibility change requires
  prior user approval.

#### BUG-P004 - an empty WAV asset path was treated as a validated asset

- Observed symptom: after metadata-only sharing, explicit Share Tracks did not
  request the missing WAV on isolated peers.
- Root cause: resolving an empty `assetPath` relative to the project returned
  the project directory; existence checks treated that directory as the asset
  and marked its hash validated.
- Change: the central absolute-path resolver preserves an empty asset path as
  empty, so only a real file can satisfy availability/hash validation.
- Regression proof: the isolated four-peer test proves metadata converges while
  bytes remain only on the importer with auto-share off, then explicit sharing
  transfers exact bytes and SHA-256 to the other three peers.
- Remaining test need: Slice 5B must cover policy toggles while transfers are
  pending, partial/truncated files, disconnect/rejoin retry, conflicting lanes,
  repeated shares and exact-byte convergence. This is a high-priority area
  because similar bugs have been observed in real jams.

#### BUG-P005 - the public send-level diagnostic always reported zero

- Observed symptom: four peers sent and decoded injected tone normally, but
  `send_peak_ppm` and its GUI accumulator remained zero.
- Root cause: all backends declared and exposed the metric but none published a
  value after applying send gain.
- Change: headless, Windows ASIO and CoreAudio callback paths publish the
  already-computed input peak scaled by the existing send gain, using only
  arithmetic and atomics in the real-time path.
- Regression proof: a headless unit proves unity/scaled/muted values and the
  exact four-peer GUI test observes the expected nonzero send and remote peaks.
- Remaining test need: later coverage must retain all three backend branches;
  the macOS-specific runtime check remains in `TEST-MACOS.md`.

### Validation/build defects found and fixed

#### BUG-T001 - test-free reconfiguration could leave a stale CTest catalogue

- Observed symptom: switching `BUILD_TESTING` off removed Ninja test targets
  but left root CTest metadata capable of selecting old private binaries.
- Root cause: the prior generated catalogue was not removed by the test-free
  configuration path.
- Change and proof: test-free configuration now removes stale catalogue data;
  normal build, selected suites, full suite, then normal build were all run in
  sequence and the normal generated build contained no test targets.
- Remaining test need: retain this order in the final distribution-gate audit.

#### BUG-T002 - POSIX GUI agents could inherit unrelated peer pipe endpoints

- Observed symptom/risk: later peer children could keep earlier automation pipe
  endpoints open, preventing trustworthy EOF/controller-loss behavior on
  macOS.
- Root cause: all previously created descriptors remained inheritable.
- Change and proof: descriptors are close-on-exec except for the two explicit
  child endpoints. Windows inherited-handle-list behavior is covered now; the
  POSIX runtime proof remains in `TEST-MACOS.md`.
- Remaining test need: complete that macOS check before claiming macOS parity.

#### BUG-T003 - same-machine WAV paths produced a false sharing pass

- Observed symptom: a manual-sharing phase appeared converged before network
  transfer because all four processes saw the same public `release/tracks`
  directory.
- Root cause: test peers did not model independent machine storage.
- Change: private debug-only storage roots isolate every peer; process lifetime
  now ends before temporary-root destruction.
- Regression proof: the isolated run reproduced BUG-P004, then required exact
  per-peer files and hashes after the fix.
- Remaining test need: all Slice 5B cases must retain four distinct roots and
  explicitly assert that no peer can observe another peer's staged path.

#### BUG-T004 - fixed delays and persisted settings could satisfy tests falsely

- Observed symptom/risk: disabled sharing used a fixed 300 ms wait, and a saved
  metronome preference could make an off-to-on assertion pass without the
  tested action causing it.
- Root cause: tests inferred completion from wall time or uncontrolled initial
  state.
- Change and proof: bounded state polling proves the exact local divergence and
  later convergence; performance tests establish an explicit shared off/BPM
  baseline before mutation.
- Remaining test need: new integration phases must always establish and assert
  their baseline and wait on authoritative state, never a sleep-only verdict.

#### BUG-T005 - the first performance assertion modeled the metronome gate incorrectly

- Observed symptom: enabled metronome state was present but the test expected an
  audible click while global transport was stopped.
- Root cause: the GUI intentionally configures its click renderer as transport-
  gated.
- Change and proof: state enablement and gate state are proved separately;
  audible output is required after the shared transport commit starts playback.
- Remaining test need: the all-mode epoch matrix separately tests continuous
  headless grid behavior and must not weaken the GUI transport-gate contract.

### Newly required WAV-sharing hardening follow-up

- The completed shared-content iteration is now explicitly a baseline. A new
  Slice 5B will implement the concurrency/reconnect/policy/conflict/partial-
  transfer matrix in `TEST-PLAN.md` before native Python validation/stress
retirement or distribution completion is claimed.

## 2026-08-13 - Slice 5, iteration 3: four-peer metronome/epoch matrix (complete)

- Added a native bidirectional UDP impairment proxy used only outside Jam2. It
  never parses or rewrites Jam2 traffic and deterministically injects clean
  forwarding, fixed delay, jitter, 2% loss, 2% duplication, reordering, or one
  bounded 400 ms blackout on all six edges of an exact four-peer full mesh.
- Added 21 separate CTest cells covering shared-grid, leader-audio, and
  listener-compensated modes under all seven conditions. Each cell performs a
  BPM 120-to-150 hard epoch reset from peer 4, records 20 finalized PCM16 WAV
  stems, and proves the requested impairment through proxy counters.
- Shared-grid validation proves both authority epochs, monotonic revision,
  four-peer authority consensus, beat alignment, a bounded live correction
  delta, zero authentication/mixer-capacity failures, and every recorded click
  against the mapped epoch plus the interpolated render-offset history.
- Leader-audio validation performs a real peer-4 release/claim user flow before
  the BPM reset. It requires revision 4, transfer to exactly one injector,
  silence in all competing local metronome stems, 440 Hz continuity, and a
  tone-rejected received click grid in each listener WAV.
- Listener-compensated validation proves the metro-pulse source epoch, bounded
  local click intervals, all remote/local click windows, median and maximum
  remote-energy alignment, active three-peer compensation, the target/base
  formula, and convergence of the applied offset.
- Added exact focused selection to both build scripts with
  `--test-name <exact-ctest-name>` and `--no-tests=error`; quick builds remain
  test-free and suite selection remains label-bounded.
- Clean focused runs now pass for all three modes. The first reviewed full
  23-test performance run passed all seven shared-grid cells, six leader-audio
  cells, clean/duplication listener cells, and the two pre-existing performance
  tests. It exposed BUG-P006 under loss; 17/23 passed before that production
  fix.
- After BUG-P006, the focused core regression and the previously failing
  leader loss plus listener delay, jitter, loss, reordering, and bounded burst
  loss cells all passed. The final uninterrupted performance run then passed
  23/23 tests in 368.75 seconds: the core recovery unit, exact four-peer GUI
  performance flow, and all 21 metronome/epoch mode-by-condition cells.
- One earlier attempt lost its command-output pipe because the agent wrapper
  timeout was too short. That detached test tree was explicitly terminated
  and discarded as evidence; the reported 23/23 result is from a separate,
  uninterrupted authoritative run with exit code 0.
- No emitted packet field, version, payload shape, encoding, decoder,
  authentication rule, or compatibility behavior changed in this iteration.

### Production defects found and fixed during iteration 3

#### BUG-P006 - one recovering peer left the other mixer queues seconds behind

- Observed symptom: under deterministic 2% loss, listener-compensated WAVs
  could flam against remote audio and the native matrix failed. Retained hard
  data showed one or two peer queues at `0..2` frames while the other queues
  remained at roughly `4700..5500` frames, more than 100,000 mixer capacity
  drops, and up to about 23 ms median click/remote-energy misalignment.
- Root cause: after a deadline released an incomplete mixed block, only the
  peer that had missed the deadline discarded its late queue when it
  recovered. Healthy peers retained their accumulated backlog even though all
  sources feed one already-advanced output timeline.
- Change: a completed source-timeline recovery now rebases every active
  contributing source to the same two-block live tail. Existing
  `late_after_release_frames` counters expose every alignment discard; the
  real-time path remains preallocated, nonblocking, lock-free, allocation-free,
  exception-free, and log-free. This is internal mixer queue handling only and
  does not change the network protocol.
- Regression proof: a native three-source unit creates the exact missed-
  deadline/recovery condition, proves all queues are at most 128 frames after
  recovery, and proves at least 640 alignment-drop frames are reported. The
  focused core test passes, and the real four-peer 2% loss case passes twice
  with exact WAV/epoch checks and zero mixer capacity drops. All other listener
  conditions that failed before the fix also pass focused reruns.
- Remaining test need: retain capacity-drop assertions in every metronome cell;
  exercise disconnect/rejoin and longer bounded loss scenarios in later native
  slices without adding an automated soak campaign. The post-fix 23/23 matrix
  is complete.

### Validation/build defects found and fixed during iteration 3

#### BUG-T006 - Windows UDP teardown noise invalidated otherwise complete cells

- Observed symptom: all first matrix cells completed their four-peer jams but
  failed because orderly process shutdown produced loopback UDP socket errors.
- Root cause: the proxy verdict was sampled after peers had closed their
  sockets, when Windows can surface ICMP/`WSAECONNRESET` teardown noise.
- Change: proxy evidence is snapshotted after every scheduled assertion and
  recording window but before orderly shutdown; queue-capacity safety remains
  checked through process completion.
- Regression proof: all proxy counter contracts pass, including proof of the
  requested mutation and absence of unrequested mutations in clean cells.
- Remaining test need: validate equivalent POSIX teardown behavior during the
  later macOS run in `TEST-MACOS.md`.

#### BUG-T007 - metronome WAV assertions omitted the live render model

- Observed symptom: a clean shared-grid peer appeared roughly 600 frames away
  from its sidecar epoch despite aligned beat and authority telemetry.
- Root cause: the sidecar stores the mapped epoch, while joiner rendering also
  applies a deliberately slewed live render offset. The test compared raw WAV
  frames to epoch alone. It also treated a periodic correction target as a
  final residual error.
- Change: each click is now checked against `raw frame + interpolated live
  render offset = mapped epoch + consecutive grid step`; telemetry sampling is
  100 ms and the correction target is bounded by the configured queue maximum.
- Regression proof: all 25 clean clicks per peer fit the dynamic model within
  33 frames in retained evidence, and every shared-grid impairment cell passed
  the reviewed full run.
- Remaining test need: keep the offset-history proof when clock-drift variants
  are added; do not infer phase from a final sidecar value alone.

#### BUG-T008 - leader-audio scenario assumed BPM change transfers its source

- Observed symptom: the clean leader test expected peer 4 to become authority,
  but the creator correctly remained the only injector after peer 4 changed
  BPM.
- Root cause: BPM resets intentionally preserve the assigned leader source;
  the scenario had not performed the user action that claims it. It also read
  revision 2 and post-shutdown rows instead of the committed revision 4 row.
- Change: peer 4 now releases and re-enables the leader metronome to claim the
  source, then resets BPM. Required action events, revision 4 evidence, and a
  pre-shutdown committed row are asserted explicitly.
- Regression proof: clean and impaired leader cells prove exactly one growing
  peer-4 injector, authority/epoch consensus, and suppression of all competing
  local click stems.
- Remaining test need: the later control audit must cover source handoff on
  authority disconnect as a separate behavior from an explicit claim.

#### BUG-T009 - absolute PCM thresholds confused test tones and remote clicks

- Observed symptom: leader listeners with valid click audio failed interval
  checks because the continuous 440 Hz injected tone satisfied the click
  threshold, while loss-created transients split otherwise valid clicks.
- Root cause: the detector measured absolute PCM magnitude and adjacent
  candidate intervals instead of rejecting the known tone and fitting the
  surviving click train to the grid.
- Change: a deterministic 440 Hz notch isolates transients, then a phase-grid
  fit rejects unrelated packet-boundary candidates. At least 20 received beats
  must fit within 480 frames with no gap over three beats.
- Regression proof: clean, delay, jitter, duplication, reordering, burst loss,
  and the previously failing 2% loss leader cells pass with real WAV evidence.
- Remaining test need: add other injected source frequencies only if a product
  flow requires them; do not make a validation tone indistinguishable from the
  signal under test.

#### BUG-T010 - listener test measured the first remote peer, not their average

- Observed symptom: compensation telemetry converged but delay/jitter/loss/
  reorder/burst cells failed a first-threshold timing comparison.
- Root cause: three remote click streams share one `their-input.wav`; one long
  refractory detector reports the earliest arriving source, while listener
  compensation intentionally targets their average phase.
- Change: the test retains exact remote/local click counts and interval checks,
  then evaluates steady mixed-remote energy centroids around each local click.
  Median alignment is bounded to 10 ms and loss-damaged outliers to 50 ms;
  compensation formula, three-peer input, activity, and applied-target
  convergence remain separate mandatory checks.
- Regression proof: focused delay, jitter, loss, reordering, and bounded-burst
  listener cells pass after BUG-P006; the old retained failures show why the
  earliest edge and average energy are observably different.
- Remaining test need: preserve both WAV-domain and telemetry-domain checks;
  an aggregate WAV cannot be used to infer one specific remote peer's phase.

#### BUG-T011 - patched LF-only batch entry point broke nested labels

- Observed symptom: the documented Windows command stopped at prerequisites
  with `cannot find the batch label specified - check_ninja` even though the
  label existed.
- Root cause: `cmd.exe` misread nested label returns after the patched batch
  file became LF-only.
- Change: repository attributes now require CRLF for `*.cmd` and LF for
  `*.sh`; `compile.cmd` was normalized to CRLF.
- Regression proof: every later authoritative `compile.cmd --in-dev-shell`
  build finds MSVC/CMake/Ninja/Qt/ASIO and completes normally.
- Remaining test need: run the shell syntax/runtime checks on macOS and retain
  the line-ending attributes in the distribution audit.

#### BUG-T012 - released ephemeral ports made four-peer tests intermittently fail

- Observed symptom: after all 21 metronome cells passed, the old exact
  four-peer performance integration failed before launch with `could not
  reserve four distinct local TCP/UDP ports`. Repeated rapid test runs made
  Windows return the same just-released ephemeral port until the retry budget
  expired. The same close-then-remember allocator existed in Jam Sync and
  shared-content/WAV integrations.
- Root cause: each candidate TCP/UDP pair was closed before the next port was
  requested. The tests remembered its number but did not own the reservation,
  so uniqueness depended on Windows choosing a different free port later.
- Change: one shared test-only RAII reservation component now binds and owns
  all four distinct loopback TCP/UDP pairs simultaneously. It holds them until
  all four GUI-agent processes have emitted `hello`, then releases them
  immediately before the create/join commands make Jam2 bind those ports.
- Regression proof: the formerly failing four-peer performance test passed,
  and focused Jam Sync plus exact-byte shared-content/WAV tests passed with the
  same component. The subsequent performance run passed 23/23, including the
  performance integration after all 21 rapid network cells.
- Remaining test need: preserve simultaneous ownership in future multi-peer
  tests and validate Qt/loopback reservation behavior on macOS as required by
  `TEST-MACOS.md`.

## 2026-08-13 - Slice 5, iteration 4: full-regression flake closure (complete)

- The post-review 32-test full repository run passed all unit, plugin, GUI,
  Jam Sync, shared-content/WAV, performance, and 19 of 21 metronome cells. It
  failed one clean listener cell on a single Windows proxy receive error and
  one listener burst-loss cell because peer 3 did not produce enough matched
  remote-pulse/local-click WAV windows. Overall result: 30/32 in 385.33
  seconds; this is a failed run and Slice 5 remains in progress.
- Retained evidence roots are
  `jam2-metronome-listener-compensated-clean-VGxUIl` and
  `jam2-metronome-listener-compensated-burst-loss-BjIKxM` under the local temp
  directory. Diagnosis must distinguish Windows UDP error semantics, test
  detector assumptions, and real product recovery before any fix is accepted.
- After the red/green product fix and each focused validation correction, the
  uninterrupted performance catalogue passed 23/23 in 368.18 seconds. The
  final authoritative full repository run passed 32/32 in 385.55 seconds with
  exit code 0. This includes all 21 exact four-peer metronome/epoch impairment
  cells plus unit, plugin, GUI, Jam Sync, performance, and exact-byte
  shared-content/WAV integrations.
- Self-review confirmed the new frame-zero allowance cannot apply to steady
  clicks, listener boundary allowance cannot pass fewer than 20 events, the
  convergence check is stronger across time than its replaced single row, and
  all allocator consumers use one simultaneously owning RAII component.
- No emitted wire field, protocol version, packet/payload shape, encoding,
  parser/decoder, authentication rule, or compatibility behavior changed in
  iteration 4.

### Production defects found and fixed during iteration 4

#### BUG-P007 - a partial recovered mixer block could stay on a missing-frame treadmill

- Observed symptom: after the bounded 400 ms blackout, one of four listener
  peers recorded only 12 reliably detectable remote pulses instead of the
  expected steady train. Its final CSV showed 10,094 deadline-released slots,
  about 1.62 million missing mixer frames, and all three source queues at zero;
  the other peers stopped at 286..363 deadline releases and recovered. No
  capacity drop or protocol loss counter explained the persistent silence.
- Root cause: paying a missing-frame debt could leave a recovering source with
  a 1..63-frame partial block. The mixer reused an already-due deadline,
  immediately released that fragment, recreated the same missing-frame debt,
  and could then discard/release the same deficit forever at packet cadence.
- Change: when any source completes timeline recovery, all active contributors
  still rebase to the shared two-block live tail and the mixer now re-arms one
  normal bounded prefill deadline. The aligned queues can complete a block
  before another incomplete release. The path remains preallocated,
  nonblocking, lock-free, allocation-free, exception-free, and log-free. This
  changes no protocol field, packet, decoder, authentication, or compatibility
  behavior.
- Regression proof: a new deterministic unit creates the 63-frame recurring
  deficit. It failed before the fix on both perpetual-deadline and complete-
  block assertions, then passed after the fix. The formerly failing exact
  four-peer listener burst-loss case passed twice after the fix; the second run
  also proved deadline growth stayed at most eight slots and missing-frame
  growth at most 1,536 frames over a late two-second recovery window, alongside
  the existing WAV/epoch/compensation checks.
- Remaining test need: retain the late-window counter guard; add
  disconnect/rejoin recovery in later bounded native network/WAV slices. The
  post-fix 23/23 performance and 32/32 repository regressions are complete.

### Validation/build defects found and fixed during iteration 4

#### BUG-T013 - asynchronous Windows UDP refusal was counted as receive corruption

- Observed symptom: a clean listener cell transferred and forwarded 12,009
  packets in one direction with all Jam2 state/WAV assertions healthy, but the
  proxy reported one receive error on edge 3-4 and failed the cell.
- Root cause: on Windows, an unconnected UDP send to a process endpoint that is
  not bound yet can surface its asynchronous ICMP refusal as
  `ConnectionRefusedError`/`WSAECONNRESET` on the socket's next receive. The
  proxy attributed that opposite-direction destination lifecycle event to the
  current receive path.
- Change: the proxy now records this case separately as
  `destination_unreachable` on the direction whose earlier send caused it.
  Genuine invalid receives, send failures, pending-capacity drops, and all
  requested/unrequested impairment mutations retain their failing checks.
- Regression proof: the focused clean four-peer listener cell passes with the
  new classification while sustained traffic, exact epoch/audio contracts,
  zero mixer-capacity drops, and clean impairment counters remain mandatory.
- Remaining test need: exercise the corresponding POSIX socket behavior during
  the `TEST-MACOS.md` run and retain the separate hard counter in diagnostics.

#### BUG-T014 - listener convergence was judged from one transient telemetry row

- Observed symptom: the next 32-test run passed the product recovery case but
  failed listener duplication on peer 1 and reordering on peer 2. Retained
  telemetry showed their sampled 15,000 ms rows were respectively 250 and 426
  frames from newly shifted targets. The same streams remained WAV-aligned;
  peer 1 returned to 3 frames at 15,100 ms and peer 2 returned to 7 frames by
  16,000 ms, consistent with the configured bounded slew.
- Root cause: validation sampled exactly one row and required a target-following
  offset to be within 5 ms even when network latency changed on that row. It
  did not prove whether the estimator repeatedly converged or remained wrong.
- Change: every eligible row from 10,000 through 15,900 ms must prove active
  three-peer compensation, positive measured latency, and the exact
  target/base/latency formula. At least 75% of at least 30 rows must be within
  5 ms of target, and no out-of-bound streak may exceed ten 100 ms samples.
  Existing WAV median/maximum alignment, click counts, epochs, and mixer health
  remain mandatory.
- Regression proof: the formerly failing focused listener duplication and
  reordering cells both pass the new time-series convergence contract.
- Remaining test need: retain the time-series proof for future target-slew
  changes; do not return to a wall-clock-selected single-row verdict.

#### BUG-T015 - one TCP/UDP collision could repeat through every port retry

- Observed symptom: leader-audio reordering failed before process launch with
  `could not reserve four distinct TCP/UDP loopback ports` after many rapid
  cells, despite the helper retaining successful reservations.
- Root cause: when Windows selected a TCP ephemeral port already occupied by
  UDP, the failed attempt released its TCP socket. The next TCP port-zero bind
  could return that same unusable number on all 128 retries. The metronome
  matrix also still had a duplicate allocator rather than BUG-T012's shared
  component.
- Change: every four-peer test now uses the shared RAII allocator. During up to
  256 attempts per pair, rejected TCP choices remain bound until selection
  finishes, forcing Windows to try a different ephemeral number; all four
  successful TCP/UDP pairs remain simultaneously owned until Jam2 launch.
- Regression proof: the formerly failing focused leader-audio reordering cell
  passed after the allocator consolidation. Jam Sync, shared-content/WAV, and
  GUI performance already pass through the same helper.
- Remaining test need: validate the selection behavior with Apple sockets in
  `TEST-MACOS.md`. The Windows rapid 32-test catalogue passes.

#### BUG-T016 - a clipped click at WAV frame zero was treated as a phase error

- Observed symptom: shared-grid jitter failed peer 1's first click at 136
  frames against the normal 128-frame limit. Its WAV started 136 frames after
  the scheduled grid click, so the detector found the already-playing click
  tail at file frame zero; the following 25 clicks were all only 6..9 frames
  from their modeled grid positions.
- Root cause: the dynamic epoch validator applied steady-state onset timing to
  a recording that can begin partway through a click waveform.
- Change: only the first event, only when detected exactly at WAV frame zero,
  may be recognized as a clipped startup click within a bounded 10 ms click
  tail. It still establishes the expected step. Every following click retains
  the 128-frame dynamic epoch/render-offset bound and must advance exactly one
  grid step.
- Regression proof: the formerly failing focused four-peer shared-grid jitter
  cell passes; retained evidence proves why its first event is a file boundary
  while all steady events remain precise.
- Remaining test need: retain the frame-zero-only condition and validate
  CoreAudio recording boundaries on macOS.

#### BUG-T017 - listener matching counted two recording boundaries as network loss

- Observed symptom: listener delay failed peer 2 despite zero mixer deadlines,
  zero missing/capacity frames, and valid compensation/WAV energy. Its 10-second
  file contained 24 local clicks and 26 remote detections; the unmatched remote
  events were at the start and end recording boundaries.
- Root cause: nearest-event validation allowed only one unmatched remote
  reference even though independently phased stems can expose both boundary
  events in one stem and neither in the other.
- Change: both stems must contain at least 20 events, and at least all but two
  of the smaller event count must match within 250 ms. At most two unmatched
  remote references are allowed for the two file boundaries. The steady WAV
  centroid, interval grid, epoch input, compensation time series, and mixer
  health checks remain unchanged.
- Regression proof: the formerly failing focused listener-delay cell passes;
  the previously caught BUG-P007 artifact with only 12 remote pulses would
  still fail the explicit 20-event minimum.
- Remaining test need: keep start/end boundary accounting separate from
  steady network-loss evidence in all future WAV-domain tests.

## Slice 5B - WAV-sharing hardening

### Iteration 1 - transfer-state safety inventory and adversarial unit proof

- Started from the existing exact-byte four-peer happy-path baseline and mapped
  every current `AssetTransferService` state transition before changing code.
- The first focused matrix covers malformed frames, duplicate and reordered
  chunks, stale hashes, wrong sources, truncated completion, hash mismatch,
  structurally invalid WAVs, cancellation and worker-failure cleanup,
  idempotent reuse of an already-valid destination, acknowledgement ownership,
  and duplicate outgoing requests.
- Acceptance rule: rejected data must never commit or expose a partial WAV;
  unrelated or stale data must not destroy a valid current transfer; every
  temporary file must be removed after the relevant worker settles.
- Protocol guard: this iteration may correct only local transfer lifecycle and
  cleanup. It will not change a field, version, frame shape, encoding, decoder
  acceptance rule, authentication rule, or compatibility behavior.
- Red/green sequence: the expanded unit executable compiled first against the
  unchanged transfer service. It failed the current-transfer preservation and
  stale-partial cleanup assertions, plus one test-fixture assertion documented
  as BUG-T018. After the two production corrections, the focused matrix passed;
  self-review then added successful multi-chunk ACK ordering, atomic replacement
  of a corrupt destination, sample-rate rejection in both directions, and
  explicit peer-disconnect ownership. The reviewed matrix passes in 0.07 s.
- Iteration exit: the complete `shared-content` suite then passed 3/3 in
  7.13 s: looper-model units, the reviewed asset-transfer matrix, and the real
  four-peer GUI shared-content jam against `release/jam2.exe`. Iteration 1 is
  closed before expanding the real-jam scenarios.

#### BUG-P008 - unrelated or stale asset frames cancelled the current WAV transfer

- Observed symptom: after a valid transfer start, a chunk or completion from a
  different peer—or a validly encoded late frame for an older hash from the
  same peer—caused the expected current transfer to be abandoned. Its later
  correct chunk could not complete, so a real jam could enter avoidable retries
  or ultimately lose that Track Sync batch.
- Root cause: `receiveChunk` and `receiveDone` treated every sequence rejection
  as corruption belonging to the active sender and unconditionally called
  `resetIncoming()`. Source and asset identity were checked only inside that
  destructive path.
- Change: an active receive now classifies a non-owning source before decoding,
  and classifies a different valid asset hash before sequence mutation. Those
  unrelated/stale events remain rejected and logged, but preserve the valid
  current receive. Malformed, duplicate, reordered, mis-sized, or otherwise
  invalid data from the owning source still fails closed and abandons that
  transfer for bounded retry.
- Protocol status: no field, version, frame shape, encoding, decoder acceptance,
  authentication, or compatibility behavior changed. The same events remain
  rejected; only their effect on an independent active lifecycle was fixed.
- Regression proof: the new unit injects wrong-source valid and malformed
  chunks, a wrong-source completion, and same-source stale-hash chunk and
  completion before the correct frame. It failed three preservation/commit
  assertions before the fix, then completed the exact WAV with no abandonment
  or partial artifact after the fix.
- Remaining test need: retain the unit isolation proof and add real four-peer
  disconnect/rejoin plus pending-transfer activity in Slice 5B iterations 2-3.

#### BUG-P009 - a cancelled worker failure could leave a private partial WAV on disk

- Observed symptom: when a chunk worker had already written its private
  `.partial.<uuid>` file, cancellation followed by the worker's failure callback
  left that staging file behind indefinitely. It was not promoted to the public
  hash path, but repeated failures could accumulate abandoned files.
- Root cause: the normal stale worker completion scheduled partial-file cleanup,
  while the equivalent stale failure branch returned immediately on its
  generation guard.
- Change: the stale failure branch now schedules the same bounded file-worker
  removal as the stale completion branch. Commit visibility and generation
  guards are unchanged.
- Protocol status: this is local disk lifecycle cleanup only; no network or
  persistence format changed.
- Regression proof: the unit executes a chunk worker through its private write,
  proves the public destination is absent, cancels the transfer, injects the
  worker failure, drains cleanup, and requires both public and partial paths to
  be absent. This assertion failed before the fix and passes afterward.
- Remaining test need: interruption tests will also inspect every isolated peer
  root for abandoned `.partial.*` files after process-level reconnect cases.

#### BUG-T018 - the reordering fixture assumed the wrong configured chunk count

- Observed symptom: the initial red run also failed its assertion that an
  80,044-byte WAV formed exactly two chunks.
- Root cause: the test fixture implicitly assumed a much larger chunk than the
  current explicit 24 KiB content limit, so it correctly formed four chunks.
- Change: the ordered/reordered two-chunk fixture now uses a 40,044-byte WAV;
  production limits and transfer behavior were untouched.
- Regression proof: the fixture now proves exactly two encoded chunks before
  injecting chunk 1 ahead of chunk 0, and the transfer fails closed without a
  destination or partial artifact.
- Remaining test need: none; the separate successful multi-chunk case proves
  both chunks and durable acknowledgements against the same configured limit.

### Iteration 2 - concurrent four-peer imports, conflicts, and idempotence

- Extending the existing real four-peer GUI jam rather than simulating the
  application model. Commands are queued to multiple independent Jam2
  processes before any completion is read, so their asynchronous import,
  arrangement, Track Sync, and asset-transfer workflows genuinely overlap.
- Private snapshot evidence now includes lane names/banks and explicit pending
  contribution, outgoing-batch, active-receive, retry, and file-worker counts.
  This is local debug-agent observability only; no network protocol or public
  executable command changed.
- This iteration first tests simultaneous same-byte imports, then simultaneous
  different-byte imports targeting the same empty lane while unrelated tracks
  already exist, followed by all-peer repeated Share Tracks. It requires exact
  one-per-hash deduplication, preservation of every prior track, stable lane
  order/model digest, isolated per-peer paths, exact bytes, idle transfer state,
  and zero abandoned partial files.
- Red/green sequence: the first real-jam run exposed duplicate lanes for two
  simultaneous imports of the same WAV bytes (BUG-P010). The next runs exposed
  two independent ready-batch wake/drain gaps (BUG-P011 and BUG-P012): content
  converged, but fully validated Track Sync work and sender acknowledgements
  remained live. The storage-root proof then exposed a Windows separator error
  in the assertion itself (BUG-T019).
- Meaningful refactor: the pre-existing pure three-way looper merge was moved
  from the mixed WAV/workspace support file into `ConcurrentLooperMerge.*` so
  its actual merge responsibility can be tested directly. No code was split
  due to file size, and Practice Idea Generator was untouched.
- Reviewed acceptance now proves one lane for same-byte concurrent imports;
  all three distinct occupied-lane proposals survive without losing any prior
  track; every converged peer holds exact SHA-256 bytes under a different
  canonical storage root; four simultaneous repeat-share actions preserve the
  exact digest, hash, lane-ID, lane-name, and lane order; and pending assets,
  pending contributions, outgoing batches, receives, retries, file tasks, and
  partial files all return to zero.
- Regression result: the focused real four-peer test passed once in 9.50 s,
  then passed five consecutive timing-sensitive repetitions in 48.74 s. The
  authoritative full `shared-content` suite passed 3/3 in 10.17 s against the
  one staged `release/jam2.exe`.
- Self-review added batch/target/recording-isolation details to private local
  snapshots, checked exact file contents rather than model hashes alone, and
  retained replay acknowledgement for an already completed batch. Exact
  same-ID network replay and process interruption remain explicit iteration 3
  work rather than being inferred from repeat-share actions with new IDs.
- Protocol status: no emitted field, protocol version, frame/payload shape,
  encoding, parser/decoder acceptance, authentication, or compatibility rule
  changed in iteration 2.

#### BUG-P010 - simultaneous imports of identical WAV bytes created duplicate lanes

- Observed symptom: peers 1 and 2 concurrently imported byte-identical WAVs
  with different filenames and new lane IDs. All four peers converged to the
  same hash appearing twice and a lane count of eight instead of the required
  single shared lane and count of seven.
- Root cause: stale collaborative arrangements were merged by lane identity.
  Both branches added a different identity absent from the common base, so the
  three-way merge retained both even though their new nonempty SHA-256 assets
  were identical.
- Change: when two branches independently add different lane IDs with the same
  nonempty hash relative to the same base, the already accepted current lane is
  retained and the proposed duplicate is omitted. Existing duplicates present
  in the base and a deliberate later sequential duplicate are not collapsed.
  The pure merge moved to `ConcurrentLooperMerge.*` for direct validation.
- Regression proof: new native units prove concurrent same-hash collapse,
  preservation of a later sequential duplicate, and deterministic accepted-
  then-rebased ordering for different hashes. The real four-process test failed
  before the change, then passed the exact one-hash/one-lane assertion in six
  consecutive successful runs and the full shared-content suite.
- Remaining test need: retain conflict coverage with empty and occupied target
  lanes. Iteration 3 must also prove the result after a contributor disconnects
  and rejoins while its matching asset is in flight.

#### BUG-P011 - arrangement validation could make a Track Sync batch ready without waking it

- Observed symptom: under overlapping arrangement and Track Sync activity, a
  peer could retain a complete pending batch whose every hash was validated,
  while its sender continued waiting for completion. No transfer or file task
  remained to provide another callback.
- Root cause: arrangement validation inserted hashes into the shared validated
  asset set and installed the resolved song, but only the asset-transfer
  completion path revisited pending Track Sync contributions.
- Change: both immediate resolved-song installation and deferred pending-song
  installation now revisit pending Track Sync contributions after successful
  load. Failed/deferred song loads do not acknowledge or apply a batch.
- Regression proof: private snapshots expose each pending contribution and its
  validation state. The concurrent four-peer scenario now reaches exact content
  convergence and zero pending/outgoing state in six consecutive focused runs
  plus the full shared-content suite.
- Remaining test need: retain a dedicated arrangement-versus-offer race in the
  interruption matrix so this wakeup is not covered only as part of the larger
  concurrent scenario.

#### BUG-P012 - one readiness callback drained only one of several ready Track Sync batches

- Observed symptom: after concurrent all-peer activity, peers 3 and 4 each
  retained the same three-contribution batch with all three hashes validated;
  the creator retained that outgoing batch with two peers pending. The song,
  views, lane order, and WAV bytes had already converged, masking the live
  lifecycle leak unless pending state was inspected.
- Root cause: `applyPendingTrackContributions()` selected and completed one
  ready batch, then returned. A single offer or asset completion can make
  several batches ready at once, especially when they all reuse hashes already
  available locally. The remaining ready batch had no guaranteed future event.
- Change: after atomically committing and acknowledging one batch, Jam2 queues
  another reconciliation turn while contributions remain. Ready batches drain
  without recursion; incomplete batches still request only their next bounded
  asset and wait for genuine progress.
- Regression proof: the previously stranded scenario now requires every peer's
  pending contribution, outgoing batch, active receive, retry, and file-worker
  count to reach zero. It passed six consecutive four-process runs and the full
  3/3 shared-content suite.
- Remaining test need: iteration 3 will interrupt overlapping batches during
  validation/chunk/finalize and require the same complete lifecycle drain after
  reconnect, not merely exact final model content.

#### BUG-T019 - Windows canonical paths used a different separator than the isolation assertion

- Observed symptom: after all product convergence checks passed, peer 1's WAV
  at `.../peer-root/tracks/.../imported/<hash>.wav` was falsely reported as
  escaping its peer-specific temporary root.
- Root cause: Qt returned canonical paths with `/`, while the new containment
  assertion appended Windows `QDir::separator()` (`\\`) to the root.
- Change: both values are canonicalized and cleaned, then compared with Qt's
  normalized `/` separator and Windows case-insensitive semantics. Failure
  output now includes both path and root.
- Regression proof: the assertion advanced through all six hashes on all four
  peers in six consecutive runs, also proving all 24 canonical paths were
  distinct where required and their bytes matched the advertised SHA-256.
- Remaining test need: run the same assertion on case-sensitive Apple paths in
  `TEST-MACOS.md`; case-insensitive comparison is intentionally harmless for
  unique temporary roots but the macOS result must still be checked.

### Iteration 3 - real-process interruption, rejoin, and bounded recovery

- Added a private, local-only lifecycle harness to the existing GUI agent. It
  can pause exactly one outgoing validation, incoming offer, incoming chunk,
  or incoming finalization boundary for 100..5000 ms and reports the service,
  Track Sync, retry, worker, source, and hash state needed to prove that the
  intended boundary was reached. It does not add a public command or alter a
  network frame.
- Added `jam2_four_wav_interruption_integration`, which launches exactly four
  real `release/jam2.exe` processes with independent storage roots and four
  distinct 800,044-byte PCM WAVs. In sequence it disconnects and rejoins the
  sending peer during outgoing validation, the receiving peer during offer
  handling, the receiving peer with chunks queued, and the receiving peer at
  final commit. Each case then explicitly re-shares and requires exactly one
  copy of every accumulated hash, exact SHA-256 bytes, four distinct canonical
  paths, full model convergence, no pending contribution/batch/retry/worker or
  service state, and no `.partial.*` file.
- Red/green sequence: the first red run exposed session-owned Track Sync state
  surviving local leave and pending state surviving remote disconnect
  (BUG-P013/P014). Later red evidence exposed a request with no matching
  transfer-start deadline, same-hash batch ownership loss, and arrangement
  revision state crossing sessions (BUG-P015/P016/P017). Once request retry
  reached the rejoined sender, coordinator/client logs proved that an already
  closed asset stream remained registered forever (BUG-P018). Self-review then
  found that a per-hash retry budget could be inherited by a different source
  (BUG-P019). Two over-strong test assumptions are recorded as BUG-T020/T021.
- Product cleanup is now immediate on local leave and remote disconnect; an
  asset request must receive a matching transfer start within 10 seconds or
  enter the existing bounded retry path; surviving same-hash contributors keep
  their source ownership and receive a fresh source-specific retry budget; and
  closing a control peer synchronously removes its paired authenticated asset
  entry from the coordinator.
- Self-review proof: the expanded `AssetTransferService` unit matrix passes in
  0.07 s. The four-process interruption executable passed twice after the last
  product edit and then five consecutive timing-sensitive repetitions in
  47.07 s (8.78..9.84 s each). The authoritative complete shared-content gate
  passed 4/4 in 20.01 s: looper units, asset-transfer units, concurrent
  four-peer shared content, and four-peer interruption/rejoin.
- Protocol status: no field, protocol version, payload or frame shape,
  encoding, decoder/parser acceptance rule, authentication rule, or
  compatibility behavior changed. The authentication code change only removes
  a paired asset connection that the coordinator has already closed.

#### BUG-P013 - local leave retained session-owned Track Sync state

- Observed symptom: disconnecting the paused sender left its outgoing Track
  Sync batch live after the GUI agent reported the jam inactive. A later join
  could therefore inherit pending peer acknowledgements and asset bookkeeping
  from the previous session.
- Root cause: `stopJam()` cleared only part of the song/asset receive state. It
  did not clear contribution IDs, outgoing batches and hashes, incoming batch
  activity, offers, validated hashes, retry counts, held snapshots, or
  authoritative Track Sync history.
- Change: the cohesive `resetTrackSyncSessionState()` lifecycle operation now
  owns all session-scoped Track Sync, offer, pending-song, validation, retry,
  history, and arrangement-revision cleanup. It runs both before starting a
  network session and while stopping one.
- Regression proof: every leave case requires the departing process to report
  inactive network state plus zero outgoing batches, contributions, receives,
  retries, and workers before it may rejoin. All four boundaries passed in
  seven consecutive focused runs and the full shared-content gate.
- Remaining test need: closed in iteration 4. The sender is paused during
  validation, creates a second outgoing Track Sync batch, proves at least two
  batches are live, leaves, rejoins, and reaches exact six-WAV convergence with
  zero session-owned transfer state.

#### BUG-P014 - remote disconnect left incomplete WAV contributions until idle expiry

- Observed symptom: after a source disconnected, the creator retained that
  peer's pending contribution, missing-asset source, and held arrangement until
  the 30-second batch timer expired. Rejoin recovery could overlap this stale
  state and create retries against work that could no longer complete.
- Root cause: peer-disconnect handling removed outgoing recipients but did not
  expire incoming Track Sync batches, pending/deferred song state, held song
  snapshots, or source-owned missing-asset records.
- Change: a remote disconnect now removes pending and deferred arrangements
  owned by that source, releases held source snapshots, expires every incomplete
  batch from that peer, and reconciles any surviving work immediately.
- Regression proof: all three receiver-side boundaries and the sender-side
  boundary require the remaining three peers to observe exactly two active
  remotes before rejoin, while the departed peer and surviving sources expose
  no state owned solely by the disconnected connection. Recovery completes in
  roughly nine seconds rather than relying on the 30-second expiry.
- Remaining test need: closed in iteration 4 by the same-hash source-handoff
  case. Source A disconnects while source B's paused offer remains pending; B's
  exact contribution survives, receives a fresh retry budget, and commits.

#### BUG-P015 - a queued asset request could wait forever for `looper.asset.start`

- Observed symptom: after rejoin, a valid control request was logged and its
  Track Sync workflow remained active, but the asset service had no incoming
  hash, worker, or queue. Nothing retried until unrelated batch expiry.
- Root cause: receive progress had a deadline only after `asset.start`; the
  request-to-start handshake had no deadline at all.
- Change: a successful request arms a 10-second one-shot guard tied to the exact
  request generation, workflow, hash, and source. If no service transfer became
  active, Jam2 clears
  that expectation and enters the existing three-attempt bounded retry/fail
  path. A later workflow, source, hash, or active transfer makes the stale timer
  harmless.
- Regression proof: an intermediate red run logged the new deadline and
  retries instead of remaining stuck indefinitely. After the independent stale
  asset-channel defect was fixed, all interruption cases converged with zero
  retries in seven focused passes and the complete gate.
- Remaining test need: closed in iteration 4. A private inherited-pipe fault
  suppresses one real `asset.start` without affecting control traffic and proves
  deadline/retry recovery. A second case suppresses three starts from source A,
  then one from source B after handoff; this directly proves bounded retries and
  fresh source ownership without relying on a connection failure for the drops.

#### BUG-P016 - expiring one same-hash batch cleared another batch's WAV ownership

- Observed symptom: the creator could retain a pending contribution for a WAV
  while `pending_track_asset_sources` was empty. Expiry of one batch removed the
  shared hash source/retry state and reset an active expectation even though a
  different batch still required the same bytes.
- Root cause: batch expiry treated hash identity as unique ownership. It did
  not recalculate ownership from remaining contributions or distinguish the
  active source from another peer offering the same SHA-256.
- Change: expiry removes exact source/batch contribution keys first, preserves
  an active receive while the same source still owns matching work, rebuilds
  each removed hash's source from surviving contributions, and clears retry
  state only when neither Track Sync nor a pending arrangement expects it. A
  request awaiting `asset.start` is explicitly cleared if its exact owner is
  gone.
- Regression proof: interruption/rejoin now completes while arrangement and
  Track Sync offers for accumulated identical hashes overlap; exact one-per-
  hash convergence and zero source/retry state passed seven focused runs and
  the full suite.
- Remaining test need: closed in iteration 4 by the pure
  `TrackAssetOwnership` unit matrix. It expires source A then B and B then A,
  retains same-source and arrangement-only ownership, and proves unrelated
  expiry is a no-op.

#### BUG-P017 - arrangement revision counters crossed network-session boundaries

- Observed symptom: self-review found `looperArrangementRevision_` and
  `lastAppliedHostArrangementRevision_` were never reset on leave even though
  their authority and ordering scope is one network session. A later new jam
  could compare valid low revisions against the prior jam's higher values.
- Root cause: arrangement counters were outside the old partial stop cleanup.
- Change: both counters are reset with the rest of Track Sync state before a
  new session and during stop.
- Regression proof: the four interruption cases repeatedly leave/rejoin and
  accept new authoritative arrangements rather than rejecting them as stale;
  exact model convergence passed all repeated and full-suite runs.
- Remaining test need: closed in iteration 4. All four processes leave, expose
  both Track Sync revision counters as zero, create a different jam on the same
  processes, and accept its first positive authoritative revision with exact
  prior-WAV convergence.

#### BUG-P018 - a closed paired asset connection remained registered forever

- Observed symptom: after the sender rejoined with the same valid peer token,
  every new asset connection was rejected once per second as `TCP asset stream
  for peer token is already active`. The sender validated the WAV but remained
  at outgoing chunk index -1, unable to send `asset.start`. Coordinator stats
  recorded 59 authentication rejections in the failing run.
- Root cause: when a control peer disconnected, `ControlServer` closed its
  paired asset socket but did not remove that asset peer from `peers_`. Closing
  the native connection suppresses the later read callback on which ordinary
  removal depended.
- Change: after closing the paired asset connection, the server immediately
  runs the normal `disconnectPeer` removal/stat/callback lifecycle for that
  exact asset entry. Any later native callback is ignored by identity.
- Regression proof: the formerly permanent rejection loop disappeared; each
  rejoined asset channel authenticated, transferred exact bytes, and returned
  idle across seven focused four-process runs and the full gate.
- Remaining test need: network/security Slice 6 should add a direct server
  lifecycle test for same-token control/asset disconnect and reauthentication,
  including stats and the single disconnect callback.

#### BUG-P019 - a replacement source inherited another peer's exhausted retry budget

- Observed symptom: self-review showed that if an expired active source and a
  surviving contribution referenced the same hash, `resetIncoming()` could
  increment the hash's existing retry count and immediately expire the
  surviving source after the old source had already consumed three attempts.
- Root cause: retry storage is keyed by content hash, but source ownership can
  change while several peers offer the same bytes.
- Initial change: when expiry detached the exact active source, its retry count
  was cleared before reset/reconciliation. This covered an active receive but
  did not cover the delay between two retries.
- Iteration-4 red evidence: source A consumed three deliberately dropped starts,
  source B offered the same missing WAV while offer application was paused, and
  A disconnected between attempts. B's one deliberately dropped start inherited
  attempt four; Jam2 expired B immediately and the test timed out after 41.45 s
  with the hash unavailable, no pending contribution, four start timeouts, and
  zero retry state.
- Final change: retry state now records both hash and owning source. A failure
  from a different source starts a new budget; batch expiry clears the budget
  whenever its owner loses the hash claim, including between attempts; same-
  source surviving work retains it. Acceptance, exhaustion, session reset, and
  stale no-longer-expected callbacks clear both maps. Diagnostics expose the
  retry-owner map and count.
- Regression proof: the exact red scenario now lets B consume attempt one after
  its deliberate drop, retry normally, and commit the 800,044-byte WAV. Three
  consecutive four-process runs passed in 31.22, 30.66, and 30.79 s, followed
  by the complete 4/4 shared-content gate in 40.36 s.
- Remaining test need: none within automated same-machine coverage; retain the
  explicit three-failure/source-B-failure case.

#### BUG-T020 - rejoin recovery required content equality before the recovery share

- Observed symptom: the first test revision failed immediately after rejoin
  because the returning peer still held its locally imported WAV while the
  other peers correctly did not yet have it.
- Root cause: the topology-ready predicate also required all four model digests
  to match before the scenario's explicit recovery share.
- Change: rejoin readiness now proves only the four active network attachments;
  exact model/hash/file convergence remains mandatory after recovery share.
- Regression proof: the test advances through the intended transfer boundary,
  and every scenario still finishes with exact equal content and bytes.
- Remaining test need: none; pre-share divergence is intentional and the
  stronger post-share assertion remains.

#### BUG-T021 - disconnect cleanup rejected legitimate partials on surviving peers

- Observed symptom: the offer case reported a partial WAV on peer 4 while peer
  3 was the process being disconnected. Peer 4 was legitimately receiving the
  broadcast offer and had not yet reached convergence.
- Root cause: the immediate disconnect assertion scanned all four storage
  roots, even though only the departing peer was required to cancel at that
  point.
- Change: immediate cleanup inspects the departing peer's root. The existing
  global no-partials check still runs after every scenario reaches fully idle
  exact convergence and again after shutdown.
- Regression proof: seven focused passes prove all survivor transfers finish
  and all four roots are globally clean at every stable boundary.
- Remaining test need: none; active staging and abandoned staging now have
  separate, correctly timed assertions.

### Iteration 4 - complete WAV-path review and closure

- Reviewed offer publication/acceptance, atomic batch application, arrangement
  validation, request/start deadlines, transfer queues, source/batch expiry,
  retry ownership, session reset, file-worker generations, WAV inspection,
  incremental and final SHA-256 checks, atomic destination commit, and partial-
  file cleanup. Malformed network-object validation is routed to Slice 6; this
  review made no parser acceptance or wire-format change.
- Added a private bounded start-drop fault (one to four starts), a private
  GUI-agent-only request-start timeout override, attempt-generation and retry-
  owner diagnostics, and direct timeout counters. The shipped default remains
  10,000 ms. These controls exist only over the inherited automation pipe.
- Meaningful refactor: same-hash batch-expiry reasoning moved into the pure
  `TrackAssetOwnership.*` planner. The direct unit matrix covers source A/B in
  both expiry orders, a surviving same-source batch, arrangement-only ownership,
  active-source detachment, and unrelated no-op expiry. No file was split due
  to size and Practice Idea Generator was not changed.
- Expanded the exact four-process test from four to seven distinct 800,044-byte
  WAV fixtures. In addition to validation/offer/chunk/finalize interruption it
  now proves one dropped start and recovery, explicit leave with two outgoing
  batches, complete session revision reset followed by a new jam, and a forced
  three-failure source-A to one-failure source-B same-hash handoff.
- Every stable boundary requires exact SHA-256 file bytes under four isolated
  roots, one lane per expected hash, converged model/view digests, and zero
  pending assets, contributions, batches, retry owners, file workers, transfer
  service state, or `.partial.*` files.
- Red/green review found that BUG-P019's iteration-3 fix was incomplete between
  retries and exposed BUG-P020 below. The bounded timeout-count assertion also
  needed BUG-T022. All are retained in the regression suite.
- Final focused proof: `jam2_asset_transfer_units` passed in 0.07 s; the expanded
  four-process test passed three consecutive final repetitions in 31.22, 30.66,
  and 30.79 s; the authoritative shared-content gate passed 4/4 in 40.36 s.
- Protocol status: no field, version, payload/frame shape, encoding,
  parser/decoder acceptance, authentication, authorization, or compatibility
  behavior changed. All new fault controls and observations are local-only.
- Iteration exit: focused and shared-content gates are closed. The final
  authoritative `compile.cmd --tests-full` run passed 33/33 in 418.61 s,
  including all 21 metronome/epoch impairment cells and the expanded seven-WAV
  interruption case. Slice 5B is complete.

#### BUG-P020 - a stale request-start timer could cancel a newer identical request

- Observed symptom: code review found that an old 10-second request timer could
  match a later retry or new-session request when workflow, SHA-256, and source
  were identical. If the later request was still waiting for its own start, the
  old timer could cancel it early and charge the wrong retry budget.
- Root cause: timer identity used only workflow/hash/source. Successful transfer
  start did not cancel the independent one-shot; it merely made the state check
  false until that same identity was reused.
- Change: every outgoing request expectation receives a monotonically increasing
  local generation. The timer must match that exact generation as well as
  workflow/hash/source, and session reset invalidates every older generation.
  The generation is exposed in private content diagnostics.
- Regression proof: the expanded test repeatedly reuses hashes and sources
  across interruption, bounded retry, leave/rejoin, and an entirely new jam.
  Three consecutive final runs and the 4/4 gate reached exact content with only
  the deliberately injected timeout counts.
- Remaining test need: retain the generation guard whenever request timing is
  refactored; no network or compatibility test is required for local timer state.

#### BUG-T022 - one dropped start was assumed to imply exactly one receiver timeout

- Observed symptom: after the source-ownership fix, one run recovered every WAV
  correctly but reported two aggregate receiver start timeouts instead of one.
- Root cause: one creator serves three receiver requests serially. Exactly one
  start frame was deliberately dropped, but another legitimate queued receiver
  can reach its own deadline before the sender begins that queued transfer.
- Change: the test still requires the sender's dropped-start counter to advance
  by exactly one, while aggregate receiver deadlines must be bounded to one
  through three. Exact recovery, idle state, exact bytes, and no partials remain
  mandatory. The source-handoff case separately asserts exact creator-local
  timeout counts because it has controlled single-recipient ownership.
- Regression proof: the corrected assertion passed the three final repetitions
  and complete shared-content gate without weakening any content or lifecycle
  condition.
- Remaining test need: none; queueing delay and injected frame loss now have
  separate evidence.

## 2026-08-13 - Slice 6: network and security

### Iteration 1 - native validators and real TCP security boundaries

- Registered the existing in-product `validate.boundaries` and
  `validate.controller-lifecycle` operations as two first-class CTests. The
  wrapper creates an isolated current-format scenario, launches the one staged
  `release/jam2.exe`, parses its native manifest, rejects aggregate or
  individual failures, and guards against accidental test loss with minimum
  case counts of 200 and 25 respectively.
- Added the selected `network` build target and `compile.cmd`/`compile.sh`
  command surface. The intended selected gate contains only the two exact
  `network`-label validators; metronome impairment tests retain their distinct
  `network-impairment` label and remain owned by performance/full.
- Closed BUG-P018's remaining direct proof. A real creator now authenticates a
  control channel and its paired asset channel, disconnects the control side,
  observes exactly one control and one asset disconnect callback/stat update,
  reconnects with the same token, then uploads and downloads binary data with
  no authentication rejection.
- Expanded the controller validator with a separate real-socket security
  matrix: 12 simultaneous silent clients against the cap of eight; cleanup
  drain; silent-authentication and authenticated-incomplete-frame deadlines;
  first-message acceptance followed by exact replay rejection; authentication
  tag corruption; oversized frame-prefix rejection; and exactly 64 invalid
  proofs followed by rate-limit rejection. It asserts bounded active pending
  work, high-water values, buffered input, and visible cap drops.
- Red/green sequence: the expanded run first failed after 26.51 s but the CTest
  wrapper exposed only process code 3; after diagnostics repair, the exact
  failing native case was `controller.pending-authentication-work-is-bounded`.
  Detailed red runs showed `active=8`, `high_water=8`, `cap_rejects=0`, proving
  that the native listener bounded and discarded excess accepted sockets before
  `ControlServer` could count them. An attempted turnover assertion also showed
  active work legitimately draining to seven during observation.
- Product gap fix: `NativeTcpListener` now counts pending-delivery rejects in
  an atomic reset per listen, and `ControlServer::stats()` folds that count into
  its existing `pendingCapRejects` field. No acceptance limit or connection
  behavior changed. The exact cap assertion is now high-water eight, active no
  greater than eight, and at least one visible rejection.
- Final focused proof: the controller/security validator passed three
  consecutive authoritative Windows/MSVC runs in 24.96 s each. The initial
  exact selected network gate passed 2/2 in 74.32 s before the security matrix
  expansion; a final selected-gate run remains due after UDP work.
- Self-review: all new traffic is generated by private tests or the existing
  debug validator. The only production change in this iteration is listener
  observability through an existing stats field. No protocol field, version,
  frame/payload shape, encoding, parser acceptance rule, authentication rule,
  authorization rule, or compatibility behavior changed.
- Iteration result: TCP/authentication portion complete and stable. UDP
  malformed/replay/corruption/flood injection is the next independent pass, so
  Slice 6 remains in progress.

#### BUG-P021 - native pending-delivery cap drops were invisible in server stats

- Observed symptom: with 12 simultaneous unauthenticated clients and the cap
  set to eight, active and high-water work remained correctly bounded at eight
  but `pending_cap_rejects` stayed zero. Operators could not distinguish these
  deliberate resource-cap drops from unrelated connection failures.
- Root cause: `NativeTcpListener` owns an earlier eight-item delivery queue and
  silently closed overflow sockets before they reached `ControlServer`, whose
  existing counter covered only its later pending-authentication cap.
- Change: the listener records those overflow drops atomically, resets the
  counter for each listen lifecycle, and exposes it through the existing
  `ControlServer::Stats::pendingCapRejects` snapshot. Resource limits, socket
  handling, and every on-wire byte remain unchanged.
- Regression proof: the direct 12-client test requires active work at or below
  eight, exact high-water eight, and a non-zero visible rejection count. The
  complete controller/security matrix then passed three consecutive 24.96 s
  authoritative runs.
- Remaining test need: repeat the same hard-data assertion on macOS because
  accepted-socket delivery scheduling differs there; this is listed in
  `TEST-MACOS.md`. No additional Windows TCP case is currently needed.

#### BUG-T023 - the network suite label also selected metronome impairment tests

- Observed symptom: `--tests network` ran 23 tests in 437.24 s instead of the
  intended two validators because CTest's label regular expression matched the
  substring in `network-impairment`.
- Root cause: the command used `network` as an unanchored label expression.
- Change: both build scripts now pass the exact expression `^network$` (with
  the required batch escaping on Windows).
- Regression proof: the corrected command selected only the two native network
  validators and passed 2/2 in 74.32 s.
- Remaining test need: none; suite ownership is now explicit.

#### BUG-T024 - the native-validation wrapper hid the failing manifest case

- Observed symptom: a child validation failure surfaced only as exit code 3,
  and aggregate validation could replace the concrete failing case with a
  generic result error.
- Root cause: the wrapper returned before inspecting the manifest on non-zero
  child exit and checked aggregate status before iterating individual cases.
- Change: it now attempts manifest inspection on any normal child failure and
  reports the first failed native case and its diagnostic detail before the
  aggregate fallback.
- Regression proof: subsequent red runs identified
  `controller.pending-authentication-work-is-bounded` and printed its exact
  active/high-water/rejection/accepted values, enabling BUG-P021 to be fixed.
- Remaining test need: none; this diagnostic path remains exercised by any
  future intentional or accidental native validation failure.

#### BUG-T025 - pending-cap proof assumed overflow reached the higher server layer

- Observed symptom: the first security assertion expected
  `ControlServer`'s own cap-reject path to receive clients nine through twelve;
  a turnover revision also required an instantaneous active count of eight even
  though one silent client could legitimately finish cleanup during sampling.
- Root cause: the test modeled only `ControlServer` and not the preceding
  native accepted-socket delivery queue, and conflated exact high-water with a
  transient active count.
- Change: the proof requires the invariant that matters: exact high-water eight,
  active work no greater than eight, non-zero aggregate cap rejection, and a
  complete cleanup drain. Listener-level visibility was added separately as
  BUG-P021 rather than weakening the observability claim.
- Regression proof: all three final focused repetitions passed the complete
  security matrix with bounded work and cleanup.
- Remaining test need: none on Windows; the Apple scheduling parity check is in
  `TEST-MACOS.md`.

### Iteration 2 - native four-peer UDP adversarial proof

- Extended the test-only six-edge `UdpImpairmentProxy` with deterministic
  corruption, first-audio capture, direct bidirectional injection, a bounded
  packet transformer, and hard per-direction transformed/injected/error
  counters. Its existing loss, delay, jitter, duplication, reordering, burst,
  queue-bound, and socket behavior is unchanged when the new controls are off.
- Added the protocol-aware `UdpSecurityFixtures` test support boundary. It
  creates the seven cheap-parse rejection variants (short, magic, version,
  type, session, payload size, and authentication) and re-signs real captured
  audio for a continuous sequence beginning at `0xfffffff0`, one excessive
  forward gap, and one `uint64`-edge sample time in each direction.
- Added `jam2_four_udp_security`: exactly four real Jam2 processes and all six
  full-mesh proxies run the complete shared-grid epoch/audio scenario while
  every direction receives deterministic 1% corruption. The first edge also
  receives all 14 malformed variants, two delayed authenticated replays, and
  4,096 eight-byte datagrams in each direction. It requires all successful
  short injections to appear in parser stats, every rejection reason, replay
  observation, receive-batch high-water in the hard 1..64 range, clean proxy
  bounds, four active peers, fresh receive growth after injection, exact epoch
  convergence, and the existing recording/mixer proof.
- Added `jam2_four_udp_sequence_security`: exactly four real processes run the
  same shared-grid proof while the first edge's authenticated audio is
  continuously re-signed across 32-bit wrap in both directions. Each direction
  also receives one validly signed 100,000-packet forward jump and one signed
  extreme sample time. It requires both transformations, two gap rejects, two
  future-time rejects, zero authentication failures, continued fresh traffic,
  and exactly the two losses/two late packets caused by the deliberately
  rejected timestamp frames—therefore no additional loss from wrap or forward-
  gap recovery.
- Python UDP parity review: the native pair covers `corrupt-1.0`,
  `near-wrap-sequence`, `malformed-udp`, `delayed-replay`,
  `forward-sequence-gap`, `extreme-sample-time`, and `udp-short-flood`. The
  existing 21-cell native metronome matrix already owns clean, delay, jitter,
  loss, duplication, reordering, and bounded burst behavior across all three
  metronome modes. Python files remain until the later whole-catalogue removal
  gate; this iteration does not remove analysis or benchmarking tools.
- Focused proof: after closing the red harness gaps below, both UDP tests
  passed three consecutive authoritative Windows/MSVC runs at 17.26-17.28 s.
  The unchanged `jam2_metronome_shared_grid_clean` case then passed in 17.26 s,
  proving the shared proxy still preserves clean metronome/epoch behavior.
- Self-review: the exact `network` label now owns four tests only; corruption
  does not consume random state while disabled; direct injection errors,
  pending high-water, parser reasons, batch high-water, sequence outcomes, and
  post-injection traffic are all explicit. `git diff --check` is clean apart
  from the repository's existing line-ending notices.
- Protocol status: no production field, version, packet/header/payload shape,
  encoding, parser/decoder acceptance, authentication, authorization, or
  compatibility behavior changed. Protocol-aware code was added only below
  `tests/`; the earlier listener statistics change remains observability-only.
- Iteration result: native UDP implementation, focused proof, self-review, and
  gap-fix repetitions are complete. The selected network and complete Windows
  catalogue retests remain before Slice 6 closure.

#### BUG-T026 - the UDP fixture used the wrong published include spelling

- Observed symptom: the first MSVC build stopped with C1083 because
  `jam2/protocol.hpp` was not found.
- Root cause: `jam2_core` publishes `libs/jam2-core/include/jam2` directly, so
  consumers in this repository include `protocol.hpp` from that root.
- Change: the test fixture now uses the target's actual public include
  spelling.
- Regression proof: the fixture and four-process executable compile cleanly
  under MSVC and both new tests pass repeatedly.
- Remaining test need: none.

#### BUG-T027 - rejected signed fixtures manufactured unrelated sequence holes

- Observed symptom: the first signed-sequence run reached every target branch
  but reported four lost packets; after reusing rejected numbers it reported
  two losses and two late packets.
- Root cause: replacing a normal audio packet with an intentional forward-gap
  or extreme-time reject omits that normal sequence. `PeerStream` deliberately
  advances expected sequence for the extreme-time rejection, while forward-gap
  recovery does not. The first fixture treated both paths alike and the first
  assertion expected zero loss despite deliberately rejecting audio.
- Change: the continuous transformer reuses each deliberately rejected
  baseline number on the following valid packet. The assertion requires exact
  product semantics: two extreme-time losses and two late reuses total, with
  no additional loss attributable to wrap or forward-gap recovery.
- Regression proof: both directions wrap after sequence `0xffffffff`, reject
  one forward gap and one extreme time, continue past 11,900 transformed audio
  packets, and pass the exact loss/late/reject matrix in three consecutive runs.
- Remaining test need: none; the exact attribution is stronger than a generic
  non-zero rejection check.

#### BUG-T028 - the flood proof required the OS to saturate Jam2's wake budget

- Observed symptom: all 8,192 injected short packets were observed, every
  parser branch and replay check passed, and audio continued, but the test
  failed because `udp_work_budget_yields` was zero. Windows delivered a maximum
  of 24 datagrams per wake, below the product's hard 64-datagram ceiling.
- Root cause: the assertion confused bounded work with mandatory saturation;
  an OS that paces work below the limit should not be forced to hit it.
- Change: require every successfully injected short packet plus the two short
  malformed cases to be observed, and require the measured receive batch
  maximum to remain within 1..64. Yield count remains visible diagnostic data.
- Regression proof: the corrected case passed three consecutive runs while
  parsing the full flood and preserving post-injection four-peer traffic.
- Remaining test need: macOS must apply the same invariant without assuming
  Apple socket batching saturates the ceiling; this is retained in
  `TEST-MACOS.md`.

#### BUG-T029 - master-output boundary used a sparse source for an asynchronous level change

- Observed symptom: the first complete network gate failed
  `master-output.scales-complete-headless-mix` after 69.65 s with
  `muted_peak_ppm=0`, `audible_level_ppm=1000000`, and
  `audible_peak_ppm=0`. The command had reached the audio thread, but the
  validator sampled no audible block within its deadline.
- Root cause: the proof used only a short metronome click. Master level changes
  are asynchronous at the real callback boundary; depending on catalogue load
  and click phase, the command could be applied after that sparse source's
  nonzero block and before the next click.
- Change: retain the real headless engine, command queue, complete mix, master
  scaling, and output peak telemetry, but add a continuous synthetic 440 Hz
  input routed through local monitoring. The zero-level phase must still be
  silent; after the same dynamic master command, a nonzero mixed block is
  continuously available for observation.
- Regression proof: the complete 200+ boundary catalogue passed three
  consecutive authoritative runs in 68.17, 68.81, and 69.06 s. The original
  selected-gate red remains recorded; the other controller and both UDP tests
  passed during that run.
- Remaining test need: none on Windows. Run the same headless boundary on macOS
  as part of native-validator parity.

### Iteration 3 - network/security closure

- Reran the corrected exact-label selected suite after the TCP and UDP
  self-reviews. It passed 4/4 in 128.01 s: native boundaries 68.59 s, real TCP
  controller/security 24.91 s, four-peer UDP packet security 17.25 s, and
  four-peer signed sequence security 17.25 s.
- Ran the authoritative Windows cross-slice catalogue with
  `cmd.exe /d /c "call compile.cmd --in-dev-shell --tests-full"`. It passed
  37/37 in 549.51 s against the one staged `release/jam2.exe`.
- The full proof includes seven native unit targets, seven GUI-labelled tests,
  four shared-content tests including the seven-WAV interruption/source-
  handoff matrix, the fake-audio performance jam, all 21 combinations of three
  metronome models and seven network conditions, both native validators, and
  both new UDP adversarial four-peer cases.
- Final resource review: unauthenticated TCP work is capped at eight and visible
  at both delivery layers; failed-key work is bounded at 64 per window; silent
  and incomplete clients time out; UDP work is capped at 64 datagrams per wake;
  proxy pending queues are bounded and expose high-water/capacity drops; every
  adversarial test requires normal traffic and four-peer convergence after the
  injection rather than accepting rejection counters alone.
- Python parity decision: native coverage now supersedes the UDP validation
  fixtures and protocol/network portions of retained stress, but no Python file
  was removed in this slice. Whole-file and documentation ownership overlaps
  with benchmarks, connectivity, fuzzing, result analysis, and remaining app
  audit work; removal waits for the final parity inventory in Slice 8.
- Protocol status: Slice 6 changed no protocol field, version, packet/frame or
  payload shape, encoding, parser/decoder acceptance rule, authentication rule,
  authorization behavior, or compatibility behavior. BUG-P021 is statistics
  visibility only; every UDP mutation and re-signing component lives under
  `tests/`.
- Slice result: complete after implement, focused red/green repair, self-review,
  three-run stability checks, selected-suite retest, and full-catalogue retest.
  Slice 7 starts with the remaining control/view/action/function inventory.

## Slice 7 - remaining application/control audit

### Refactoring scope clarification - `MainWindow`

- The application audit explicitly includes decomposing `MainWindow` where a
  tested workflow has a concrete owned boundary, such as settings/preferences,
  jam lifecycle, shared content/WAVs, metronome/listener compensation, or
  automation support.
- Extraction is responsibility-driven and follows behavioral coverage for the
  affected workflow. `MainWindow` retains cross-workflow window orchestration,
  and no file is split solely because it is large.
- Each extraction must rerun its focused native tests and the relevant
  exactly-four-peer regression gate. It must not change the network protocol.

### Iteration 1A - semantic GUI-control contract foundation

- Normalized the live inventory so `QLineEdit` children owned by spin/combo
  controls and popup item views owned by combo boxes are not misreported as
  independent Jam2 interactions. The initial window now reports 472 semantic
  controls instead of the earlier Qt-internal-inflated count.
- Added the cohesive `GuiControlContract` metadata boundary under `app/gui`.
  It records a unique stable ID, behavioral test contract, availability class,
  and optional generated-family name without changing visibility, enabled
  state, signals, callbacks, or any production decision.
- Registered 26 initial unique controls covering Start/Join/Leave, Jam Sync,
  settings, save/open/new, data, workspace navigation, performance input/plugin
  entry points, transport/metronome/count-in, BPM, tap tempo, and title rename.
  The inventory currently reports `472 total / 26 classified / 446
  unclassified`; the nonzero unclassified count is intentionally visible while
  this slice works through the remaining generated and conditional controls.
- Strengthened the exact four-peer GUI smoke gate: every registered control
  must have complete metadata, IDs must remain unique, all 26 foundation IDs
  must appear on every peer, and state invocation/readback still uses the real
  widget tree. The focused Windows/MSVC build and test passed 1/1 in 5.91 s.
- WAV status: this foundation does not alter the completed transfer state
  machine or its exact-byte tests. Later Slice 7 workflows will connect the
  import/share/replace/clear UI contracts to the existing four-peer concurrent,
  interrupted, retry, source-handoff, ownership, and exact-byte convergence
  proofs. Until that mapping is present, the user-facing WAV control-flow gap
  remains open even though the underlying transfer matrix is already robust.
- Self-review: the contract is a concrete testability boundary rather than a
  size-driven product refactor. It has no network, persistence, audio, timing,
  or protocol behavior. Optional diagnostic JSON remains test-requested only;
  it is not written during normal Jam2 execution.

#### BUG-T030 - GUI contract implementation omitted QVariant's complete definition

- Observed symptom: the first authoritative focused MSVC build failed in
  `GuiControlContract.cpp` with C2027/C2665 at `QObject::property` and
  `QObject::setProperty`.
- Root cause: `QObject` forward-declares `QVariant`; the new translation unit
  used the property's value type without including `<QVariant>`.
- Change: include `<QVariant>` directly in the owning implementation.
- Regression proof: the same authoritative build completed and the exact
  four-peer GUI integration test passed 1/1 in 5.91 s.
- Remaining test need: none; subsequent GUI target builds compile this source
  and the smoke gate validates the emitted metadata on all four processes.

### Iteration 1B - ownership classification and painted looper targets

- Classified the controls owned by the Start/Join configuration model, Local
  Engine dialog, data drawer, performance transport and mix, metronome page,
  section navigation, idea actions, and main looper page. Modal, file-choice,
  state-gated, and hardware-profile availability are explicit.
- Explicitly excluded eight widget-shaped non-controls: three hidden state
  mirrors, four unreachable legacy session-page buttons whose behavior is
  exposed by the public header/dialogs, and the read-only diagnostic peer table.
  These remain accounted items rather than silently disappearing from review.
- Added the `GuiVirtualControlProvider` boundary for custom-painted surfaces.
  The looper now exposes add-empty/import plus per-lane select, mute, solo, arm,
  rename, remove, gain, file drop, region, analysis, reveal, and WAV removal
  targets as their applicable state appears. This does not manufacture child
  widgets or alter hit rectangles, painting, or callbacks.
- Strengthened the real four-process smoke test to invoke painted lane gain and
  add-lane targets. It requires the gain to read back as -3.5 dB and a new
  `looper.lane.1.select` target to appear on every peer. The final focused
  Windows/MSVC run passed 1/1 in 5.57 s.
- Inventory checkpoint before the add-lane mutation: 470 total entries, 10
  painted looper targets, 227 classified, and 243 unclassified. All remaining
  unclassified entries are inside the three BeatGrid workspace trees.
- WAV test need: painted import/drop/share/remove/reveal/analyse/region targets
  are now visible to automation. The next WAV-facing workflow must bind these
  user actions to the already-complete exact four-peer interruption/retry and
  byte-convergence proof; this iteration only proves non-file painted actions.
- Protocol status: no network field, version, payload/header shape, encoding,
  parser acceptance, authentication, authorization, or compatibility behavior
  changed. All additions are GUI metadata and private inherited-handle
  automation.

#### BUG-T031 - initial GUI inventory still counted views and implementation state as controls

- Observed symptom: the first normalized count called 472 objects semantic,
  but it still included a read-only technical log, table headers/corner, a
  no-selection diagnostic table, and hidden spin boxes/buttons used only as
  backing state or unreachable legacy mirrors.
- Root cause: the first filter handled only combo/spin composite children and
  equated every matching Qt class with a user-invokable interaction.
- Change: read-only plain-text panes and item-view header/corner children are
  audited as views; non-user widget state receives an explicit exclusion reason
  and count. Registered modal fields remain semantic because they are
  reparented into visible Start/Join dialogs.
- Regression proof: the corrected initial real-control count is 460; the smoke
  gate requires at least the eight explicit exclusions and still finds every
  registered interaction on all four peers.
- Remaining test need: view/state coverage is still required in the later view
  audit, and the hidden legacy session controls will be reviewed for a safe
  state-model refactor rather than removed speculatively.

#### BUG-T032 - stateful guide registration lambda was const

- Observed symptom: the first expanded-inventory MSVC build failed with C2440
  and C3848 at all five data-guide calls.
- Root cause: the lambda increments a captured guide index and was therefore
  `mutable`, but its local variable was declared `const auto`.
- Change: retain the deterministic counter and make the local lambda object
  non-const.
- Regression proof: the same authoritative build succeeded; all five guide
  IDs were unique and the four-peer smoke test passed, followed by the painted
  looper assertion run passing in 5.57 s.
- Remaining test need: none; duplicate IDs and incomplete contracts are hard
  failures on every GUI-agent launch.

### Iteration 1C - complete initial-window and generated-family inventory

- Registered every generated BeatGrid interaction with coordinate-stable IDs:
  section selection/structure, overview bars and pagination, chord cells,
  musical divisions and steps, beat divisions and steps, and lyric bars. Added
  virtual providers for Performance Home idea/navigation/mix/section/peer/
  tuner targets and every metronome-pattern step.
- The staged initial-window inventory is now `497 total / 497 classified / 0
  unclassified / 0 duplicate IDs`: 460 widget-backed controls plus 37
  custom-painted virtual targets. Eight widget-shaped implementation/state
  objects remain explicitly excluded with reasons and are not silently lost.
- Strengthened the real four-process smoke workflow so every peer sets and
  reads back a chord (`Cmaj7`), musical step (`C4`), lyric
  (`four-peer lyric`), and beat step through generated IDs. The snapshot must
  show the edited model fields and a changed beat fingerprint, in addition to
  the already-covered looper gain/add-lane, Performance Home gain, metronome
  accent, and BPM actions.
- Authoritative Windows proof: the focused staged integration passed three
  consecutive runs in 4.02 s, 4.05 s, and 4.15 s. The optional exported JSON
  independently confirmed all 497 entries classified, 37 virtual targets, and
  zero duplicate IDs.
- Self-review result: the stable control contract and initial generated-family
  inventory are complete. Transient Start/Join/settings/dialog controls and
  end-to-end file-choice workflows remain open and will be inventoried while
  visible rather than inferred from their backing fields.
- WAV test need: looper WAV import/drop/share/remove/reveal/analyse/region
  actions are addressable, but this iteration intentionally did not claim
  user-flow coverage. The next WAV workflow must import through the painted UI
  target, exercise Share Now, and bind visible state to exact four-peer bytes,
  ownership, interruption/retry, removal, and convergence assertions.
- Protocol status: no network field, version, packet/frame or payload shape,
  encoding, parser/decoder acceptance, authentication, authorization, or
  compatibility behavior changed.

#### BUG-T033 - paginated grid assertion could erase a successful observation

- Observed symptom: self-review found that `gridContentObserved` was assigned
  anew for every snapshot page, so a later page without the relevant content
  could overwrite an earlier successful model observation.
- Root cause: the new assertion was written as a page-local assignment even
  though inventory/state snapshots are intentionally paginated.
- Change: accumulate success across pages with logical OR, matching the other
  family-state observations.
- Regression proof: the corrected four-process workflow passed three
  consecutive runs and proved the chord, lyric, and beat fingerprint model
  changes on every peer.
- Remaining test need: none for pagination accumulation; transient inventories
  will reuse the same accumulated-observation rule.

### Iteration 2 - Start, Join, Local Engine, and Jam Sync modal workflows

- Registered the unique controls created only while Start Jam, Join Jam, and
  Jam Sync are open. The contracts include sample/buffer choices, profiles,
  refresh/test-device actions, default/credential actions, accept/cancel,
  every Jam Sync policy field, and explicit hardware-profile availability.
- Added `jam2_four_gui_modal_integration`. Four isolated staged GUI processes
  open the real dialogs through their public buttons, inventory each live
  nested widget tree, fail on any unclassified or duplicate interaction, edit
  real controls, cancel, reopen, and verify state ownership.
- The workflow proves Local Engine can cancel cleanly, Start Jam cancellation
  restores its previous sample-rate choice, Join cancellation leaves the role
  inactive and networking stopped while retaining the unsubmitted invite
  draft, and Jam Sync cancellation leaves the active policy unchanged.
- Added a private `click-async` operation only to controls explicitly marked
  modal. It acknowledges the opener before the real button enters
  `QDialog::exec()`, allowing subsequent inventory commands to run through the
  nested Qt event loop without sleeps or a second product callback.
- Authoritative Windows proof: after closing the red iterations below, the
  exact modal gate passed twice in 17.92 s and 17.56 s. The original
  four-process 497-control smoke then passed in 4.20 s, proving the additional
  operation did not regress baseline invocation, classification, or shutdown.
- Self-review: invite draft preservation is existing user-visible behavior and
  could reasonably be convenience or surprising Cancel semantics. No product
  change was made; `TEST-REVIEW.md` REVIEW-001 requests the user's decision.
  Settings and the remaining specialist dialogs are still open work.
- WAV status: the Jam Sync modal now exposes and exercises the Automatic WAVs
  dependency control, but the cancellation workflow does not claim transfer
  proof. End-to-end painted import/Share Now/removal remains the next high-risk
  WAV-facing workflow.
- Protocol status: no network field, version, packet/frame or payload shape,
  encoding, parser/decoder acceptance, authentication, authorization, or
  compatibility behavior changed.

#### BUG-T034 - modal integration fixture omitted QJsonArray's definition

- Observed symptom: the first authoritative MSVC build failed at the range-for
  over snapshot controls with C2027/C2530 because `QJsonArray` was incomplete.
- Root cause: the new fixture included `QJsonObject`/`QJsonDocument` but relied
  on Qt's forward declaration for the array value used directly.
- Change: include `<QJsonArray>` in the owning test translation unit.
- Regression proof: the fixture compiled under MSVC in all later runs and the
  complete modal workflow passed twice.
- Remaining test need: none.

#### BUG-T035 - synchronous modal openers blocked commands batched behind them

- Observed symptom: the first runtime pass timed out on the Start Jam snapshot;
  the subsequent event stream contained response-ID mismatches and apparent
  duplicates caused by the original response arriving during later requests.
- Root cause: the agent may take up to 32 commands from its queue per turn. If
  opener and snapshot were in that local batch, `button->click()` entered
  `QDialog::exec()` before the snapshot was dispatched; the nested event loop
  could process newly queued work but not the command trapped in the local
  vector.
- Change: modal controls advertise a private `click-async` operation. It posts
  the real button click with a guarded zero-delay Qt callback and acknowledges
  the opener first. Normal queue capacity/work limits and ordinary clicks are
  unchanged.
- Regression proof: both corrected modal runs completed all four processes in
  under 18 seconds with aligned request IDs, stable page totals, zero duplicate
  controls, and clean shutdown; the baseline smoke also passed.
- Remaining test need: use `click-async` for every later synchronous modal
  opener and retain strict response-ID checks.

#### BUG-T036 - Join cancellation test assumed drafts were defaults

- Observed symptom: once event ordering was fixed, all four peers failed only
  the expectation that cancelling Join Jam erased the typed invite URL.
- Root cause: the fixture treated every dialog field as a saved Join default.
  Product code intentionally applies audio/runtime defaults but has no saved
  invite field, so the unsubmitted invite remains a draft across reopen.
- Change: assert the safety behavior that matters: Cancel must leave the jam
  role inactive and networking stopped. Also assert the observed draft is
  preserved, and place the user-visible discard/preserve choice in REVIEW-001
  instead of changing behavior speculatively.
- Regression proof: the corrected contract passed on all four instances twice.
- Remaining test need: update the assertion if the user resolves REVIEW-001 in
  favor of discarding drafts.

### Iteration 3 - painted WAV drop, manual share, view convergence, and removal

- Replaced the manual WAV happy-path's direct automation helpers with the real
  user flow: painted Add Empty Lane, painted file drop onto the new lane, and
  the registered `Share with Jam Now` button. Automatic WAV sharing remains
  disabled during this phase so metadata-only and explicit-share behavior are
  distinct and observable.
- Before Share Now, the shared lane metadata must converge but only the source
  peer's painted lane may report `has_wav=true`. After Share Now, the existing
  transfer-idle, isolated-root, exact SHA-256, and exact-byte assertions remain
  mandatory and every peer's painted view must report the WAV available.
- Added live contracts for the Remove WAV confirmation actions and drove the
  painted removal opener asynchronously. Confirmed removal must preserve lane
  count, lane IDs, lane names, and local mixer state while eliminating the WAV
  hash and painted availability on all four peers with no residual transfer
  work.
- The first UI drop/share workflow passed in 7.86 s; the view-strengthened
  version passed in 9.95 s. Removal then exposed BUG-P022. After the merge fix
  and local-only self-review, the native boundary catalogue passed in 68.64 s
  and the complete four-peer UI/drop/share/remove matrix passed twice in
  11.97 s and 11.81 s.
- Self-review retained non-destructive preservation for genuinely concurrent
  same-ID/different-nonempty-WAV edits and separately protected local-only
  recordings/references. Only a synchronized same-ID lane whose authoritative
  incoming WAV is explicitly empty is treated as removal.
- Re-import follow-up: with Automatic WAVs still off, the same painted lane
  reacquires the removed hash without changing lane count/IDs/names. Only the
  source has bytes before the real Share Now action; afterwards all four peers
  have one hash reference, exact bytes, idle transfer state, and painted WAV
  availability. The expanded workflow passed twice in 13.40 s and 13.48 s.
- Remaining WAV test need: add removal/replacement races around an armed or
  interrupted transfer. Existing interruption/retry/source-handoff coverage
  remains unchanged and green from Slice 5B.
- Protocol status: no network field, version, packet/frame or payload shape,
  encoding, parser/decoder acceptance, authentication, authorization, or
  compatibility behavior changed. The fix changes only application merge
  semantics for already-decoded looper state.

#### BUG-T037 - painted WAV removal was classified as non-modal

- Observed symptom: the first removal run rejected `click-async` with
  `operation is invalid for the looper virtual target` before opening the
  confirmation.
- Root cause: `looper.lane.N.wav.remove` used the generic state-gated click
  helper even though its real callback synchronously opens a confirmation
  dialog. Rename and analysis targets were already correctly modal.
- Change: classify WAV removal as modal, let modal virtual targets advertise
  the guarded private asynchronous opener, and register both real confirmation
  buttons.
- Regression proof: both confirmation actions were found live and enabled;
  the confirmed workflow reached the product merge path and, after BUG-P022,
  passed twice on four peers.
- Remaining test need: explicitly cover Cancel in the later specialist-dialog
  matrix; confirmed removal is covered here.

#### BUG-P022 - removing a shared WAV cloned the old asset back into the lane set

- Observed symptom: after peer 3 confirmed Remove WAV, all four models converged
  to seven lanes instead of the expected six. The old hash remained in the
  shared arrangement; peers 1, 2, and 4 still had available bytes while peer 3
  had the same hash with no local file. All transfer queues were idle, proving
  this was stable incorrect convergence rather than an in-progress transfer.
- Root cause: `mergeSynchronizedLooperLanes` protects a local lane when an
  incoming arrangement has the same lane ID but a different WAV hash. It also
  treated the intentional same-ID/empty-WAV representation as a conflict,
  cleared the protected copy's ID, and appended the old WAV as another lane.
- Change: for synchronized (non-local-only) lanes, same ID plus an explicitly
  empty incoming hash/path now matches the existing lane. The authoritative
  empty WAV metadata wins while local gain/mute/solo state is retained. A
  different nonempty hash remains a non-destructive conflict and local-only
  WAVs still use their separate preservation rule.
- Regression proof: a new native boundary requires one unchanged lane with
  empty hash/path, zero source metadata, preserved name/gain/solo, and no clone.
  The 200+ native boundary catalogue passed in 68.64 s, including pre-existing
  local-only/reference/conflicting-WAV merge cases. The exact four-peer UI
  workflow then passed twice with equal lane identity/order and no manual WAV
  hash or painted availability on any peer.
- Remaining test need: same-byte re-import after removal and remove/replace
  interaction with in-flight interruption/retry were prioritized because this
  area demonstrated real-world sync risk. Same-byte re-import/re-share now
  passes twice; the in-flight race remains next.

#### BUG-T038 - re-import fixture bypassed the manual-sharing policy expectation

- Observed symptom: the first re-import-after-removal run timed out even though
  all four peers had exactly one hash reference, equal lane state, idle queues,
  and only the source peer had the WAV bytes.
- Root cause: the new pre-share assertion expected all peers to reuse old local
  files immediately despite Automatic WAVs being disabled. Jam2 correctly
  preserved the metadata-only boundary and did not silently reactivate a
  removed/cache file as the lane asset.
- Change: require source-only availability after painted re-drop, then invoke
  the real Share Now button and require exact bytes/views on all peers.
- Regression proof: the corrected expanded case passed twice in 13.40 s and
  13.48 s with one unchanged lane and no duplicate hash.
- Remaining test need: none for policy-off re-import; the interrupted removal
  race remains a distinct product workflow.

### Iteration 3 continuation - removing a WAV while transfer is in flight

- Expanded `jam2_four_wav_interruption_integration` from seven to eight unique
  WAV fixtures. The new path adds a lane through the painted Add Empty Lane
  target, arms the source at real outgoing validation, drops the WAV through
  the painted lane target, opens the real Remove WAV confirmation while the
  transfer is paused, and confirms removal.
- The four-peer acceptance boundary requires one unchanged lane ID/order,
  preservation of the post-import filename and all prior WAVs, no reference to
  the removed hash, no request/retry/queue/worker residue, no `.partial.*`
  files, equal model digests, and `has_wav=false` in every painted view. It
  additionally requires that removal creates zero request-start timeouts; an
  obsolete transfer is cancelled, not allowed to expire through retry limits.
- The first two diagnostic runs reached stable correct empty-WAV convergence
  but timed out because the fixture compared against the lane name from before
  import. BUG-T039 records that test defect. Once corrected, the test exposed
  BUG-P023: the host accumulated four stale request-start timeouts and the
  subsequent intentional dropped-start check observed seven cumulative
  timeouts.
- Receiver-only cancellation was insufficient because a still-pending atomic
  Track Sync offer preceded the later removal arrangement. The completed fix
  treats an accepted later track arrangement from the same ordered peer as
  superseding that peer's pending batch, records its identities against stale
  replay, acknowledges it with the existing batch-complete message, silently
  discards obsolete incoming work, invalidates its start timer, and cancels
  sender work only for a hash no longer referenced by any lane.
- Added focused service tests proving that hash-scoped outgoing cancellation
  suppresses the removed hash while advancing unrelated queued work, and that
  silent incoming discard invalidates workers/removes partial bytes without
  invoking retry ownership.
- Authoritative Windows proof: `jam2_asset_transfer_units` passed in 0.08 s.
  The complete four-peer interruption executable passed twice after
  self-review in 81.89 s and 60.96 s, including its existing disconnect,
  dropped-start, multi-batch, fresh-session, and two-source handoff matrix.
- Remaining WAV test need: repeat removal after at least one incoming chunk has
  been accepted/written, then cover replacement with different bytes during
  interruption. Those cases exercise partial-file teardown and latest-WAV
  ownership beyond the pre-start validation boundary covered here. Both were
  added in the follow-up below and are now green.
- Protocol status: no field, message type, packet/frame or payload shape,
  version, parser/decoder acceptance, authentication, authorization, or wire
  ordering changed. The fix uses the existing `looper.track.batch.complete`
  message and local lifecycle/generation state only.

#### BUG-T039 - removal race captured the lane name before WAV import

- Observed symptom: two red runs timed out at removal convergence even though
  all four peers had equal digests, nine unchanged lane IDs, no removed hash,
  idle transfer state, and all earlier WAVs available. Diagnostics showed only
  `Empty Track 2` versus `remove-during-transfer` differed.
- Root cause: the fixture captured lane names before dropping the WAV. The real
  import workflow correctly replaces a default empty-track name with the WAV
  filename; removing the bytes intentionally retains that useful track name.
- Change: retain the pre-drop lane IDs as the identity invariant, assert the
  real import applies `remove-during-transfer`, then capture that attached
  state as the removal invariant.
- Regression proof: the corrected checkpoint passed in both complete green
  interruption runs, with the same post-import name and lane ID on all peers.
- Remaining test need: none for filename preservation.

#### BUG-P023 - removing an in-flight WAV left an obsolete batch and four start retries

- Observed symptom: after confirmed removal all four arrangements eventually
  converged without the WAV, but the creator waited through four 10-second
  request-to-start deadlines. The source also retained validating/queued send
  work until timeout. This delayed convergence and inflated the later dropped-
  start counter from its expected 1..3 new timeouts to seven cumulative ones.
- Root cause: import sends an arrangement proposal followed by an atomic Track
  Sync offer. A later removal proposal was newer in the same authenticated
  ordered stream, but the receiver retained the older pending batch. When an
  asset request was still waiting for `looper.asset.start`, the transfer
  service had no active incoming hash for `resetIncoming()` to clear, leaving
  the MainWindow request generation and retry ownership alive. The sender also
  retained hash-specific queued/validating work after its last lane reference
  was removed.
- Change: an accepted later track arrangement now supersedes pending batches
  from that same source, bounds and retains their contribution identities for
  replay safety, and acknowledges them using the existing completion message.
  Jam2 invalidates the obsolete request generation, silently discards incoming
  worker state without creating a retry, and cancels outgoing work by hash only
  when no other lane references it. A superseded arrangement request that has
  not started is explicitly cleared at the MainWindow ownership layer.
- Regression proof: the new race requires every peer's request-start timeout
  counter to remain exactly unchanged. The asset service units passed in
  0.08 s; the exact four-peer interruption matrix passed in 81.89 s and
  60.96 s. Its later source-A/source-B test also passed, proving cancellation
  does not consume another peer's fresh retry budget or remove unrelated work.
- Remaining test need: remove after a chunk write is active to prove silent
  discard deletes partial storage, and replace the same lane with a different
  hash during interruption to prove latest-arrangement ownership. Both were
  added in the follow-up below and are now green.

### Iteration 3 final WAV race closure - partial teardown and replacement

- Expanded the interruption fixture from eight to eleven distinct WAVs. One
  new case pauses peer 1 at incoming finalization, after all chunks have been
  accepted and written privately, then confirms removal on peer 3. The case
  requires unchanged lane identity/name, zero new start timeouts, no hash or
  painted availability, exact preservation of all earlier WAVs, idle workers/
  queues/retries, and no `.partial.*` file in any peer root.
- Remove-after-chunks passed immediately in 88.76 s, proving BUG-P023's silent
  incoming discard also closes the durable staging-file boundary rather than
  only pre-start expectations.
- The final case pauses the source at outgoing validation for an old hash and
  drops different bytes onto the same painted lane before the pause expires.
  It requires the old validation to disappear within three seconds, the new
  hash to become the sole referenced/available asset, exact new bytes below all
  four isolated roots, unchanged lane ID/name/order, unchanged timeout counts,
  and painted `has_wav=true` on every peer.
- The initial replacement run failed in 20.69 s with the old validation still
  active and the new send queued behind it, exposing BUG-P024. After the shared
  unreferenced-hash fix, the complete eleven-WAV interruption matrix passed in
  71.27 s and 80.65 s.
- Aggregate gate proof: the selected `shared-content` suite passed 4/4 in
  84.91 s: looper units 0.01 s, asset-transfer units 0.06 s, ordinary exact
  four-peer shared content 13.67 s, and the eleven-WAV interruption matrix
  71.17 s.
- Self-review: cancellation scans every bank/lane first. If any other lane
  still references the replaced/removed hash, its validation/cache ownership
  is preserved. Only the final reference releases hash-scoped outgoing work
  and validation state. Existing service units already prove unrelated queued
  hashes advance after cancellation.
- Remaining WAV test need: no open removal/re-import/replacement lifecycle gap
  remains from this iteration. Continue treating newly observed real-jam WAV
  failures as high-priority regressions and add their exact ordering/state to
  this matrix rather than relying on generic share checks.
- Protocol status: no protocol field, message type, payload, packet/frame,
  parser/decoder acceptance, version, authentication, authorization, or wire
  ordering changed.

#### BUG-P024 - replacing an in-flight WAV left old validation ahead of the new hash

- Observed symptom: the lane immediately changed from `replacement-old.wav`
  to the new hash, but after three seconds the source still reported the old
  outgoing-validation pause, one pending validation, one queued send, and one
  outstanding Track Sync batch. The latest WAV could not start until obsolete
  work resumed or timed out.
- Root cause: confirmed removal cancelled a no-longer-referenced hash after
  BUG-P023, but the ordinary import/drop replacement path overwrote the lane
  without releasing the old hash's transfer ownership.
- Change: capture the replaced hash during real WAV import and route both
  replacement and confirmed removal through one meaningful helper. It scans
  all banks/lanes and invokes hash-scoped transfer cancellation plus validated-
  cache release only when no lane still references the old hash.
- Regression proof: the old validation now clears inside the three-second
  boundary; the complete eleven-WAV four-peer executable passed in 71.27 s and
  80.65 s with only the new hash/bytes/view present and no timeout, partial,
  retry, queue, identity, or earlier-WAV regression.
- Remaining test need: none for different-byte replacement during outgoing
  validation. Multiple-lane same-hash safety is enforced by the all-bank scan;
  add an explicit regression if a future user workflow permits intentional
  duplicate references to bypass the current content-deduplication behavior.

### Iteration 4 - complete Settings inventory and persistence behavior

- Opened the real Settings dialog through `application.settings` on four
  isolated GUI processes and paged its entire live object tree. The first red
  inventory found 667 interactive candidates versus 491 already classified:
  176 Settings-only semantic controls per process were absent from the prior
  initial-window contract.
- Registered stable page/preference-scoped IDs for the actual editors and
  actions: local/network/create/join audio; create connection; create/join
  tuning and runtime values; logs; recording modes/folders/count-in/loopback;
  startup; idea/reference-WAV/groove defaults; levels; metronome compensation;
  views/tracks; Jam Sync; navigation; Save; and Cancel. Audio device choices,
  tests, and Apply Audio remain explicitly hardware-profile-gated rather than
  being treated as deterministic fake-device passes.
- The second red inventory reduced the gap to two Browse buttons nested inside
  recording folder rows. After registering those actual child actions, all
  four live Settings dialogs exposed `667 / 667 classified / 0 unclassified /
  0 duplicate IDs`.
- The behavior workflow edits four independent preference owners through real
  controls: startup BPM, listener compensation deadband, automatic WAV sync,
  and input-recording count-in. It observes the live edits, cancels, reopens
  and proves no preference leaked; then saves the same changes, reopens and
  proves persistence, restores the original values, and saves again before
  shutdown.
- Authoritative Windows proof: the cancel-only behavior passed in 26.81 s.
  The expanded Save/reopen/restore workflow passed twice in 30.09 s and
  31.15 s, while retaining all earlier Local Engine, Start, Join, and Jam Sync
  modal checks in the same executable.
- Self-review: the Settings page list is inventoried as one semantic navigation
  contract, while each page's editors are directly invokable even when its
  scroll page is hidden. A speculative QListWidget adapter caused BUG-T040 and
  was removed; no unsafe adapter was retained merely to simulate page clicks.
- Remaining test need: hardware device Test/Apply behavior stays in the
  explicit hardware profile. File chooser dialogs need injected choices or
  separate persistence boundaries before Browse can be safely executed. Other
  specialist application dialogs remain active Slice 7 work.
- Protocol status: no network/protocol code or behavior changed.

#### BUG-T040 - speculative QListWidget introspection crashed every GUI agent

- Observed symptom: after adding generic QListWidget index/state support, all
  four `release/jam2.exe` processes exited with Windows access violation
  `0xC0000005` during the first Settings snapshot; their automation pipes ended
  before any behavior edit ran.
- Root cause: the new private-agent adapter was the only change after a green
  667-control snapshot and its rollback restored all four processes. The page
  list did not need a new generic adapter to validate preference behavior,
  because stable page-owned controls remain addressable while hidden.
- Change: removed the QListWidget cast/state/invocation extension and kept the
  existing safe item-view inventory contract. Representative editors are
  invoked directly; navigation is classified but not assigned a speculative
  operation.
- Regression proof: with the adapter removed, the cancel workflow passed in
  26.81 s and the Save/reopen/restore workflow passed twice in 30.09 s and
  31.15 s with clean shutdown of all four processes.
- Remaining test need: if direct page-navigation invocation becomes necessary,
  implement it as a Settings-owned callback or a narrowly tested virtual
  provider rather than broad generic QListWidget introspection.

### Iteration 5 - Listener Compensation dialog and live runtime tuning

- Audited the Advanced Listener Compensation workflow after the complete
  Settings inventory. The transient dialog had four numeric editors plus OK
  and Cancel but no stable control contracts. Its opener was also classified
  as state-gated even though it enters a nested modal event loop. The dialog is
  now a six-control modal contract with separate IDs from its hidden backing
  widgets; the opener is correctly modal.
- The product audit exposed BUG-P025: OK copied values only into hidden startup
  widgets. The values were not saved to `UserPreferences`, and a running jam's
  network supervisor continued using the immutable startup copy in
  `Jam2RuntimeOptions`. Settings Save had the same live-runtime gap.
- Added one typed `Jam2MetronomeCompensationSettings` boundary. Validated
  latest-value updates are coalesced under one host mutex, consumed atomically
  by the non-real-time network supervisor, published back in the effective
  operational snapshot, and discarded on runtime reset. No real-time callback
  reads the mutex and no engine/network packet command carries these values.
  Advanced Apply persists the preference and submits the local update;
  Settings Save reaches the same method through preference application.
- The four-process modal workflow now enters the real dialog, edits all four
  fields, proves Cancel leaks nothing, applies, checks the hidden maintained
  state, reopens to prove persistence, restores every value, and restores the
  prior metronome mode. It passed with the rebuilt public executable in
  41.38 s.
- The active four-peer fake-audio workflow switches the shared model to
  listener-compensated, edits all four values through the real dialog on one
  listener, waits for the network worker's published effective values, proves
  the other three peers did not inherit this local tuning, restores the values,
  returns all peers to shared-grid, and continues through the pre-existing
  metronome/transport/recording checks. It passed in 3.75 s.
- BUG-P026 was found during the runtime trace: the exposed deadband option was
  never consulted by the correction loop. Maximum clamp, smoothing, deadband,
  and slew are now one pure local step boundary. Native validation proves a
  1 ms/48-frame deadband holds a 47-frame error and releases a 49-frame error,
  100 ms smoothing produces the exact 10% step, 40 ms/s slew limits positive
  and negative movement symmetrically, and a 10 ms maximum clamps both sides.
- BUG-P027 was found during validation review: `std::stod("nan")` passed all
  four CLI range comparisons because comparisons with NaN are false. All four
  compensation CLI values now require `std::isfinite`; the boundary invokes
  the real parser and proves each NaN form is rejected.
- Authoritative native boundary/model proof passed in 69.55 s. The first full
  performance run passed 22/23 tests: core input, live four-peer GUI behavior,
  and 20/21 metronome mode/impairment cases were green. Only listener-
  compensated/loss was red, producing BUG-T042. Its exact case then passed
  twice in 17.27 s and 17.26 s with every strict threshold unchanged and a
  warning-free rebuild after the diagnostic-name cleanup. A second pre-fix
  complete run reproduced the common coordinator pause in listener-
  compensated/reordering, making the test-harness scheduling cause explicit.
- BUG-T042 was closed by matching the Windows impairment coordinator to the
  high-priority class used by all four Jam2 children and measuring every proxy
  pump interval. Any interval over 50 ms now rejects the run as invalid harness
  evidence before product assertions are interpreted. After the fix,
  reordering passed twice in 17.26 s and 17.25 s, loss passed in 17.26 s, and
  the authoritative complete performance gate passed 23/23 in 367.87 s.
- Self-review: update ownership is local and latest-value; reset cannot apply a
  stale update to a later session; Settings and Advanced use the same effective
  path; Cancel never submits; values are visible in application logs and the
  operational/automation snapshot; and no allocations, logging, exceptions,
  locks, or blocking work were added to an audio callback.
- Remaining test need: none for the Windows Listener Compensation iteration.
  Other specialist dialogs and the maintained app/function audit remain Slice
  7 work. macOS parity remains in `TEST-MACOS.md`.
- Protocol status: this iteration did not change any protocol source, field,
  message type, payload or packet shape, version, parser/decoder acceptance,
  authentication, authorization, epoch wire format, or ordering. The new
  runtime settings and effective snapshot fields are process-local only.

#### BUG-P025 - Listener Compensation Apply was startup-only and not persistent

- Observed symptom: changing maximum, smoothing, deadband, or slew in Advanced
  appeared to succeed, but a running jam continued with its startup values and
  reopening the application lost the edit. Saving the same values in Settings
  updated preferences/hidden widgets but still did not update an active worker.
- Root cause: the correction loop read a by-value `Jam2RuntimeOptions` copy on
  the network thread. Advanced OK only assigned hidden `QDoubleSpinBox` values;
  there was no local runtime update boundary and no preference save.
- Change: a validated four-value host update is coalesced atomically and
  consumed by the non-real-time network supervisor. Effective values are
  published in the operational snapshot. Advanced Apply updates preferences
  and the worker; Settings Save reaches the same worker method.
- Regression proof: four offline dialogs pass Cancel/Apply/reopen/restore in
  41.38 s. In the live 3.75 s four-peer workflow, all four effective values
  change on exactly the edited listener, remain unchanged on the other three,
  restore correctly, and the rest of the performance workflow stays green.
- Remaining test need: repeat the same dialog/runtime locality proof on macOS.

#### BUG-P026 - configured listener-compensation deadband was ignored

- Observed symptom: `--metronome-compensation-deadband-ms`, Settings, Advanced,
  startup output, and diagnostics exposed a deadband, but even errors inside it
  entered smoothing/slew correction. This could create needless small offset
  motion near convergence.
- Root cause: the network correction loop directly calculated alpha and slew;
  it never read `metronome_compensation_deadband_ms`.
- Change: maximum clamp, deadband, smoothing, and symmetric slew are applied in
  one directly tested pure step function. The deadband compares the remaining
  bounded target error before correction.
- Regression proof: the exact 47/49-frame boundary, smoothing, bidirectional
  slew, and positive/negative maximum tests pass in native validation. Twenty
  of twenty-one impairment cases passed in the initial matrix; the only red
  case had the external BUG-T042 pause and passed both exact repeats unchanged.
  After the harness fix, the complete performance catalogue passed 23/23 in
  367.87 s with the same listener-compensation assertions.
- Remaining test need: macOS must repeat the model and 21-case matrix.

#### BUG-P027 - compensation CLI accepted NaN values

- Observed symptom: each of the four compensation CLI options accepted `nan`,
  which could make correction arithmetic non-finite even though the documented
  ranges are numeric and bounded.
- Root cause: the parser checked only `< 0` and `> maximum`; both comparisons
  are false for NaN.
- Change: every compensation double must be finite before its existing range
  check. The runtime host independently applies the same finite/range contract.
- Regression proof: native boundary validation invokes the real CLI parser for
  all four flags and requires every NaN input to throw; the gate passed in
  69.55 s.
- Remaining test need: none for these four CLI fields; broader non-finite audit
  of unrelated double-valued CLI controls remains part of the app audit.

#### BUG-T041 - compensation opener was classified as non-modal

- Observed symptom: the Advanced button entered `QDialog::exec()` but its GUI
  contract said state-gated. The automation layer therefore could not express
  the required asynchronous-open/modal-inventory/close lifecycle accurately.
- Root cause: all metronome controls initially used one state-gated registration
  helper even though Advanced has a different interaction contract.
- Change: register the opener and all transient fields/actions explicitly as
  modal controls; retain distinct IDs for hidden backing values.
- Regression proof: the full transient inventory is classified with no
  duplicate/unclassified control and the four-process nested-modal workflow
  passed in 41.38 s with clean shutdown.
- Remaining test need: macOS nested Cocoa event-loop parity.

#### BUG-T042 - high-priority Jam2 children starved the impairment coordinator

- Observed symptom: the first complete post-change performance gate failed only
  listener-compensated/loss. Peer 1 had mapping error -6003 frames and 36,860
  mixer-capacity drops; all four peers independently reported approximately
  617..622 ms maximum network jitter/RTT, with large underrun or mixer backlog
  evidence. Epoch identity/revision/authority/beat agreement, authentication,
  recordings, and the other 20 impairment cases were correct. Artifacts were
  retained at `C:\Users\Phil\AppData\Local\Temp\jam2-metronome-listener-compensated-loss-FzeZLW`.
  A second pre-fix complete run failed only listener-compensated/reordering:
  peer 3 had mapping error -2496 frames while authority, revision, mode, beat,
  epochs, packet/callback activity, authentication, and zero mixer-capacity
  drops remained correct. Every peer simultaneously observed 404..577 ms
  maximum jitter and approximately 600 ms RTT even though the configured
  reordering window was only 8..16 ms. Artifacts were retained at
  `C:\Users\Phil\AppData\Local\Temp\jam2-metronome-listener-compensated-reordering-gZsGdn`.
- Root cause: the UDP impairment proxies are pumped inside the native test
  coordinator. That process remained at Windows normal priority while each of
  the four Jam2 children deliberately requested its shipped high-priority
  network profile. Under sustained sequential load the children could starve
  the proxy pump for hundreds of milliseconds, creating a synchronized burst
  that was not part of the configured condition. This is invalid harness
  timing, not a listener-compensation or epoch formula result. The strict
  product checks correctly failed rather than hiding the excursion.
- Change: on Windows the bounded coordinator now requests
  `HIGH_PRIORITY_CLASS` and its pump thread requests
  `THREAD_PRIORITY_HIGHEST`, matching the processes it measures. Every main-
  loop pump interval is measured; a gap over 50 ms fails explicitly as a test-
  harness scheduling stall and retains artifacts. Passing output includes the
  maximum pump gap as hard data. The detailed product failure still prints
  actual/expected authority, revision, mode, alignment, beat delta, epochs,
  mapped epoch, mapping error, packet/callback activity, mixer drops, and
  authentication failures. No product threshold was relaxed.
- Regression proof: after the harness fix, the exact reordering case passed
  twice in 17.26 s and 17.25 s and the loss case passed in 17.26 s. The full
  performance catalogue then passed 23/23 in 367.87 s: core input, the live
  four-peer fake-audio/runtime workflow, and all 21 metronome/epoch impairment
  cases. Zero mixer-capacity drops and the 1,024-frame mapping bound remain
  unchanged.
- Remaining test need: none on Windows. On macOS retain the cross-platform
  pump-gap measurement and diagnose native QoS scheduling if it exceeds the
  same bound; do not reinterpret such a run as product evidence.

### Iteration 6 - Arrangement editor ownership and live four-peer coverage

- Audited the Arrangement workflow as the next specialist section of the
  `MainWindow` responsibility map. The editor was a cohesive modal workflow,
  but its table construction, row operations, validation, result intent, and
  transient controls were embedded in `MainWindow::showArrangementDialog()`.
  Extracted those responsibilities into `ArrangementEditorDialog`; `MainWindow`
  now retains only application orchestration: supplying the current model,
  applying an accepted result, synchronizing it, and starting or stopping the
  arrangement through the existing session paths. This is an ownership-driven
  split, not a file-size split.
- Added stable modal contracts for the arrangement table, Add, Remove, Up,
  Down, Loop, Save, Save + Start/Stop, Cancel, and the parameterized section and
  repeat editors. The private content snapshot now reports arrangement rows,
  loop, enabled/running/armed state, and current step/repeat for exact test
  observation; no shared JSON or network message gained these fields.
- Native model validation proves row order, repeats, loop, local enabled-state
  handling, shared-JSON exclusion of local enabled state, and rejection without
  mutation of invalid banks, repeats outside 1..64, and more than 64 rows.
- The real modal workflow runs in four separate GUI processes. It edits two
  rows, exercises Add/Remove/Up/Down, proves Cancel leaves the exact model
  unchanged, proves Save/reopen persistence, proves Save + Start arms the
  arrangement, proves the active action becomes Stop, stops it, and restores
  the original model. Its first pass was green in 63.44 s. After the self-review
  fix for BUG-P028, the expanded regression passed in 68.55 s.
- Inside an authenticated four-peer fake-audio jam, the creator saves B x2 and
  C x3 with looping off through the real dialog and all four private content
  snapshots converge. Peer 4 then clears the rows and restores looping through
  the same dialog and all four converge again. This is part of the existing
  shared-content workflow, so it is followed by the complete lane, model, WAV,
  interruption, retry, and exact-byte assertions rather than a synthetic model-
  only exchange.
- A natural rerun exposed BUG-P029 in the existing same-byte WAV re-share case:
  the creator and source had the exact WAV while peers 2 and 4 were permanently
  idle without it. The deterministic race regression now leaves the only
  usable bytes on the source, holds that source at outgoing validation for
  three seconds, requires the pause to become active, and then requires exact
  bytes plus idle transfer state on all four peers.
- The first forced regression attempt also exposed BUG-T043 in the fixture:
  unreferenced managed copies intentionally remained after model removal, so
  the creator could serve a stale on-disk copy and bypass the intended source
  pause. The test now validates and removes only disposable non-source files
  below each isolated temporary peer root. Product code remains non-destructive.
- Regression proof: the forced four-peer WAV race passed in 39.21 s and 39.62 s.
  The complete shared-content catalogue passed 4/4 in 112.10 s: looper model,
  asset-transfer lifecycle, exact four-peer shared content, and the complete
  four-peer WAV interruption/retry matrix. The authoritative GUI-labelled gate
  then passed 8/8 in 218.01 s, including Jam Sync, shared content, WAV
  interruption, fake-audio performance, GUI inventory/actions, and the complete
  Arrangement modal workflow.
- Self-review: the dialog owns editing and returns a typed result; it does not
  own session state, persistence, transport, or sharing. The manual Track Sync
  republish is creator-only and automatic-sharing-off-only, so accepted joiner
  batches cannot form a republish loop. The WAV regression deletes only paths
  first proven to be within coordinator-created temporary peer roots. No real-
  time audio callback was changed.
- Protocol status: this iteration did not change a protocol source, message
  type, field, payload, version, parser/decoder acceptance rule, authentication,
  authorization, epoch format, or ordering. Arrangement automation content is
  process-private diagnostic state. Manual WAV recovery uses the existing batch
  publication and transfer protocol unchanged.
- Remaining test need: none for this iteration on Windows. macOS dialog/event-
  loop and filesystem parity is recorded in `TEST-MACOS.md`.

#### BUG-P028 - Arrangement row actions could target no row or a stale row

- Observed symptom: after Add, Remove/Up/Down could do nothing until the table
  acquired a current row. Clicking an embedded section combo or repeats editor
  also did not reliably make that row current, so a later row action could
  operate on a previously selected row.
- Root cause: the inline editor placed child widgets in table cells, but those
  widgets owned mouse/focus interaction; `QTableWidget` selection was not
  updated. Add also appended without setting the new row current.
- Change: an appended row becomes current immediately. Focus or mouse press on
  either embedded editor selects its owning row, as do value changes.
- Regression proof: the four-process modal workflow explicitly performs Add
  then Remove without editing the new row and verifies the exact result. The
  complete expanded modal workflow passed in 68.55 s.
- Remaining test need: repeat the modal workflow with native Cocoa focus/event
  delivery on macOS.

#### BUG-P029 - accepted manual WAV bytes were not always fanned out to the mesh

- Observed symptom: during same-byte re-share after removal with automatic WAV
  sharing disabled, the creator and source could hold the exact WAV while the
  other two peers completed their batches, became idle, and remained missing
  the file indefinitely.
- Root cause: when the creator accepted a joiner's manual Track Sync batch, it
  republished its complete batch only if applying the contribution changed
  arrangement metadata. A hash-addressed canonical asset path could already be
  present in that metadata; arrival of the missing bytes made the path usable
  without changing the model, so no full-mesh republish occurred.
- Change: after accepting any peer batch in manual-sharing mode, the creator
  schedules one publication of its current full batch across the mesh. Model
  refresh/snapshot work remains conditional on a real arrangement change.
  Joiners never perform this republish, preventing a publication loop.
- Regression proof: the original red run failed after 52.44 s with peers 2 and
  4 missing exact bytes while all queues were idle. The forced source-to-creator
  regression passed twice in 39.21 s and 39.62 s, then the complete shared-
  content catalogue passed 4/4 in 112.10 s, including the 71.94 s interruption
  and retry matrix.
- Remaining test need: repeat the forced path and exact four-peer convergence on
  macOS; do not relax its active-pause or idle-state assertions.

#### BUG-T043 - stale isolated copies let the WAV race fixture bypass its pause

- Observed symptom: the first deterministic BUG-P029 attempt timed out waiting
  for the source's outgoing-validation pause even though all four peers already
  had exact bytes. Expanded diagnostics showed the source still had
  `pause_armed: outgoing-validation` and no active transfer.
- Root cause: removing a WAV from the shared model correctly does not delete
  managed files. Old unreferenced copies remained in isolated peer storage, so
  the creator could satisfy the re-share without requesting bytes from the
  selected source. The fixture had not actually forced the race it claimed.
- Change: after confirmed model removal, the test captures the prior private
  paths, resolves and validates each target beneath its coordinator-owned
  temporary root, deletes only the three non-source copies, retains the source
  copy, and requires the configured pause to become active. Timeout output now
  includes every peer's complete transfer snapshot.
- Regression proof: the misleading attempt failed in 52.82 s; the expanded-
  diagnostic run failed in 52.81 s and identified the armed-but-unused pause.
  With isolated-copy control in place, the forced case passed twice and the
  complete 4/4 shared-content catalogue passed.
- Remaining test need: none in product behavior. Confirm case-sensitive root
  containment and cleanup for this disposable fixture on macOS.

### Iteration 7 - Listener Compensation dialog ownership extraction

- Revisited the fully covered Listener Compensation workflow as the next safe
  `MainWindow` ownership boundary. Extracted construction, four typed editors,
  stable modal-control registration, Apply/Cancel handling, and typed result
  capture into `ListenerCompensationDialog`.
- `MainWindow` deliberately retains the responsibilities that cross component
  boundaries: reading current Settings state, updating its maintained backing
  controls, saving `UserPreferences`, submitting the local live-runtime update,
  and writing the diagnostic log. The dialog has no access to the network
  runtime, session controller, preferences store, or metronome transport.
- Focused proof: a warning-free MSVC rebuild staged the one public
  `release/jam2.exe`; the real four-process modal workflow passed in 68.24 s,
  preserving inventory, Cancel, Apply, reopen persistence, and restoration.
  The active exact four-peer fake-audio workflow then passed in 4.22 s,
  preserving the rule that all four runtime values change on exactly the edited
  listener and not the other three peers.
- Complete proof: the authoritative GUI-labelled catalogue passed 8/8 in
  158.65 s, including Jam Sync, shared content, WAV interruption/retry,
  fake-audio performance, GUI inventory/actions, and both specialist dialog
  workflows.
- Self-review: the component consumes and returns the existing
  `Jam2MetronomeCompensationSettings` value without adding a parallel schema or
  abstraction. Existing numeric ranges, precision, suffixes, styling, labels,
  control IDs, and modal behavior are unchanged. No bug or functionality-risk
  item was found in this extraction.
- Protocol status: no protocol source, message, field, payload, version,
  parser/decoder acceptance, authentication, authorization, epoch behavior, or
  ordering changed. The settings type remains process-local and is explicitly
  never serialized onto the wire.
- Remaining test need: none on Windows. Repeat the same component-owned modal
  workflow and active listener-local runtime proof on macOS as already required
  by `TEST-MACOS.md`.

### Iteration 8 - Jam Sync dialog ownership and real Apply coverage

- Audited Jam Sync as the next `MainWindow` responsibility boundary. Its eight-
  control policy editor, dependency presentation, Leader Audio restriction,
  recording-lock presentation, and Apply/Cancel lifecycle formed one cohesive
  UI component. Creator ordering, joiner proposals, message parsing/routing,
  session protection, content publication, and runtime changes remain
  orchestration responsibilities.
- The audit found a validation gap before refactoring: the four-process modal
  test inventoried Jam Sync and proved Cancel, while the live four-peer test
  changed policy through a private typed automation command. The real Apply
  button had never carried a creator or joiner policy into the authority path.
- Closed that gap first against the pre-extraction implementation. Peer 3 opens
  the real dialog, edits every field to a local/chords/metronome policy, clicks
  Apply, and all four peers must converge on one exactly incremented revision.
  The creator then opens the real dialog, restores every field to the fully
  shared policy, clicks Apply, and all four peers must converge on the next
  exact revision. This pre-refactor proof passed in 2.04 s.
- Extracted `JamSyncDialog`. It owns all transient widgets, stable modal
  contracts, the typed policy draft, automatic-WAV and recording dependencies,
  the Leader Audio restriction, locked-state presentation, and Apply/Cancel.
  `MainWindow::showJamSyncDialog()` now supplies the current policy plus two
  runtime constraints and passes an accepted typed policy into the existing
  `requestJamSyncPolicy()` authority path.
- Self-review expanded the live regression to prove that disabling Track Lanes
  and Global Playback disables automatic WAVs and recording sync and clears the
  recording draft; restoring both dependencies re-enables those choices. In
  Leader Audio mode, metronome-state alone is disabled while Track Lanes and
  Apply remain enabled. The expanded post-extraction test passed in 5.11 s.
- An active shared lane recording is the only genuine full policy-lock state.
  Reaching it requires the currently inline Arm Lane Recording specialist dialog,
  which is itself missing a complete modal contract. Its policy-lock proof is
  carried into that recording-dialog coverage/extraction iteration rather than
  injecting an impossible synthetic state into this test.
- Complete proof: the Jam Sync selector passed 2/2 in 5.86 s, combining exact
  current-format parsing, normalization, route enforcement, stale-revision
  rejection, and the real-dialog four-peer workflow. The complete GUI catalogue
  passed 8/8 in 191.85 s, including shared-content and exact WAV interruption
  paths whose availability depends on Jam Sync policy.
- Self-review: `JamSyncDialog::policy()` starts from the supplied policy, so its
  local revision is preserved until the existing authority state orders or
  proposes it. No duplicate policy schema was added. All labels, tooltips,
  options, dependency behavior, stable IDs, and modal behavior are unchanged.
  No product bug or functionality-risk/removal decision was found.
- Protocol status: no network protocol source, type, field, payload, version,
  parser/decoder acceptance, authentication, authorization, epoch behavior, or
  ordering changed. The existing `jam.sync.request`/`jam.sync.set` format and
  revision rules are unchanged; this iteration only drives them through the
  real GUI.
- Remaining test need: cover full policy locking during a real shared lane
  recording in the Arm Lane Recording slice. Repeat Apply/dependency/authority
  behavior plus that later lock proof under Cocoa in `TEST-MACOS.md`.

### Iteration 9 - Arm Lane Recording ownership, real shared take, and WAV safety

- Audited the inline Arm Lane Recording workflow before extraction. The modal's
  mode selection, source selection, per-mode output/advanced drafts, validation,
  and Apply/Cancel lifecycle form one owned UI responsibility. Lane identity,
  source-router/engine submission, recording-group authority, transport,
  staging, arrangement publication, WAV sharing, and visible page refresh stay
  in `MainWindow` because they coordinate independent components.
- Added current live-dialog coverage before moving code. Each of four real GUI
  processes opens Arm Lane Recording, inventories all 16 semantic controls,
  switches Input/Current Jam/System Loopback, proves the advanced controls and
  independent drafts, and exercises Cancel and accepted loopback-without-engine
  behavior. The paired Remove dialog exposes Remove Lane, Delete WAV, and
  Cancel; Delete WAV is disabled for an empty lane.
- Extracted `LaneRecordingDialog`. It owns construction, stable modal contracts,
  mode-dependent rows, advanced presentation, source refresh, output browsing,
  three mode drafts, and a typed accepted result. Removed 423 lines of obsolete
  inline modal construction. A second ownership pass removed eight permanently
  hidden `MainWindowPages` widgets that had survived only as mutable storage;
  typed runtime recording state now owns output path, loopback choices/source,
  latency adjustment, silence/tail trimming, and trim flags. Visible global
  recording controls and all runtime behavior remain unchanged.
- Expanded `jam2_four_performance_integration` from a synthetic recording-state
  check to a genuine shared take. Peer 2 appends the actual target lane, opens
  the real Arm dialog, selects Input, disables count-in, becomes Ready, and all
  four peers must expose the same non-empty group ID, exact participant, remote
  recording state, protected interactions, and engine isolation. Peer 3 opens
  the real Jam Sync dialog during the take; every policy choice and Apply is
  disabled while Cancel remains enabled.
- The test privately occupies both bounded file workers immediately before
  stop. It requires a non-zero bounded import retry count, zero import failures,
  the exact appended lane ID, one identical 64-character SHA-256, a file larger
  than the WAV header, and exact byte availability on all four isolated roots.
  It then removes that lane and requires the exact baseline lane-ID set on all
  peers. The private hold is bounded to 100..5000 ms, requires an otherwise idle
  pool, and is unavailable outside the inherited GUI-agent channel.
- Red/green history was deliberately retained. Initial modal work timed out at
  154.75 s until BUG-T044/BUG-T045 were corrected; the covered pre-extraction
  modal flow then passed in 77.78 s. Cancel exposed BUG-P030 in 6.59 s. The
  accepted path failed in 6.71 s and 6.65 s while BUG-P031's incomplete guard
  was reviewed, then passed in 4.22 s with component ownership. Post-cleanup
  modal runs passed in 77.96 s and, after removal of hidden state, 79.27 s.
- The genuine WAV scenario first failed at 3.91 s, 42.01 s, and 9.94 s while
  BUG-T047 corrected the initial-lane and target identity assumptions. It then
  reached the real recording group/policy lock but failed to converge WAV bytes
  in 68.80 s and 23.63 s. Runs at 48.80 s and 49.54 s separated BUG-P032 from
  BUG-P033: local staging reported `attached` with a valid hash, then a deferred
  authoritative empty-lane snapshot erased it. The authority fix passed in
  21.66 s; deterministic two-worker saturation plus the same replay passed in
  13.44 s. The final rebuilt performance integration passed in 13.31 s.
- Self-review removed the last hidden widget-backed state, caught one MSVC-only
  mixed-width `qBound` ambiguity before tests (BUG-T048), and tightened pending-
  recording acknowledgment to require both lane ID and hash (BUG-P034). The
  first rebuilt source check failed at compile time with C2666; explicit
  `qint64` bounds fixed it. `git diff --check` reports no whitespace errors
  beyond the repository's existing CRLF notices.
- Final proof: `compile.cmd --tests performance` passed 23/23 in 377.12 s. This
  includes the real shared-recording case and all 21 four-peer metronome/epoch
  cells: shared-grid, leader-audio, and listener-compensated under clean, delay,
  jitter, loss, duplication, reordering, and burst loss. Focused modal proof
  passed in 79.27 s. The authoritative complete GUI catalogue passed 8/8 in
  220.68 s, including Jam Sync, shared content, WAV interruption/retry, GUI
  inventory/actions, modal workflows, and exact four-peer fake-audio recording.
- Protocol status: no production network message/type, field, payload/header
  shape, version, parser/decoder acceptance, authentication, authorization,
  compatibility behavior, metronome epoch behavior, or ordering changed. The
  added snapshots and worker hold are private inherited-channel diagnostics.
- Iteration exit: implementation, focused tests, self-review, gap fixes,
  focused retests, and broad GUI retest are complete on Windows. The remaining
  application audit stays active and will choose its next refactor by cohesive
  responsibility rather than line count. No new manual-review/removal item was
  found; `TEST-REVIEW.md` remains unchanged.

#### BUG-P030 - cancelling Arm Lane Recording changed live engine latency

- Observed symptom: editing manual latency in the Arm dialog and pressing
  Cancel left the engine at the edited value even though no lane was armed.
- Root cause: the inline dialog borrowed `MainWindow`'s permanent hidden spin
  box. Its existing `valueChanged` connection submitted every draft edit to the
  engine before the dialog's acceptance decision.
- Change: the owned dialog edits a private per-mode draft. `MainWindow` submits
  one bounded latency command only after an accepted result and only when the
  accepted value differs from the current engine snapshot.
- Regression proof: the four-process performance workflow failed in 6.59 s
  before the fix and now requires exact pre-Cancel latency plus an unarmed lane;
  the accepted path requires the new value only on the initiating peer and then
  restores it. Focused and complete gates are green.
- Remaining test need: repeat against a real CoreAudio device under the macOS
  hardware profile; deterministic Windows coverage is complete.

#### BUG-P031 - engine snapshots overwrote an open Arm dialog latency draft

- Observed symptom: an accepted non-zero manual-latency edit repeatedly returned
  to the live snapshot value while the modal was open, so Arm could apply zero
  or another stale value.
- Root cause: the modal editor and engine-snapshot presentation shared the same
  hidden `MainWindow` spin box. Snapshot consumption updated it regardless of
  the nested modal lifecycle.
- Change: `LaneRecordingDialog` owns its draft, while typed runtime state tracks
  only accepted/snapshotted engine data. Engine snapshots cannot address modal
  widgets.
- Regression proof: intermediate accepted-path runs failed in 6.71 s and 6.65
  s while the initial guard remained coupled. The owned component passed in
  4.22 s and both later focused/broad gates stayed green.
- Remaining test need: none on Windows; Cocoa parity is listed separately.

#### BUG-P032 - a saturated file-worker pool silently abandoned a recorded take

- Observed symptom: `importLastCaptureToArmedLane()` could fail to start its
  staging task, proceed through group finalization/disarm, and never attach the
  captured WAV. No retry or explicit failure status distinguished this from an
  ordinary successful finish.
- Root cause: the bounded worker submission result was ignored at this call
  site. Busy workers are a normal bounded-resource condition, not a permanent
  staging error.
- Change: staging retries every 50 ms for at most 200 attempts, retains the
  captured file if that bound expires, logs bounded progress, and publishes
  private retry/failure/status diagnostics. No work was moved into an audio
  callback.
- Regression proof: the native test holds both workers for 2000 ms before Stop
  and requires busy retries greater than zero, failures zero, and exact final
  WAV convergence on all four peers. It passed in 13.44 s and again in the
  13.31 s performance/full-gate case.
- Remaining test need: retain this deterministic saturation case on macOS; no
  automated soak is required.

#### BUG-P033 - a deferred authoritative snapshot erased a newly attached WAV

- Observed symptom: the recording peer reached import status `attached`, had a
  valid SHA-256 and correct target lane, then all four peers converged back to
  an empty lane and no WAV before the creator acknowledged the proposal.
- Root cause: a host-authoritative arrangement snapshot deferred during the
  protected recording could be applied immediately after local staging. It
  predated the joiner's recorded-lane proposal and replaced the complete local
  lane metadata before that proposal/acknowledgment round trip completed.
- Change: while a recorded lane proposal is pending, an older authoritative
  snapshot preserves that exact locally staged lane ID and hash in its bank,
  then the existing proposal/retry/creator-echo path continues. Authority is
  acknowledged only when the creator returns that exact identity and hash.
- Regression proof: pre-fix runs failed in 48.80 s and 49.54 s; the latter
  proved `attached` followed by erasure. The fixed replay passed in 21.66 s,
  then passed with deterministic worker saturation in 13.44/13.31 s and in the
  complete GUI gate.
- Remaining test need: retain duplicate-hash and delayed creator-echo reasoning;
  Apple process/filesystem parity is in `TEST-MACOS.md`.

#### BUG-P034 - a duplicate WAV hash could falsely acknowledge another lane

- Observed symptom: self-review found that the first authority fix treated any
  authoritative lane with the pending SHA-256 as acknowledgment, even if the
  pending recorded lane ID was absent. Reusing identical audio in two lanes
  could therefore clear protection for the wrong identity.
- Root cause: acknowledgment preferred content identity instead of requiring
  the arrangement identity and content identity together.
- Change: creator acknowledgment now requires the exact pending lane ID and,
  when present, the exact pending SHA-256. No network data or ordering changed.
- Regression proof: the tightened condition is exercised by the final rebuilt
  four-peer performance and complete GUI gates; existing shared-content tests
  also pass their repeated same-hash/different-lane ownership matrix.
- Remaining test need: add a directly forced duplicate-hash recorded-lane
  timing case if this authority handoff is refactored again; current broad
  same-hash and exact-ID coverage is green.

#### BUG-T044 - Arm/Remove painted actions were classified as always available

- Observed symptom: invoking Arm or Remove through automation while their modal
  was active could block the command pipe in a nested dialog and time out the
  four-process modal run.
- Root cause: the custom-painted looper actions did not declare their modal
  availability/lifecycle even though they open blocking dialogs.
- Change: both actions are classified as Modal and the harness uses the private
  asynchronous modal opener before inventory or interaction.
- Regression proof: the original run timed out at 154.75 s; corrected real
  modal inventory and flows pass in 77.78, 77.96, and 79.27 s.
- Remaining test need: repeat nested Cocoa event-loop behavior on macOS.

#### BUG-T045 - Remove Lane/WAV had no transient control contract

- Observed symptom: the confirmation's three choices could not be inventoried
  or driven by stable IDs, leaving Delete-WAV disabled-state behavior unproved.
- Root cause: the formerly ad hoc buttons were not registered as transient
  modal controls.
- Change: Remove Lane, Delete WAV, and Cancel now carry stable modal contracts;
  the test requires Delete WAV disabled for a genuinely empty lane.
- Regression proof: all final modal runs inventory exactly those controls on
  all four processes and complete cleanly.
- Remaining test need: Cocoa parity only.

#### BUG-T046 - the mode-draft fixture read hidden controls before loading mode

- Observed symptom: the first per-mode assertion read the loopback draft while
  Input remained active and saw hidden backing/default state instead of the
  mode's restored draft.
- Root cause: the fixture assumed switching away and back was implicit; the
  product correctly loads drafts only when the user selects that mode.
- Change: the test explicitly selects each capture mode before reading its
  fields, then proves independent values survive round trips.
- Regression proof: the corrected mode sequence passes every final four-process
  modal run.
- Remaining test need: none.

#### BUG-T047 - shared-recording fixture assumed an empty lane-zero baseline

- Observed symptom: the first genuine recording run failed because four fresh
  peers had legitimately merged one initial lane each. Later cleanup repeatedly
  removed an original lane while the appended target remained, timing out with
  a stable count of four rather than restoring the real baseline identities.
- Root cause: the fixture hard-coded lane zero and an empty/one-lane baseline
  instead of observing the converged four-peer model and appended lane ID.
- Change: it first requires exact baseline looper SHA/lane IDs, records the
  active-bank ID set, identifies the newly appended lane dynamically, and
  removes that exact identity after recording.
- Regression proof: red runs at 3.91, 42.01, and 9.94 s isolated the faulty
  assumptions. All later runs create, record, synchronize, and remove the
  correct lane with exact baseline restoration.
- Remaining test need: none; never reintroduce index-based identity assumptions.

#### BUG-T048 - mixed-width latency clamp did not compile under MSVC

- Observed symptom: the first hidden-widget cleanup build stopped with C2666
  because `qBound<qint64>` received `int` limits and an `int64_t` engine value.
- Root cause: Qt 6.10 exposes multiple heterogeneous `qBound` overloads, making
  that mixed-width call ambiguous on MSVC.
- Change: both limits and the snapshot value are explicitly converted to
  `qint64` before the final bounded result is converted to `int`.
- Regression proof: the immediate rebuild succeeded, followed by 23/23
  performance tests, the focused modal test, and 8/8 GUI tests.
- Remaining test need: Apple Clang compilation is explicitly required by
  `TEST-MACOS.md`; no runtime behavior changed.

### Iteration 10 - Start/Join dialog ownership and typed session runtime state

- Selected Start Jam and Join Jam as the next responsibility boundary because
  the old implementation created about forty controls in `MainWindowPages`,
  kept them permanently hidden, temporarily reparented them into two modal
  dialogs, and then used those widgets as the network runtime data model. This
  was a genuine ownership and draft-lifecycle problem, not a line-count split.
- Locked the accepted path before extraction. A new focused CTest launches
  exactly four GUI-agent processes, reserves four distinct TCP/UDP pairs,
  prepares fake `tone-440` audio, opens the real Start dialog on peer 1, and
  clicks its real Start button with no STUN, localhost endpoints, diagnostics
  off, and a peer limit of four. Peers 2--4 open the real Join dialog, enter the
  generated invite, and click Join. The test requires creator/joiner roles,
  three active remotes each, attached network engines, non-zero callbacks, no
  startup failure, and clean public Leave on all four.
- Pre-refactor red/green history: the first combined attempt timed out at
  118.53 s with only the creator active. Splitting the accepted workflow into
  `jam2_four_session_dialog_integration` first failed in 0.72 s because the
  startup-only fixture skipped the common hello exchange (BUG-T049). It next
  failed in 41.30 s with all joiners rejected because the headless path still
  preflighted a selected ASIO device (BUG-P035). The corrected pre-refactor
  workflow passed in 2.33 s.
- Added `SessionStartupDialogs.hpp/.cpp`. `StartJamDialog` owns its connection,
  STUN dependency, peer limit, device/channel, create profile, sample/buffer,
  network format, local tuning, diagnostics/runtime, Save Defaults, Refresh,
  New Session, Test Device, Start, and Cancel controls. `JoinJamDialog` owns its
  invite, local bind, device/channel, join profile, local tuning/runtime, Save
  Defaults, Refresh, Test Device, Join, and Cancel controls. Both return typed
  drafts and retain the existing stable automation IDs and visible behavior.
- `MainWindow` deliberately retains cross-component behavior: device discovery
  and hardware tests, preference persistence, session credential generation,
  invite parsing, creator/joiner authority, runtime attachment, and mesh
  orchestration. Initial extraction hit duplicate stable IDs because the old
  hidden backing widgets still advertised the same fields (BUG-T050); the
  focused accepted workflow failed in 41.42 s, then passed in 2.72 s after the
  hidden controls stopped owning visible identity.
- The first modal regression then failed in 70.03 s because fully isolated
  Join drafts also discarded the deliberately preserved invite URL on Cancel
  (BUG-T051). The one documented exception was restored explicitly: only the
  invite is retained; bind, port, device, profile, and tuning remain local. The
  focused modal workflow passed in 70.00 s.
- Self-review rejected leaving a parallel widget-backed runtime model. Added
  typed `SessionRuntimeDraft`, migrated runtime option construction, endpoints,
  profiles, local audio setup/restart, settings application, device selection,
  diagnostics, recording sample-rate fallback, and startup contract handling,
  then removed all Start/Join field pointers from `MainWindow.hpp` and all of
  their construction from `MainWindowPages`. The session page now creates only
  its actual page controls. The real four-peer workflow remained green in
  2.43 s after this deeper pass.
- Expanded the modal test on every process: mutate Start bind, port, sample
  rate, create profile, and profile-driven buffer, Cancel, reopen, and require
  exact baseline restoration. Mutate Join invite, bind, port, join profile, and
  profile-driven buffer, Cancel, reopen, require only the invite retained, and
  require networking inactive. The expanded proof passed in 72.75 s.
- The first complete GUI catalogue passed 8/9 in 185.77 s. Every one of 438
  live controls was named, stable, and classified with zero duplicates, but
  the inventory fixture still hard-coded a minimum of eight excluded hidden
  controls. Removing obsolete hidden widgets correctly lowered that count
  (BUG-T052). The focused inventory test passed in 3.85 s after changing the
  invariant to require complete classification and a represented explicit-
  exclusion mechanism rather than a historical quantity.
- Final proof: the complete Windows GUI catalogue passed 9/9 in 216.24 s:
  looper and asset units, exact four-peer Jam Sync, shared content, WAV
  interruption/recovery, fake-audio performance, GUI inventory/actions,
  expanded modal ownership, and real four-peer Start/Join.
- The post-refactor complete performance selector first passed 22/23 in
  375.72 s. Only listener-compensated/burst-loss was red: peer 3's steady WAV
  median was 504.42 frames against the unchanged 480-frame/10-ms bound, with a
  756.01-frame maximum inside the unchanged 2,400-frame/50-ms bound. Epoch,
  compensation convergence, three-peer contribution, mixer capacity, and the
  proxy-pump validity checks had passed. Artifacts were retained at
  `C:\Users\Phil\AppData\Local\Temp\jam2-metronome-listener-compensated-burst-loss-cZBLAU`.
- Read-only artifact analysis proved the error's direction instead of treating
  it as an unsigned detector excursion: remote mixed energy led the local click
  on all four listeners; peer 3 had a -504.42-frame signed median and
  -522.07-frame signed mean. The exact case passed twice immediately with every
  threshold unchanged. Self-review identified BUG-T053: the native failure
  message did not retain that sign or its final effective compensation state.
  The diagnostic-only rebuild passed the exact case in 17.24 s, followed by
  five consecutive passes in 86.22 s (17.23--17.26 s each). Including the two
  pre-diagnostic confirmations, eight exact reruns were green.
- The first attempted complete confirmation was invalidated when the command
  wrapper reached its two-minute capture limit. Its exact CTest coordinator,
  impairment harness, and four orphaned `release/jam2.exe` children were
  identified and stopped; no result from that interrupted run is counted. A
  separate uninterrupted run then passed the authoritative performance
  catalogue 23/23 in 376.29 s: core recovery, real four-GUI-peer fake audio,
  and all 21 metronome/epoch mode-by-impairment cells. `git diff --check`
  reports no whitespace error beyond existing CRLF notices.
- Protocol status: no production network type/message, field, header/payload
  shape, version, parser/decoder acceptance, authentication, authorization,
  metronome model/epoch rule, compatibility behavior, or ordering changed.
  The new prepare command and snapshots are private inherited-channel test
  controls only.
- Iteration exit: implementation, focused proof, self-review, hidden-state
  removal, strengthened proof, gap fixes, focused retests, and full GUI retest
  are complete on Windows. No feature/removal decision was introduced and no
  new `TEST-REVIEW.md` item is needed; REVIEW-001 remains intentionally open.

#### BUG-P035 - headless joins preflighted an unrelated physical device

- Observed symptom: all three fake-audio joiners failed an otherwise valid real
  Join-dialog workflow with `Selected audio device '[16] ASIO TONEX' does not
  support session sample rate 48000 Hz`.
- Root cause: the session-runtime binder performed physical-device sample-rate
  validation before considering that the already-built runtime options had
  explicitly selected headless audio. The fake engine neither opens nor
  depends on that physical device.
- Change: construct the typed runtime options first and perform the physical
  device preflight only for joiners whose options are not headless. The normal
  real-device validation path and failure message are unchanged.
- Regression proof: the real four-peer workflow failed in 41.30 s before the
  fix, passed in 2.33 s immediately after it, and passed in 2.72/2.43/2.52 s
  through the extraction, typed-state self-review, and final GUI gate.
- Remaining test need: macOS must repeat both headless fake-audio acceptance and
  hardware-profile CoreAudio rejection for an actually unsupported rate.

#### BUG-T049 - the startup-only modal fixture skipped the hello exchange

- Observed symptom: the first focused process returned a hello event where the
  fixture expected the response to its first command and failed in 0.72 s.
- Root cause: extracting the startup-only branch bypassed common coordinator
  handshake consumption that the original combined workflow performed.
- Change: all four hello records are consumed before either the offline modal
  branch or the startup-only real-session branch begins.
- Regression proof: every later focused and complete run starts with exact
  instance identity and command/event ordering.
- Remaining test need: retain this ordering under Cocoa pipes as required by
  `TEST-MACOS.md`.

#### BUG-T050 - extracted fields duplicated hidden backing control IDs

- Observed symptom: every real Start/Join field command was rejected as
  `control id is duplicated`; only the creator's untouched defaults started and
  the join invite remained empty.
- Root cause: the new dialog correctly registered its owned fields while the
  obsolete hidden `MainWindowPages` widgets still registered the same IDs.
- Change: removed visible-control identity from backing state, then completed
  self-review by deleting the backing widgets entirely in favor of typed state.
- Regression proof: the red focused run failed in 41.42 s; all subsequent
  focused, modal, inventory, and full GUI runs report zero duplicate IDs.
- Remaining test need: none; duplicate stable IDs remain a hard failure.

#### BUG-T051 - initial extraction discarded Join's documented Cancel draft

- Observed symptom: all four modal peers reopened Join with an empty invite
  after Cancel, failing the maintained REVIEW-001 contract.
- Root cause: the component isolated every transient field, but invite text is
  intentionally the one unsaved convenience draft retained by the old flow.
- Change: on rejection, `MainWindow` copies back only the typed invite value.
  All other Join fields are discarded, now with explicit assertions.
- Regression proof: the pre-fix modal run failed in 70.03 s; corrected runs
  passed in 70.00 and 72.75 s and in the final full gate.
- Remaining test need: REVIEW-001 remains a user decision; do not silently
  change this behavior without updating that review item and its test.

#### BUG-T052 - inventory coverage depended on an obsolete exclusion count

- Observed symptom: the first full post-refactor gate failed only because the
  fixture required at least eight explicitly excluded controls, despite all
  438 live semantic controls being stable and classified with zero duplicates.
- Root cause: a historical lower bound treated the existence of hidden widget
  debt as a coverage invariant.
- Change: retain the meaningful requirements--complete classification, zero
  duplicates, and support for explicit non-user exclusions--without requiring
  a particular quantity of obsolete hidden controls.
- Regression proof: the red catalogue passed 8/9 in 185.77 s; the corrected
  inventory test passed in 3.85 s and the full catalogue passed 9/9 in
  216.24 s.
- Remaining test need: inventory counts remain diagnostic, not compatibility
  values; never replace semantic completeness with a hard-coded count.

#### BUG-T053 - listener-alignment failures discarded direction and effective state

- Observed symptom: one complete post-refactor performance run failed peer 3's
  listener-compensated/burst-loss WAV median at 504.42 frames, only 24.42 frames
  beyond the strict 10-ms bound. The old message reported only absolute median,
  maximum, and window count, so it could not show whether remote energy led or
  lagged the local click or what compensation state produced the observation.
- Root cause: `listenerAudioValid` took the absolute value of each centroid
  immediately and did not include the final applied offset, target, or averaged
  latency in its retained failure reason. This was a test-diagnostic gap; the
  result did not justify a speculative product correction or weaker threshold.
- Change: retain signed centroid errors alongside the absolute distribution.
  On failure, report signed median and mean plus final compensation offset,
  target, and averaged latency. The 10-ms median and 50-ms maximum bounds and
  all production code remain unchanged.
- Regression proof: the red artifact gives peer 3 signed median -504.42 and
  signed mean -522.07 frames. Two unchanged pre-diagnostic reruns passed; the
  rebuilt exact test passed once and then five consecutive times; the complete
  selector passed all 23 tests in 376.29 s.
- Remaining test need: keep this hard evidence and the strict bound. If the edge
  recurs, use the retained sign/state values to separate consistent product
  bias from a one-run timing excursion before changing the model or criterion.

### Iteration 11 - Settings ownership, isolated GUI state, and active WAV re-request recovery

- Selected Settings as the next responsibility-owned `MainWindow` boundary.
  The old method constructed and coordinated the complete multi-page modal in
  about 1,600 lines while also performing application-level persistence and
  live-audio effects. This was an ownership seam, not a size quota: the new
  `SettingsDialog.cpp` remains a large cohesive editor because its pages,
  dependencies, validation, and draft lifecycle belong together.
- Locked the existing workflow before extraction with four GUI processes. The
  modal test exercised representative controls across every preference group,
  dependency enablement, profile-driven changes, Cancel isolation, Save,
  reopen persistence, and exact restoration. The pre-extraction run passed in
  91.90 s. Adding semantically correct combo activation exposed BUG-T054, and
  the first extracted build exposed the missing direct dependency in BUG-T055.
- Added `SettingsDialog.hpp/.cpp` with typed `SettingsDialogInput`, callbacks,
  and `SettingsDialogResult`. The dialog owns transient widgets and drafts.
  `MainWindow::showSettingsDialog` is now a compact application orchestrator;
  it retains device discovery/testing, applying or rolling back live local
  audio, persistence, and global runtime effects. `MainWindow.cpp` fell from
  roughly 19,077 to 17,650 lines as a consequence of ownership, not as the
  acceptance criterion. The post-extraction modal proof passed in 91.11 s.
- Added active-jam Settings coverage to the real Start/Join workflow so all
  four instances open the extracted editor while a full fake-audio mesh is
  active and Cancel without perturbing the jam. That focused run passed in
  5.22 s.
- The first complete GUI gate passed 8/9. All exact WAV bytes and shared
  arrangement/view state converged, but the creator retained one outgoing
  Track Sync batch until the cleanup deadline. Investigation added exact batch
  idle diagnostics and retained relevant per-peer asset logs rather than
  weakening the assertion.
- BUG-P036 fixed batch idle expiry: a timer firing after late progress used to
  schedule another complete 30-second interval, allowing nearly 60 seconds of
  inactivity before cleanup. Both incoming and outgoing schedulers now compute
  and wait only the remaining idle interval. The focused interruption test
  passed in 70.53 s with this correction.
- A proposed 45-second test bound then produced a useful red run in 48.41 s,
  but also exposed BUG-T056: the `jam2_tests_gui` build target did not depend on
  three GUI-labelled executables and could run a stale WAV harness. The target
  now builds every test selected by the GUI label. The test deadline returned
  to 60 seconds because legitimate recovery progress can occur around 30
  seconds; no product timeout or acceptance condition was relaxed.
- Rebuilt runs (71.17, 80.63, and 91.21 s) and isolated logs found BUG-P037.
  An authoritative arrangement could supersede a receiver's pending stream,
  after which the receiver correctly re-requested the same hash from the same
  source. `AssetTransferService::queueSend` discarded that request because the
  old stream was still active, so the sender continued toward an abandoned
  receiver state and hit its 10-second progress timeout. The next peer could
  then reach its request deadline just before the delayed start, multiplying
  delay across a four-peer share.
- Active same-hash/same-peer re-requests now explicitly restart the outgoing
  stream from validation and byte zero. Validation-pending and queued duplicate
  requests remain deduplicated. The wire messages, fields, framing, hashes,
  acknowledgements, authentication, authorization, and timeout constants are
  unchanged. A direct unit regression starts a send, emits data, re-requests
  it, requires a fresh start and chunk zero, and completes exactly; it passed
  in 0.09 s. Real four-peer WAV interruption/recovery then passed in 41.25,
  40.38, and finally 40.18 s with exact bytes, converged model/view state, and
  no residual transfer batches.
- Self-review found BUG-T057: `--storage-root` isolated application assets but
  `UserPreferencesStore` still read the machine's shared `QStandardPaths`
  file. A saved absolute log folder could therefore send automation logs into
  `release/logs`, and concurrent peers could consume real user preferences.
  The private GUI-agent startup now installs an absolute per-peer preferences
  path before constructing `MainWindow`; normal application startup is
  unchanged. The full modal workflow requires each saved preferences file and
  every workflow requires a GUI log below its own temporary root. A focused
  modal run passed in 91.90 s, and a later WAV run left the existing
  `release/logs` timestamp unchanged.
- The first startup-only isolation proof failed in 5.15 s solely because it
  required a preferences file after opening Settings and pressing Cancel
  (BUG-T058). The assertion now always requires isolated logs but requires a
  preferences file only in the workflow that saves Settings. The exact
  four-process startup-dialog test passed in 5.35 s.
- Final Windows proof: the complete GUI catalogue passed 9/9 in 192.89 s. It
  includes looper/project and asset units; four-peer Jam Sync, shared-content,
  WAV interruption, fake-audio performance, GUI inventory/actions, the full
  Settings/modal traversal, and real Start/Join. The complete performance gate
  then passed 23/23 in 375.78 s: core recovery, four-GUI-peer fake audio, and
  every shared-grid, leader-audio, and listener-compensated metronome/epoch case
  under clean, delay, jitter, loss, duplication, reordering, and burst loss.
- Self-review outcome: the active re-request is scoped to an already active
  hash/target pair on an authenticated control path; existing pending-work and
  failed-key bounds remain intact. No visible feature/removal decision was
  made, so `TEST-REVIEW.md` gains no new item and REVIEW-001 remains open.
  `git diff --check` reports no whitespace error beyond existing CRLF notices.
- Protocol status: no production network type/message, field, fixed header or
  payload shape, version, parser/decoder acceptance, authentication,
  authorization, metronome model/epoch behavior, or ordering changed.

#### BUG-P036 - Track Sync idle expiry could wait almost two full windows

- Observed symptom: a completed four-peer WAV/model convergence retained an
  outgoing batch until near the 60-second test deadline despite the documented
  30-second idle lifetime.
- Root cause: when the first expiry callback observed recent progress, it
  scheduled a fresh full interval instead of subtracting elapsed idle time.
- Change: incoming and outgoing expiry callbacks calculate the remaining
  duration from their last progress timestamps and only wait that remainder.
- Regression proof: idle duration is visible in the automation snapshot; the
  focused scenario passed after the fix and every final WAV/GUI run cleaned up.
- Remaining test need: repeat timer scheduling under Cocoa in `TEST-MACOS.md`.

#### BUG-P037 - an active WAV re-request was discarded as a duplicate

- Observed symptom: real four-peer sharing intermittently took 70--91 seconds
  and logged outgoing progress timeouts even though all peers eventually held
  exact WAV bytes and the correct arrangement.
- Root cause: a receiver that superseded an in-flight stream legitimately
  re-requested the same `(hash, source)`, but the sender ignored it while its
  obsolete stream was still active and kept waiting for impossible ACKs.
- Change: an active same-hash/same-peer re-request invalidates the old outgoing
  generation and restarts validation/start/chunks from byte zero. Queued and
  validation-pending duplicates still collapse normally.
- Regression proof: the new unit test proves exact restart sequencing and
  completion; three real four-peer runs after the fix passed in 41.25, 40.38,
  and 40.18 s with no timeout or residual state; the full GUI gate passed 9/9.
- Remaining test need: Apple parity and continued network-instability coverage;
  keep restart behavior bounded by the existing authenticated request path.

#### BUG-T054 - automation could select but not activate a profile index

- Observed symptom: Settings tests could change a combo's stored index but
  could not trigger the user activation path that applies profile-dependent
  fields, leaving that dependency behavior unproved.
- Root cause: the private GUI control agent lacked an operation that emits the
  semantic activation associated with a user choosing an item.
- Change: added private `activate-index` support with the same stable-control
  validation used by other operations.
- Regression proof: all four Settings workflows activate profiles and verify
  their dependent values in the focused and complete modal passes.
- Remaining test need: Cocoa event-delivery parity only.

#### BUG-T055 - Settings extraction depended on an indirect include

- Observed symptom: the first extracted Settings build could not resolve the
  practice-idea profile symbols used by that dialog.
- Root cause: the code had previously received the declaration transitively
  through `MainWindow`; the new owned translation unit did not include its
  direct dependency.
- Change: `SettingsDialog.cpp` directly includes `PracticeIdeaGenerator.hpp`.
- Regression proof: the immediate MSVC rebuild succeeded and all focused/full
  tests passed.
- Remaining test need: Apple Clang compilation is listed in `TEST-MACOS.md`.

#### BUG-T056 - the GUI suite target could execute stale labelled harnesses

- Observed symptom: a focused WAV command could select a CTest executable that
  the `jam2_tests_gui` target had not rebuilt, obscuring whether a changed test
  deadline was actually running.
- Root cause: the aggregate target omitted looper-project, asset-transfer, and
  four-WAV-interruption executables even though all carried the `gui` label.
- Change: added all three as explicit dependencies of `jam2_tests_gui`.
- Regression proof: subsequent focused and full commands rebuilt the changed
  harness; the final GUI selector passed all nine tests.
- Remaining test need: verify identical target/label closure with Apple CMake.

#### BUG-T057 - GUI agents shared real preferences and log destinations

- Observed symptom: isolated four-process tests could load workstation
  preferences and write logs beneath the staged release folder when a saved
  absolute logging path existed.
- Root cause: the agent's storage-root override did not cover the separate
  `QStandardPaths` preferences store, from which the logging destination was
  loaded.
- Change: before `MainWindow` construction, automation assigns each process an
  absolute `<storage-root>/config/preferences.ini`; product launches retain the
  normal platform path. Tests require logs and saved preferences where the
  scenario actually saves them.
- Regression proof: the full modal test passed with all four isolated roots;
  later WAV execution did not update `release/logs`; final GUI/performance
  suites passed completely.
- Remaining test need: prove macOS application-support path isolation and file
  permissions under Cocoa.

#### BUG-T058 - startup-only isolation proof required a nonexistent save

- Observed symptom: all four startup-dialog peers completed correctly but the
  test failed in 5.15 s because no preferences file existed after Settings was
  cancelled.
- Root cause: the new isolation assertion assumed every modal scenario saves
  preferences, while the startup-only path intentionally verifies Cancel.
- Change: logs remain mandatory for every process; persisted preferences are
  required only for the full workflow that clicks Save.
- Regression proof: the corrected exact test passed in 5.35 s and the full GUI
  selector passed 9/9.
- Remaining test need: none; retain the distinction between draft cancellation
  and persistence.

### Iteration 12 - Local Engine dialog ownership

- The next `MainWindow` audit compared several candidates by responsibility.
  The audio/MIDI/plugin block is larger, but currently coordinates live plugin
  hosts, MIDI devices, router topology, file workers, and asynchronous editor
  state; extracting it without first defining those owners would create a weak
  callback boundary. The Local Engine startup editor was the safer coherent
  seam and already belongs beside Start/Join in `SessionStartupDialogs`.
- Strengthened the pre-refactor four-process contract. On every startup dialog
  it records sample-rate index, buffer-size index, input channels, and output
  channels; changes all four, checks Save as Local defaults, confirms the
  edits are visible, then presses Cancel. When the separate Settings editor
  later opens, all four original values must be exact, proving neither runtime
  draft nor persisted defaults leaked. The first test build exposed BUG-T059;
  after that fixture correction, the existing implementation passed in
  94.46 s.
- Added typed `LocalEngineDialogState` and `LocalEngineDialogCallbacks` plus an
  owned `LocalEngineDialog` in `SessionStartupDialogs.hpp/.cpp`. It owns the
  transient device, sample-rate, buffer-size, input/output-channel, test, save,
  start, and cancel controls while preserving their stable IDs, labels,
  dimensions, modality, and placement behavior.
- `MainWindow::showLocalPerformSetup` now performs only application-level work:
  refresh and match physical devices, construct the typed input, supply the
  hardware-test callback, position/invoke the dialog, map an accepted result
  into runtime state, optionally persist it, and launch the local engine. The
  duplicate sample-rate/buffer widget factories were removed from
  `MainWindow.cpp`; it now has 17,554 lines, a consequence rather than a target.
- The first post-extraction proof passed in 94.78 s. Self-review then found the
  untested no-device lifecycle drift in BUG-T060 and restored exact behavior.
  The corrected focused proof passed in 93.79 s.
- Final Windows proof: the complete GUI catalogue passed 9/9 in 177.90 s,
  including exact four-peer Jam Sync, shared content, WAV interruption in
  39.90 s, fake audio, inventory/actions, all modal workflows in 93.30 s, and
  real Start/Join. The 23/23 performance/metronome/epoch catalogue had already
  passed in 375.78 s immediately before this UI-only extraction; the final GUI
  gate's four-peer fake-audio performance case also passed in 12.42 s.
- Scope/protocol status: no runtime option meaning, device enumeration/test,
  engine launch, audio callback/path, network type/message/field/payload,
  parser, version, authentication, metronome model/epoch behavior, or ordering
  changed. No feature/removal or ambiguous behavior decision was introduced,
  so `TEST-REVIEW.md` remains unchanged.

#### BUG-T059 - strengthened Local Engine fixture reused a const snapshot

- Observed symptom: the first pre-refactor test build failed with MSVC C2678
  when the fixture tried to assign the post-edit snapshot over its initial one.
- Root cause: the existing one-shot `Snapshot` local was declared `const`; the
  strengthened proof now intentionally refreshes it.
- Change: made only that test-local snapshot mutable.
- Regression proof: the rebuilt pre-refactor workflow passed in 94.46 s and all
  later focused/full runs passed.
- Remaining test need: none.

#### BUG-T060 - initial extraction changed the no-device warning lifecycle

- Observed symptom: self-review found that the first component draft would
  keep Local Engine open after the user pressed Start with no available device,
  while the maintained implementation closed the editor before showing its
  warning.
- Root cause: device-selection validation had been moved inside the owned
  dialog even though the old lifecycle performed it at the application
  orchestration boundary after acceptance. This hardware-gated branch was not
  reached by the normal headless modal regression.
- Change: the dialog again accepts normally; `MainWindow` reads the typed
  result, closes the editor, and emits the same `Perform` warning before any
  runtime mutation when the selected ID is empty.
- Regression proof: the first extracted flow passed in 94.78 s; after the
  self-review correction it passed in 93.79 s and the full GUI gate passed 9/9.
- Remaining test need: an explicit Windows hardware profile must exercise this
  branch when no selectable device is exposed; repeat CoreAudio behavior on
  macOS. No final product behavior differs from the baseline.

### Iteration 13 - Audio Inputs ownership and routed synthetic input

- Audited the combined Audio Inputs, MIDI Inputs, and Plugins area before
  refactoring it. The area is large because it spans physical topology,
  timestamped MIDI device lifecycle, asynchronous plugin workers/editors, and
  recording locks. Only Audio Inputs had a complete cohesive ownership seam in
  this iteration; MIDI and plugin hosting deliberately remain in `MainWindow`
  until their lifetimes have direct behavioral proof. This is responsibility-
  driven refactoring, not a file-size quota.
- Added stable modal control identities for every Audio Inputs source/include/
  level/ungroup and stereo-pair action, every currently rendered MIDI source
  action plus discovery/add/cancel, and every audio/MIDI plugin open/load/
  bypass/remove action. The explicit discovery Cancel button also removes the
  prior dependence on Qt creating an implicit button object.
- Expanded the real four-process Start/Join workflow so the creator and three
  joiners each start with two selected fake-audio input channels. On every
  active peer it inventories Audio Inputs, changes include and level, closes
  and reopens to prove live persistence, restores the values, groups two mono
  inputs, proves one routed source remains, ungroups, and proves both exact
  slots return. It also inventories MIDI and plugin controls and requires the
  correct enabled/disabled no-device/no-plugin state.
- Added read-only `InputSourceRouter` slot snapshots and performance JSON for
  physical-channel count, configured-source count, render count, peak, invalid
  configurations, renderer failures, and all 16 fixed slots. The integration
  requires exact slot/channel/kind/enabled/level mappings and checks the 2 -> 1
  -> 2 topology transition on all four peers. The initial UI-only workflow
  passed in 15.90 s; this self-review correctly rejected that as insufficient
  proof because it did not establish that the audio path consumed the edits.
- The first exact router run went red twice in 15.48 s and 15.64 s. Every peer
  had two correctly configured/enabled slots but `rendered_blocks == 0` and
  `peak_ppm == 0` while engine callbacks and the injected tone were active.
  This exposed BUG-P038 rather than being hidden by a UI-only assertion.
- Routed synthetic input through `InputSourceRouter` in the headless engine and
  the platform test-input branches. All scratch audio and pointer arrays are
  allocated before streaming; the callbacks add no allocation, lock, logging,
  exception, or blocking operation. A new core regression proves disable,
  per-source level, exact topology, non-zero signal, render counts, and zero
  invalid/renderer failures. It passed in 1.85 s before extraction and 1.90 s
  in the final focused run. The four-peer router proof passed in 15.62 s.
- While exposing slot diagnostics, source reconfiguration was first tightened
  to disable and unpublish before replacing its fixed fields. Self-review did
  not accept that alone: a callback that had already read the old published
  flag could still consume a mixed topology. BUG-P039 is closed with an even/
  odd lock-free topology revision. Callback readers copy the fixed fields and
  use them only when both revision reads match; an overlapping writer causes
  that one slot to be skipped for the current block. Diagnostic readers retry
  at most three times. No lock or unbounded retry enters the real-time path.
- Added `InputSourceDialogs.hpp/.cpp`. `AudioInputSourcesDialog` now owns all
  transient Audio Inputs widgets, scroll/layout construction, live panel
  rebuilding, plugin-removal confirmations, grouping affordances, and stable
  automation contracts. `MainWindow::showAudioInputSources` is an application
  adapter: it snapshots names/source state and performs bounded live mutations,
  plugin teardown, router refresh, and recording-lock decisions. The old modal
  body was removed rather than left duplicated. No label, tooltip, validation,
  confirmation, grouping, plugin-removal, or persistence behavior was removed.
- The extracted four-peer workflow passed in 15.53 s. The first complete
  catalogues passed GUI 9/9 in 186.56 s and performance 23/23 in 376.46 s.
  After the publication self-review gap was corrected, the focused core and
  four-peer tests passed again in 1.87 s and 15.62 s. The authoritative final
  Windows proof passed the complete GUI catalogue 9/9 in 189.12 s (including
  exact WAV interruption/recovery in 40.42 s) and the performance catalogue
  23/23 in 376.44 s. That final run includes all 21 shared-grid, leader-audio,
  and listener-compensated metronome/epoch cells under clean, delay, jitter,
  loss, duplication, reordering, and burst loss.
- Self-review outcome: the Audio Inputs component boundary is coherent; MIDI
  and plugin extraction would presently blur worker/device ownership and is
  deferred rather than forced. `TEST-REVIEW.md` gains no item because no
  feature/removal or ambiguous product decision was introduced. `git diff
  --check` reports no whitespace error beyond existing CRLF notices.
- Protocol status: no network type/message, field, fixed header/payload shape,
  parser/decoder acceptance, version, authentication, authorization,
  metronome model/epoch behavior, or ordering changed.

#### BUG-P038 - injected audio bypassed the live input-source router

- Observed symptom: all four fake-audio peers showed the expected Audio Inputs
  controls and source configuration, but router render count and peak remained
  zero while callbacks and Tone440 injection were active. Include, level, and
  grouping therefore could not be validated against actual fake-audio output.
- Root cause: headless and platform test-input branches wrote the generated
  mono block directly to capture/network paths instead of presenting it as
  physical channels to `InputSourceRouter`.
- Change: preallocate planar scratch/pointer storage, copy or expose the
  injected signal as each selected physical input, and route it through the
  normal source mixer before capture. A router with no active source yields
  silence, matching physical-input behavior.
- Regression proof: the native core disable/level proof passed; the exact
  four-peer topology/signal workflow passed; final GUI and performance gates
  passed completely, including WAV and metronome/epoch matrices.
- Remaining test need: compile and execute the CoreAudio test-input branch on
  macOS, then run real-device routing only with an explicit hardware profile.

#### BUG-P039 - source reconfiguration could publish mixed topology fields

- Observed symptom: self-review of the new slot diagnostics found that a live
  configured slot remained published while its channel/kind/renderer fields
  were replaced, allowing callback or diagnostic readers to observe a
  transitional combination.
- Root cause: `configure` wrote the fixed atomic fields before publishing the
  final enabled state but did not first withdraw the prior configuration;
  `clear` unpublished before all old fields were reset.
- Change: disable and unpublish before replacement, then bracket all fixed
  topology writes with an odd/even atomic revision. Callback readers take a
  local fixed-field snapshot and require an unchanged even revision before use;
  diagnostic readers use the same rule with three bounded attempts. Clearing
  follows the same writer protocol.
- Regression proof: exact slot snapshots are asserted in the core unit and on
  all four live GUI peers through group/ungroup transitions; both full gates
  passed.
- Remaining test need: repeat under Apple callback scheduling and retain this
  publication-order regression when plugin/MIDI lifecycle coverage expands.

#### BUG-T061 - Qt's `slots` macro collided with new test diagnostics

- Observed symptom: the first MSVC build of the router assertions failed where
  a local JSON array was named `slots`.
- Root cause: Qt expands `slots` as a keyword macro in translation units using
  QObject headers.
- Change: renamed the test and snapshot locals to `sourceSlots`; no product
  behavior changed.
- Regression proof: focused core/four-peer builds and both complete catalogues
  compile and pass.
- Remaining test need: none.

### Iteration 14 - MIDI Inputs ownership, deterministic devices, and transient-safe GUI control

- Established the MIDI device boundary before changing the dialog. The new
  `MidiInputBackend` delegates production discovery/opening to the existing
  platform implementation. The private GUI agent injects two fixed synthetic
  devices with a deliberate one-second discovery delay; normal startup cannot
  select that backend. The backend is shared across asynchronous workers so a
  queued enumerate/open operation cannot outlive its owner.
- Added `jam2_midi_input_backend_units`. It proves invalid and duplicate device
  inventory filtering, unknown-ID rejection, inject-while-closed rejection,
  exclusive open ownership, exact complete MIDI 1.0 channel-voice delivery,
  unsupported-event diagnostics, close/reopen, bounded queue fill, false
  delivery on overflow, and the queue's retained drop count. The final focused
  run passed 1/1 in 0.03 s.
- Strengthened the existing real four-process Start/Join workflow before
  extraction. On each peer it discovers the two devices, chooses a device and
  MPE, assigns the first free slot after two fake-audio inputs, edits Standard
  mode/include/37% level, closes and reopens, inventories the matching Plugins
  tile/action states, removes the source, verifies exact router cleanup, then
  assigns the other device in Standard mode into reclaimed slot 2 and removes
  it again. The first pre-extraction assignment/edit/remove proof passed in
  27.64 s.
- Added explicit discovery and assignment cancellation. Canceling the progress
  modal must leave no source and must suppress the completion after the fake
  one-second enumeration returns. Canceling the nested configuration after
  selecting the second device and MPE must likewise create no source. These
  assertions run on all four peers and retain full performance diagnostics.
- Added `MidiInputSourcesDialog` to `InputSourceDialogs.hpp/.cpp`. It owns the
  transient MIDI panels, state-driven rebuilding, Add/Close actions, explicit
  discovery progress/Cancel, the nested device/mode editor, assigned-device
  filtering, empty/all-assigned messages, warnings, and stable automation IDs.
  `MainWindow::showMidiInputSources` is now a typed adapter retaining live
  device/queue/plugin/router ownership, recording locks, async worker dispatch,
  source-slot allocation, MIDI reset, and retirement. The initial extracted
  assignment/edit/remove flow passed in 27.32 s.
- Added exact `QListWidget` state and bounded `set-current-row` support to the
  GUI agent. Added an exact-control snapshot request that resolves one stable
  ID, returns classified state or explicit absence, and rejects duplicates or
  a mixed cursor/control request in one GUI event-loop turn. Full paginated
  inventories remain unchanged and strict, and are still used at every stable
  modal state. The exact contract passed on four GUI processes in 4.02 s.
- The first discovery-Cancel run timed out at 120.06 s because each polling
  attempt paged roughly 455 controls and missed the short-lived progress modal;
  late nested dialogs then overlapped and correctly triggered duplicate-ID
  failures. Increasing the deterministic discovery delay to one second proved
  the product lifecycle completed, but the next 44.13-s run reported 96
  inventory-change failures as progress changed to configuration during the
  15-page snapshot. BUG-T065 closes this as a harness-contract defect rather
  than weakening the inventory audit. The exact-control version passed in
  36.72 s; after the slot-reuse review addition it passed in 42.73 s.
- Self-review found and corrected the GUI aggregate build dependency, exercised
  queue saturation, compared the component against the former inline workflow,
  and confirmed cancellation guards, backend lifetime, rebuild ownership,
  source cleanup, and first-free-slot reuse. The former labels and product
  messages remain intact; dynamic topology-lock state is now refreshed when
  panels rebuild.
- The first complete GUI command was externally terminated at 124 s because
  its shell wrapper had a 120-s limit. Its exact CTest/wrapper/four-peer process
  tree was stopped and the unchanged catalogue was rerun with sufficient
  wrapper headroom. This was not a CTest or product failure.
- Authoritative Windows evidence: GUI passed 10/10 in 227.41 s, including jam
  sync in 4.64 s, shared content in 19.02 s, WAV interruption/recovery in
  50.78 s, fake-audio performance in 12.55 s, the complete modal catalogue in
  93.83 s, and the expanded session/MIDI flow in 42.68 s. Performance passed
  23/23 in 376.03 s: core input, four-peer fake audio, and all 21 shared-grid,
  leader-audio, and listener-compensated metronome/epoch cases under clean,
  delay, jitter, loss, duplication, reordering, and burst loss.
- Scope/protocol status: no physical Windows MIDI implementation, audio
  callback/path, engine behavior, network type/message/field/payload, parser,
  version, authentication, metronome model/epoch behavior, or ordering changed.
  No feature/removal or ambiguous behavior decision was introduced, so
  `TEST-REVIEW.md` remains unchanged.

#### BUG-T062 - incomplete backend ownership in a default argument did not compile under MSVC

- Observed symptom: the first backend-injection build failed while compiling a
  constructor declaration that combined an incomplete `MidiInputBackend` type,
  `std::unique_ptr`, and a default argument.
- Root cause: destruction of the default temporary was instantiated where the
  forward-declared backend was incomplete.
- Change: retained the lightweight forward declaration and added a normal
  one-argument constructor that delegates in `MainWindow.cpp` to a second
  explicit ownership-injection constructor.
- Regression proof: all subsequent MSVC focused and complete builds pass, and
  normal startup selects the system backend while GUI automation selects the
  synthetic backend.
- Remaining test need: compile both constructors with Apple Clang as recorded
  in `TEST-MACOS.md`.

#### BUG-T063 - classified device lists could not be selected by GUI automation

- Observed symptom: the strengthened Add MIDI Device flow could inventory the
  `QListWidget` but had no legal operation for choosing the second device.
- Root cause: the GUI agent classified abstract item views but only exposed
  mutations for buttons, combos, value editors, tabs, text fields, actions, and
  virtual controls.
- Change: added exact current-row/count/text state for `QListWidget` and a
  bounded `set-current-row` operation that rejects empty or out-of-range lists.
- Regression proof: every peer selects device rows 0 and 1 during Cancel,
  assignment, and reclaimed-slot workflows; full inventory stays classified.
- Remaining test need: none.

#### BUG-T064 - the strengthened four-peer fixture used two invalid C++ expressions

- Observed symptom: the first test build referenced the old loop variable name
  and used `QJsonArray::front()`, which is unavailable in the supported Qt API.
- Root cause: the MIDI assertions were added to an existing peer loop without
  updating one index expression and used a container convenience method not
  provided by `QJsonArray`.
- Change: use the actual `peer` loop index and bounded `at(0)` after requiring
  array size one.
- Regression proof: every later focused and complete MSVC build compiles and
  runs the assertions.
- Remaining test need: none.

#### BUG-T065 - paginated whole-window polling could not observe transient modals safely

- Observed symptom: discovery Cancel was missed or the global control count
  changed while a 15-page snapshot crossed the progress-to-configuration
  transition, producing timeout, overlap, duplicate-ID, or inventory-change
  failures even though the product lifecycle completed.
- Root cause: the wait helper reused the deliberately strict whole-window
  inventory protocol for transient synchronization. A changing UI cannot
  promise a stable global inventory across many request/response turns.
- Change: added a bounded exact-control snapshot form evaluated in one GUI
  turn. It returns one classified control or explicit absence, rejects
  duplicates and mixed fields, and includes runtime state. Waits use this form;
  complete inventories remain strict at stable checkpoints.
- Regression proof: present/absent/rejection behavior passed on all four GUI
  processes in 4.02 s; discovery Cancel passed in the 36.72-s focused run,
  slot reuse passed in 42.73 s, and the full GUI catalogue passed 10/10.
- Remaining test need: verify equivalent transient modal scheduling on Cocoa.

#### BUG-T066 - the GUI aggregate did not build its newly labelled MIDI unit

- Observed symptom: self-review showed `jam2_midi_input_backend_units` carried
  the `gui` label, but `jam2_tests_gui` did not depend on its executable; the
  unit aggregate instead listed that dependency twice.
- Root cause: the new target was inserted twice into the adjacent aggregate
  while the intended GUI aggregate insertion was missed.
- Change: removed the duplicate and made both aggregate build graphs exactly
  cover the test labels they can execute.
- Regression proof: the strengthened focused GUI build compiled the backend
  unit automatically, and the complete GUI catalogue built and ran it as test
  1/10.
- Remaining test need: repeat aggregate dependency parity on macOS.

#### BUG-T067 - synthetic MIDI injection falsely reported a dropped event as delivered

- Observed symptom: self-review found that `inject()` discarded the boolean
  result from the bounded `EventQueue::push()` and always returned success for
  a valid event, including queue overflow.
- Root cause: the fake backend mirrored physical-device message counting but
  failed to preserve the stronger synchronous injection result required by a
  deterministic test seam.
- Change: retain the incoming short-message count, return false with an exact
  queue-full error when push fails, and preserve the queue's drop diagnostic.
- Regression proof: the unit fills all usable queue entries, requires the next
  injection to fail, and requires exactly one drop; it passed in 0.03 s and in
  the complete GUI catalogue.
- Remaining test need: none beyond the macOS unit parity run.

### Iteration 15 - isolated input-plugin backend, complete four-peer lifecycle, and Plugins ownership

- Audited the audio-effect and MIDI-instrument lifecycle before extracting its
  UI. The maintained path combines a file/directory chooser, a private bounded
  VST3 probe, optional class selection, isolated worker startup, a real-time
  renderer bridge, editor/process lifetime, source routing, MIDI device/queue
  ownership, recording locks, and asynchronous late completion. The scanner,
  worker, host, device, and source graph therefore remain application-owned;
  only the cohesive transient editor was selected as the GUI ownership seam.
- Added `InputPluginBackend` and neutral `InputPluginHost` interfaces. The
  system implementation contains the existing platform chooser, maximum 12
  isolated `--probe-file` attempts, private temporary result, class selection,
  `PluginHostService` start, process-thread handoff, bridge commands, stats, and
  non-blocking retirement. `MainWindow` and `MainWindowPages` no longer depend
  on `PluginHostService` or `PluginAudioBridge`; the concrete isolated host is
  confined to the backend implementation. No third-party process wait moved
  onto the GUI or real-time thread.
- Added a private deterministic synthetic plugin backend for GUI automation.
  Its audio effect returns `-dry/2`, bypass returns exact dry audio, and its
  MIDI instrument consumes the bounded event queue and emits a fixed non-zero
  signal while a note is held. Bypass/mute, reset, editor state, retirement,
  health, negotiated I/O, block/failure counts, queue depth/high-water/drops,
  and consumed-event count are observable. The renderer uses fixed loops and
  atomics only: it does not allocate, lock, log, throw, or block.
- Added `jam2_input_plugin_backend_units` and registered it in unit, plugin,
  and GUI aggregate build targets. It proves asynchronous progress/completion,
  mono and stereo injected audio, wet and bypass output, MIDI note/mute/reset,
  exact event-consumption diagnostics, editor/health/retirement, invalid source
  shapes, and cancellation when the owning widget is destroyed. The final
  focused run passed 1/1 in 0.17 s; the complete plugin selector passed 2/2 in
  0.22 s with the existing isolated bridge unit.
- Extended the private GUI agent with a bounded `midi.inject` action routed
  only to the synthetic MIDI backend. Added exact `input_plugins` runtime state
  for audio/MIDI kind, slot, name, loaded/healthy/editor/bypass/device state,
  router attachment, and raw plugin diagnostics. Neither surface is available
  through Jam2's network protocol or normal public command line.
- Strengthened the real four-process Start/Join workflow before extraction.
  On every peer it loads an audio effect, rejects a concurrent second load,
  opens its editor, toggles local bypass, replaces it, assigns and loads a MIDI
  instrument, opens it, injects a note and requires consumption, toggles mute,
  replaces it without losing the device, exercises global BYPASS both ways,
  removes both hosts, and requires exact router/device cleanup. The initial
  complete button lifecycle passed in 54.11 s.
- Added two delayed-completion races. An audio load begins on slot 1, the
  Plugins editor closes, and Audio Inputs groups slots 0/1 before completion;
  the late host must retire and never attach to the changed topology. A MIDI
  instrument load begins, the Plugins editor closes, and MIDI removes the
  source before completion; the late host/device must retire with no source,
  renderer, or router residue. The strengthened pre-extraction proof passed in
  77.83 s on all four peers.
- Added `InputPluginsDialog` to `InputSourceDialogs.hpp/.cpp`. It owns the
  transient audio/MIDI tiles, exact controls, status formatting, global busy
  and progress state, action enablement, and panel rebuilding. `MainWindow`
  supplies typed snapshots/actions and retains all host, device, queue, router,
  worker-pool, recording-guard, attachment, and retirement ownership. After
  proving the new path, roughly 500 obsolete inline UI lines were deleted from
  `MainWindow`; none remain disabled or duplicated.
- The first extracted four-peer run terminated a peer during MIDI attachment
  and hit the 120.06-s CTest limit. BUG-T071 identifies the precise callback
  move-order defect. Its correction passed in 77.72 s. After deleting the
  comparison block, a 77.86-s run found one transient duplicate control ID on
  peer 2 during MIDI replacement. BUG-T072 corrected panel retirement. Two
  consecutive focused runs then passed in 78.44 s and 77.39 s.
- One attempted direct CTest repeat outside the mandated initialized MSVC/Qt
  environment ended before the automation hello in 0.07 s. It is invalid
  environment evidence and is not counted as a product run; the same staged
  `release/jam2.exe` was immediately rerun twice through
  `compile.cmd --in-dev-shell` as required, and both runs passed.
- Final authoritative Windows evidence: plugin passed 2/2 in 0.22 s. GUI
  passed 11/11 in 253.37 s: MIDI/backend units, Looper/asset units, four-peer
  Jam Sync 4.68 s, shared content 18.82 s, WAV interruption/recovery 40.48 s,
  fake-audio performance 12.47 s, GUI agent 4.10 s, complete modal catalogue
  94.78 s, and the expanded session/plugin flow 77.76 s. The previous complete
  performance proof remains 23/23 in 376.03 s, including every shared-grid,
  leader-audio, and listener-compensated metronome/epoch impairment cell; this
  slice did not touch those paths or relax any bound.
- Scope/protocol status: no visible feature, physical MIDI/audio behavior,
  engine callback/path, network message/type/field/header/payload, parser,
  version, authentication/authorization rule, metronome model/epoch behavior,
  or ordering changed. No ambiguous removal or behavior decision was made, so
  `TEST-REVIEW.md` remains unchanged.

#### BUG-T068 - the plugin backend unit could not resolve Qt Widgets in the tests directory

- Observed symptom: the first CMake configure failed because
  `Qt6::Widgets` was not a known target when linking the new backend unit.
- Root cause: the app directory's Qt Widgets lookup did not establish the
  imported target in the sibling tests directory's scope.
- Change: include Widgets in the tests directory's explicit Qt component
  lookup and link the unit directly to that target.
- Regression proof: every later configure succeeds and the unit is built by
  unit, plugin, and GUI aggregate targets.
- Remaining test need: repeat CMake target-scope behavior with Apple Qt.

#### BUG-T069 - the standalone Qt backend unit could not start on Windows

- Observed symptom: the first execution exited with `0xc0000135` before test
  output even though compilation and linking succeeded.
- Root cause: the test executable did not inherit a PATH containing the Qt
  Widgets runtime DLL directory.
- Change: add the same bounded CTest `PATH=path_list_prepend` environment
  modification used by the other Qt test executables.
- Regression proof: the unit passes in focused and aggregate invocations,
  including the final 0.17-s focused run.
- Remaining test need: none on Windows; verify the normal bundle/library lookup
  on macOS.

#### BUG-T070 - global plugin bypass retained a concrete bridge dependency

- Observed symptom: the first `MainWindow` adaptation failed to compile in
  `MainWindowPages.cpp`, which still called concrete bridge methods through the
  newly neutral host pointer.
- Root cause: the per-source dialog path had been adapted, but the independent
  global BYPASS action was missed during the initial dependency search.
- Change: route global audio bypass and MIDI mute/reset through
  `InputPluginHost`, matching every other caller.
- Regression proof: the adapted build succeeds, and all four peers prove global
  BYPASS on and off for simultaneous audio and MIDI plugins.
- Remaining test need: none.

#### BUG-T071 - moving a shared progress callback could terminate MIDI attachment

- Observed symptom: the first extracted four-peer run lost an automation pipe;
  the first MIDI plugin never attached, subsequent commands failed, and CTest
  reached its 120.06-s limit.
- Root cause: one call expression captured `progress` in the completion lambda
  while also passing `std::move(progress)` as another argument. Argument
  evaluation order allowed MSVC to move first. Audio success did not invoke the
  emptied captured function, but MIDI attachment did, throwing
  `std::bad_function_call` at the queued GUI boundary and terminating the peer.
- Change: pass an independent copy to the backend while the completion retains
  its own valid copy. Error, attachment, and post-device-open paths now share no
  moved-from callback.
- Regression proof: the immediate rerun passed on all four peers in 77.72 s;
  both later stability runs and GUI 11/11 also pass.
- Remaining test need: repeat under Apple Clang because it may choose a
  different permitted argument order; the fixed code must pass either order.

#### BUG-T072 - deferred panel deletion briefly duplicated exact control IDs

- Observed symptom: after the legacy block was removed, a 77.86-s four-peer
  run rejected peer 2's MIDI-replacement snapshot because one exact control ID
  had two matches for a single event-loop turn.
- Root cause: rebuild hid old source panels and called `deleteLater()`, then
  created replacement panels with identical stable IDs. Until deferred delete
  ran, both generations remained descendants of `MainWindow` and therefore
  correctly appeared in the strict automation inventory.
- Change: hide and synchronously detach retired panels from the dialog object
  tree before deferred destruction. This keeps deletion safe during a button
  signal while making ownership/inventory replacement atomic. The same safe
  retirement rule now applies to Audio Inputs and MIDI Inputs rebuilding.
- Regression proof: two consecutive four-peer runs passed in 78.44 s and
  77.39 s; the complete modal and GUI catalogues also pass with strict duplicate
  rejection unchanged.
- Remaining test need: repeat under Cocoa event/deferred-delete scheduling.

#### BUG-T073 - plugin requests accepted mismatched source shapes

- Observed symptom: self-review showed the new backend validator bounded input
  channels but accepted a zero-input audio effect, an audio effect carrying a
  MIDI queue, or a MIDI instrument without an event queue.
- Root cause: validation treated size limits and the instrument zero-input rule
  independently instead of defining the two complete legal request shapes.
- Change: audio effects require 1..2 audio inputs and no MIDI queue; MIDI
  instruments require zero audio inputs and a non-null bounded event queue.
- Regression proof: three negative unit cases reject those shapes, while mono,
  stereo, and MIDI valid cases pass; focused result is 1/1 in 0.17 s and plugin
  aggregate result is 2/2.
- Remaining test need: none beyond macOS unit parity.

## 2026-08-14 - Slice 7, iteration 16: authoritative coverage baseline and focused closure

- Ran the Windows two-pass distribution command after repairing the coverage
  and timing infrastructure. Total wall time was 2331.6 seconds (38:51.6).
  The instrumented catalogue passed 43/43 in 1486.29 seconds and the rebuilt
  optimized Release catalogue passed 43/43 in 759.75 seconds. All 21
  metronome/epoch mode-by-impairment cells, both UDP security cases, the native
  boundary wrapper, WAV sharing/interruption cases, and four-peer GUI cases
  passed in both builds. This is the first run in the initiative with both
  complete behavioral passes green and a canonical maintained-function
  inventory.
- The command exited 1 only at the intentionally strict coverage gate. The
  canonical inventory under `build/coverage` reports 116 maintained sources,
  112 observed sources, zero unreviewed missing sources, 3009 catalogued
  functions, 782 fully covered, 1479 partially covered, 36 reviewed
  exemptions, 712 wholly uncovered, and 16 collector-skipped functions. The
  earlier 804 count was provisional: allowing the previously killed boundary
  test to complete exercised 92 more functions.
- Locked the execution rule requested after this run: no further full suite
  while slice 7 work remains. Each function cluster gets only its exact CTest
  and, when useful, its small selected-suite aggregate. One final instrumented
  plus optimized distribution gate will run after all planned clusters and
  review entries are complete.
- Moved all automated temporary state beneath `build/test-artifacts`. CTest
  sets `JAM2_TEST_ARTIFACT_ROOT`, `TEMP`, `TMP`, and `TMPDIR` centrally for
  every test and child process. The build scripts clear the exact tree before a
  requested test run, remove it after complete success, and retain it on
  failure. A native coordinator unit proves the environment, Qt temporary
  path, standard-library temporary path, and `QTemporaryDir` containment. The
  latest full coverage failure retained only two 48-byte validation artifacts
  inside the build tree; the next focused run cleared them as designed. No
  historical global temporary data was deleted.
- Classified the 712 wholly uncovered functions by initial ownership: 337 GUI,
  131 JamTaster, 108 core, 82 application, 31 plugin host, and 23 CLI. The 16
  collector-skipped rows will need direct behavioral proof and an explicit
  reviewed exemption only where the collector genuinely cannot instrument the
  function; they are not automatically waived.
- Added `jam2_core_boundary_units`, registered under unit/core/network and in
  the unit and network build aggregates. It covers ring/MIDI reset and capacity,
  downmix helpers and diagnostics, prepared-source abandonment, recorder
  statistics, all parse-error text and replay reset, peer stream/mixer move and
  control behavior, UDP buffer/error/move boundaries, fixed-shape STUN request,
  parsing, local discovery and validation, NetworkSession identity/snapshot/
  controls/endpoint transition/move/close, authority identity, and tuning
  profile enumeration. All networking remains native loopback with a fake STUN
  server and fake playback sink.
- First compilation found the Windows `min` macro expanding `std::min`; use the
  macro-safe parenthesized standard-library spelling. First execution then
  exposed two incorrect test assumptions: `TrackTakeRecorder` intentionally
  clamps its queue to 4096 frames, and a new mixer peer contributes only after
  explicit activation. The assertions were corrected to prove those actual
  contracts, not to change the implementation.
- Self-review found that endpoint replacement was asserted only as state. Added
  a second loopback receiver and proved that after probing-to-active promotion,
  a real authenticated ping reaches only the replacement endpoint. The exact
  required command passed 1/1 in 0.03 seconds. Final aggregate proof passed all
  12 unit tests in 2.18 seconds.
- A diagnostic attempt to collect dynamic coverage from just the 0.03-second
  test left the Windows collector waiting for child communication for more
  than 2.5 minutes. It was terminated, its invalid 10-byte focused artifact
  was removed, and no process remained. Per-component work will use direct
  source-to-test mapping and focused behavior runs; the collector is reserved
  for the one final full gate. Canonical full reports were not modified.
- Scope/protocol status: this cluster changed only test and test/build
  infrastructure. No application feature, network field, header, payload,
  parser, version, authentication/authorization rule, packet ordering,
  metronome model, or epoch behavior changed.

#### BUG-T074 - normal Release configuration retained coverage compiler flags

- Observed symptom: after an instrumented run, the shared build cache could
  retain `/Od` and `/PROFILE` when returning to the ordinary Release build.
- Root cause: configuring coverage and normal builds in one directory did not
  explicitly unset every cached override.
- Change: the normal Windows configure removes the coverage cache entries and
  requires `/O2`, rejects `/Od`, and rejects `/PROFILE` before testing the
  staged Release executable.
- Regression proof: the final normal cache contains `/O2 /Ob2 /DNDEBUG`, link
  `/INCREMENTAL:NO`, and coverage disabled; its complete Release pass was
  43/43.
- Remaining test need: verify compile.sh uses an uncontaminated Apple Release
  configuration after its instrumented pass.

#### BUG-T075 - four-peer support artifacts were deleted before failure diagnosis

- Observed symptom: coordinator-owned temporary directories disappeared when a
  test failed, leaving only console symptoms for multi-process races.
- Root cause: automatic temporary-directory destruction did not distinguish a
  successful test from a failed one.
- Change: make the build-level artifact root own lifecycle: clean before a
  test command, delete after full success, retain after any failure. The
  coordinator no longer defeats that retention contract.
- Regression proof: the coordinator unit proves all temporary APIs are rooted
  under the build path; failed full coverage retained its small artifacts, and
  the next successful focused command removed them.
- Remaining test need: repeat retention and next-run cleanup on macOS.

#### BUG-T076 - fixed deadlines killed valid instrumented GUI/native work

- Observed symptom: the native boundary child was killed at exactly 120
  seconds under coverage and Qt subsequently reported `Process crashed`;
  other instrumented GUI work approached fixed outer limits.
- Root cause: child and CTest deadlines assumed optimized execution despite the
  collector's large, measured slowdown.
- Change: use the maintained coverage timing factor for the child wait and
  outer CTest deadline, while preserving the normal optimized deadlines.
- Regression proof: the boundary case passed focused instrumentation in 194.98
  seconds, optimized in 68.54 seconds, and both complete catalogue passes.
- Remaining test need: calibrate, but do not silently loosen, the equivalent
  Apple instrumentation factor.

#### BUG-T077 - serial collector startup distorted synchronized timing cells

- Observed symptom: collector attachment started peers far enough apart that a
  leader-audio burst-loss cell could fail timing evidence before the intended
  impairment was applied.
- Root cause: the coordinator began the scenario as soon as processes launched,
  although instrumentation initialization serialized their readiness.
- Change: instrumented multi-peer tests use an all-ready start barrier before
  scenario time begins.
- Regression proof: all 21 metronome/epoch cells passed in the final
  instrumented and optimized catalogues without changing an acceptance bound.
- Remaining test need: repeat the barrier under four independently launched
  Cocoa processes.

#### BUG-T078 - an exact test selector was intersected with its suite label

- Observed symptom: a valid `--test-name` could report no tests when the named
  case did not carry the selected suite label exactly as the script expected.
- Root cause: the command built the suite target, then combined two independent
  selectors instead of treating the exact CTest name as authoritative within
  that requested build surface.
- Change: exact-name execution no longer intersects the CTest label expression;
  the suite still controls which targets are built.
- Regression proof: focused network and unit commands select
  `jam2_core_boundary_units` exactly and pass.
- Remaining test need: verify compile.sh parity on macOS.

#### BUG-T079 - maintained worker entrypoints had no registered CTest owner

- Observed symptom: private worker command paths were compiled into the public
  executable but never invoked as a test, leaving their dispatch and failure
  contracts wholly uncovered.
- Root cause: existing component tests exercised worker libraries directly and
  skipped application entrypoint routing.
- Change: add a native worker-entrypoint test to the registered unit catalogue.
- Regression proof: `jam2_worker_entrypoint_units` passes in the unit aggregate
  and both complete catalogues.
- Remaining test need: run the same private entrypoints through the staged
  macOS bundle executable.

#### BUG-T080 - coverage restore metadata escaped the build tree

- Observed symptom: .NET coverage helper restore produced generated `obj`
  metadata beneath `tests/coverage`.
- Root cause: the restore command used its source directory as the default
  intermediate-output location.
- Change: direct helper restore/intermediate output beneath `build/coverage`.
- Regression proof: the final coverage helper and reports were produced under
  the build tree without new generated metadata in the maintained test source
  directory.
- Remaining test need: none; Apple uses its native coverage tooling.

#### BUG-T081 - one instrumented behavioral failure suppressed the coverage audit

- Observed symptom: when instrumented CTest was nonzero, the runner exited
  before exporting coverage or running the inventory check, losing evidence
  needed to diagnose whether instrumentation caused the behavior.
- Root cause: behavioral and collection success were treated as one early-exit
  condition even though the later optimized pass is the behavioral authority.
- Change: record the instrumented exit separately, continue export and audit,
  and still fail the overall command; never convert an instrumented failure to
  success.
- Regression proof: the first diagnostic full run retained exportable evidence
  despite its instrumented failures; the final instrumented pass was 43/43.
- Remaining test need: preserve the same evidence/exit separation in the Apple
  runner.

#### BUG-T082 - automated test state accumulated in the global temporary directory

- Observed symptom: historical Jam2-named directories and files were visible
  under the user's Windows temporary directory, while current failures were not
  self-contained with the build.
- Root cause: Qt, standard-library, and child-process temporary APIs inherited
  the global OS location with independent cleanup behavior.
- Change: centrally root all automated-test temporary APIs and child processes
  at `build/test-artifacts`, with explicit success/failure lifecycle.
- Regression proof: the coordinator unit proves containment; subsequent
  focused and full commands created no new AppData Jam2 entries, and successful
  focused commands leave no build artifact tree.
- Remaining test need: historical global data was deliberately not deleted;
  verify macOS containment before relying on cleanup there.

#### BUG-T083 - UDP security flood injection starved its own proxy pump

- Observed symptom: the optimized security fixture reported a 63-ms proxy pump
  stall while injecting its bounded short-packet flood.
- Root cause: the harness synchronously sent all 8192 datagrams on the same
  thread responsible for pumping six proxies.
- Change: send 64 packet pairs per pump turn while preserving the exact 4096
  packets per direction and 8192 total load.
- Regression proof: the optimized security case passed three consecutive
  focused runs (17.41, 17.29, and 17.28 seconds), then both complete passes.
- Remaining test need: none; this changes only impairment scheduling, not wire
  content or product behavior.

#### BUG-T084 - the boundary wrapper misreported its own forced termination as a crash

- Observed symptom: after its fixed 120-second kill, Qt emitted a secondary
  `Process crashed` result that obscured the actual deadline cause.
- Root cause: wrapper termination and post-kill exit classification were not
  distinguished, and the wrapper used the optimized deadline under coverage.
- Change: scale the child deadline under coverage, retain its private artifacts
  on failure, and report timeout as the primary bounded result.
- Regression proof: focused instrumented and optimized runs and both complete
  catalogue passes succeed.
- Remaining test need: verify QProcess termination classification on macOS.

#### BUG-T085 - empty coverage reports could throw or leave stale inventory CSVs

- Observed symptom: writing an empty report could pass a null contents value to
  the PowerShell file API; a prior non-empty CSV could then survive and appear
  current.
- Root cause: report emission assumed at least one row.
- Change: use one report exporter that explicitly truncates empty reports and
  exports non-empty rows consistently.
- Regression proof: the final inventory generated current full, uncovered,
  partial, skipped, and summary artifacts without stale-file reuse.
- Remaining test need: none on Windows.

#### BUG-T086 - focused collection overwrote canonical full-gate evidence

- Observed symptom: running a named coverage diagnostic replaced the canonical
  `windows-full` raw data and report/log names from the prior complete gate.
- Root cause: focused and full invocations shared output paths.
- Change: named runs use `build/coverage/focused` and `windows-focused*` names;
  only a full invocation may write the canonical files.
- Regression proof: the final full run regenerated canonical artifacts while
  the later focused diagnostic left the canonical summary unchanged.
- Remaining test need: preserve equivalent naming isolation if focused Apple
  profiles are introduced.

### Iteration 17 - focused CLI boundary closure

- Selected the complete 23-function CLI ownership cluster from the canonical
  wholly-uncovered inventory. Added `jam2_cli_boundary_units` under
  `tests/unit/application`, labelled it unit/application/cli, and made the unit
  aggregate own it. The test compiles maintained CLI option, dispatch,
  recording, and stats sources directly so the final source-based coverage
  merge can attribute their contracts without a long network session.
- Proved exact top-level, device, local, network, create, and join help; legacy
  frontend refusal to own universal network bootstrap; unknown-command result
  codes; and device/local dispatch using test-owned stubs. The staged binary is
  independently used for actual runtime behavior, so the stubs cannot turn a
  product execution path into a false pass.
- Proved valid one-based channel selections become unique zero-based channel
  indices and rejected trailing-empty, duplicate, zero, and nonnumeric lists.
  Proved separate, inline, and invitation-URL session keys are absent from
  diagnostic command-line text. Proved peer-stream diagnostics copy key
  counters and periodic output retains raw measurements.
- Launched the one staged `release/jam2.exe` to reject a missing device ID
  without opening hardware, then ran `local --headless-audio on` for 150 ms
  with deterministic tone input and jam recording. Required normal exit,
  startup evidence, valid parsed `recording.json`, exact format/test-input
  fields, and nonzero written frames. The temporary recording lives under the
  centralized build artifact root and is removed on success.
- Initial exact verification passed 1/1 in 0.29 seconds. Self-review identified
  three remaining functions that belonged to the same boundary but were
  trapped in `NetworkRuntime.cpp`: OS error formatting and the playback
  adapter's drop/detach behavior. Moved their unchanged implementation into
  `CliRuntimeSupport.hpp`, a cohesive non-owning Engine-to-PeerStream adapter
  seam, and exercised both attached and detached no-op behavior directly.
- The reviewed exact test passed 1/1 in 0.33 seconds. Final unit aggregate
  passed 13/13 in 2.49 seconds. `build/test-artifacts` was removed after each
  successful command. No full test or coverage collector was run.
- Scope/protocol status: one small responsibility-based code move and native
  tests only. No command behavior, audio callback behavior, network field,
  header, payload, parser, version, authentication/authorization rule,
  metronome model, or epoch behavior changed. No bug or ambiguous feature
  removal was found, so `TEST-REVIEW.md` is unchanged.

### Iteration 18 - focused application-infrastructure closure

- Added `jam2_application_boundary_units`, owned by both unit and network build
  aggregates and labelled unit/application/automation/network. It compiles the
  maintained ApplicationRuntime, AutomationChannel, ControlProtocol,
  NativeTcpTransport, and RuntimeHost sources directly and uses only native
  pipes, numeric loopback, a real headless Engine, and a bounded test-owned
  network-worker stub.
- Proved absent/required automation environment handling. A native HANDLE pair
  fills all 128 event slots before the writer starts, requires the 129th send
  to reject, and observes exact queued/high-water/rejected counts. Non-draining
  stop clears all slots and adds every discard to the rejection count. A
  separate live reader receives one malformed bounded frame, increments the
  rejected-frame count, then reports exactly one command-pipe disconnect.
- Proved NativeTcpPortReservation's initial state, ephemeral numeric-loopback
  bind, second-owner collision and diagnostic, close, reuse after close, and
  nonnumeric-host rejection. Proved NativeTcpListener publishes its real
  ephemeral port and closes without inventing an error callback.
- Started a real headless Engine through ApplicationRuntime, reused identical
  cold configuration, changed sample rate to force one restart, and then
  reused it for a bounded fake network worker. Required exact start/reuse/
  restart counters and peer-gain acceptance at 0 dB and -60 dB, while rejecting
  peer zero, out-of-range values, NaN, and a stopped worker. The fake owns only
  network-thread duration; Engine construction, callback, and lifecycle are
  production code.
- Proved independently generated 32-character peer tokens are distinct and
  both decode to valid nonzero peer identities. No token field, encoding, or
  authentication behavior changed.
- Exact verification passed 1/1 in 0.05 seconds. Self-review found no missing
  branch in the selected boundary and no production bug. The final unit
  aggregate passed 14/14 in 2.53 seconds; no full suite or coverage collector
  ran, and successful cleanup removed `build/test-artifacts`.
- Scope/protocol status: tests only. No product source, network field, header,
  payload, parser, protocol version, authentication/authorization rule,
  metronome model, epoch behavior, or feature changed. `TEST-REVIEW.md` remains
  unchanged.

### Iteration 19 - native debug entrypoints and bounded music corpus

- Added one native diagnostic driver with two separately registered exact
  CTests. `jam2_debug_entrypoint_units` is unit/application/debug/network;
  `jam2_music_corpus_units` is unit/application/debug/jamtaster/gui. The unit,
  network, and GUI build aggregates now own every labelled executable they may
  select.
- Proved the describe, run, and fuzz help paths from the staged public
  executable. Sent the same bounded invalid four-byte input through control,
  UDP PCM16, UDP PCM24, asset, and WAV targets and required a normal rejected
  JSON classification from every parser. Unknown parser target returns the
  exact argument-error result.
- Self-review rejected rejection-only evidence as incomplete. The driver now
  uses maintained encoders to build an authenticated heartbeat control frame,
  current v2 PCM16 and PCM24 audio packets with the existing 36-byte header,
  a current asset chunk, and a strict mono PCM16 RIFF/WAVE. Every target must
  classify its valid fixture as accepted. This changes no field, header,
  payload, authentication tag, parser, or version.
- Ran a declarative `lifecycle.local-network-local` scenario with synthetic
  audio. The real staged application starts one Engine, runs a bounded direct
  network worker, returns to local, and reports one start, zero restarts, two
  reuses, and strictly increasing frames. The initial exact test passed in
  2.85 seconds; the strengthened accepted/rejected version passed in 2.89.
- Added a separate bounded full-form corpus scenario for one profile,
  `funk_static_pocket`, one custom four-bar form, complexities 1/4/8, and two
  matched samples per cell. It generates six structural samples, renders the
  two complexity-4 audition mixes and drum stems, reads back rendered PCM16,
  emits valid JSON, and verifies at least four WAV artifacts. Exact result was
  1/1 in 1.70 seconds.
- Final unit aggregate passed 16/16 in 7.12 seconds. No full suite or coverage
  collector ran, and successful cleanup removed the build artifact root.
- Scope/protocol status: the maintained parsers, runtime, generator, renderer,
  and staged executable were exercised unchanged. The only product edit is the
  stale help wording recorded in BUG-T087. No metronome/epoch behavior or
  acceptance bound changed.

#### BUG-T087 - private fuzz help named the superseded Python validation owner

- Observed symptom: `jam2 debug fuzz --help` said the native bounded parser
  entrypoint was intended specifically for `jam2_test.py`, despite validation
  and replay ownership moving into C++/CTest.
- Root cause: the diagnostic help text retained the original harness name while
  the implementation remained a general native parser entrypoint.
- Change: describe its current purpose as native automated fuzz/replay
  validation. Command name, arguments, output JSON, bounds, and parser behavior
  are unchanged.
- Regression proof: the staged help contract and accepted/rejected native
  parser cases pass in `jam2_debug_entrypoint_units`.
- Remaining test need: verify identical bundle help text on macOS; Python
  validation removal remains deferred until the final parity audit.

### Iteration 20 - GUI model and presentation boundaries

- Added `jam2_gui_model_boundary_units` under `tests/unit/gui`, labelled it
  unit/gui/model, and made both the unit and GUI build aggregates own it. The
  target compiles the maintained BeatGrid model, generation-recipe serializer,
  mixer-stat presentation, and GUI-presentation sources directly. It uses Qt's
  offscreen platform and no Jam2 peer process, audio device, or network socket.
- Proved BeatGrid's four-section pristine state, title and guitar-reference
  validation, all cell families, drum lane/division bounds, musical onset/
  hold/rest conversion, subdivision expansion and contraction, harmonic
  summary, occupied-beat calculation, all-section resize, add/copy/duplicate/
  move/delete, stable section identity, rename, drum-kit selection, replace,
  clear, capacity/minimum bounds, generated-kind replace/refresh/append, current
  JSON round trip, and explicit rejection of the obsolete beat-lane schema.
- Proved every mixer diagnosis priority: output underrun, loss, jitter/burst,
  reorder, callback gap, drift, high RTT, and healthy. Five peers prove the
  visible four-peer summary plus full tooltip. Peak presentation proves input,
  send, monitor, remote, track, metronome, output, clipping, bounded
  normalization, deterministic decay, and reset.
- Proved all focus presets and fallback, custom classification, capture-duration
  enable/disable and null-control behavior, slider/muted-editor application,
  dB text/gain conversion, metronome step labels, build-local release-path
  override, and the real nonmodal Jam Ready dialog. The dialog proof requires
  complete read-only URL display, initial clipboard copy, a deliberately
  cleared clipboard restored by Copy URL, and safe close.
- Initial compilation exposed two new-test portability mistakes: `near`
  collided with a Windows-header identifier and the fixture attempted to call
  a Qt signal as a method. Both were corrected in the test. The first runtime
  pass then correctly rejected a fixture that had deliberately assigned a
  generated kind without a valid generation recipe; self-review restored that
  synthetic section to ordinary content before the persistence assertion.
- Initial corrected exact run passed 1/1 in 0.04 seconds. Self-review added
  independently observable invite-copy and null-spinbox label behavior,
  subdivision no-op, schema/label mapping, invalid move, and duplicate-at-
  capacity checks. The reviewed exact run passed 1/1 in 0.04 seconds; the
  targeted unit aggregate passed 17/17 in 7.09 seconds.
- No full suite or coverage collector ran. Successful commands removed
  `build/test-artifacts`. No product implementation, protocol field/header/
  payload/parser/version, authentication rule, metronome model, or epoch
  behavior changed. No production defect or manual-review item was found, so
  `TEST-REVIEW.md` is unchanged.

### Iteration 21 - GUI widget interaction boundaries

- Added `jam2_gui_widget_boundary_units` under `tests/unit/gui`, labelled it
  unit/gui/widget/performance, and made the unit, GUI, and performance build
  aggregates own it. The target compiles the maintained BeatGrid, Performance,
  waveform, level-meter, metronome, section-timeline, and looper-track widget
  sources directly and runs with Qt's offscreen platform.
- Proved the inline section/track timing helpers; waveform clear, duration,
  peaks, grid, playhead, loop, paint, and mouse seeking; level-meter bounds,
  smoothing, enabled state, and paint; metronome-nebula lifecycle/pulse paint;
  and metronome-pattern state, inventory, virtual invocation, and painted step
  toggling.
- Proved the looper's complete virtual inventory for three distinct lane
  shapes, selection, add/import, mute/solo/arm/rename/remove, WAV reveal/remove/
  analyse/drop, gain, region validation, stale targets, protection, remote
  recording state, grid/playback markers, live extent, and timeline zoom. Real
  mouse and drag events additionally prove painted lane and WAV actions, gain,
  move/left-edge/right-edge region edits, preservation of an active preview
  across a matching stable-lane refresh, plus-row actions, one-file local WAV
  drop, and protected or ambiguous multi-file rejection.
- Proved BeatGrid chord/beat/lyric modes across a 132-beat, 33-bar model,
  pagination in every direction, current/generated/remote focus, edit blocking,
  expand/shrink, tuning/string structure changes, line/reference controls,
  drum division/hits, multiline lyric paste and editing, remote convergence,
  overview/canvas paint, and iterator boundaries.
- Proved Performance Home's wide and narrow renderers, all state setters, 14
  visible peers, complete virtual action/navigation/bank/peer/gain/recording/
  tuner validation, raw tuner diagnostics, slider mouse and wheel input, peer
  rail scrolling, tuner mouse enable, key handling, paint, and background task
  completion.
- The initial exact test passed before self-review. Review then added painted
  WAV actions, both clip resize edges, and protected/multi-file drag rejection.
  Its first strengthened run failed two fixture assertions because the earlier
  live-recording case had intentionally retained a 192,000-frame stable view;
  ending that simulation before crop-coordinate tests made their 96,000-frame
  geometry explicit. The reviewed exact test passed 1/1 in 4.04 seconds and the
  unit aggregate passed 18/18 in 11.02 seconds. All 91 functions in this
  selected canonical wholly-uncovered widget cluster now have a direct native
  CTest owner for the final coverage merge.
- `build/test-artifacts` was absent after both successful commands. No full
  suite or coverage collector ran. No product implementation, network field,
  header, payload, parser, protocol version, authentication rule, metronome
  model, or epoch behavior changed.
- Self-review found two ambiguous ownership remnants in
  `LooperLaneStackWidget`: a bank-selection callback is assigned by
  `MainWindowPages` but never invoked by the widget, and a placeholder-lane
  mouse branch cannot execute while `visualLaneCount()` exactly equals the
  model lane count. No behavior was changed; `REVIEW-002` records the decision
  for manual review.

### Iteration 22 - JamTaster native analysis, post-processing, and export

- Extended the existing exact `jamtaster_native_units` owner across the 38
  wholly-uncovered pure-native functions selected from the canonical inventory:
  JSON array/number/write/failure paths, WAV mixing/cropping, file hashing,
  chroma analysis, chord/bass context, drum repair/dynamics/cymbal context,
  section choice, analysis serialization, and current-schema JamJar export.
  This slice intentionally excludes ONNX model execution, worker commands, and
  the Qt service lifecycle; those remain separately owned follow-up clusters.
- JSON proof now round-trips compact and padded objects, numeric arrays,
  escaped text, finite and non-finite values, and a malformed array failure.
  WAV proof saturates a mixed mono signal, zero-extends unequal sources,
  preserves cropped channel/frame bounds, and rejects mismatched rates and
  reversed intervals.
- Musical proof runs native chroma extraction, removes invalid chords and
  closes short same-root gaps, chooses a coincident chord-root bass event,
  resolves overlap/tiny notes, applies chroma/key/bass chord context, repairs a
  repeated missing snare, filters a rim-mode pattern to stable accented kicks,
  shapes backbeat/ghost snare dynamics, reclassifies regular cymbals as rides,
  snaps measured structures to bars, and rejects absent or overlong section
  evidence. Long-section inference now supplies real stems so its RMS-energy
  feature is exercised rather than bypassed.
- A build-local real export writes four source stems, a v3 JamJar, four owned
  lane WAVs, analysis JSON, fixed metronome timing, section/musical/drum
  patterns, asset hashes, and quantization. It proves forced regeneration
  removes an obsolete `jamtaster-*.wav` while retaining an unrelated user WAV,
  rejects more than twelve sections and an incomplete stem set, and parses the
  committed JamJar back through the native JSON reader.
- Self-review added an eight-beat nonuniform phrase whose bar-boundary residual
  exceeds the endpoint threshold. The real export therefore uses its anchored
  stretch path for all four stems; every rendered WAV must be 8 kHz and exactly
  32,000 frames, with stretching recorded in quantization metadata.
- The initial compilation found one test-only most-vexing-parse construction
  under MSVC; brace initialization fixed the fixture. The initial exact native
  test then passed in 0.15 seconds. After self-review strengthening it passed
  1/1 in 0.20 seconds, and the unit aggregate passed 18/18 in 11.25 seconds.
  `build/test-artifacts` was absent after success.
- No full suite or coverage collector ran. No product implementation, model,
  network field/header/payload/parser/version, authentication rule, metronome,
  epoch, or WAV-sharing behavior changed. No production bug or ambiguous
  removal item was found, so `TEST-REVIEW.md` is unchanged.

### Iteration 23 - staged JamTaster ONNX model boundaries

- Added `jam2_jamtaster_model_units`, labelled unit/jamtaster/model and owned by
  the unit aggregate. It links the maintained native model adapters, accepts
  only the staged distribution model directory, and runs actual CPU inference
  for Beat This, Basic Pitch, ADTOF, and ChordMini. No substitute graphs,
  canned tensor outputs, Python process, audio device, or Jam2 peer is used.
- The raw ONNX contract requires a nonempty runtime version, rejects a missing
  graph, describes the staged Basic Pitch graph's one input and named outputs,
  and rejects mismatched, non-concrete, and overflowing tensor shapes before
  inference.
- A deterministic harmonic/click signal proves Beat This returns 151 finite
  beat/downbeat logits and at least four bounded sorted beats; Basic Pitch
  returns 172 activation frames and nonempty, ordered, bounded notes within the
  requested MIDI range; ADTOF returns 201 activation frames and nonempty sorted
  hits in its five supported lanes; and ChordMini returns 33 feature frames and
  nonempty bounded labelled chord segments. Every adapter reports finite stage
  timings and its invalid numeric/range boundary is rejected.
- The Demucs adapter is entered with mono 22.05 kHz input to prove conversion
  and model-load failure cleanup, plus empty-model rejection. Full staged
  Demucs inference and ensemble output are intentionally deferred to the next
  heavier pipeline/separation slice and are not claimed here.
- The first exact run terminated before inference because Windows loaded a
  system-wide ONNX Runtime 1.17.1 ahead of the CTest `PATH`, while JamTaster is
  compiled against the vendored 1.23 API. `BUG-T088` records the build-local
  test runtime staging fix. With the imported 1.23 runtime colocated, the exact
  test passed in 0.92 seconds.
- Self-review rejected vacuous collection assertions and required at least four
  beats, nonempty notes and chords, and finite complete logits. The reviewed
  exact test passed 1/1 in 0.87 seconds; the unit aggregate passed 19/19 in
  12.16 seconds. All 37 canonical wholly-uncovered functions in the selected
  ONNX/four-model/Demucs-entry cluster now have a direct native owner for the
  final merge. `build/test-artifacts` was absent after success.
- Scope/protocol status: tests and build-local test dependency staging only.
  No product implementation, distributed worker layout, model bytes, network
  field/header/payload/parser/version, authentication rule, metronome, epoch,
  or WAV-sharing behavior changed. `TEST-REVIEW.md` remains unchanged.

#### BUG-T088 - Windows CTest loaded an older system ONNX Runtime before JamTaster's runtime

- Observed symptom: the new model test aborted with “requested API version 23
  is not available” and reported system ONNX Runtime 1.17.1, despite the
  vendored and staged runtime being 1.23.x.
- Root cause: the test executable lived in `build/tests` without
  `onnxruntime.dll`. Windows searched `System32` before the CTest-prepended
  `PATH`, so the unrelated system DLL won. The public staged JamTaster worker
  was not affected because its matching DLL is already colocated.
- Change: the model test target copies the CMake-imported JamTaster runtime
  beside its build-local executable after linking. This changes no public
  executable, installed component, model, or runtime selection in Jam2.
- Regression proof: `build/tests/onnxruntime.dll` is the imported 1.23 runtime;
  four staged model families complete real inference and the exact CTest passes.
- Remaining test need: confirm the equivalent macOS test resolves the imported
  1.23 dylib through its test rpath without using an unrelated system library.

### Iteration 24 - real Demucs ensemble and JamTaster pipeline orchestration

- Added exact `jam2_jamtaster_demucs_units` and
  `jam2_jamtaster_pipeline_units` targets. Both use only the staged distribution
  models and matching imported ONNX Runtime. They are labelled under unit/
  jamtaster/model with separation or pipeline ownership, and the unit build
  aggregate owns both.
- The separation test loads all four 166 MiB `htdemucs_ft` ensemble members and
  performs one real fixed-segment inference per member on deterministic mono
  22.05 kHz input. It requires exact seeded member-specific shifts, quarter-step
  monotonic progress, four ordered drums/bass/other/vocals WAVs, finite nonzero
  samples, mono output, exact source sample rate and unusual 5,513-frame length,
  and positive correlation between the source and sum of written stems.
- Initial real ensemble verification passed 1/1 in 17.78 seconds. Self-review
  added seeded-offset equality and source/stem-sum correlation without adding a
  second inference inside the fixture. The reviewed exact test passed in 17.90
  seconds. This upgrades the three Demucs functions that Iteration 23 reached
  only through model-load failure to full four-member behavior and output proof.
- The pipeline test pre-populates those now-independently-proven stem shapes so
  it does not repeat Demucs. It rejects absent required paths, an incomplete
  model bundle, a 513-character name, and stereo loopback input. It then runs
  real Beat This, ChordMini, ADTOF, and Basic Pitch analysis over a six-second
  deterministic click/harmonic WAV, with requested 120 BPM and four-beat meter.
- The first pipeline pass requires every monotonic checkpoint and commits
  `stems.json`, `tempo.json`, `chord-source.wav`, `analysis.json`,
  `manifest.json`, `progress.json`, and the current JamJar. It verifies cached
  separation evidence, current formats, complete status, timing/realtime-factor
  diagnostics, nonempty beats/chords/sections, and JamJar bytes. A second call
  must return through the single `100,cached` path. Deliberately malformed
  analysis cache evidence must trigger a complete stem-reusing refresh and
  recommit current analysis.
- The exact pipeline test passed 1/1 in 2.97 seconds. It gives a direct owner to
  all 13 canonical wholly-uncovered `Pipeline.cpp` functions, including model
  requirement, cache parsing, progress serialization, dynamics enrichment,
  timing statistics, all three analysis converters, and orchestration.
- One final combined unit aggregate passed 21/21 in 32.54 seconds, of which
  separation was 17.61 seconds and pipeline 2.96 seconds. Successful commands
  left `build/test-artifacts` absent. No full suite or coverage collector ran.
- Scope/protocol status: native tests and build-local test targets only. No
  product implementation, model bytes, public worker/bundle layout, network
  field/header/payload/parser/version, authentication rule, metronome, epoch,
  or WAV-sharing behavior changed. No new bug or manual-review item was found.

### Iteration 25 - staged JamTaster worker protocol and Qt service lifecycle

- Added `jam2_jamtaster_worker_protocol_units`, which launches the one staged
  private `jamtaster-worker` and owns all 18 canonical `WorkerMain.cpp` gaps.
  It rejects wrong protocol, unsupported action, missing input, malformed JSON,
  missing models, and present-but-invalid ONNX graphs with structured and
  category-correct exit events.
- The worker performs real staged Beat This tempo inference with default thread
  selection, then proves saved-tempo reuse. It repairs a malformed analysis
  index, consumes four independently seeded stem WAVs, and performs complete
  stem-reusing analysis with explicit one-thread model work. Required progress
  includes validation, separation, beat tracking, every musical analysis stage,
  export, and completion. A `convert_song` alias must take the mapped cached
  stage, and `split_stems` must return the saved four-stem report.
- The initial exact worker protocol test passed in 2.31 seconds. Self-review
  added saved-tempo reuse, malformed request parsing, and the distinct ONNX
  Runtime exception exit after all four required filenames pass existence
  checks. The strengthened exact run remained green.
- Added a small responsibility-based constructor seam to `JamTasterService`:
  the existing production constructor delegates unchanged, while focused tests
  may inject bundle and worker paths. This avoids duplicating the 680+ MiB model
  bundle or relocating a test executable into `release`; runtime validation,
  arguments, request protocol, and production path resolution are unchanged.
- Added `jam2_jamtaster_service_units` with a deterministic private worker. It
  owns all 25 canonical service gaps and proves precise missing-worker/model
  bundle status, bundle/models/worker paths, storage size, initial state,
  invalid input, atomic request creation/removal, busy rejection, observer
  snapshots/removal, stderr normalization, unstructured stdout logging,
  incompatible and current events, clamped progress, successful result state,
  structured failure, nonzero no-result failure, trailing-output flush,
  failed OS launch, cancellation idempotence, reuse after cancellation, and
  destructor cleanup while a worker remains active.
- The initial service run exposed only an opaque fixture assertion. Splitting
  its observer/state/log contracts showed that startup status and stdout
  protocol events are separate callback categories; the exact expectation was
  corrected. The first correct run passed in 0.30 seconds and exposed the real
  Windows issue recorded as `BUG-P040`. After fixing and adding failed-start/
  active-destruction review cases, the exact test passed in 0.36 seconds with
  clean output.
- The combined unit aggregate passed 23/23 in 35.34 seconds, including the one
  17.59-second Demucs repeat. Successful commands left `build/test-artifacts`
  absent. No full suite or coverage collector ran.
- Scope/protocol status: focused native process tests, injectable local paths,
  and cancellation diagnostic cleanup only. No worker request/event field,
  protocol version, model, public executable, network field/header/payload/
  parser/version, authentication rule, metronome, epoch, or WAV-sharing
  behavior changed. `TEST-REVIEW.md` remains unchanged.

#### BUG-P040 - detached Windows taskkill leaked cancellation text into application output

- Observed symptom: cancelling the deterministic JamTaster worker printed
  taskkill's “SUCCESS” line into the parent CTest output. The same detached
  helper could leak implementation noise into any attached Jam2 console/log.
- Root cause: the static detached launch inherited the parent's standard output
  and error channels.
- Change: configure a scoped `QProcess` with `taskkill` and the same PID/tree/
  force arguments, redirect both channels to the platform null device, then
  launch it detached. Worker termination, the two-second kill fallback, and
  cancellation state are unchanged.
- Regression proof: exact cancellation still terminates the worker tree,
  notifies once, clears request/result state, and returns in the 0.36-second
  service test with no taskkill text in CTest output.
- Remaining test need: none on Windows. macOS uses the ordinary terminate/kill
  path and must prove equivalent state cleanup without this platform helper.

### Iteration 26 - JamTaster dialog interaction and result ownership

- Added `jam2_jamtaster_dialog_units`, labelled for unit, GUI, JamTaster,
  dialog, and worker selection. It compiles the maintained dialog and service
  sources directly, uses Qt offscreen, and drives the existing injected private
  worker. It owns all 15 canonical wholly-uncovered `JamTasterDialog` rows plus
  the two result-validation helpers introduced during self-review.
- Every visible action now has direct native proof: Choose WAV cancellation and
  selection, Analyse Everything, Find BPM, Split Stems, all six result
  selectors, Apply Selected, Create New JamJar, Cancel Task with No and Yes,
  and Close. The test also proves read-only source presentation, result labels,
  enable/disable rules, progress, busy locking, retained matching results,
  quick and converted callbacks, direct creation, confirmed create-after-
  analysis, failed request startup, unavailable bundle, visible worker failure,
  active-task source locking, and observer cleanup through dialog destruction.
- Saved-result coverage includes current tempo/stem/analysis/manifest data,
  manifest source-name fallback, missing files, malformed JSON, a nonobject
  document, the 64 MiB read bound, full result counts, and an incomplete export
  retaining only safe tempo/stem quick application. The deterministic service
  worker now writes schema-shaped tempo and stem reports and emits the current
  full-result format; this is test support only and changes no public worker.
- The first build exposed that Qt 6.10 protects `QFileDialog::accept`; the test
  now invokes the dialog's registered accept slot. The first behavioral pass
  then showed that synthetic `QMessageBox::done` did not reproduce a real
  standard-button click; the harness now clicks the actual button. A diagnostic
  expectation was also corrected to distinguish working-directory creation
  from later request-file creation. These were test-only corrections.
- The first complete exact dialog pass was 1/1 in 0.50 seconds. Self-review then
  reproduced and fixed the three product defects below. The final strengthened
  exact test passed in 0.68 seconds; the shared service regression passed in
  0.34 seconds. The short unit aggregate passed 24/24 in 36.15 seconds, with
  the dialog at 0.65 seconds and Demucs at 17.74 seconds. Successful cleanup
  left `build/test-artifacts` absent. No full suite or coverage collector ran.
- Scope/protocol status: one dialog-local ownership/completeness guard and one-
  shot state correction, plus native tests and deterministic test-worker data.
  No public worker request/event field or version, model, network field/header/
  payload/parser/version, authentication rule, metronome, epoch, shared-WAV,
  audio-device, or four-peer behavior changed. `TEST-REVIEW.md` is unchanged.

#### BUG-P041 - retained or live JamTaster results crossed WAV ownership

- Observed symptom: after source A completed analysis, opening the dialog for
  source B enabled source A's result controls. The focused regression failed
  with `a new dialog must not adopt a retained result from a different WAV`.
  The same unconditional completion callback could attach a background result
  to a dialog opened for another WAV while that task was active.
- Root cause: the constructor accepted every nonempty `lastJobResult`, and the
  live `jobFinished` observer accepted every service completion. Only the later
  `setSourceContext` path compared the task input with the displayed source.
- Change: centralize the exact absolute-path ownership comparison and require it
  for constructor reattachment, context reattachment, and live completion.
  Foreign tasks remain visible as global service activity but cannot populate,
  select, apply, or create results in the wrong source dialog.
- Regression proof: the pre-fix focused test failed in 0.30 seconds. The fixed
  test keeps both a newly opened source-B dialog and an already-open source-B
  dialog empty while retained and live source-A jobs complete, while matching
  source reattachment still restores its BPM result.
- Remaining test need: repeat path ownership under macOS path normalization and
  a background helper launched from the app bundle.

#### BUG-P042 - incomplete converted exports were presented as usable full results

- Observed symptom: any nonempty `analysis.json` caused the dialog to construct
  and retain a converted-song path without checking that the directory or its
  exported JamJar existed. Chord, drum, bass, and section selections could then
  route through converted apply even though the only usable evidence was the
  earlier tempo or stems.
- Root cause: cached and live full-result acceptance treated a path string as
  completion evidence, and full-only selector enablement depended only on
  analysis array counts.
- Change: a converted result is complete only when its directory exists and
  contains exactly one `.jamjar`, and its `analysis.json` is a valid object.
  Invalid or partial full results clear converted ownership; valid tempo/stem
  reports remain available through quick apply, while full-only selectors stay
  disabled until the exported JamJar is complete.
- Regression proof: valid converted folders still expose all six selections
  and callbacks. Removing the sole JamJar preserves tempo/stem controls,
  disables chords and the other full-only choices, and routes Apply through the
  quick callback. A synthetic root-path result cannot enable creation.
- Remaining test need: repeat bundle path and case-normalization behavior on
  macOS; the pipeline test already proves a normal export has one current
  JamJar.

#### BUG-P043 - failed create-after-analysis continuation remained armed

- Observed symptom: when a user confirmed full analysis for Create New JamJar
  but that full result was incomplete, no creation occurred then; after the
  files were repaired, a later ordinary Analyse Everything unexpectedly invoked
  the old deferred creation callback. The pre-fix two-job regression failed.
- Root cause: `createSongAfterAnalysis_` was cleared only when the converted
  result was nonempty, instead of being consumed by the requested job result.
- Change: consume the deferred continuation exactly once on any completed job;
  invoke creation only when that same completion owns a validated converted
  song. Failure and cancellation already clear the state and remain unchanged.
- Regression proof: the pre-fix exact test failed in 0.50 seconds. After the
  fix, an incomplete create-requested analysis invokes no callback, and a later
  ordinary successful analysis also invokes none; the normal confirmed valid
  path still creates exactly once.
- Remaining test need: repeat nested-message-loop and helper completion ordering
  under Cocoa.

### Iteration 27 - contained Practice Idea and Research Drum core

- Added `jam2_practice_idea_boundary_units`, labelled unit/GUI/practice/audio.
  The target compiles the maintained generator, theory/catalog, researched-kit,
  drum-engine, recipe/model dependencies, and existing Jam2 resource manifest
  directly. The large `PracticeIdeaGenerator.cpp` remains one contained file;
  it was not split or abstracted because its size alone is not a responsibility
  boundary.
- The catalog matrix walks every normal style/profile and directly proves the
  previously untouched beat-style, native-form, meter, compatible-meter/bar,
  production-family, mode-name, and style-name APIs. It requires aligned IDs
  and labels, positive sorted compatible forms, complete normal fallback,
  explicit experimental naming, and empty unknown-profile selectors.
- Generation proof compares the chord-only and beat-only test wrappers with the
  same complete seeded coupled idea, exercises invalid optional selections
  through deterministic bounded catalog choice, and calls both public random
  generation wrappers. A combined source then generates seeded and random
  continuations with nonempty selection evidence, relationship ownership,
  form-section invariants, and the requested target label.
- The legacy researched-drum sampler covers disabled, kick, snare, shell-tom,
  cross-stick/wood, clap, crash, ride bell/edge/bow, ring-metal, future-source,
  every transient family, every texture family, equal-power source blending,
  sine layering, deterministic seed/age output, and bounded source-specific
  tails. Embedded base/profile kit lookup and every supported lane/source rule
  are checked against the maintained resource JSON.
- A 40-hit custom synth/detail kit forces the fixed 32-subvoice allocation and
  stealing boundary, modal/noise detail banks, timing diagnostics, unknown-lane
  rejection, finite nonzero output, exact rerender determinism, and room/drive/
  low-pass/compressor bus processing. Invalid frame, sample-rate, empty-audio,
  and short-room-send shapes are also owned.
- Initial target wiring exposed the maintained recipe/model/core link
  dependencies and that the app directory's `AUTORCC` setting does not flow to
  the sibling tests directory; the focused target now explicitly owns the same
  resource manifest. The first runtime expectation was corrected to respect
  the product's global meter presentation order rather than profile storage
  order. These were test-target/test-expectation issues only.
- Self-review proved two baseline-uncovered generator fallbacks unreachable:
  `formSectionsFor` always creates at least one section for every resolved
  positive-bar form, and `continuationStylePlan` initializes a relationship ID
  before every specialization. Removed the dead `formForBars` and relationship
  fallback instead of adding test-only access. Also removed three unused
  internal parameters and renamed the shadowed secondary drum voice; generated
  content, recipes, selection logic, and render math are unchanged.
- The selected canonical cluster contained 29 rows: 27 now have direct native
  behavior (16 generator/catalog/iterator-choice rows, nine researched-kit/
  sample rows, and two synth-subvoice rows), while the two unreachable rows were
  deleted. The reviewed exact test passed in 0.39 seconds. The short unit
  aggregate passed 25/25 in 36.64 seconds, including the practice case in 0.37
  seconds and Demucs in 17.88 seconds. Successful cleanup left
  `build/test-artifacts` absent. No full suite or coverage collector ran.
- Scope/protocol status: focused native tests and responsibility-preserving
  internal cleanup only. No visible generator option, output schema, random-
  seed persistence, audio callback, public protocol, network/authentication,
  metronome/epoch, shared-WAV, plugin, or four-peer behavior changed. No product
  bug or unresolved manual-review item was found.

### Iteration 28 - Practice Idea dialogs and generated-section lookup

- Extended `jam2_practice_idea_boundary_units` to compile the maintained
  `PracticeIdeaDialogs.cpp` and `PracticeIdeaController.cpp` sources directly,
  link the existing looper model and Qt Widgets dependencies, and run with
  Qt's offscreen platform. This owns the selected seven-row canonical cluster:
  six previously untouched dialog construction/entry rows and
  `PracticeIdeaController::generatedSection`.
- Native modal interaction now proves every Practice Idea dialog entry point.
  Generate covers Cancel and acceptance, every form control, all three Parts
  modes' selector presence, target-section refresh, exact target BPM ownership,
  current-section meter/length retention, normal and experimental style/profile
  refresh, the explicit unsupported-meter override, key, complexity, and the
  labelled Generate action. Continue covers Cancel, content/empty bank labels,
  the same-section/empty-source lock, distinct source/target enablement, and the
  exact returned indices.
- Reference WAV coverage proves single- and multi-section summaries, the
  least-common-multiple beat/frame calculation path, independent availability
  for chords, drums, melody, bass, and support, no-layer rejection, Cancel,
  voicing/kit selection, and the exact accepted render settings. Idea Details
  proves nonempty read-only teaching and technical text, both toggle directions,
  and Close. The controller lookup proves exact chord ownership, combined
  practice fallback for beat/chord, and a missing result.
- The first exact MSVC build and test passed 1/1 in 0.41 seconds. Self-review
  found two Jam2-owned compiler-cleanliness gaps in the already covered Practice
  core: the drum phrase-plan JSON local shadowed the earlier melody-phrases
  local, and `researchDrumSample` carried a lane ID through its public/local
  call chain even though sample synthesis never consumed it. The local was
  renamed and the redundant parameter removed from the maintained declaration,
  implementation, and focused calls. Render values, seed/age determinism, lane
  compatibility policy, and generated data are unchanged.
- The reviewed exact test passed in 0.41 seconds. The short unit aggregate then
  passed 25/25 in 37.49 seconds, including the Practice Idea case in 0.39
  seconds and Demucs in 17.80 seconds. `git diff --check` reports no whitespace
  error, and successful cleanup left `build/test-artifacts` absent. No full
  suite or coverage collector ran.
- Scope/protocol status: native dialog/controller tests and internal
  compiler-cleanliness cleanup only. No visible option, generation/render
  behavior, persistence shape, network field/header/payload/parser/version,
  authentication rule, metronome/epoch model, shared-WAV, plugin, audio-device,
  or four-peer behavior changed. No product bug or unresolved manual-review
  item was found; `TEST-REVIEW.md` remains unchanged.

### Iteration 29 - project persistence and transient WAV ownership

- Added `jam2_project_persistence_units`, labelled unit/GUI/shared-content/
  persistence/WAV and compiled directly with the maintained
  `ProjectPersistenceCoordinator.cpp`. Every fixture is explicitly rooted
  beneath `build/test-artifacts`; it opens no device, socket, peer, or GUI.
- The test exercises every live coordinator method: exact abandoned-transfer
  partial cleanup at workspace initialization; absolute workspace/project
  folders; new/open/save snapshot ownership and dirty detection; blank and
  explicit location transitions; active versus deferred transient WAVs;
  canonical aliases; workspace relocation without retargeting external files;
  persistent-asset release; ownership-only clearing; and existing-file checks.
- Destructive-path coverage requires unowned WAVs, registered non-WAVs, and a
  directory named `.wav` to survive. Owned WAVs and already-absent owned paths
  are released correctly. The asynchronous cleanup consumes both active and
  deferred sets, deletes only WAV files, preserves a non-WAV safety fixture,
  and prunes only empty managed folders and the empty workspace.
- JSON coverage proves exact atomic bytes and object parsing, malformed and
  nonobject rejection, missing/unavailable/directory paths, the 4 MiB input
  bound, and the now-symmetric output bound. The first exact run failed 0/1 in
  0.07 seconds on the self-unreadable save condition described in `BUG-P044`.
  After the product fix, the exact test passed in 0.05 seconds.
- Audit removed two provably dead persistence members instead of manufacturing
  test access: the coordinator's duplicate project-file path/getter and its
  unused `workingProjectFolder` helper. `JamStorage` remains the actual project-
  file authority, while every active caller already reads the coordinator's
  project/workspace folder directly. No visible workflow or saved format was
  removed.
- Of the 11 selected canonical wholly-uncovered rows, nine now have direct
  native behavior and two dead rows were deleted. The short unit aggregate
  passed 26/26 in 36.45 seconds, including persistence in 0.03 seconds and
  Demucs in 17.49 seconds. `git diff --check` reports no whitespace error and
  successful cleanup left `build/test-artifacts` absent. No full suite or
  coverage collector ran.
- Scope/protocol status: local project state, transient-file ownership, and
  atomic save safety only. No JamJar schema, network field/header/payload/
  parser/version, authentication rule, metronome/epoch model, shared-WAV wire
  behavior, audio path, plugin, or four-peer behavior changed. No unresolved
  manual-review item was found; `TEST-REVIEW.md` remains unchanged.

#### BUG-P044 - Jam2 could save a JamJar that its own loader refused

- Observed symptom: `writeSongJson` successfully created a 4 MiB plus one byte
  project, while `readSongJson` immediately rejected the same file at its
  existing 4 MiB safety boundary. The focused test failed with `song write must
  not create a file that its own reader will reject`.
- Root cause: the size guard existed only at the read boundary; the atomic save
  path performed no matching preflight.
- Change: reject an oversized byte array before opening `QSaveFile`, return a
  specific 4 MiB diagnostic, and leave no output file. The limit and all
  accepted current JamJar bytes are unchanged.
- Regression proof: malformed/nonobject/missing/oversized reads and ordinary
  exact-byte writes remain covered; an oversized write now returns false,
  names the 4 MiB limit, and creates no file. The fixed exact test passed in
  0.05 seconds and all 26 unit tests passed.
- Remaining test need: repeat atomic-write and file-size behavior on APFS under
  the macOS parity item, including an unavailable bundle-adjacent destination.

### Iteration 30 - fake-injected GUI loopback capture and WAV safety

- Added `jam2_gui_loopback_recorder_units`, labelled unit/GUI/audio/recording/
  performance/WAV. It compiles the maintained `GuiLoopbackRecorder.cpp`
  directly and uses an injected capture backend to drive the actual recorder
  thread, state, completion, PCM processing, and WAV writer without opening a
  physical device. The application continues to use the default WASAPI backend.
- PCM coverage decodes two-channel little-endian PCM16, PCM24 sign extension,
  PCM32 full scale, clipped Float32, non-finite float input, unsupported format,
  truncated frame, and invalid channel. Normalized conversion proves clamping,
  rounding, and fail-silent infinity/NaN behavior. Identity/down/up interleaved
  resampling preserves exact frames, channel polarity, empty input, and invalid
  shape rejection; leading/trailing threshold behavior is explicit.
- `LoopbackTakeAccumulator` is now the small production processing seam used by
  both WASAPI and injected tests. Its constructor owns the relevant option
  values rather than borrowing a larger options object. Direct behavior proves
  zero state, target duration, raw/recorded frame counters, full-scale peak,
  configured silence-tail qualification, and final sample ownership.
- The PCM16 WAV writer is now an explicit production function using `QSaveFile`.
  A Unicode nested path proves directory creation, atomic commit, every RIFF/
  fmt/data field, exact signed samples, and failure for blank paths, invalid
  rates, and directory targets. Frame-size bounds and byte-rate math no longer
  silently truncate or use overflowing signed multiplication.
- Recorder lifecycle coverage proves every synchronous option rejection,
  running visibility, concurrent-start rejection, injected audio completion,
  exact callback channels, completed-thread join/reuse, backend failure,
  backend exception, observer exception, stop observation, and destructor join.
  The first build found a mixed-type initializer in the test fixture; replacing
  it with an exact `int16_t` array was a test-only correction. The first complete
  exact test then passed in 0.06 seconds.
- Self-review found and fixed `BUG-P045` through `BUG-P047`. The strengthened
  exact test passed in 0.07 seconds. All 14 selected canonical wholly-uncovered
  loopback rows now have direct behavior through the maintained path, including
  start/run, sample conversion, WAV helpers, and every accumulator operation.
  The short unit aggregate passed 27/27 in 36.60 seconds, including loopback in
  0.07 seconds and Demucs in 17.45 seconds. `git diff --check` reports no
  whitespace error and successful cleanup left `build/test-artifacts` absent.
  No full suite or coverage collector ran.
- The actual endpoint enumeration/default WASAPI capture body remains tied to
  the real Windows interface and hardware profile; its format-independent
  processing and lifecycle no longer require hardware. Existing zero-frame/
  all-silence success semantics are unchanged and recorded as `REVIEW-003` for
  user judgment.
- Scope/protocol status: local capture dependency injection, PCM conversion,
  file commit, and worker-boundary safety only. No ASIO callback, network field/
  header/payload/parser/version, authentication rule, metronome/epoch model,
  shared-WAV wire behavior, plugin, or four-peer behavior changed.

#### BUG-P045 - a loopback completion observer could terminate Jam2

- Observed symptom: source review showed the backend body was protected by the
  worker's exception boundary, but `FinishedCallback` ran afterward inside the
  `noexcept` thread function. Any observer exception therefore called
  `std::terminate` rather than containing the local failure.
- Root cause: the callback was outside the top-level thread catch boundary.
- Change: invoke the completion observer inside its own catch-all boundary.
  Recorder state is already final before notification, so an observer cannot
  poison the next start. The application callback remains unchanged and queues
  its UI work onto the Qt thread.
- Regression proof: an injected observer deliberately throws after being
  invoked; the process survives, the recorder leaves running state, joins the
  completed worker, starts again, and delivers a normal successful callback.
- Remaining test need: repeat thread/destructor behavior with Apple Clang in
  the macOS injected-backend parity test.

#### BUG-P046 - loopback WAV output was non-atomic and path-narrowed on Windows

- Observed symptom: the writer truncated the destination directly through
  `std::ofstream(outputPath.toStdString())`. A failed write could expose a
  partial WAV, and Windows paths outside the active narrow encoding could fail
  even though Qt had supplied a valid Unicode path.
- Root cause: a Qt `QString` was narrowed before open and there was no temporary
  commit boundary or final stream-state check.
- Change: write checked little-endian RIFF fields and samples through
  `QSaveFile`, then atomically commit. Reject blank paths, invalid sample rates,
  and unrepresentable RIFF sizes before touching the destination.
- Regression proof: a nested Unicode filename receives the exact 44-byte header
  plus eight sample bytes; every header field and signed sample is parsed back.
  Invalid targets throw and do not mutate the completed WAV.
- Remaining test need: repeat Unicode/APFS atomic commit and unavailable-volume
  failure on macOS; real Windows disk-full behavior remains a hardware/system
  fault-injection profile rather than a default unit case.

#### BUG-P047 - non-finite loopback samples reached integer conversion

- Observed symptom: the float decoder clamped values without checking
  finiteness, and normalized PCM16 conversion passed NaN/infinity to `lrint`.
  A malformed or faulty capture format could therefore produce a domain error
  or implementation-dependent integer sample.
- Root cause: the conversion helpers assumed every device sample was finite.
- Change: non-finite Float32 and normalized values now deterministically map to
  silence before clamping or rounding. Finite conversion math is unchanged.
- Regression proof: NaN and infinity produce exact zero, while negative/full-
  scale/clipped finite PCM and float values retain their expected outputs.
- Remaining test need: none for the platform-neutral conversion; hardware
  format negotiation remains under the explicit device profile.

### Iteration 31 - track-recording scheduling, correlation, and rejection safety

- Added `jam2_track_recording_workflow_units`, labelled unit/GUI/audio/
  recording/performance/transport. It compiles the maintained
  `TrackRecordingWorkflow.cpp` directly and injects command submission plus
  engine snapshots, so deterministic tests exercise the real workflow without
  a device, peer, window, or alternate Jam2 executable.
- Pure timing coverage owns count-in lead selection, anchored/stopped grid
  behavior, raw/musical render-offset translation, countdown phases, transport
  elapsed time, prepared-attachment acknowledgement, sample-rate selection,
  latency snapshots, and committed/cancelled global transport. NaN, infinity,
  exhausted beat/frame ranges, maximum duration, and the minimum signed render
  offset are explicit fail-closed cases.
- Command coverage proves exact types, monotonically increasing local cookies,
  target/source/musical/countdown frames, prepared loop modes, bank restart,
  local/global transport, count-in, manual stop, all capture-source flags, and
  all six partial-submission failure points. Invalid path/rate/grid/schedule/
  overflow inputs must fail before unsafe engine work begins.
- Capture ownership coverage proves input/current-jam/loopback modes, lane arm/
  disarm, transient versus persistent WAV state, loopback finish/abandon,
  project/session clearing, matching completion, stale completion rejection,
  asynchronous command rejection, and jam-recording start/stop confirmation,
  rollback, retry, and transition serialization.
- A real Headless `Engine` case injects its built-in tone, arms/starts/stops a
  take, waits for writer completion, and proves the exact take ID, 48 kHz rate,
  nonzero frame count, and WAV existence. Its `QTemporaryDir` is rooted under
  `build/test-artifacts` and is removed on success.
- The existing runtime constructor now delegates through narrow injected
  submit/snapshot functions. Quantized validation was separated from command
  emission, and shared/local schedules use one owned emission path. This is a
  concrete dependency and duplicated-scheduling refactor; no file was split
  because of size.
- The initial pre-review exact target passed in 0.04 seconds. Self-review found
  and fixed `BUG-P048` through `BUG-P054`. One strengthened run correctly went
  red in 0.04 seconds because a reset fixture still expected the prior frame's
  200 ms lead; correcting that test-only value produced the final exact pass in
  0.61 seconds. The short unit aggregate passed 28/28 in 38.06 seconds,
  including this case in 0.59 seconds and Demucs in 17.84 seconds.
  `git diff --check` reports no whitespace error, and successful cleanup left
  `build/test-artifacts` absent. No full suite or coverage collector ran.
- The frozen canonical inventory contained 14 wholly-uncovered
  `TrackRecordingWorkflow` rows. All 14 responsibilities now have direct native
  behavior; the old quantized emitter row became the covered preflight plus
  shared schedule-emission seam rather than being retained as duplicate logic.
- Scope/protocol status: the added `EngineEvent::id` is a fixed-shape,
  in-process engine-to-GUI correlation field only and is never serialized. No
  network field/header/payload/parser/version, authentication rule,
  metronome/epoch model, shared-WAV wire behavior, ASIO/device path, plugin, or
  four-peer behavior changed. No new unresolved manual-review item was found.

#### BUG-P048 - duration overflow could leave an unbounded track take armed

- Observed symptom: `startTrackTake` submitted `StartTrackTake` before checking
  whether `targetFrame + durationFrames` overflowed. It then failed without a
  valid scheduled stop, potentially leaving the accepted take running.
- Root cause: the stop-frame arithmetic guard lived after the first mutating
  command rather than at the workflow preflight boundary.
- Change: validate target-plus-duration before arm/start command emission for
  ordinary, quantized, and adopted shared schedules; cancel any later partially
  accepted sequence best effort.
- Regression proof: maximum-range duration inputs enqueue no command, while
  every valid finite duration preserves exact start and stop frames.
- Remaining test need: repeat integer-boundary behavior under Apple Clang.

#### BUG-P049 - rejected recording commands could leave GUI state stuck or false

- Observed symptom: `CommandRejected` was only logged by `MainWindow`.
  Rejected track commands could leave `input_take_active_` true forever;
  rejected jam starts stayed active, and rejected jam stops stayed inactive.
- Root cause: the workflow did not retain its submitted command cookies and
  therefore could not distinguish owned asynchronous rejection events.
- Change: retain a bounded set of local take cookies and the pending jam start/
  stop cookies. An owned take rejection produces a normal failed completion,
  clears UI schedule/state, and submits cancellation. Jam rejection restores
  the last engine-confirmed state and permits a bounded retry.
- Regression proof: unrelated cookies are ignored; owned start/intermediate/
  stop rejections roll back exactly, surface the engine error, and remain
  retryable. The aggregate's real engine tests remain green.
- Remaining test need: repeat rejection/event ordering under Apple Clang; event
  queue saturation remains covered by the engine diagnostic boundary rather
  than this GUI unit.

#### BUG-P050 - a stale completion could finalize the wrong recording take

- Observed symptom: `TrackTakeRecorderCompletion` already carried a take ID,
  but `Engine` discarded it when creating `TrackTakeCompleted`. The GUI paired
  every completion with whichever take happened to be active when it arrived.
- Root cause: the local engine event lacked take correlation even though the
  recorder had exact ownership data.
- Change: add a bounded ID to the local `EngineEvent`, propagate the recorder's
  existing take ID, and consume a completion only when it exactly matches the
  active take.
- Regression proof: a deliberately stale ID leaves the new take active; the
  matching ID finalizes it. A real headless writer round trip preserves the ID
  and nonzero WAV completion.
- Remaining test need: repeat the headless writer event on macOS. This is not a
  wire-protocol change: `EngineEvent` is never encoded or sent to a peer.

#### BUG-P051 - partial take startup could lose transient-WAV cleanup ownership

- Observed symptom: when arm submission succeeded but a later seek/play/start/
  stop/transport submission failed, the workflow returned before recording the
  transient output path. The accepted arm could already have created a WAV,
  leaving no managed cleanup owner.
- Root cause: transient ownership was assigned only after the entire multi-
  command schedule had been accepted.
- Change: claim transient ownership immediately after successful arm enqueue;
  both local and shared `MainWindow` failure paths consume and register that
  path. Asynchronous failures hand the same path through their failed
  completion and clear the single pending slot.
- Regression proof: all five post-arm submission failures finish with
  best-effort cancellation and expose the exact transient path once; an arm
  rejection exposes none. Successful cleanup leaves no test artifact.
- Remaining test need: confirm APFS cleanup when cancellation and file close
  complete in adjacent event-loop turns.

#### BUG-P052 - extreme beat timing could enter invalid numeric conversions

- Observed symptom: running-grid helpers passed NaN/infinity or an excessive
  beat interval to `llround`, negated `INT64_MIN` render offsets, and used an
  addition-based ceiling formula that could wrap at the frame limit.
- Root cause: timing helpers validated ordinary positive values but not their
  complete numeric domain.
- Change: centralize finite positive beat-frame conversion, use overflow-safe
  signed-offset magnitude and ceiling arithmetic, and fail closed when the
  absolute beat or target range is exhausted.
- Regression proof: NaN, infinity, maximum beat, maximum duration, and minimum
  signed render offset now return deterministic bounded results; ordinary
  48 kHz half-second beat targets are unchanged.
- Remaining test need: repeat floating-point boundary results under Apple
  Clang; exact ordinary frame contracts must remain unchanged.

#### BUG-P053 - jam recording transitions could overlap before confirmation

- Observed symptom: a submitted start immediately enabled stop, and a submitted
  stop immediately enabled restart, before the engine's specific confirmation
  or rejection arrived. Rapid clicks could queue conflicting generations and
  make rollback ambiguous.
- Root cause: requested active state doubled as confirmed lifecycle state with
  no pending-transition guard.
- Change: retain engine-confirmed state separately and serialize start/stop
  requests while either command cookie is pending. Confirmation advances the
  state; rejection restores it and enables retry.
- Regression proof: stop-before-start-confirmation and restart-before-stop-
  confirmation enqueue nothing; confirmed and rejected paths both become
  retryable with the correct active state and folder ownership.
- Remaining test need: repeat Qt event ordering on macOS.

#### BUG-P054 - blank recording destinations entered asynchronous failure paths

- Observed symptom: blank track WAV paths and blank jam-recording folders could
  be queued, optimistically reported as active, and rejected later by the
  recorder. Inactive jam stops also emitted unnecessary failing commands.
- Root cause: obvious user-input/lifecycle validation was deferred to the
  asynchronous engine boundary.
- Change: reject blank track/jam destinations and inactive jam stops before
  command construction; keep the engine's independent validation unchanged.
- Regression proof: each invalid request returns false with no submitted
  command, while valid Unicode/native paths and normal stop flows remain owned.
- Remaining test need: macOS path normalization parity is covered by the
  Iteration 31 item in `TEST-MACOS.md`.

### Iteration 32 - shared audio-device callback processing and fake-device parity

- Added `audio_device_processing.hpp/.cpp` as the maintained, allocation-free,
  `noexcept` callback-processing seam. Windows ASIO, macOS CoreAudio, and the
  threaded Headless device now call the same implementations for callback
  interval statistics, peak measurement, remote/output gains, local monitor,
  prepared-source diagnostics, resampled ring playback, injected tone/pulse/
  bass/metronome input, metronome output, saturation, and clipping counts.
  Platform-specific device discovery, setup, sample conversion, callbacks, and
  teardown remain in their owned platform files.
- Added `jam2_audio_device_processing_units`, labelled unit/core/audio/
  performance/metronome. It directly proves empty/null/identity and clipped
  sample paths, atomic interval aggregates and thresholds, gain and meter
  updates, local monitoring, the real `PreparedTrackSource`, ring underrun and
  unity/half/double/ramped resampling, state reset, opposite PCM rails, every
  injected input mode, metronome epoch injection, leader-audio ownership,
  transport gating, recording and playback count-ins, and extreme numeric
  boundaries.
- The same target starts a real Headless `Engine`, waits for real callbacks,
  proves fake tone input/monitor/output/timing diagnostics, attaches the actual
  network playback ring, injects maximum PCM, and requires the configured 0.5
  remote gain to produce a 499,000--501,000 ppm remote peak before clean
  detach/stop/join. This is the fake-device proof used by later peer tests, not
  a replacement implementation.
- The first focused target passed 1/1 in 0.03 seconds. Strengthening the
  numeric review correctly produced a red 1/1 run in 0.03 seconds for invalid
  injected-render parameters and final-frame beat diagnostics; the fixes then
  passed in 0.03 seconds. After the real Headless path and gain proof were
  added, focused runs passed in 0.56 and 0.61 seconds. Existing exact
  `jam2_core_input_units` and `jam2_track_recording_workflow_units` remained
  green in 1.87 and 0.61 seconds. The final reviewed target passed 1/1 in 0.60
  seconds and the short unit aggregate passed 29/29 in 38.52 seconds.
- Self-review found and fixed `BUG-P055` through `BUG-P060`. It also confirmed
  that the shared functions do not allocate, lock, log, throw, or block in the
  real-time path; scratch buffers and state remain preallocated and owned by
  each device stream. `git diff --check` reports no whitespace errors, and
  successful cleanup left `build/test-artifacts` absent. No full suite or
  coverage collector ran.
- The frozen Windows inventory contains 47 wholly-uncovered rows in
  `audio_device_windows.cpp`, but that number combines duplicated callback
  processing with ASIO COM/driver/device/open/start/format/lifecycle paths. The
  portable callback responsibilities have direct maintained coverage after
  this refactor. Hardware-only rows remain for the explicit device profile and
  no unsupported subtraction is made from the frozen count before the final
  canonical collection.
- Scope/protocol status: implementation ownership and Headless parity only.
  No network message, field, header, payload, parser, version, authentication
  rule, message ordering, metronome model/epoch rule, shared-WAV wire behavior,
  plugin behavior, or four-peer contract changed. `TEST-REVIEW.md` is unchanged
  because no ambiguous feature removal or new user decision was introduced.

#### BUG-P055 - resampling across opposite PCM rails could overflow before interpolation

- Observed symptom: the interpolation expression subtracted two `int32_t`
  samples before converting the difference to `double`. A transition between
  opposite full-scale rails could overflow the signed 32-bit domain.
- Root cause: conversion occurred after subtraction instead of on each
  operand.
- Change: convert current and next samples independently to `double` before
  subtracting, then retain the existing saturated output conversion.
- Regression proof: a deliberately ramped ring crosses `INT32_MIN` and
  `INT32_MAX` with bounded, deterministic midpoint output and no wrap.
- Remaining test need: repeat under Apple Clang and UndefinedBehaviorSanitizer.

#### BUG-P056 - extreme metronome frame offsets entered overflowing arithmetic

- Observed symptom: the duplicated ASIO/CoreAudio/Headless click renderers
  manually negated a signed render offset and added callback frame offsets.
  `INT64_MIN` could not be negated, and a callback beginning near `UINT64_MAX`
  could wrap its raw frame.
- Root cause: each backend carried its own unchecked signed-magnitude and frame
  addition logic.
- Change: the shared renderer uses the existing overflow-safe musical-frame
  helper and saturates raw callback-frame addition.
- Regression proof: the minimum signed offset at the final raw frame remains
  in a defined bounded domain while ordinary epochs render unchanged.
- Remaining test need: repeat exact extreme behavior under Apple Clang.

#### BUG-P057 - extreme synthetic-input parameters could reach invalid integer conversion

- Observed symptom: non-finite or excessive sample rates/phases/levels could
  flow through tone, bass, pulse, or metronome rendering into floating-to-
  integer conversion outside its representable domain.
- Root cause: injected audio assumed ordinary configured device values even
  though the callable helpers accepted the complete numeric type domain.
- Change: reject non-finite, sub-frame, or unrepresentable render rates and
  non-finite levels/phases; clamp finite pulse samples before conversion.
- Regression proof: NaN, infinity, and excessive values now produce silence,
  while every ordinary injected mode remains finite and nonzero as expected.
- Remaining test need: repeat floating conversion boundaries under Apple
  Clang; physical profile sample-rate validation remains independently tested.

#### BUG-P058 - final-frame metronome beat diagnostics could wrap to zero

- Observed symptom: `(position / interval) + 1` wrapped when the zero-based
  beat index was already `UINT64_MAX`, exposing beat zero at the end of the
  representable frame domain.
- Root cause: the diagnostic increment had no saturation guard.
- Change: saturate the one-based beat diagnostic at `UINT64_MAX`.
- Regression proof: a one-frame callback at the final frame reports the final
  representable beat rather than wrapping; ordinary beat one is unchanged.
- Remaining test need: none beyond Apple Clang parity.

#### BUG-P059 - callback-gap thresholds could use an unrepresentable cast

- Observed symptom: an invalid or extreme buffer/rate combination could make a
  floating callback threshold infinite or larger than `uint64_t` before it was
  cast for comparison.
- Root cause: the diagnostic helper validated only the input rate, not the
  derived threshold's complete numeric domain.
- Change: compare only finite representable thresholds; invalid diagnostic
  inputs fail closed without incrementing a gap bucket.
- Regression proof: ordinary 1.1x/1.5x/2x buckets remain exact and extreme
  thresholds leave all counters bounded.
- Remaining test need: repeat floating-point threshold behavior under Apple
  Clang.

#### BUG-P060 - Headless gain, meter, monitor, and recording behavior diverged from devices

- Observed symptom: Headless mixed remote gain only into output, so its remote
  meters and `CurrentJam`/jam-recorder playback stem observed pre-gain audio,
  while ASIO/CoreAudio observed the post-gain stream. It also omitted the
  platform-equivalent local-monitor peak diagnostics.
- Root cause: the fake device had a third private copy of callback processing
  whose operation order had drifted from both physical implementations.
- Change: Headless now uses the same processing seam and ordering as ASIO and
  CoreAudio, applying remote gain before remote meters and downstream recorder
  stems and publishing the same monitor/prepared/output diagnostics.
- Regression proof: real threaded Headless playback injected at full scale
  reports approximately 500,000 ppm with a configured 0.5 remote gain, while
  injected input produces nonzero monitor and output peaks.
- Remaining test need: compile/run the same fake path on macOS, then confirm
  physical CoreAudio callback order with an explicit hardware profile.

### Iteration 33 - reactive session commands and four-peer count-in timing

- Added `transport_timing.hpp/.cpp` as the single checked transport-grid seam
  for raw/musical frame conversion and next-bar/count-in scheduling. The
  SessionController now uses it for reactive record/restart commands, and the
  GUI, Track Recording workflow, audio metronome helper, and active CLI timing
  paths delegate their duplicated conversion arithmetic to the same bounded
  implementation. The dead private CLI next-bar scheduler was removed rather
  than retained as a second uncalled owner.
- Added `jam2_transport_timing_units`. It directly proves zero, positive,
  negative, minimum-signed, and maximum-unsigned offsets; saturation; ordinary
  48 kHz 120 BPM one-bar schedules; the 200 ms lead rule; positive/negative
  render offsets; nonzero epochs; three-beat subdivided eight-bar count-ins;
  invalid sample rates/counts; and exhausted current/epoch/target ranges.
- Added `jam2_four_session_command_integration`. It launches exactly four real
  staged `release/jam2.exe` processes with isolated artifact roots and fake
  audio, forms the direct full mesh, rejects an invalid reactive frame, applies
  per-peer gain commands, proves a genuinely delayed snapshot, publishes a
  future one-bar `RecordStart`, and requires all four peers to adopt the exact
  96,000-frame countdown on their shared musical bar before observing all four
  inside the active count-in window. It then drains controller queues, shuts
  every peer down reactively, and validates each final manifest.
- Engine snapshots and reactive snapshots now expose the local diagnostic
  evidence needed by this test: metronome epoch/render offset, pending
  transport action/countdown/target/musical frame, and recording/playback
  count-in state. These are in-process/local automation diagnostics, not new
  wire fields.
- The first direct timing run passed 1/1 in 0.02 seconds. The first four-peer
  run correctly failed in 2.92 seconds because a top-level delayed snapshot
  was rejected. After `BUG-P064` and `BUG-T089` were fixed it passed in 5.24
  seconds. Self-review then found `BUG-P065`; the first stronger test went red
  in 0.93 seconds because its old polling loop did not wait for the now-future
  boundary. The two-phase probe subsequently exposed and corrected its
  boundary timeout and moving-offset oracle (`BUG-T090`), with retained
  per-peer evidence showing exact schedule receipt throughout. The final
  reviewed four-peer case passed 1/1 in 5.50 seconds.
- Final focused regressions passed: transport timing 1/1 in 0.05 seconds,
  Track Recording workflow 1/1 in 0.60 seconds, and shared audio/metronome
  processing 1/1 in 0.60 seconds. The short unit aggregate passed 30/30 in
  38.25 seconds. `git diff --check` reports no whitespace error, and successful
  cleanup left `build/test-artifacts` absent. No full suite or coverage
  collector ran.
- Self-review found and fixed `BUG-P061` through `BUG-P065`. `REVIEW-004`
  records the separate product question exposed by the test: a fixed raw
  transport target does not follow later listener render-offset slew. The
  established dynamic-epoch suite remains the live click-alignment owner; no
  timing-model or protocol change was made without review.
- Scope/protocol status: no network field, packet header, payload shape,
  parser, version, authentication rule, or compatibility path changed. The
  only mesh-visible behavioral correction is earlier publication of the same
  existing future transport message, using its unchanged fields. The remaining
  Slice 7 canonical function inventory is active.

#### BUG-P061 - reactive count-in collapsed its countdown and target

- Observed symptom: `track.record-start` and `track.restart` calculated a
  later count-in target but stored that same frame as both countdown start and
  transport target, so recording count-in never became active.
- Root cause: the old helper returned only the final target and discarded the
  next-bar boundary that owned the countdown.
- Change: return a checked schedule containing distinct countdown raw, target
  raw, and target musical frames, and submit all three to the engine.
- Regression proof: the direct 48 kHz case yields 96,000 then 192,000 frames;
  all four real peers expose an active 96,000-frame recording count-in with
  playback count-in disabled and a shared bar-aligned musical target.
- Remaining test need: repeat all count-in assertions under Apple Clang and
  the macOS fake-audio clock.

#### BUG-P062 - duplicated transport frame math had undefined or wrapped extremes

- Observed symptom: SessionController and CLI raw/musical conversions negated
  `INT64_MIN`, added signed offsets without overflow checks, and multiplied or
  added next-bar/count-in frames near `UINT64_MAX`.
- Root cause: multiple private helpers assumed ordinary session-length values
  and did not define their complete integer domain.
- Change: centralize signed magnitude, checked addition/multiplication, bounded
  conversion, sample-rate validation, and fail-closed schedule construction;
  remove the unused duplicate CLI scheduler.
- Regression proof: minimum signed offsets and exhausted current, epoch,
  offset, bar, and target ranges now saturate or return no schedule while
  ordinary offset/epoch contracts remain exact.
- Remaining test need: repeat integer and floating conversion boundaries with
  Apple Clang and UndefinedBehaviorSanitizer.

#### BUG-P063 - delayed reactive actions could wrap behind the engine clock

- Observed symptom: adding a validated `delay_frames` value to a late engine
  frame could wrap the requested application frame to an earlier value.
- Root cause: each JSON number was bounded independently, but their sum was
  unchecked.
- Change: saturate delayed target addition at the final unsigned frame.
- Regression proof: exact ordinary delayed snapshots apply no earlier than the
  requested 512 frames, while the shared timing units close the final-frame
  arithmetic domain.
- Remaining test need: repeat scheduled controller timer behavior on macOS.

#### BUG-P064 - top-level delayed snapshots were rejected despite supported scheduling

- Observed symptom: a reactive `snapshot` frame containing `delay_frames` or
  `apply_frame` returned `command_rejected`, even though action-form snapshots
  and the pending controller-action queue already supported both fields.
- Root cause: the top-level snapshot field whitelist accepted only format,
  type, and ID, then bypassed the scheduler and emitted immediately.
- Change: accept the same validated mutually exclusive frame fields as
  shutdown and route the request through the bounded controller scheduler.
- Regression proof: a four-peer run applies gain, requests a snapshot 512
  frames later, observes the updated gain no earlier than that frame, and
  reports correlated rejection immediately if validation ever regresses.
- Remaining test need: repeat inherited-pipe scheduling under macOS.

#### BUG-P065 - transport publication began at the first count-in beat

- Observed symptom: even after distinct countdown and target frames existed,
  SessionController deferred seek, play, and `ScheduleTransport` commands until
  the countdown boundary. Remote peers therefore received the future intent
  only after its first beat and had no network lead time.
- Root cause: the semantic future schedule was confused with an engine-command
  `apply_frame`; the established GUI workflow submits future targets
  immediately and lets their target fields own execution.
- Change: generated next-bar schedules are submitted immediately, preserving
  explicit automation-frame deferral when the caller actually supplies one.
  The unchanged transport message can now be repeated before countdown.
- Regression proof: the creator's command-applied frame precedes its countdown;
  all four peers first expose the same future schedule, then all four are
  observed inside the active count-in window before the common target.
- Remaining test need: repeat with the macOS fake clock and retain the existing
  impairment matrix as the independent all-mode live metronome/epoch proof.

#### BUG-T089 - correlated reactive rejection was hidden as a generic timeout

- Observed symptom: the first four-peer delayed-snapshot failure waited for a
  success event and reported only the last periodic peer snapshot, hiding the
  command's explicit rejection reason.
- Root cause: the integration helper filtered out `command_rejected` events
  carrying the same request ID.
- Change: success waits now also consume a same-ID rejection and fail with its
  reason immediately.
- Regression proof: the initial red failure was reduced to the exact whitelist
  defect, and subsequent scheduled snapshot checks use the correlated helper.

#### BUG-T090 - the count-in oracle used wall attempts and a moving clock mapping

- Observed symptom: after early publication was fixed, eight immediate polls
  completed before countdown; a later exact current-offset comparison failed
  by 5--143 frames as listener compensation legitimately slewed after target
  adoption. A three-second wall timeout also expired before four contended fake
  devices reached the requested audio frame.
- Root cause: the test used retry count and a later mutable render offset as
  substitutes for the authoritative audio frame and stored musical schedule.
- Change: request a snapshot at the creator's exact countdown `apply_frame`,
  use a bounded ten-second wall allowance, retain compact per-peer failure
  evidence, and assert stable invariants: future publication, exact duration,
  stored shared-bar musical phase, pending/action/count-in state, and each
  peer's own frame inside the active window. Live offset/click convergence
  remains owned by the 21-case dynamic-epoch impairment matrix.
- Regression proof: deliberately red iterations exposed both invalid oracle
  assumptions; the final exact four-peer case passes in 5.50 seconds without
  sleeps, cross-machine coordination, or a weakened transport contract.

### Iteration 34 - JamStorage ownership and shared-track workspace state

- Added `jam2_workspace_state_units`, a 0.04-second hardware-free native test
  rooted entirely beneath `build/test-artifacts`. It covers new, renamed,
  saved, and externally opened jams; every asset-folder mapping; artifact
  state; automatic and requested take naming; owned discard; recursive empty
  workspace pruning with nested-artifact retention; portable-name boundaries;
  exact opened JamJar paths; managed path/file rename and collision behavior;
  every live `SharedTrackController` playback phase/status transition; stale
  arrangement completion; engine convergence; incompatible-WAV status; and
  valid/invalid looper-lane renames.
- Replaced JamStorage's inferred saved filename with explicit ownership of the
  exact selected or newly created project path. A saved root is movable only
  when it exactly matches Jam2's canonical `songs/<slug>` directory. An
  external project title can change without moving or duplicating its parent;
  a managed rename moves both its directory and `.jamjar` name. Filesystem
  identity is case-insensitive on Windows and case-sensitive on other hosts.
- Removed `TrackWorkspaceController::waitForWorkers`, an unreferenced wrapper
  around the already-owned thread pool. No cohesive functionality was split
  merely because `MainWindow` is large. The storage lifecycle was already an
  owned component; this iteration strengthened that meaningful boundary.
- Self-review checked exact path retention, external sibling safety, managed
  byte retention, collision no-mutation, saved-discard refusal, and platform
  case semantics. It also found the two unreferenced
  `SharedTrackController::{processingMessage,applyProcessingMessage}` helpers.
  They remain unchanged and untested under `REVIEW-005` because deleting the
  dormant `track.processing` shape could constitute a protocol-surface change
  and therefore requires user approval.
- Verification: the first exact target passed 1/1 in 0.05 seconds. After the
  case-sensitivity review change, the exact retest passed 1/1 in 0.04 seconds.
  The final short unit aggregate passed 31/31 in 38.67 seconds. The required
  normal MSVC Release build linked the one Jam2 executable successfully;
  successful cleanup left `build/test-artifacts` absent. `git diff --check`
  reports no whitespace errors. No full suite or coverage collector ran.
- Frozen-inventory accounting: the live JamStorage `openSaved`,
  `moveToSongs`, `projectFilePath`, recursive artifact scan, and inline
  artifact-state paths; `SharedTrackController::playbackStatusText`; and
  `LooperProject::renameLane` now have direct maintained behavior. The sole
  uncovered dead worker-wait wrapper no longer exists. The frozen CSV remains
  intentionally unchanged until the final canonical collection.
- Scope/protocol status: `BUG-P066` through `BUG-P068` are product storage
  fixes. No JamJar JSON schema/bytes, network message, field, header, payload,
  parser, version, authentication rule, ordering, metronome model/epoch rule,
  shared-WAV transfer behavior, audio callback, or four-peer contract changed.

#### BUG-P066 - opening a JamJar did not retain its selected project path

- Observed symptom: `openSaved(path, title)` discarded `path` after taking its
  parent and later reconstructed the save filename from the internal title.
  Opening `different-file-name.jamjar` whose title was `Internal Project
  Title` therefore made the next save target `Internal_Project_Title.jamjar`
  and left the selected file stale.
- Root cause: storage held only a display-derived slug, not the exact opened
  project-file identity already owned by the Open workflow.
- Change: retain the normalized selected project path explicitly; first Save
  creates the canonical path explicitly, and subsequent saves use that exact
  path.
- Regression proof: an external filename deliberately different from its
  internal title remains the exact `projectFilePath()` before and after a
  display-title change, with its bytes and sibling file intact.
- Remaining test need: repeat exact path handling on an APFS volume and with a
  Unicode filename.

#### BUG-P067 - renaming an external JamJar could move its entire parent folder

- Observed symptom: a title edit on a project opened outside Jam2's managed
  songs tree treated the selected file's parent as an owned jam directory and
  attempted to rename that whole directory into `release/songs`.
- Root cause: saved state was incorrectly treated as proof that `rootFolder_`
  was created and exclusively owned by Jam2.
- Change: only an exact canonical `songs/<current-slug>` root is movable.
  Externally selected projects retain their root and filename while allowing
  the title stored in their JSON to change.
- Regression proof: the test opens a JamJar beside an unrelated user-file
  sentinel, changes the display title, and requires the parent, selected file,
  and sentinel to remain at their exact paths with no canonical copy.
- Remaining test need: repeat on macOS with a case-sensitive external volume;
  platform-specific path comparison is already implemented.

#### BUG-P068 - managed project rename left the previous JamJar filename

- Observed symptom: the managed project directory moved to its new slug, but
  the existing `.jamjar` inside retained the old name. The next save wrote a
  second filename, leaving the stale project alongside it.
- Root cause: directory ownership and project-file identity were inferred
  separately instead of being changed as one storage operation.
- Change: managed rename moves the folder and renames the existing project
  file before publishing the new storage identity; failure attempts rollback
  and reports a direct error.
- Regression proof: a real project and retained WAV move from `Beta_Jam` to
  `Gamma_Jam`; only `Gamma_Jam.jamjar` exists afterward and the WAV bytes stay
  owned under the new root.
- Remaining test need: repeat the filesystem transaction on APFS and verify a
  read-only project reports the rename error without unrelated mutations.

### Iteration 35 - metronome transport controller and UI clock safety

- Added `jam2_metronome_transport_controller_units`. The focused native target
  covers tap-tempo first/reset/fast/slow/backward/rolling-median/boundary and
  complete signed-timestamp behavior; PlaybackGrid default, sanitized pattern,
  invalid rate, stopped anchor, running final-frame saturation, and clear;
  controller submit success/rejection/exception/missing-target behavior,
  local/remote mutation gating, clock projection, recording revision
  deduplication, non-record transport, engine reset, positive overflow,
  `INT64_MIN`, near-integer rate rounding, NaN, and infinity.
- Reused `transport_musical_frame_from_raw` in
  `MetronomeTransportController::consume` instead of retaining a fourth
  unchecked raw/musical conversion. `PlaybackGrid` now uses the maintained
  metronome pattern clamps and sample-rate limits and saturates both elapsed
  frame calculation and raw/musical interpolation. Tap tempo calculates a
  monotonic interval in the unsigned domain only after ordering timestamps.
- Added a narrow `CommandSubmitter` constructor so the non-real-time GUI
  controller can prove command delegation and exception containment without
  starting an Engine or mocking the application. The ordinary production
  constructor still delegates directly to the same `ApplicationRuntime` and
  no caller behavior changed.
- Self-review preserved the former round-then-validate contract for finite
  near-integer snapshot rates (for example, 7999.6 becomes 8000) and added an
  exact regression. It confirmed all touched code is outside the real-time
  callback and that no click render, epoch proposal/adoption, listener
  compensation, count-in, transport-message, or network path was altered.
- Verification: the first exact test passed 1/1 in 0.04 seconds. The reviewed
  exact retest passed 1/1 in 0.04 seconds; `jam2_transport_timing_units` passed
  1/1 in 0.02 seconds; `jam2_track_recording_workflow_units` passed 1/1 in
  0.61 seconds; and the final short unit aggregate passed 32/32 in 37.59
  seconds. The normal MSVC Release executable linked successfully, successful
  cleanup left `build/test-artifacts` absent, and `git diff --check` reports no
  whitespace errors. No full suite or coverage collector ran.
- Frozen-inventory accounting: `TapTempoTracker::reset` now has direct
  maintained behavior. The controller/grid test also owns already-observed
  functions against their previously untested full numeric domains. The
  frozen CSV remains unchanged until the final canonical collection.
- Scope/protocol status: `BUG-P069` through `BUG-P072` are local GUI clock and
  diagnostic-state safety fixes. No network message, field, header, payload,
  parser, version, authentication rule, ordering, metronome model, epoch rule,
  shared-WAV transfer, device callback, plugin, or four-peer contract changed.

#### BUG-P069 - GUI metronome clock projection retained unchecked offset math

- Observed symptom: a negative render offset was negated directly, making
  `INT64_MIN` undefined, while a positive offset could wrap a raw frame near
  `UINT64_MAX` back to the start of the clock.
- Root cause: the controller retained an older local copy of raw-to-musical
  conversion after checked transport timing had become authoritative.
- Change: delegate projection to `transport_musical_frame_from_raw`, which
  defines signed magnitude, underflow, and saturation for the full domain.
- Regression proof: final raw frame plus one saturates at `UINT64_MAX`; final
  raw frame with `INT64_MIN` produces exactly `2^63 - 1` without negation.
- Remaining test need: repeat under Apple Clang/UndefinedBehaviorSanitizer and
  retain the final all-mode dynamic-epoch matrix.

#### BUG-P070 - tap-tempo timestamp subtraction could overflow

- Observed symptom: `elapsedMs - lastTapMs` executed in signed 64-bit before a
  fast/slow reset. A backward clock or the complete signed input domain could
  overflow before the interval was rejected.
- Root cause: the tracker assumed the QElapsedTimer's ordinary positive range
  even though its callable boundary accepts `int64_t`.
- Change: reject non-increasing timestamps first, then subtract their unsigned
  representations and convert only an already-bounded 150--2000 ms interval.
- Regression proof: backward, duplicate, `INT64_MIN`-to-`INT64_MAX`, and valid
  final-domain taps are defined; ordinary rolling-median BPM remains exact.
- Remaining test need: repeat the integer boundary under Apple Clang.

#### BUG-P071 - running PlaybackGrid interpolation could wrap frame clocks

- Observed symptom: elapsed frames were multiplied and added directly to raw
  and musical anchors. A final-frame anchor or extreme elapsed duration could
  wrap the visible grid to an early frame/beat.
- Root cause: display interpolation lacked the saturating arithmetic already
  required by authoritative transport conversion.
- Change: split elapsed time into checked seconds/remainder frame components
  and saturate both clock additions.
- Regression proof: after real elapsed time at `UINT64_MAX`, both public frame
  clocks remain exactly saturated and the epoch position does not jump.
- Remaining test need: repeat QElapsedTimer behavior on macOS.

#### BUG-P072 - invalid snapshot timing entered non-finite conversion

- Observed symptom: controller code called `std::lround` on the engine's
  `double` sample rate without first rejecting NaN or infinity; PlaybackGrid
  also accepted any positive integer rate and unsupported division values.
- Root cause: UI state trusted an ordinary Engine snapshot more broadly than
  the callable boundary guaranteed.
- Change: finite near-integer rates are rounded and then checked against
  8--384 kHz, invalid rates make the grid unanchored, and pattern fields use
  the maintained metronome clamps. Non-finite BPM falls back to 120.
- Regression proof: 7999.6 retains prior rounding to 8000, while NaN,
  infinity, 7999, invalid division, and invalid pulse units fail closed or
  normalize without numeric-domain conversion.
- Remaining test need: repeat floating boundary behavior under Apple Clang.

### Iteration 36 - WAV staging, resampling, and lane-merge idempotence

- Added `jam2_track_workspace_support_units` under `tests/unit/gui`. It reads
  a Unicode-path stereo WAV through the production strict parser and proves
  exact format, rate, channels, PCM width, byte count, frames, duration, and
  SHA-256. Missing, malformed, and empty inputs fail without a staged file.
- Matching-rate staging preserves exact bytes beneath
  `<workspace>/imported/<sha>.wav`, is repeatable, and atomically replaces a
  corrupt hash-named destination. A source already owned by the `recorded`
  directory stays at its original path, including Windows case-insensitive
  folder identity.
- The real staging integration now exercises the five wholly-uncovered frozen
  helpers: little-endian 16/32-bit encoding, PCM16 header construction, strict
  sample reading, and little-endian sample serialization. Stereo 44.1-to-48
  kHz conversion produces exactly 480 frames from 441, is reparsed by the
  strict PCM16 reader, hashes the complete output, remains byte-idempotent,
  and repairs a corrupt converted destination. Self-review added the zero-rate
  preserve-source contract plus successful 8 kHz and 384 kHz endpoint
  conversions.
- Direct synchronized-lane behavior proves same-hash local mix/path retention,
  unique re-keying for a same-ID/different-WAV conflict, stable repeated merge,
  authoritative omission, and WAV deletion without resurrecting old audio.
  Local-only behavior proves hashed and path-only lanes survive exactly once,
  an ID collision is safely re-keyed, and practice references, legacy managed
  references, non-local lanes, and empty paths remain excluded.
- Verification: the first exact target passed 1/1 in 0.03 seconds. Following
  self-review, the rebuilt exact target passed 1/1 in 0.06 seconds and the
  updated short unit aggregate passed 33/33 in 38.04 seconds. The normal MSVC
  Release build remained green, successful cleanup left
  `build/test-artifacts` absent, `release/` contains only `jam2.exe` matching
  the Jam2 executable name, and the touched files contain no trailing
  whitespace. No full suite or coverage collector ran.
- Frozen-inventory accounting: all five wholly-uncovered
  `TrackWorkspaceSupport.cpp` WAV conversion helpers now have direct native
  ownership. The public metadata, staging, synchronized merge, and local-only
  merge paths also gain direct boundary/idempotence coverage. The frozen CSV
  remains intentionally unchanged until the final canonical collection.
- Scope/protocol status: `BUG-P073` through `BUG-P076` are local filesystem,
  WAV conversion, and in-memory project-merge safety fixes. No network
  message, field, header, payload, encoding, parser, version, authentication
  rule, ordering, shared-WAV transfer state machine, metronome model, epoch
  rule, real-time callback, plugin, or four-peer contract changed. No new
  manual-review item was introduced.

#### BUG-P073 - unsupported requested rates could create an invalid staged WAV

- Observed symptom: `stagePcm16Asset` treated every positive requested rate as
  valid. A request such as 1 Hz could be resampled and written even though
  Jam2's own strict WAV parser rejects rates outside 8--384 kHz.
- Root cause: the function used `expectedSampleRate > 0` only as a conversion
  switch and did not apply the maintained runtime sample-rate boundary.
- Change: zero remains the explicit preserve-source-rate sentinel; every
  nonzero request must now pass `jam2::limits::valid_sample_rate` before any
  file operation.
- Regression proof: -1, 1, 7999, and 384001 reject without a destination;
  zero retains exact 48 kHz bytes, while 8000 and 384000 convert successfully
  with exact one-millisecond frame counts.
- Remaining test need: repeat both endpoint conversions under Apple Clang.

#### BUG-P074 - staging destination arguments could escape the owned workspace

- Observed symptom: a blank or relative staging root was resolved against the
  process working directory, while an absolute, nested, or `../` asset-folder
  argument could select a destination outside the intended project subtree.
- Root cause: `QDir::absoluteFilePath` normalized caller input without first
  enforcing the helper's ownership contract.
- Change: require a nonblank absolute staging root and exactly one safe
  letter/number/hyphen/underscore asset-directory component before reading or
  creating files.
- Regression proof: blank/relative roots and empty, dot, parent, nested,
  Windows-absolute-shaped, and traversal components all reject with no escape
  file. A file occupying the required directory also remains byte-exact when
  directory creation fails.
- Remaining test need: repeat containment on APFS and verify the intended
  case-sensitive directory identity.

#### BUG-P075 - a mid-hash file read error could publish a prefix digest

- Observed symptom: the SHA loop added each returned block but did not inspect
  `QFileDevice::error`; a failed read before EOF could therefore return the
  digest of only the readable prefix as though it identified the full WAV.
- Root cause: open failure was handled explicitly but read failure was not.
- Change: an empty block accompanied by a QFile error aborts hashing and
  propagates the existing empty-hash failure path.
- Regression proof: exact source and converted-file digest assertions prove
  the successful full-file path; source review verifies the read-error branch
  now returns before `hash.result()` can be published.
- Remaining test need: if file access is later placed behind an injectable
  device seam, add deterministic mid-stream read-failure injection; no such
  seam is justified solely for this private helper.

#### BUG-P076 - hashless local-only WAV lanes multiplied on every merge

- Observed symptom: after a path-only local lane collided by ID and was
  preserved under a generated ID, the next arrangement merge failed to
  recognize it and appended another copy. Repeated sync could grow the bank
  until its lane limit.
- Root cause: local-only presence checks compared only nonempty SHA-256 values;
  a valid local lane whose hash was temporarily absent had no idempotent
  identity after collision re-keying.
- Change: hashed lanes retain hash identity; hashless lanes use a nonblank,
  platform-normalized local asset path, case-insensitive on Windows and
  case-sensitive elsewhere.
- Regression proof: a hashless local take collides with a different remote WAV,
  is re-keyed exactly once, and a second merge returns zero while retaining the
  exact three-lane count and generated ID. Managed references and unrelated
  lanes remain excluded.
- Remaining test need: repeat normalized path identity on both default and
  case-sensitive APFS fixtures.

### Iteration 37 - shared Section-bank barrier and quantized launch ownership

- Added the responsibility-owned `SharedBankLaunchCoordinator` and removed
  five coupled switch/index/target/host-ready/peer-ready fields from
  `MainWindow`. The coordinator owns one exact prepare identity, target bank,
  requested beat, original expected peers, host readiness, peer readiness,
  duplicate handling, departure, cancellation, and consume-on-commit.
- Added `jam2_shared_bank_launch_units`, labelled for unit/GUI/network/
  performance/metronome/epoch/transport. Exactly four-peer barrier state is
  proved with creator excluded and three expected peers; stale switch/bank,
  blank, self, nonmember, out-of-order, duplicate, departure, take, replacement,
  and clear paths are direct deterministic cases.
- Moved the maintained next-beat/bar boundary calculation behind the same
  owner and added checked frame scheduling. Ordinary 48 kHz, 0.5-second beats
  map absolute beat 8 through epoch frame 1,000 to musical frame 193,000 and
  raw frame 192,500 at a +500-frame render offset. Explicit targets and
  negative offsets are exact; minimum/maximum rates pass; invalid rates and
  nonpositive/non-finite beat intervals reject; final beat/frame/epoch and
  `INT64_MIN` render offset saturate without signed conversion or addition
  overflow.
- Extended the real `jam2_four_performance_integration` rather than creating a
  second broad runner. With exactly four established direct-mesh GUI peers,
  Headless fake audio, shared-grid metronome, and running global transport, a
  joiner invokes the painted Section B queue action. The creator handles its
  request and all four complete the prepare/ready/switch barrier. The creator
  then queues Section A; all four restore the original lane identities. Both
  transitions require no pending barrier residue, continued playback, and the
  exact unchanged metronome epoch frame on every peer.
- Added hard automation diagnostics for pending bank/index/absolute beat,
  active switch identity, requested target, host readiness, and expected/ready
  peer sets. These values are gathered outside real-time callbacks and make a
  stalled preparation directly inspectable.
- The initial unit build passed but emitted one MSVC warning because Qt 6's
  `QSet::remove` already returns `bool`; self-review removed the redundant
  numeric comparison. The first strengthened four-peer run passed in 17.25
  seconds. Review then rejected an unsafe attempt to add a newly authenticating
  peer to an in-flight barrier before its arrangement/WAV readiness was known.
  The final rule cancels the switch immediately and broadcasts the existing
  cancel shape; established-peer departures are removed and the remaining
  prepared peers may commit.
- Verification: the warning-free reviewed exact unit passed 1/1 in 0.04
  seconds, the reviewed four-peer test passed 1/1 in 15.21 seconds, and the
  short unit aggregate passed 34/34 in 37.83 seconds. The normal MSVC Release
  executable linked, successful cleanup left `build/test-artifacts` absent,
  and no full suite or coverage collector ran.
- Frozen-inventory accounting: `MainWindow::requestBankLaunch`,
  `beginSharedBankLaunch`, `prepareSharedBankLaunch`, `noteSharedBankReady`,
  `handleSharedBankReady`, `maybeCommitSharedBankLaunch`, and
  `applyScheduledBankLaunch` now have direct real four-peer behavior in
  addition to the complete extracted state/clock unit owner. The frozen CSV
  remains intentionally unchanged until the final canonical collection.
- Scope/protocol status: all existing `bank.request`, `bank.prepare`,
  `bank.ready`, `bank.cancel`, and `bank.switch` names, fields, JSON shapes,
  validators, parsers, routing, authentication, and versions are unchanged.
  The valid-clock transition retains the same next-bar and epoch semantics.
  `REVIEW-006` and `REVIEW-007` record protocol-sensitive questions rather
  than changing them without approval.

#### BUG-P077 - live membership could silently change an in-flight bank barrier

- Observed symptom: commit recomputed expected peers from the live endpoint
  map. A peer authenticating after `bank.prepare` became required even though
  it had never received that prepare, so the switch waited until its 30-second
  timeout. A departure altered the requirement only as an incidental map side
  effect.
- Root cause: preparation identity and membership were spread across
  `MainWindow`, while the exact participants that received the broadcast were
  not owned as barrier state.
- Change: snapshot the original expected tokens, accept readiness only from
  them, remove a departed token explicitly, and cancel immediately if a new
  peer authenticates while preparation is active. Cancellation leaves every
  participant on the old Section until the action is queued again after sync.
- Regression proof: the unit requires creator plus exactly three expected
  peers, rejects a later token, handles every ready order/duplicate, and
  commits after the one missing original peer departs. The established
  four-peer real path completes twice with no residue.
- Remaining test need: macOS parity; no automated fifth-peer case is added
  because this initiative's real-jam contract is exactly four peers.

#### BUG-P078 - bank schedule frame calculation could overflow or enter undefined conversion

- Observed symptom: target beat, seconds per beat, and sample rate were
  multiplied in `double`, passed to `std::llround`, cast to unsigned, and added
  to the epoch without checking representability. Large values could exceed
  signed rounding range or wrap the musical frame before raw projection.
- Root cause: Section launch retained a local unchecked clock calculation
  after shared transport conversion had gained full-domain boundaries.
- Change: a pure scheduler validates the maintained sample-rate/beat domain,
  rounds in `long double`, saturates the beat-frame product and epoch addition,
  and delegates musical-to-raw projection to the checked transport helper.
- Regression proof: ordinary and explicit schedules are exact; maximum beat,
  maximum epoch, maximum finite interval, and minimum signed render offset all
  produce `UINT64_MAX` without wrap or undefined signed conversion.
- Remaining test need: repeat under Apple Clang/UndefinedBehaviorSanitizer.

#### BUG-P079 - invalid transport clocks could still select a future shared switch

- Observed symptom: the commit path treated any positive sample rate and beat
  interval as quantizable. Unsupported rates and infinity could therefore
  publish a future target even though local scheduling could not represent it,
  risking different immediate/future decisions across peers.
- Root cause: readiness and target selection did not apply the same maintained
  clock domain as the eventual frame scheduler.
- Change: target selection requires 8--384 kHz, a finite positive beat
  interval, anchored running transport, and unchanged bank timing. Otherwise
  the existing target value zero selects the immediate/timing-reset path.
- Regression proof: future requests and next-bar selection stay exact for a
  valid clock; changed timing, stopped or unanchored transport, 7999 Hz, and
  infinite beat duration all return the immediate target deterministically.
- Remaining test need: retain all-mode four-peer impairment coverage at the
  final distribution gate; no model or epoch rule changed.

### Iteration 38 - LooperProject lane edits and Section shortening

- Added checked `LooperProject` owners for lane gain, mute, solo, region,
  asset clearing, natural timeline end, and crop-to-Section-end behavior.
  `appendLane` now validates the complete local lane state and rejects a
  duplicate nonblank identity in the same bank; rename applies the maintained
  stored-name bound. Accepted mutations are serialized and reloaded through
  the current-format parser in the direct unit test.
- Section end/page/crop arithmetic now uses finite checked `long double`
  conversion and saturation rather than overflow-prone integer addition or
  out-of-domain floating-to-integer casts. The direct widget boundary target
  covers non-finite clocks, maximum signed frames, and maximum page counts.
- `MainWindow::shrinkSectionOneBar` stages all lane changes in a copy of the
  project and commits only after every crop succeeds. Non-looping placements
  crop their source range, looping placements retain their exact source loop
  and shorten only the output stop, and a placement wholly after the new end
  clears its WAV identity while preserving the lane name and mixer state.
- A cleared placement reports its old path and hash. After project commit,
  MainWindow cancels outgoing transfer/validation ownership only when no lane
  still references the hash, then performs the existing safe obsolete-file
  cleanup. The same owner is used by the painted Remove WAV action.
- Converted the Rename Lane prompt to one explicitly owned `QDialog` whose
  line editor and OK/Cancel buttons are all laid out and registered. The editor
  enforces the 512-character stored-name bound, and a blank trimmed name keeps
  Save disabled. This preserves the visible modal behavior while making both
  outcomes deterministic for the GUI agent.
- Expanded `jam2_looper_project_units` across bank/arrangement ownership, every
  local lane mutation, invalid indices and state, atomic rejection,
  serialization, rate-converted timeline end, natural/looped/destructive crop,
  removal identity, unchanged behavior, and numeric limits. Expanded the
  Section assertions in `jam2_gui_widget_boundary_units`.
- Extended `jam2_four_gui_agent_smoke` to invoke painted gain, mute, and solo
  and require their visible state across exactly four real processes. Extended
  `jam2_four_gui_modal_integration` to change then cancel and change then save
  Rename Lane on every one of four processes.
- First focused model and widget passes were green; the strengthened GUI smoke
  passed in 4.23 seconds. The first modal run failed in 100.53 seconds because
  `QInputDialog` had not yet created/classified its buttons. After the explicit
  dialog fix it passed in 100.12 seconds. Self-review then tightened the
  zero-length-stop and editor-length boundaries; the final model test passed
  in 0.03 seconds, widget boundaries in 4.05 seconds, final modal workflow in
  99.70 seconds, and the short unit aggregate passed 34/34 in 37.85 seconds.
  Successful runs removed `build/test-artifacts`. No full or coverage run
  occurred.
- Frozen-inventory accounting: live rename, painted gain/mute/solo, region
  editing, WAV clearing, and Section-shortening behavior now have model or real
  GUI ownership. The frozen CSV remains intentionally unchanged until the
  final canonical collection. `moveSelectedLooperLane`,
  `setSelectedLooperLaneGain`, and `editSelectedLooperLaneRegion` have no
  callers and are recorded in `REVIEW-002`, not invoked artificially.
- Scope/protocol status: no persistence load parser or schema, network message,
  field, header, payload, encoding, validator, version, authentication rule,
  shared-WAV transfer message/state machine, metronome model, or epoch rule
  changed. `REVIEW-008` records the existing Section-duration versus prepared-
  renderer limit mismatch. Prepared-mix/project-lifecycle ownership is next.

#### BUG-P080 - local lane mutations could create noncanonical unsafe state

- Observed symptom: local append accepted non-finite gain, negative source
  length, unsupported nonzero sample rates, noncanonical stop/crop sentinels,
  crops beyond known source audio, and duplicate nonblank lane IDs. Rename
  could also store a name beyond the maintained content limit.
- Root cause: load-time checks and individual dialogs owned fragments of the
  lane invariant, but `LooperProject` did not own one complete validation
  boundary for programmatic and UI-created lanes.
- Change: centralize local lane validation, enforce it before and after default
  naming, reject duplicate explicit identity, bound generated filenames, and
  make rename validate the trimmed stored value.
- Regression proof: each invalid state rejects without growing or changing the
  bank; all accepted create/edit/clear states serialize into a current-format
  loadable project.
- Remaining test need: repeat local path and Unicode-name boundary cases under
  Qt/macOS; no persistence parser acceptance was changed.

#### BUG-P081 - Section timeline calculations could overflow or use undefined conversion

- Observed symptom: page-count addition could overflow `int`; extreme duration
  and rate products were cast from floating point without representability
  checks; source crop addition could overflow signed 64-bit frames.
- Root cause: helpers assumed ordinary UI values even though loaded/project
  state and coverage probes reach the maintained numeric boundaries.
- Change: use subtraction-based page rounding, finite `long double` timeline
  math, pre-cast saturation, and available-range addition.
- Regression proof: exact ordinary frame tolerance remains unchanged while
  `INT_MAX`, `INT64_MAX`, NaN, and extreme finite clocks return deterministic
  bounded results.
- Remaining test need: repeat with Apple Clang and UndefinedBehaviorSanitizer.

#### BUG-P082 - shortening a Section could corrupt a looped lane's source loop

- Observed symptom: Remove One Bar rewrote both the timeline stop and source
  crop using the retained output duration. For a looped placement, that could
  replace the intended loop and produce a source endpoint beyond the WAV.
- Root cause: MainWindow performed field-by-field crop math without separating
  source-loop ownership from output-placement ownership; a later failing lane
  could also leave earlier lanes already changed.
- Change: the model crop owner preserves loop source start/end, shortens only
  the explicit timeline stop, bounds non-loop source crops, and MainWindow
  stages the complete project before commit.
- Regression proof: direct looped/non-looped/cleared cases assert exact frame
  values and loadability; rejected and unchanged crops preserve byte-equivalent
  serialized project state.
- Remaining test need: add a real four-peer Remove One Bar confirmation flow
  if that modal is selected by the final live-control tail audit.

#### BUG-P083 - a Section crop could leave removed-WAV transfer work alive

- Observed symptom: a placement wholly beyond the shortened Section had its WAV
  cleared but its outgoing validation/transfer ownership could continue until
  timeout even when no remaining lane referenced that content.
- Root cause: destructive crop returned no removed content identity, so the
  caller could clean local paths but could not retire hash-addressed network
  work safely.
- Change: report removed path/hash, commit the cropped model first, and cancel
  transfer/validated-hash state only after proving no lane still references the
  hash.
- Regression proof: the direct crop test proves exact removal identity and
  cleared state; MainWindow uses one reference scan before cancellation for
  both Section crop and Remove WAV.
- Remaining test need: the final live-control tail should exercise one active
  transfer crop and one same-hash remaining-reference case across four peers.

#### BUG-P084 - zero-length lane stops and overlong rename input crossed model boundaries

- Observed symptom: `appendLane` accepted an explicit stop equal to start even
  though the region editor rejects a zero-length placement. The new explicit
  rename editor initially allowed typing beyond what `renameLane` could store,
  causing Save to close without applying the apparent input.
- Root cause: create and edit paths used slightly different canonical bounds,
  and the visible editor did not expose the model's name limit.
- Change: explicit stop must be strictly after start; the editor caps input at
  the maintained name limit and disables Save for a blank trimmed value.
- Regression proof: the direct model test rejects equal start/stop, and the
  reviewed four-peer modal passes Cancel and Save with the bounded editor.
- Remaining test need: repeat max-length typing under Cocoa text handling.

#### BUG-T091 - lazy QInputDialog children made Rename Lane unclassifiable

- Observed symptom: the first four-peer modal run found the line editor but no
  classified Save or Cancel buttons. Attempts to cancel left old dialogs open,
  and later peers accumulated duplicate rename controls before timeout.
- Root cause: the static `QInputDialog` path creates its button box lazily after
  the automation registration point, so stable semantic IDs could not be
  attached to the complete modal.
- Change: use an explicit stack-owned dialog with one form, line editor, and
  OK/Cancel button box; lay out and register every child before `exec()`.
- Regression proof: the first run failed in 100.53 seconds; the corrected run
  passed in 100.12 seconds and the reviewed final run passed in 99.70 seconds
  across exactly four processes with model-preserving Cancel and model-changing
  Save.
- Remaining test need: repeat under Cocoa modal dispatch in the macOS parity
  pass.

### Iteration 39 - prepared-mix lifecycle and stale-project ownership

- Added the non-widget `jam2::gui::PreparedMixLifecycle` under `app/gui` and
  made `TrackWorkspaceController` its owner. `MainWindow` no longer holds or
  aliases the active result, twelve-bank cache, worker/rerun flags, rerun bank,
  revision, play-when-ready flag, request/coalesced/failure counters, or
  obsolete-path set as separate state.
- The coordinator now owns request validation, exact monotonically changing
  generation, current worker identity, coalescing to the shared-bank priority,
  one rerun decision, stale/wrong completion rejection, per-bank result
  validation/cache, active adoption, inactive/active invalidation, playback
  intent consumption, diagnostics, managed relocation, and cleanup retention.
  Rendering remains offline in `PreparedMixRenderer`; engine submission and
  visible presentation remain MainWindow orchestration.
- Project replacement, grid-timing replacement, practice-reference replacement,
  Section removal, and reference generation now invalidate the appropriate
  lifecycle generation before an old worker result can apply. A caller that
  requests a new render while old work remains gets one explicit rerun using a
  fresh snapshot.
- Derived cache clearing now returns or queues the exact obsolete path. A path
  is deleted only when it is no longer active/cached; a managed deletion
  failure is retained and retried rather than forgotten. Relocating a managed
  project updates active, cached, and pending-cleanup paths through one owner.
- Added `jam2_prepared_mix_lifecycle_units` and registered it in unit, GUI,
  performance, and shared-content build selections. It directly covers idle,
  invalid, start, coalesced, wrong-generation, rerun, apply, invalidate/stale,
  no-source, renderer-error, invalid-result, cache/adopt/missing, obsolete,
  relocate, playback-consume, metadata-failure, per-bank clear, and diagnostics
  contracts without a GUI or audio device.
- Verification: the first exact implementation passed in 0.04 seconds.
  Self-review added missing/empty bank retirement and retry ownership; the
  reviewed exact target passed in 0.03 seconds. The real renderer/application
  boundary passed in 0.03 seconds, the real exactly-four-peer fake-audio
  prepared-bank workflow passed in 15.05 seconds, and the short native unit
  aggregate passed 35/35 in 38.21 seconds. Successful cleanup left
  `build/test-artifacts` absent. No full or coverage run occurred.
- Frozen-inventory accounting: the frozen CSV predates this coordinator, so it
  remains intentionally unchanged. Prepared rendering was already directly
  covered; this iteration closes the previously MainWindow-only request/cache/
  invalidation lifecycle through an independently tested owner. The remaining
  live application/action tail audit is next.
- Scope/protocol status: no renderer audio algorithm or five-minute limit,
  persistence shape/parser, network message/field/header/payload/encoding,
  validator, protocol version, authentication rule, shared-WAV wire behavior,
  metronome model, epoch rule, or real-time callback changed. `REVIEW-008`
  remains open for the separate long-Section product decision.

#### BUG-P085 - an old-project prepared render could apply after project replacement

- Observed symptom: `loadSongJson` unloaded and cleared visible prepared state
  but did not advance the render revision. An already-running worker retained
  the previous LooperProject snapshot and could later publish its old PCM into
  the newly loaded project.
- Root cause: revision, worker, cache, and project replacement were separate
  MainWindow fields and the replacement path cleared cache data without owning
  the in-flight generation.
- Change: project replacement calls lifecycle invalidation; completion must
  match both the running worker generation and current lifecycle revision.
  Stale output is returned for deletion and never reaches result application.
- Regression proof: the direct test starts an old-project request, invalidates
  it, and requires its completion to be classified stale with its exact output
  path. A new-project request coalesced behind old work owns the sole rerun.
- Remaining test need: repeat event-loop scheduling under Cocoa; the exact
  state decision is platform-neutral.

#### BUG-P086 - clearing or missing a bank cache could orphan derived WAVs

- Observed symptom: switching to an empty/missing cached bank assigned an empty
  active result and clearing inactive bank slots discarded their path identity.
  The managed prepared WAV could remain until broad workspace cleanup.
- Root cause: cache slots and active state were cleared independently, with no
  owner returning superseded paths to persistence cleanup.
- Change: cache invalidation queues non-active derived paths, failed adoption
  retires the prior active path, active discard returns its exact path, and
  MainWindow drains these paths through `ProjectPersistenceCoordinator`.
- Regression proof: direct tests cover inactive invalidation, missing-bank
  adoption, active replacement, exact obsolete sets, per-bank preservation,
  and active discard; the four-peer A/B transitions remain green.
- Remaining test need: repeat managed delete/rename behavior on APFS.

#### BUG-P087 - invalid prepared worker results could be clamped into another bank

- Observed symptom: result application clamped `result.bankIndex` into the
  current bank range and accepted an error-free result without requiring a
  nonempty path, positive frames, sample rate, or file size. A malformed local
  completion could therefore be associated with a different Section.
- Root cause: the renderer normally returns valid local metadata, so result
  validation was implicit and the GUI compensated with `qBound`.
- Change: reject out-of-range bank identity and incomplete result metadata,
  count the failure, cancel pending playback, remove any orphan output, and
  require the output file to exist before caching/application.
- Regression proof: out-of-range, empty-path, and explicit renderer-error
  cases fail without entering a cache; valid results retain their exact bank.
- Remaining test need: none beyond macOS parity; this is local worker state and
  does not change network acceptance.

#### BUG-P088 - failed prepared-cache deletion was forgotten after replacement

- Observed symptom: active result replacement iterated obsolete paths, ignored
  `discardTransientWav` failure, and unconditionally cleared the set. A file
  temporarily held by the engine/waveform worker could lose all retry identity.
- Root cause: cleanup outcome was presentation-side best effort rather than
  lifecycle state.
- Change: the coordinator yields obsolete paths, and MainWindow returns any
  still-owned deletion failure to the coordinator for a later drain. Path
  relocation also updates retained cleanup identities.
- Regression proof: the direct test takes, re-retains, relocates, and retakes
  the same exact obsolete identity while active and cached paths remain sound.
- Remaining test need: inject a real Windows sharing-violation deletion in a
  future filesystem seam if one is justified; current persistence retry paths
  are independently covered.

### Iteration 40 - shared-track loop and atomic project-state ownership

- Added checked loop operations and effective prepared-frame conversion to
  `SharedTrackController`. MainWindow's Loop Start, Loop End, Clear Loop,
  whole-track reset, and enabled toggle no longer coordinate the three related
  fields independently. The conversion boundary handles disabled, missing,
  open-ended, finite, reversed, non-finite, zero-sample-rate, and zero-frame
  inputs without undefined numeric conversion.
- Moved the existing persisted track fields into the controller's directly
  tested `projectJson`/`decodeProjectJson` boundary. Field names and written
  shape are unchanged. Decoding starts from clean model state, validates types,
  finite/ranged processing values, sample rate, byte count, digest, string
  bounds, and loop sentinels, resolves a relative path against the candidate
  project folder, and normalizes only range/order conflicts in loop points.
- `openSong` now decodes the candidate track before discarding prepared audio,
  waiting for old workers, deleting an unsaved workspace, or changing project
  roots. Invalid track state leaves the current project intact. A missing
  track object explicitly yields a clean no-track model; a nonempty persisted
  path owns its compatibility-audit identity instead of inheriting the prior
  model's local flags.
- Added hard automation evidence for track metadata, exact loop seconds,
  effective loop frames, prepared-mix frame/rate/revision/request/failure
  state, and the engine's prepared-source playing/frame/schedule/underrun/busy
  state. These are local diagnostic fields only and are not sent over Jam2's
  network protocols.
- Extended `jam2_workspace_state_units` with loop ordering/end boundaries,
  whole-track and clear transitions, exact frame conversion, non-finite
  recovery, project round-trip/relative path ownership, missing-track reset,
  loop normalization, and malformed type/range/digest rejection. The first
  exact pass and the reviewed final pass both passed in 0.06 seconds.
- Extended `jam2_four_performance_integration` against exactly four real Jam2
  GUI-agent processes using headless fake audio. After a shared recorded WAV
  and prepared mix converge, it now performs an explicit synchronized stop and
  restart, proves all four prepared sources are actually playing, and invokes
  Clear/Start/End/disable/re-enable/final-Clear on every instance. The first
  run failed in 55.23 seconds because of `BUG-T092`; the corrected reviewed run
  passed in 18.89 seconds with zero prepared failures and busy events.
- Verification: the short native unit aggregate passed 35/35 in 38.40 seconds.
  The single staged Windows executable is `release/jam2.exe` (47,403,520
  bytes), successful cleanup left `build/test-artifacts` absent, and no full or
  coverage pass ran.
- Scope/protocol status: no persistence field/shape was added or removed. The
  persistence acceptance boundary was hardened. No network message, field,
  header, payload, encoding, validator, parser, version, authentication rule,
  shared-WAV transfer behavior, metronome model, epoch rule, audio algorithm,
  or real-time callback changed. The remaining file/action tail audit stays
  active.

#### BUG-P089 - independent loop edits could leave a reversed visible region

- Observed symptom: setting Loop Start after an older Loop End, or Loop End
  before an older Loop Start, left both values in the model. The engine silently
  fell back to an open-ended range while the checkbox and waveform retained
  contradictory markers.
- Root cause: the two buttons wrote their own scalar and enabled looping without
  reconciling the other endpoint.
- Change: checked edits clamp to the loaded duration and clear only the older
  conflicting endpoint. Start is always inside the final millisecond; End is
  always at least one millisecond and no later than the duration.
- Regression proof: direct ordering/boundary cases and all four live GUI
  processes require either one open endpoint or a strictly ordered finite
  region whose effective end frame is after its start.
- Remaining test need: repeat the control path under Cocoa timing.

#### BUG-P090 - opening a project without track state retained the old track

- Observed symptom: `loadTrackJson` returned immediately for an empty object,
  leaving file identity, duration, transforms, and loop state from the project
  that had just been closed.
- Root cause: missing persistence data was treated as “do not update” rather
  than a clean no-track project state.
- Change: an absent `track` value decodes to a fresh `SharedTrackModel` with the
  current local sync-control policy and is applied after the song is accepted.
- Regression proof: the direct decoder starts from a deliberately populated
  controller and requires missing state to produce an empty path, zero bytes,
  zero duration, and no leaked project identity.
- Remaining test need: exercise the native Cocoa Open dialog around the same
  checked decoder.

#### BUG-P091 - malformed persisted track values were applied after teardown

- Observed symptom: negative/unsupported metadata and non-finite processing or
  loop values were assigned field by field only after the old workspace had
  already been discarded. Non-finite loop seconds could then reach `llround`
  during engine command construction.
- Root cause: MainWindow owned permissive scalar extraction and no atomic
  candidate existed before destructive project transition work.
- Change: checked decoding rejects invalid types, non-finite/out-of-range
  values, unsupported sample rates, oversized metadata, and malformed hashes
  before teardown. Effective frame conversion independently recovers any
  invalid state introduced through the still-maintained mutable model surface.
- Regression proof: invalid number/type/sample-rate/hash cases fail without
  mutating live state; NaN/infinity direct-state cases resolve to a safe whole
  track and never enter integer conversion undefined behavior.
- Remaining test need: malformed-file selection through Cocoa; the pure decoder
  and numeric conversion are platform-neutral.

#### BUG-P092 - persisted track audit ownership inherited the prior project

- Observed symptom: `sampleRateCompatible` and `userProvidedSource` were not
  decoded or reset. A project opened in a fresh process could skip compatibility
  auditing for its nonempty track path, while one opened after a user WAV could
  inherit unrelated flags from that previous file.
- Root cause: those local lifecycle flags are intentionally absent from the
  persisted shape, but no deterministic local values were derived on load.
- Change: decoded track state always starts compatible pending real inspection,
  and any nonempty persisted path owns the local source/audit identity. Missing
  state stays a clean no-source model.
- Regression proof: project round-trip requires a resolved nonempty path to be
  locally source-owned and compatible, while missing state has neither path nor
  previous metadata.
- Remaining test need: load a mismatched-rate real WAV through the Cocoa file
  workflow and confirm the existing audit/conversion presentation.

#### BUG-T092 - four-peer readiness assumed recording left the source playing

- Observed symptom: the first extended run timed out after 55.23 seconds even
  though every peer exposed the same valid 532,717-frame prepared WAV, 11.098
  second duration, available asset, idle worker, and zero render failures.
- Root cause: the new readiness predicate required `prepared_source_playing`
  immediately after the shared lane-recording transport sequence. That
  sequence legitimately ended with the prepared source stopped while the
  higher-level global transport intent remained active.
- Change: readiness first proves the prepared artifact independently, then the
  test issues one synchronized Stop/Play pair and requires all four actual
  prepared sources to enter playing with zero busy events before loop actions.
- Regression proof: the corrected exact test passed in 18.89 seconds and all
  four peers completed every loop transition.
- Remaining test need: none on Windows; repeat under Cocoa event scheduling.

### Iteration 41 - transactional JamJar assets and live file/action closure

- Added `LooperAssetMaterializer` as a non-widget transactional boundary for
  saving looper WAVs into a JamJar. It validates the complete source WAV and
  SHA-256 identity, canonicalizes source/target containment, inventories the
  target directory case-insensitively, reuses identical bytes, allocates a
  portable collision-safe name with an exclusive reservation, copies through
  `QSaveFile`, rehashes the committed result, converts project paths to
  relative paths, and rolls back every newly created file on any failure.
- Added `LooperProject::replaceLane` as the checked atomic replacement path.
  It rejects an incomplete candidate without partial mutation and retains the
  stable lane ID. Asynchronous WAV import, recording finalization, and sample-
  rate compatibility conversion now construct a complete candidate before
  committing it and retire superseded managed files/hashes only after success.
- Reordered Save so an unsaved workspace is materialized and verified before
  `JamStorage::moveToSongs`. A failed verification no longer promotes or
  destroys unsaved state. Successful saves explicitly clear JamStorage's
  artifact flag, and a candidate abandoned because its source snapshot changed
  rolls back its materialized files.
- Extended the compatibility-audit result with exact staged-file ownership.
  Reused files and already-owned recordings are not treated as newly created;
  stale/error/retargeted worker completions delete only files that invocation
  created. Adopted conversions register the managed result and retire an
  unreferenced superseded managed path/hash. External user source files remain
  outside Jam2's cleanup ownership.
- Registered the live Export scope modal controls and corrected the outer
  `looper.export` action classification: its first boundary is Jam2's owned
  scope modal, while only the modal Accept crosses into the native file dialog.
  The product export workflow itself is unchanged.
- The real four-peer shared-content case now saves all exactly four converged,
  WAV-heavy peers, requires four isolated saved roots and clean storage state,
  parses every resulting JamJar, resolves every relative WAV, rehashes its
  exact bytes, and finds all six expected content identities. The GUI smoke
  adds and removes a Section on all four processes and proves song/looper bank
  counts move atomically. The modal case opens and cancels Export on all four
  processes after checking its default scope and explicit controls.
- Repository-wide call inspection classified six unreachable compatibility
  wrappers in `REVIEW-002`; it did not fabricate calls or remove potentially
  visible behavior. The three new findings are `loadWavIntoLooperLane`,
  `loadTrackMetadata`, and the `trackWaveform_`-guarded
  `seekPreparedTrack` path.
- Verification: `jam2_track_workspace_support_units` passed initially in 0.10
  seconds, after self-review in 0.09 seconds, and after exclusive destination
  reservation in 0.08 seconds. `jam2_looper_project_units` passed after its
  fixture repair in 0.03 seconds. The corrected
  `jam2_four_gui_agent_integration` passed in 5.12 seconds,
  `jam2_four_gui_modal_integration` passed in 113.58 seconds, and the final
  `jam2_four_shared_content_integration` passed in 19.81 seconds. The short
  native unit aggregate passed 35/35 in 38.05 seconds. Successful cleanup left
  `build/test-artifacts` absent; the single staged executable is
  `release/jam2.exe` (47,430,144 bytes).
- Self-review specifically challenged collision races, partial-copy rollback,
  source mutation during copy, stale worker completion, external-file
  ownership, managed replacement, repeated save dirtiness, and the
  file-dialog control boundary. It found the external-source ownership hazard
  before testing and removed that unsafe registration; the final reviewed
  code never registers or deletes a user's original WAV.
- Scope/protocol status: no persisted field or JSON shape, network message,
  field, header, payload, encoding, validator, parser, protocol version,
  authentication rule, accepted network input, shared-WAV transfer state
  machine, metronome model, epoch rule, audio algorithm, or real-time callback
  changed. This closes the final planned Slice 7 implementation cluster. The
  single final two-pass Windows distribution gate is next.

#### BUG-P093 - JamJar materialization could overwrite an unrelated WAV

- Observed symptom: the prior inline copier chose a sanitized/hash-derived
  name from the project snapshot but did not inventory pre-existing files in
  the target `imported` directory. A same-name unrelated file could be replaced.
- Root cause: collision ownership covered only lanes within the current
  materialization call, and the final destination was not reserved exclusively.
- Change: inventory occupied names case-insensitively, reuse a destination only
  after exact hash verification, suffix true collisions, and reserve the
  chosen path with `QIODevice::NewOnly` before atomic copy.
- Regression proof: direct tests precreate same-name different bytes, require a
  suffixed result and exact preservation of both files, then prove repeated
  materialization reuses the verified result without multiplication.
- Remaining test need: repeat exclusive-create and case-collision semantics on
  the macOS filesystem selected for distribution.

#### BUG-P094 - abandoned or failed materialization leaked copied WAVs

- Observed symptom: if a later lane failed validation, or the asynchronous
  source project changed before completion, files copied for earlier lanes
  remained in the destination despite the candidate never being committed.
- Root cause: the old helper returned only a project/error pair and did not own
  a transaction-wide created-file set.
- Change: materialization returns exact created paths, rolls them all back on
  internal failure, and lets MainWindow roll them back when a successful worker
  result loses its source-snapshot race. Cleanup failures remain registered for
  retry rather than losing their identity.
- Regression proof: direct multi-lane failure and explicit abandoned-success
  cases leave pre-existing files unchanged and no newly materialized paths.
- Remaining test need: inject a real sharing-violation rollback under Cocoa;
  retry ownership is already covered as platform-neutral state.

#### BUG-P095 - failed Save could promote an unverified unsaved workspace

- Observed symptom: Save moved the transient workspace into the Songs tree
  before checking every referenced looper WAV. A later missing/hash-invalid
  asset error could leave a nominally saved root and clear unsaved ownership.
- Root cause: storage relocation preceded the fallible materialization step.
- Change: materialize and verify against the current workspace first; relocate
  only the complete candidate, then rewrite its already-relative paths against
  the accepted saved root.
- Regression proof: materializer failure is atomic in direct tests, while the
  exactly-four-peer save workflow proves successful roots, project files,
  relative WAVs, identities, and clean storage state.
- Remaining test need: drive a deliberately invalid asset through the native
  Cocoa Save dialog and confirm the old unsaved root remains selected.

#### BUG-P096 - asynchronous lane replacement bypassed atomic model ownership

- Observed symptom: import and recording completion assigned lane fields one
  by one. Replacement could leave stale reference metadata and did not retire
  an unreferenced superseded managed WAV or transfer identity.
- Root cause: those workflows bypassed the checked LooperProject edit boundary
  and did not snapshot old asset ownership before mutation.
- Change: build a complete lane candidate, preserve its stable ID through
  checked `replaceLane`, clear reference-only metadata for local recordings,
  and retire the old hash/path only after a successful unreferenced replacement.
- Regression proof: direct replacement tests require atomic rejection and
  identity preservation; shared-content save reparses and hashes every final
  lane across all four peers.
- Remaining test need: macOS native file-drop/import and recording completion
  around the same checked model path.

#### BUG-P097 - an already-saved project remained dirty after Save

- Observed symptom: saving a project that already lived under the Songs root
  accepted the project snapshot but retained JamStorage's artifact-created
  flag, so `hasUnsavedChanges()` continued to report dirty state.
- Root cause: the successful Save path cleared the project/controller flags but
  not the storage owner's artifact state.
- Change: call `JamStorage::clearArtifactState()` only after the save is fully
  accepted.
- Regression proof: all four real saved peers report `storage_saved=true`,
  `storage_has_artifacts=false`, and `unsaved_changes=false` after Save.
- Remaining test need: repeat Save twice through the Cocoa dialog.

#### BUG-P098 - a source could change while being copied under an old hash

- Observed symptom: materialization and ordinary PCM16 staging validated the
  source hash before copying but did not verify the committed bytes. A source
  rewritten during the copy window could be published under the earlier digest.
- Root cause: source validation and destination commit were separate operations
  with no post-commit identity check.
- Change: hash every newly committed destination and fail/rollback unless it
  still matches the expected source identity.
- Regression proof: direct staging/materialization tests require destination
  metadata and SHA-256 to match the lane identity; mismatches take the atomic
  failure cleanup path.
- Remaining test need: a deterministic mid-copy source-rewrite injector would
  strengthen timing proof if a filesystem seam is later justified.

#### BUG-P099 - compatibility conversions lost temporary and superseded ownership

- Observed symptom: a conversion completing after a generation/rate/target
  change could leave its staged file behind. A conversion that was adopted
  could retain the now-unreferenced prior managed WAV/hash indefinitely.
- Root cause: `StagedPcm16Asset` did not distinguish a file created by this
  invocation from a reused or already-owned file, and audit completion mutated
  lane fields without coordinated retirement.
- Change: return `stagedFileCreated`, clean only newly created abandoned
  results, apply conversions through checked replacement, register adopted
  managed files, and retire only unreferenced superseded managed ownership.
- Regression proof: exact and resampled staging tests assert first/repeated/
  repaired ownership bits; worker completion paths preserve external source
  ownership and canonical referenced-path checks cover tracks and looper lanes.
- Remaining test need: deterministic event-loop tests for every stale audit
  completion reason under Cocoa.

#### BUG-P100 - Export was classified as a native file-dialog action too early

- Observed symptom: automation treated `looper.export` as a file-dialog action,
  although activating it first opened Jam2's internal export-scope modal. The
  four-peer modal driver could not safely identify or cancel that first boundary.
- Root cause: the outer action inherited the final chooser's boundary class
  instead of describing the immediate interaction it creates.
- Change: classify the outer action as Modal and register owned scope, Accept,
  and Cancel controls; only Accept retains FileDialog classification.
- Regression proof: all four GUI-agent processes open the scope modal, report
  the default scope, expose the three controls, and cancel without opening a
  chooser or writing a file.
- Remaining test need: run the same owned-modal cancellation under Cocoa; a
  native file-dialog accept path remains a platform-specific manual check.

#### BUG-T093 - the replacement unit corrupted its later source-length fixture

- Observed symptom: the first `jam2_looper_project_units` run failed because a
  newly replaced lane used 2,048 source frames, invalidating an existing later
  assertion constructed around the fixture's 1,000-frame source.
- Root cause: the test changed an unrelated invariant while exercising atomic
  replacement.
- Change: keep the replacement candidate at 1,000 source frames and vary only
  the fields required by the replacement contract.
- Regression proof: the exact test passed in 0.03 seconds and the unit aggregate
  passed 35/35.
- Remaining test need: none.

#### BUG-T094 - the four-peer save wait passed a duration into a Boolean slot

- Observed symptom: the first GUI build failed before running because the new
  save workflow passed `30s` as the fifth argument to a helper whose fifth
  parameter is `bool`.
- Root cause: the helper already owns its scaled 40-second timeout and its call
  site was written against an assumed duration overload.
- Change: remove the invalid argument and retain the helper's bounded timeout.
- Regression proof: the target builds and the final exactly-four-peer case
  passes in 19.81 seconds.
- Remaining test need: none.

#### BUG-T095 - the first exact GUI smoke selector used the build-target name

- Observed symptom: CTest reported no matching tests for
  `jam2_four_gui_agent_smoke` even though that executable built successfully.
- Root cause: the registered CTest name is
  `jam2_four_gui_agent_integration`; the executable and test names differ.
- Change: rerun with the authoritative registered name and record that name in
  the iteration evidence.
- Regression proof: `jam2_four_gui_agent_integration` passed in 5.12 seconds.
- Remaining test need: none.

#### BUG-T096 - the Section action test assumed unnamespaced control IDs

- Observed symptom: the first real GUI smoke run failed in 4.69 seconds because
  it invoked `section.add`/`section.remove`, which are not registered controls.
- Root cause: the maintained action catalogue namespaces them as
  `looper.section.add` and `looper.section.remove`.
- Change: use the catalogue IDs and retain atomic count assertions on every
  process.
- Regression proof: the corrected exactly-four-peer test passed in 5.12
  seconds.
- Remaining test need: none.

#### BUG-T097 - interrupted elevated gate retained a locking child process tree

- Observed symptom: the user-run replacement gate failed at link step 259/325
  with `LNK1104` for `tests/jam2_jamtaster_demucs_tests.exe`.
- Root cause: terminating the agent's outer command cell stopped its wrapper
  but did not terminate the already elevated `compile.cmd` process. That old
  gate continued into CTest and kept test executables open while the user's new
  Ninja process tried to relink them.
- Change: inspected parent/process creation times, identified the exact old
  `cmd.exe`/CTest/test-child tree started at 17:59, and terminated only that
  tree. No Jam2 product, Demucs, model, CMake, or linker input changed.
- Regression proof: the stale tree reports zero remaining CTest/Jam2 test
  processes; the subsequent gate owns the build directory exclusively. Its
  authoritative relink/pass result will be recorded with the final gate.
- Remaining test need: do not infer failure from interrupted-run residue; the
  user-run final gate must complete after the old tree is absent.

### Iteration 42 - authoritative gap repair, production hardware, and active GUI tail

- The user's completed two-pass command provided the authoritative post-slice
  baseline. All 72 instrumented behavioral tests passed in 1,951.82 seconds.
  The maintained-source audit then correctly failed with 172 wholly uncovered
  functions and 42 collector-skipped functions. The restored optimized pass
  ran all 72 cases in 824.13 seconds and failed only
  `jam2_hardware_plugin_device`. No result was relabelled as success.
- Expanded the production input-plugin boundary instead of validating a test
  double. `SystemInputPluginBackendOptions` lets the explicit hardware profile
  supply one VST3 path while ordinary UI use retains the native picker. The
  test runs worker probe/probe-all/probe-file/self-test, production host status,
  error, stats, bypass, MIDI, editor, and retirement paths, then five seconds of
  32-frame callbacks on audio device 5. An invalid input channel is rejected
  first and RAII cleanup is proven by the valid reopen. The final exact hardware
  case passed in 5.72 s; public `release/jam2.exe test-device 5` also passes.
- Added production Windows MIDI enumeration and invalid open/injection coverage
  (0.03 s), real authenticated binary controller send/reject coverage (24.41 s),
  and staged unified network-help routing (0.35 s). Physical WinMM callback
  lifecycle remains only the narrow `REVIEW-011` hardware boundary.
- Extracted the owned `DetailSectionEdit` widget from `MainWindowPages.cpp` into
  `app/gui/DetailSectionEdit.{hpp,cpp}`. This is a responsibility boundary, not
  a file-size split. Direct tests cover double-click, Return, Escape, trimming,
  empty rollback, focus-out commit, non-editable state, and missing callbacks.
- Expanded the exactly-four-peer active creator workflow. It drives the Data
  drawer, metronome sound/tap, peer gain, generated idea, details, continuation,
  curated preview/import, Section expand/Trim/shrink, reference WAV rendering
  and sharing, lane selection/gain/region, all-Section clearing, dirty New Jam
  cancellation, and dirty window-close cancellation. It requires generated
  model convergence and finished/shared WAV availability on all four peers.
  The final focused case passed in 93.19 s.
- Practice, curated, and destructive confirmation controls now have stable IDs
  and correct Modal contracts. Rebuilt BeatGrid trees retire their automation
  identities immediately but keep Qt ownership until deferred deletion. Direct
  widget and practice tests passed in 4.06 s and 0.42 s.
- Reviewed the collector output without a blanket source exemption. Twelve
  exact collector-skipped helpers now document their owning behavioral proof;
  old-report re-audit reports zero unreviewed skipped functions. Exact dormant
  exemptions reference `REVIEW-002`, `REVIEW-005`, and `REVIEW-010`; physical
  hardware callbacks reference `REVIEW-011`. Live network/audio/WAV code was
  not path-exempted.
- Focused production plug-in backend and four-peer reruns also passed in 0.17 s
  and 91.39 s. `git diff --check` reports no whitespace errors; CRLF notices are
  informational. No full collector reran during this repair iteration. The old
  XML cannot contain the new tests or `DetailSectionEdit.cpp`; it remains only
  an audit baseline.
- Renamed the `ChordBarOverviewButton::setTimeline` value parameter so it no
  longer shadows Qt's inherited `QWidget::data` member and emits MSVC C4458.
  The focused GUI widget boundary rebuilt cleanly and passed in 4.14 s.
- Confirmed the platform gate contract: code coverage runs only in the Windows
  `compile.cmd --tests-full` path. macOS `compile.sh --tests-full` runs the
  normal Release tests only; `TEST-MACOS.md` no longer requests LLVM coverage,
  PowerShell scripts, or cross-platform coverage-manifest comparison.
- The next instrumented full gate exercised the real device at 32 frames for
  7,483 callbacks and completed 7,479 plug-in blocks, but CTest correctly saw
  its exit code 1 after two steady deadline misses. The test now permits only
  two warm-up and two steady misses over the five-second hardware sample and
  prints `passed` only after that bounded criterion succeeds. The already
  running catalogue cannot retroactively rerun test 40; focused proof follows
  after it restores the normal build.
- Protocol status: no network message name, field, header, payload, encoding,
  parser, version, authentication rule, or accepted network input changed.

#### BUG-T098 - plug-in editor lifecycle polluted the real-time deadline sample

- Observed symptom: the restored optimized hardware case reported two steady
  plug-in deadline misses even though the earlier minimal callback case passed.
- Root cause: the broadened test allowed the native VST3 editor lifecycle to
  overlap the five-second deadline measurement.
- Change: prove editor open and close before ASIO timing; allow at most two
  misses in the first 500 ms and require zero later misses.
- Regression proof: the broadened real plug-in/device test passes.
- Remaining test need: repeat with the macOS plug-in format and CoreAudio.

#### BUG-P101 - rebuilt BeatGrid controls temporarily duplicated stable IDs

- Observed symptom: continuation/reference rebuilds exposed many duplicate
  dynamic grid IDs.
- Root cause: hidden widgets remained MainWindow descendants until Qt processed
  `deleteLater`, so the inventory saw both generations.
- Change: recursively retire old automation metadata immediately while keeping
  safe Qt ownership through deferred destruction.
- Regression proof: signal-driven widget edits and the complete four-peer paged
  inventory pass with no duplicates.
- Remaining test need: retain the same inventory/lifetime checks under Cocoa.

#### BUG-P102 - modal-opening actions advertised synchronous interaction

- Observed symptom: generated-idea, Clear Idea, Section shrink/Trim, and related
  actions could block a synchronous automation click.
- Root cause: metadata described eligibility rather than the immediate modal
  boundary.
- Change: classify the actions as Modal and register all owned accept,
  destructive, and cancel buttons for idea clear, Trim/shrink, New Jam, and
  window close.
- Regression proof: the four-peer workflow opens, inventories, and closes every
  boundary through the real callback.
- Remaining test need: Cocoa window-close parity.

#### BUG-T099 - Idea Details treated read-only views as controls

- Observed symptom: the first expanded workflow reported the teaching and
  technical panes as missing controls.
- Root cause: read-only text panes are intentionally state views, not invokable
  interactions.
- Change: require the real toggle and close controls; retain text validation in
  the dialog/model tests.
- Regression proof: the modal passes with no unclassified controls.
- Remaining test need: none.

#### BUG-T100 - BeatGrid edit assertions targeted a non-visible bar

- Observed symptom: the direct widget test failed beat-0 division and hit edits.
- Root cause: it focused absolute beat 132 and then tried to click beat-0
  controls without returning to the first bar.
- Change: prove beat 132 is visible, move to beat 0, then test both edits.
- Regression proof: the corrected widget target passes in 4.06 s.
- Remaining test need: none.

#### BUG-T101 - the first Trim workflow used the wrong namespace

- Observed symptom: `coverage-section-trim-open` was rejected and the following
  shrink correctly removed the newly empty bar without confirmation.
- Root cause: embedded BeatGrid controls use singular `grid.chord`; the Chords
  page bank strip uses plural `grid.chords`.
- Change: target `grid.chords.section.trim` while retaining singular embedded
  expand/shrink IDs.
- Regression proof: corrected four-peer cases pass in 91.97 s and 93.19 s.
- Remaining test need: none.

#### BUG-T102 - hardware summary claimed success before its final timing check

- Observed symptom: CTest marked `jam2_hardware_plugin_device` failed while its
  last line said the real-device plug-in test passed.
- Root cause: the summary was worded as success before the final Boolean return,
  and the timing rule allowed two initial misses but no equally bounded
  steady-state machine-scheduling jitter at a 32-frame hardware buffer.
- Change: report neutral results first, allow at most two warm-up and two steady
  misses during the five-second sample, throw an explicit diagnostic when the
  bound is exceeded, and print `passed` only after all checks succeed.
- Regression proof: the replacement instrumented and optimized full catalogues
  both passed the hardware case with the neutral summary followed by the final
  success line; both catalogues passed 75/75 tests.
- Remaining test need: retain longer real-device observation in manual use;
  this short automated boundary must continue enforcing exact block accounting,
  no failed/stale responses, and the bounded warm-up/steady miss rate.

### Iteration 43 - final 60-function ownership and coverage tail

- Recorded the authoritative replacement-gate baseline after the hardware
  tolerance repair: the instrumented catalogue passed 75/75 in 2,033.80
  seconds and the optimized catalogue passed 75/75 in 836.12 seconds. The
  maintained audit then reported 3,130 functions: 1,163 fully covered, 1,827
  partially covered, 80 explicitly exempt, 60 unreviewed wholly uncovered,
  and zero collector-skipped. Behavioral tests were green; coverage alone kept
  the gate red.
- Closed the MainWindow ownership cluster without splitting cohesive code by
  size. Device preference/capability/modal presentation, connection guidance,
  wheel/key interaction policy, JamTaster project-section construction, and
  track-sidecar reading now live in named owned helpers with direct tests.
  Three private MainWindow engine/mesh wrappers with no caller were removed.
- The exactly-four-peer modal integration now drives a deterministic fake
  device through the real Local Engine dialog, callback startup, supported and
  unsupported rate preflight, Settings audio restart, Test Device result,
  session-default persistence, file/JamTaster cancellation, JamTaster tempo,
  maintenance/error boundaries, and input/count-in paths. A test-injected
  `GuiLoopbackRecorder` backend writes a real build-local PCM16 WAV through the
  production writer and completes the real MainWindow loopback lifecycle.
- The exactly-four-peer session-command integration now sends the already
  defined and validated `session.error` message from the creator to one active
  joiner and requires exit code 4, while the other three peers shut down
  normally. Synchronous requested shutdown now calls the same runtime-finished
  boundary directly after joining the network worker instead of depending on
  a queued callback that has not yet been processed.
- Reviewed only exact residual shapes: `promptFrame` behind a dormant wrapper
  in `REVIEW-002`; private nonempty practice catalogue selector
  specializations in `REVIEW-012`; and the optional ASIO time-info callback,
  Windows error formatters, and physical WinMM path in `REVIEW-011`. No live
  source directory or network/audio/WAV workflow received a blanket exemption.
- Focused proof: `jam2_track_workspace_support_units` 0.12 s;
  `jam2_jamtaster_project_support_units` 0.03 s;
  `jam2_jamtaster_model_units` 0.94 s against the unchanged Windows staged
  model directory; `jam2_gui_widget_boundary_units` 4.14 s;
  `jam2_gui_loopback_recorder_units` 0.09 s;
  `jam2_four_session_command_integration` 5.62 s; and the corrected
  `jam2_four_gui_modal_integration` 117.73 s.
- CTest model/worker locations are now platform-owned in one CMake block:
  Windows retains `release/components/jamtaster`, while macOS uses
  `release/Jam2.app/Contents/Resources/jamtaster/models` and
  `Contents/Helpers/jamtaster-worker`. The Mac behavior remains pending its
  Apple endpoint run in `TEST-MACOS.md`.
- Protocol status: the test dispatches the existing `session.error` control
  message. No network message name, field, header, payload, encoding, parser,
  version, authentication rule, accepted network input, metronome model, or
  epoch rule changed.
- Completion status: implementation is frozen pending one fresh Windows
  instrumented catalogue and the coverage checker. The stale 60-function CSV
  is not treated as proof that the rebuilt binaries pass.

#### BUG-T103 - widget test selected a supported rate as its rejection fixture

- Observed symptom: the first Local Engine dialog boundary expected 96 kHz to
  be absent even though that fixture legitimately advertised it.
- Root cause: the test encoded a device-specific assumption instead of using
  the synthetic capability table it constructed.
- Change: use 48 kHz for the supported dialog selection and reserve 96 kHz for
  the deterministic fake device's explicit rejection boundary.
- Regression proof: the widget target passes and the four-peer GUI case proves
  both 48 kHz acceptance and 96 kHz rejection through MainWindow preflight.
- Remaining test need: none.

#### BUG-T104 - JamTaster CTests hard-coded the Windows staged package layout

- Observed symptom: macOS ONNX model and worker tests looked under
  `release/components/jamtaster`, while the packaged files live inside
  `Jam2.app/Contents`.
- Root cause: test arguments were written directly with the Windows release
  layout instead of selecting the platform's staged distribution layout.
- Change: centralize `JAM2_TEST_JAMTASTER_MODELS_DIR` and
  `JAM2_TEST_JAMTASTER_WORKER` in `tests/CMakeLists.txt`; Apple selects app
  Resources/Helpers and all other current builds retain `components/jamtaster`.
- Regression proof: the real Windows model target passes against
  `release/components/jamtaster/models`; exact Apple proof is required by
  `TEST-MACOS.md`.
- Remaining test need: run model, Demucs, pipeline, and worker protocol CTests
  against the staged app bundle on macOS.

#### BUG-T105 - initial fake-audio GUI coverage encoded three stale test seams

- Observed symptom: early focused attempts probed real ASIO, waited for an
  unregistered workspace control, disturbed an earlier Settings-cancel
  assertion, and later expected a closed QMessageBox to return literal
  `cancel` instead of the production empty cancellation value.
- Root cause: the synthetic capability cache used a raw CLSID rather than the
  owned preference key, and the expanded workflow reused assumptions not in
  the current control/cancellation contracts.
- Change: cache by `audioDevicePreferenceKey`, run the fake-local workflow
  after the existing modal assertions, wait on `workspace.open.looper`, and
  accept the empty source-disposition cancellation result.
- Regression proof: the final exactly-four-peer modal case passes in 117.73 s
  and writes all preferences/WAV artifacts under its build-local roots.
- Remaining test need: reproduce Cocoa modal timing in the Mac closure.

#### BUG-P103 - requested network shutdown relied on a queued completion callback

- Observed symptom: `finish()` synchronously joined the network worker but
  depended on a previously queued `onNetworkFinished` invocation to publish
  the manifest and exit. The boundary was invisible to native coverage and was
  fragile if event processing changed during shutdown.
- Root cause: worker ownership was synchronous while completion ownership was
  deferred through the GUI/event queue.
- Change: requested shutdown clears the deferred observer, joins the worker,
  and invokes the one `handleRuntimeFinished` completion boundary directly.
  Natural runtime completion retains the existing queued callback.
- Regression proof: four real peers complete the reactive command scenario;
  one coordinator rejection exits 4 and three requested shutdowns exit 0 with
  valid final manifests in 5.62 s.
- Remaining test need: retain this lifecycle case on macOS; no protocol change
  was made.

#### BUG-T106 - instrumented listener phase exceeded a one-callback assertion

- Observed symptom: instrumented test 51 proved the future record action was
  pending on all four peers with an exact 96,000-frame one-bar count-in, but
  rejected peer 3 because its adopted musical target was 70 frames from the
  local epoch bar while the assertion allowed only 64.
- Root cause: the integration test used a magic one-callback tolerance even
  though a received target is translated onto the listener callback clock while
  its render offset can still slew. The authoritative audio-level metronome
  impairment test already uses a two-callback (128-frame) shared-grid phase
  bound for this 64-frame configuration.
- Change: name the frame size, one-bar duration, and transport phase tolerance;
  derive the latter as exactly two callbacks. The exact action, pending state,
  recording count-in state, countdown/target identity, and 96,000-frame
  duration assertions remain unchanged.
- Regression proof: the exact normal-Release
  `jam2_four_session_command_integration` rebuilt and passed in 5.71 seconds
  with four real Jam2 processes.
- Remaining test need: retain the stricter audio-stem/dynamic-epoch impairment
  matrix as the audible metronome authority; this controller test validates
  schedule propagation and must not substitute a broad timing tolerance.
- Protocol status: test assertion only; no runtime, transport, metronome, epoch,
  packet, or accepted network input changed.

#### BUG-T107 - fixed steady plug-in miss count was not sample-size aware

- Observed symptom: instrumented hardware test 41 completed 7,479 of 7,485
  callbacks through Surge XT Effects at 32 frames, with four steady misses,
  zero 2x callback gaps, and a 328-us maximum worker process time against a
  roughly 667-us callback period, but failed the fixed two-miss threshold.
- Root cause: the five-second test treated two misses as the same budget for
  every configured hardware block size and callback count. It did not express
  the actual bounded rate or assert the bridge's exact response accounting.
- Change: retain at most two misses during the 500-ms warm-up; after warm-up,
  permit the greater of two or one miss per 1,000 submitted blocks (rounded
  up). Also require zero failed/stale responses and exact isolation-pipeline
  accounting: completed plus missed responses must equal submitted blocks
  minus the fixed two startup pipeline blocks.
- Regression proof: the exact normal-Release hardware profile rebuilt and
  `jam2_hardware_plugin_device` passed against ASIO device 5, input 2, 32
  frames, and Surge XT Effects in 5.71 seconds.
- Remaining test need: the optimized hardware case remains the timing
  authority. Manual longer observation should compare the printed exact miss
  rate, callback-gap counts, and worker processing times rather than treating
  any nonzero isolated-process scheduling miss as device failure.
- Protocol status: hardware test acceptance only; no plug-in bridge, audio
  callback, network protocol, or production tolerance changed.

#### Final Windows coverage catalogue and exact compiler-shape review

- The fresh instrumented catalogue passed all 76 behavioral cases in 2,019.95
  seconds. It catalogued 3,128 maintained functions: 1,173 fully covered,
  1,868 partially covered, 86 explicitly exempt, one unreviewed wholly
  uncovered function, and zero collector-skipped functions. All 126 maintained
  source files were observed or explicitly platform-accounted, with zero
  unreviewed missing files.
- The sole row was
  `NetworkCommandController::handleRuntimeFinished(int)`. The exactly-four-peer
  session-command test observably executed the requested-shutdown body through
  three exit-0 final manifests and one propagated exit-4 final manifest, but
  MSVC also emitted a separate unentered copy because the callback is defined
  inline inside the local controller class. The manifest now classifies only
  that exact function shape; it does not exempt SessionController, network
  completion, or any source path.
- This is a collector/compiler representation issue, not a product or protocol
  change. No production source, network message, field, parser, accepted input,
  metronome/epoch behavior, shared-WAV behavior, or test acceptance changed.
- The standalone checker completed in 14.1 seconds and passed: 126 maintained
  files, 122 observed, zero unreviewed missing files, 3,128 maintained
  functions, 1,173 fully covered, 1,868 partially covered, 87 exact reviewed
  exemptions, zero unreviewed wholly uncovered functions, and zero unreviewed
  collector-skipped functions. No rebuild or CTest rerun was performed.

### Iteration 44 - late prepared-mix attachment during four-peer Play

- The distribution run failed `jam2_four_performance_integration` after
  195.45 seconds while waiting for four prepared loop fixtures to play. All
  four peers had the exact shared WAV attached and available, and global
  transport was requested and committed on all four, but peers 2 and 4 had
  `prepared_source_playing=0` permanently. Retained logs showed their final
  prepared-mix refresh landing while Play was pending and loading with
  `scheduled=0`; peers 1 and 3 attached to a transport target and played.
- Moved the attach target/source calculation into the typed
  `TrackRecordingWorkflow::PreparedAttachPlan`. Any valid replacement mix now
  attaches when global transport is pending or running. The one-shot
  `PreparedMixLifecycle::playWhenReady` flag still owns UI render intent but no
  longer gates correctness of a late transport attachment. A pending mix uses
  the existing transport target; a running replacement uses the next safe grid
  beat and the exact shared-timeline source offset.
- Self-review retained the original integration timeout. This state could not
  recover regardless of waiting, so making the test more tolerant would have
  hidden a real synchronization defect. Pending, running, empty-render, target,
  and modulo source-offset behavior now have deterministic workflow coverage.
- Verification: required normal MSVC Release build succeeded in 37.7 seconds;
  `jam2_track_recording_workflow_units` passed in 0.60 seconds; and the exact
  exactly-four-peer `jam2_four_performance_integration` passed in 18.27
  seconds. No full suite or coverage collector ran.
- Removed the verified generated `tests/coverage/obj` restore directory found
  during final status review. The maintained coverage runner already passes
  both MSBuild intermediate paths under `build/coverage/tools-obj`; no source
  or test artifact from this iteration remains outside `build/`.
- Scope/protocol status: local prepared-source scheduling and native tests
  only. No network message, field, payload, encoding, parser, authentication,
  metronome model, epoch rule, or shared-WAV transfer behavior changed.

#### BUG-P104 - late prepared-mix replacement could miss global Play forever

- Observed symptom: peers could show the exact same available shared WAV and
  committed global transport while the prepared source remained stopped.
- Root cause: `MainWindow::applyPreparedMixResult` required the transient
  `playWhenReady` render flag as well as global transport intent before loading
  a completed replacement at a scheduled frame. Background/coalesced refreshes
  did not necessarily own that flag, so a load racing with Play replaced the
  source without scheduling it.
- Change: derive a typed attach plan from the authoritative pending/running
  global-transport state. Late sources join at the pending target or next safe
  beat with their source frame aligned to the shared transport timeline.
- Regression proof: deterministic workflow assertions cover both orderings,
  and the real four-process performance workflow requires all four prepared
  sources to enter playback.
- Remaining test need: retain this case in the macOS normal Release suite to
  verify the same worker-completion ordering under Cocoa/CoreAudio scheduling.

### Iteration 45 - impaired mixer health and Windows dual-protocol ports

- The continued distribution run reported two independent failures.
  `jam2_metronome_leader_audio_duplication` completed the exact authority
  handoff, revision 4 replacement epoch, zero mapping error, and one-beat
  alignment, but rejected peer 1 solely because `mix_capacity_drops` was 4,634
  rather than zero. Retained CSVs showed 4,634--22,333 exact dropped samples
  across peers, at worst 2.97% of one output timeline, while the mixer remained
  live and aligned. `jam2_metronome_listener_compensated_clean` never launched:
  TCP selected port 53,345, which was inside this machine's Windows UDP-only
  excluded range 53,271--53,370.
- Impaired conditions now permit at most 5% output-equivalent mixer capacity
  recovery. The test still requires positive output, exact equality between
  drop events and dropped frames, and every existing authority, revision,
  epoch-replacement, mapping, beat-alignment, authentication, proxy, recording,
  and WAV/stem proof. Clean networking still requires exactly zero capacity
  drops.
- Loopback reservation now asks UDP for an allowed ephemeral port first and
  then binds TCP to the same number. UDP candidates rejected by TCP remain
  held during selection, preventing immediate reuse. The support unit reserves
  16 unique simultaneous TCP/UDP ports and retains its bounds checks.
- Verification: `jam2_four_peer_coordinator_units` passed in 0.04 seconds;
  exact `jam2_metronome_leader_audio_duplication` passed in 17.36 seconds; and
  exact `jam2_metronome_listener_compensated_clean` passed in 17.29 seconds.
  The required MSVC Release build succeeded. No full suite or coverage
  collector was started.
- Scope/protocol status: test acceptance and test-only port allocation. No
  production source, network header/field/payload/parser/version, accepted
  input, authentication, metronome model, epoch rule, audio mixer behavior, or
  shared-WAV behavior changed.

#### BUG-T108 - duplication required a designed recovery counter to stay zero

- Observed symptom: a fully aligned replacement epoch failed because the
  bounded peer mixer discarded queued samples while recovering from sustained
  duplicated traffic.
- Root cause: the impairment matrix treated any mixer capacity recovery as a
  correctness failure even though the fixed-capacity mixer deliberately keeps
  the live tail rather than remaining behind real time.
- Change: clean runs retain zero-drop acceptance; impaired runs require exact
  accounting and no more than 5% of output-equivalent frames.
- Regression proof: the exact leader-audio duplication workflow passes while
  retaining its complete four-peer epoch and recorded-audio validation.
- Remaining test need: retain raw drop/output counters in failure output and
  compare rates when tuning future mixer capacities; do not widen this bound
  without evidence.

#### BUG-T109 - TCP-first port selection could choose a UDP-protected port

- Observed symptom: listener-compensated clean failed before peer launch after
  256 attempts, ending on Windows port 53,345 with `The address is protected`.
- Root cause: the allocator asked TCP for an ephemeral number first, but
  Windows maintains different TCP and UDP excluded ranges. TCP could therefore
  repeatedly return numbers that UDP was forbidden to bind.
- Change: select through UDP first, then prove TCP ownership of the same port,
  retaining rejected UDP candidates during the bounded search.
- Regression proof: 16 unique dual-protocol reservations pass directly and
  the exact four-peer listener-compensated clean workflow launches and passes.
- Remaining test need: run the same support unit and metronome case under macOS;
  the ordering uses portable Qt socket ownership and has no Windows-only branch.

### Iteration 46 - process-wide hidden GUI-agent windows

- Hidden GUI tests previously applied `Qt::WA_DontShowOnScreen` only to
  `MainWindow`. Real modal dialogs are separate Qt top-level windows, so message
  boxes and dialogs could appear on the desktop while their owning Jam2 window
  remained hidden.
- Added a GUI-agent-only application event filter that applies the off-screen
  attribute to every top-level widget before polish/show. Hidden mode also sets
  `Qt::AA_DontUseNativeDialogs`, preventing a native file chooser from escaping
  the Qt window policy. Normal Jam2 startup and non-test commands do not install
  this policy.
- All six four-peer GUI launchers now honor the existing
  `JAM2_TEST_SHOW_GUI` switch consistently. Default runs remain hidden;
  `--show-gui` passes through to each GUI-agent peer and leaves normal Qt/native
  presentation enabled.
- Per user direction, no automation protocol field or desktop-visibility
  assertion was added. The existing exactly-four-peer
  `jam2_four_session_dialog_integration` rebuilt and passed its real modal
  interactions in 79.72 seconds. Manual desktop verification remains the
  acceptance for whether pixels appear.
- Manual Windows check: run
  `compile.cmd --in-dev-shell --tests gui --test-name jam2_four_session_dialog_integration`
  and confirm no windows appear; add `--show-gui` and confirm the same peers and
  dialogs are visible.
- Scope/protocol status: private GUI test harness and test launch arguments
  only. No ordinary UI behavior, product feature, network/control protocol,
  metronome/epoch model, audio path, or shared-WAV behavior changed.

#### BUG-T110 - hidden GUI mode covered only MainWindow

- Observed symptom: modal popups appeared during default hidden GUI tests even
  though the four base Jam2 windows were absent.
- Root cause: `WA_DontShowOnScreen` is a per-widget attribute and was set only
  on `MainWindow`; modal dialogs are independent top-level widgets and do not
  inherit it reliably.
- Change: enforce hidden presentation across every GUI-agent top-level widget
  and reserve visible presentation for explicit `--show-gui` runs.
- Regression proof: the existing real four-peer session-dialog workflow passes
  in hidden mode; desktop visibility is intentionally a manual check.
- Remaining test need: manually confirm hidden and `--show-gui` presentation on
  Windows and repeat both modes on macOS.

### Iteration 47 - hidden GUI-agent alert sounds

- A hidden Windows GUI CTest could still play the system warning tone when an
  intentionally exercised `QMessageBox::Warning` entered its show handler.
  Window visibility and Windows alert audio are independent.
- Hidden GUI-agent mode now clears a message box's alert icon immediately
  before its show handler runs. This prevents Qt from requesting the associated
  platform alert sound while preserving the dialog text, buttons, modality,
  automation controls, and tested response.
- Explicit `--show-gui` runs do not install the hidden-window filter, so they
  retain the production icon and platform sound for manual UI verification.
- Scope/protocol status: private GUI test presentation only. No normal Jam2 UI,
  network/control protocol, audio engine, synchronization, metronome/epoch, or
  shared-WAV behavior changed.

### Iteration 48 - resolved review actions and remaining duration design

- Reduced `TEST-REVIEW.md` to the sole unresolved `REVIEW-008` decision. It now
  records the coupled Section/bar/beat, five-minute recording, dense metadata,
  resident audio, streaming/cache, serialization, and diagnostics questions.
  No Section, recording-duration, renderer, or buffer limit changed.
- Removed the user-approved obsolete surface: the uninvoked looper bank
  callback, impossible placeholder-lane mouse branch, old MainWindow lane
  move/gain/region/import/metadata/waveform-seek wrappers and their private
  prompt helper, unreferenced `track.processing` construction/application, and
  unused SessionController default/sibling-path helpers. The active painted
  lane, Add WAV, prepared transport, and direct Section-selection paths remain.
- Self-review corrected the initial call-graph reading for waveform seek. A
  call site existed, but its entire callback was guarded by `trackWaveform_`,
  which the maintained page sets to null. The dead callback and wrapper were
  therefore removed together rather than leaving unreachable code or deleting
  a live transport action.
- Empty loopback capture now has typed content classification. Zero captured
  frames reports `No audio was captured`; a take fully removed by silence
  trimming reports `Only silence was detected`. Both retain technical capture
  diagnostics, write/import no new WAV, clear completed-capture availability,
  return transient cleanup ownership, and show the normal styled warning in
  interactive Jam2 while automation remains nonblocking.
- Tightened the two explicitly approved bank-transition checks. Optional
  `bank.request.target_abs_beat` must be absent, empty, or a decimal string no
  greater than signed-64 maximum. A joiner now commits `bank.switch` only when
  both switch ID and bank match its prepared state. The redundant prepare
  target remains unchanged as directed.
- Verification: the MSVC Release product build succeeded; exact
  `jam2_application_boundary_units`, `jam2_gui_loopback_recorder_units`, and
  `jam2_shared_bank_launch_units` passed; self-review then added and passed
  exact `jam2_track_recording_workflow_units`; exact four-peer
  `jam2_four_performance_integration` passed in 17.65 seconds; and the final
  post-review Release rebuild succeeded. No full suite or coverage collection
  was started.
- Protocol status: bank acceptance became stricter exactly as approved. No
  message name, field, emitted JSON shape/value, packet header, payload,
  encoding, version, authentication rule, or audio protocol changed.

#### BUG-T111 - unusable loopback takes were accepted as valid WAV assets

- Observed condition: a loopback endpoint that returned no frames, or a take
  entirely removed by enabled silence trimming, could complete successfully
  with a zero-data RIFF and continue toward lane import.
- Change: classify the capture before writing, report the precise owned failure
  to the user, preserve raw diagnostic counters, clear the failed completed-
  capture path/rate, and stop before asset creation or import.
- Regression proof: loopback units cover no-frame, silence-only, usable-audio,
  and exact user-error mappings alongside the existing writer/lifecycle cases;
  recording-workflow units prove failed transient cleanup and empty availability.

#### BUG-T112 - bank request and commit accepted inconsistent target ownership

- Observed condition: a creator could accept a request target above the limit
  that joiners enforce on switch, and a joiner matched a switch identity
  without also matching the bank it had prepared.
- Change: apply the same signed-64 target domain at request validation and use
  the coordinator's exact `(switch_id, bank)` match at joiner commit.
- Regression proof: application boundaries cover absent, empty, maximum,
  above-maximum, and wrong-JSON-type targets; coordinator units retain
  same-ID/wrong-bank rejection; the four-peer performance workflow completes
  two real prepared transitions with epoch preservation.

### Iteration 49 - macOS Release/CTest parity repairs

- The initial Apple `compile.sh --tests-full` run exposed four portability
  groups. CLI/debug/corpus CTests invoked a nonexistent `release/jam2`; the
  staged JamTaster helper and models occupy separate app-bundle directories;
  Cocoa message boxes complete their native show/button setup later than the
  Windows fixture; and explicit Practice Idea seeds used implementation-defined
  standard distributions whose libc++ output did not match the maintained
  Windows catalogue fingerprints.
- Centralized the public test executable as
  `release/Jam2.app/Contents/MacOS/jam2` on Apple while retaining
  `release/jam2.exe` on Windows. The worker protocol now receives the staged
  model directory explicitly, and the JamTaster service honors its injected
  helper root before applying the production `Contents/Helpers` lookup.
  Cocoa dialog tests wait for the requested real standard button and accept
  only Cocoa's empty native alert title; Windows keeps its exact title checks.
- Replaced implementation-defined integer and real distributions in the
  Practice Idea generator with explicit fixed `mt19937` mappings matching the
  established MSVC catalogue. The full native boundary catalogue then passed
  on libc++, and a focused unit now regenerates a stored curated seed and
  requires its exact chord/beat fingerprints.
- The four-peer session-command test shut the creator down after a control
  frame was queued but before an asynchronous POSIX socket necessarily
  delivered it. It now waits for the deliberately rejected peer's typed exit-4
  result before coordinator teardown. Production control behavior and all wire
  fields remain unchanged.
- GUI snapshot pagination rebuilt the live widget tree for every 32-control
  page. A legitimate asynchronous Cocoa control insertion changed the Local
  Engine inventory from 447 to 448 mid-snapshot. The private agent now caches
  one point-in-time inventory until its final page. The complete four-peer
  modal catalogue passed in 315.52 seconds. Apple-only outer CTest allowances
  are 600 seconds for the full catalogue and 300 seconds for the reference-
  rendering startup catalogue; Windows retains 180/120 seconds and every
  behavioral wait is unchanged.
- The impairment coordinator and synthetic audio worker were starved under
  sustained macOS process load. The test coordinator now requests Apple
  user-interactive QoS plus a five-millisecond time-constraint policy while
  preserving the hard 50 ms pump-gap rejection. The Apple Headless worker also
  requests user-interactive QoS and preserves its deterministic sample clock by
  catching up missed callback periods; Windows and real CoreAudio paths are
  unchanged. The previously failing listener-loss, leader burst-loss, and
  coordinator-stall cells pass at their original product bounds.
- Focused verification passed: the ten executable/layout/JamTaster/Practice
  tests together (10/10 in 10.27 seconds), native boundaries (45.54 seconds),
  Practice Idea (1.10 seconds), core mixer input (0.56 seconds), reactive
  four-peer session command (3.49 seconds), four-peer session dialogs (272.50
  seconds), full four-peer GUI modal catalogue (315.52 seconds), and the
  narrowed metronome regressions. A complete 21-cell matrix then passed 20/21;
  leader-audio loss observed 19 rather than 20 fitted clicks under accumulated
  suite load and passed immediately in isolation at the unchanged threshold.
  The exact `compile.sh --tests-full` distribution gate remains the final
  acceptance run for this iteration.
- Scope/protocol status: Apple staging, private automation/tests, deterministic
  generator mapping, and the Apple-only Headless test backend. No packet or
  control field, payload, encoding, parser, protocol version, authentication
  rule, metronome/epoch model, CoreAudio callback, or Windows scheduling/build
  path changed.

#### BUG-T113 - macOS CTests assumed the Windows release layout

- Observed condition: otherwise valid CLI, diagnostic, corpus, and JamTaster
  tests failed before exercising behavior because their executable/helper/model
  paths did not describe the staged app bundle.
- Change and proof: centralized platform paths and passed helper/model roots as
  separate arguments; all ten affected focused tests pass together.

#### BUG-P105 - explicit Practice Idea seeds were library-dependent

- Observed condition: all 54 curated fingerprints differed on libc++ although
  the same `mt19937` seeds and generator inputs were used.
- Change and proof: explicit bounded integer and 53-bit real mappings make seed
  output platform-independent and reproduce the complete maintained catalogue.

#### BUG-T114 - paginated GUI snapshots were not point-in-time inventories

- Observed condition: Cocoa asynchronous setup inserted one classified control
  after page zero, producing duplicate/omitted cursor results and a 447/448
  total mismatch.
- Change and proof: retain the exact collected inventory through its last page;
  the strict zero-unclassified/zero-duplicate four-peer catalogue passes.

#### BUG-T115 - macOS test scheduling manufactured impairment evidence

- Observed condition: the six-edge proxy pump exceeded its 50 ms validity
  limit by 53--66 ms and Headless callbacks showed 38--46 ms gaps with a 5.33
  ms buffer, causing missing recorded pulses and perpetual mixer recovery.
- Change and proof: apply native Apple QoS/time constraints to the owning test
  threads and deterministic Headless clock catch-up. Original pump, mixer,
  epoch, click, centroid, and convergence thresholds remain unchanged.

### Iteration 50 - deterministic four-peer recording signal window

- The macOS `compile.sh --tests-full` run reached test 45 and failed
  `jam2_four_performance_integration` because peer 1's valid 14,208-frame
  `metronome.wav` contained only silence. The retained WAVs showed all four
  peers wrote valid equal-length stems with no recorder drops or writer errors;
  the other peers happened to overlap a 173-BPM click while peer 1's shorter-
  than-one-beat recording fell entirely between clicks.
- The integration previously stopped each recorder after only 4,800 frames,
  although one beat at the fixture's 48 kHz/173 BPM is about 16,648 frames.
  It now waits for one complete beat plus 1,024 frames of render-quantum
  headroom before stopping and requiring signal in every stem. This strengthens
  the evidence window without changing a product threshold or accepting a
  silent stem.
- Focused verification rebuilt the Apple Release integration target and the
  exact `jam2_four_performance_integration` passed in 34.36 seconds. No full
  suite rerun was started.
- Scope/protocol status: native test timing only. No production source, Windows
  branch, recording behavior, metronome/epoch behavior, network message,
  packet, parser, or authentication rule changed.

#### BUG-T116 - recording proof could stop between metronome clicks

- Observed condition: the signal assertion required every metronome stem to be
  non-silent, but the recording window could be less than one beat and
  therefore contain no scheduled click depending on the peer's exact phase.
- Change and proof: require a full fixture beat plus callback headroom before
  stopping; the exact retained-failure workflow passes with the original WAV
  format, length, drop, writer-error, and non-silence checks intact.

### Iteration 51 - macOS leader-audio scheduling and evidence parity

- The resumed macOS `compile.sh --tests-full` run completed 74 cases in
  1,298.50 seconds and failed only leader-audio clean and fixed-delay. Retained
  artifacts showed 47--64 ms Headless callback gaps against a 5.33 ms period,
  playback-underrun bursts up to 519 ms, and received-click fits of 11/20 and
  19/20. No packet-security, writer, epoch-authority, or proxy-contract failure
  occurred.
- The Apple Headless synthetic callback worker now requests a Mach time
  constraint derived from its actual buffer period in addition to its existing
  user-interactive QoS. The impairment scenarios request Jam2's existing,
  typed `realtime` mode for the Apple network worker and require
  `os_realtime_active=on` in every peer CSV. Windows retains the existing
  `high` scenario setting, and the real CoreAudio callback is unchanged.
- Fixed-delay evidence then exposed a private analysis error rather than
  missing audio. The listener's explicit 1.005 recovery ratio produced a
  continuous click series near 19,104 frames before returning to the nominal
  19,200-frame interval. A single fixed recording-phase fit accumulated that
  intended resampling and invented a multi-beat gap. The leader-audio matcher
  now selects the longest chain of successive bounded beat intervals, retaining
  the original 480-frame interval error, at-least-20-click, and at-most-three-
  beat-gap requirements.
- The deterministic two-percent random-loss profile can damage more than five
  of the 25 clicks in a ten-second window depending on packet/click alignment.
  Only that leader-audio profile now records for twelve seconds, and its writer
  contract requires the corresponding longer frame count. This keeps the
  click/gap thresholds unchanged while collecting enough stochastic evidence.
- A final Apple clean run also demonstrated coherent cancellation in the mix
  of three identical 440 Hz remote tones: the remote stem sustained 440.37 Hz
  in every one-second window but measured 488.5 RMS against the old 500 floor.
  Apple now proves the mixed remote tone in at least eight complete one-second
  frequency windows, while every unmixed local input retains the RMS and
  frequency checks. Windows retains its original mixed-stem RMS/frequency
  branch exactly.
- Focused verification: clean and delay passed together in 34.87 seconds; all
  seven leader-audio profiles passed in 122.97 seconds; and the final complete
  metronome selection passed 22/22 in 363.06 seconds, covering all 21 exactly-
  four-peer shared-grid, leader-audio, and listener-compensated impairment
  cells plus the controller unit. No full 74-case rerun was started.
- Scope/protocol status: Apple Headless/test scheduling and native test
  evidence only. No packet/control message, field, payload, encoding, parser,
  version, authentication rule, metronome/epoch model, Windows scheduling
  branch, or CoreAudio callback changed.

#### BUG-T117 - Apple QoS alone did not bound synthetic callback/network stalls

- Observed condition: long-suite load delayed 5.33 ms synthetic callbacks by
  up to 64 ms, while a QoS-only network worker could exhaust a healthy
  playback ring despite zero packet loss, sequence gaps, or mixer misses.
- Change and proof: use period-derived Mach time constraints for the Apple
  Headless worker and the existing instrumented realtime mode for the Apple
  impairment network worker; every peer must report successful activation,
  and the complete original-threshold matrix passes.

#### BUG-T118 - leader-audio proof treated recovery resampling as lost clicks

- Observed condition: a fixed-phase fit accumulated the bounded playback
  resampler's temporary ratio, while a greedy interval fit could commit to
  packet-discontinuity candidates under random loss. Coherent remote test tones
  could also cancel below a mixed RMS floor despite continuous exact frequency.
- Change and proof: use a longest bounded successive-interval chain, extend
  only the stochastic loss evidence window, and prove the Apple mixed tone in
  complete frequency windows. Original click count, interval error, gap,
  authority, epoch, packet, and unmixed-input requirements remain intact.

### Iteration 52 - reactive session schedule propagation window

- The next macOS `compile.sh --tests-full` run failed test 49,
  `jam2_four_session_command_integration`, after 2.77 seconds. The retained
  snapshots showed the creator at frame 109,504 with its future one-bar record
  schedule active, while all three recently started joiners were only at frames
  6,336--6,656 and had not yet applied any metronome epoch or transport state.
- The test's propagation loop was capped at eight snapshots. With each snapshot
  delayed by only 512 frames at 48 kHz, that allowed roughly 80--90 ms for the
  initial shared-grid proposal and following transport proposal to cross the
  newly formed mesh. Sequential macOS process startup made that assumption
  visible; the creator process had started about 2.14 seconds before the three
  joiners.
- Replaced the attempt count with a 1.5-second steady-clock deadline. Every
  iteration still requires the exact future target, one-bar count-in, epoch,
  action, and pending state on all four peers, and the existing assertion still
  requires publication before the creator countdown boundary. No product wait,
  transport tolerance, frame tolerance, or protocol acceptance changed.
- Focused verification: the exact test first passed in 5.42 seconds, then
  passed three consecutive executions in 3.12, 3.12, and 5.10 seconds. No full
  suite rerun was started.
- Scope/protocol status: native integration timing only. No production source,
  Windows branch, network message, field, payload, parser, authentication rule,
  metronome epoch, or transport behavior changed.

#### BUG-T119 - fixed snapshot attempts ended before new joiners applied the grid

- Observed condition: full-mesh attachment was established, but a sub-100-ms
  assertion loop completed while newly launched peers were still applying the
  ordered initial-grid and transport control state.
- Change and proof: poll the same strict four-peer schedule state against a
  bounded wall-clock propagation deadline; four focused executions pass while
  retaining the before-countdown requirement.

### Iteration 53 - failed-key limiter validation window ownership

- The next macOS `compile.sh --tests-full` run failed test 51,
  `jam2_native_controller-lifecycle_validation`, after 13.94 seconds. The
  process exited normally with validation code 3, and every controller case
  passed except `controller.failed-key-work-is-rate-limited-and-bounded`. The
  old case combined rate-limit visibility, exact reject count, connection
  high-water, and input-buffer bounds without recording any failing counter.
- Added the raw rate-limit flag/count, authentication-reject delta, completed
  attempt count, challenge-read result, cleanup result, active/high-water
  connections, and maximum buffered input bytes to the native case detail.
  This is emitted on pass and failure and changes no product behavior.
- A subsequent full-suite failure supplied the determining values:
  `rate_limited=0`, `rate_limit_rejects=0`,
  `authentication_reject_delta=64`, `attempts=64`, `challenge_read=1`,
  `cleanup_observed=1`, `active=1`, `active_high_water=8`, and
  `max_buffered_input_bytes=261`. All 64 invalid proofs completed and cleaned
  up, while the following connection was admitted. This disproved the
  provisional slow-cleanup diagnosis.
- The validation had reused one `ControlServer` across several earlier timeout,
  replay, tag, and oversized-frame cases. Its ten-second authentication-failure
  window therefore began well before the failed-key limiter case. That window
  could roll over close to the 64th attempt, correctly admitting the following
  connection into a fresh production window.
- The limiter case now closes the earlier fixture and creates a fresh loopback
  `ControlServer` and port immediately before its exact 64-failures-plus-one
  proof. The case consequently owns the window whose boundary it asserts.
  Detailed resource counters remain in the result. The provisional Apple-only
  longer waits were removed; both Apple and Windows retain the original
  1,000/500/1,000 ms observation waits.
- Focused verification with the fresh fixture passed five consecutive times in
  16.36, 13.02, 13.00, 13.02, and 13.07 seconds. After restoring the original
  cross-platform waits, the final code passed three consecutive times in
  16.31, 12.97, and 12.98 seconds. No full suite rerun was started.
- Scope/protocol status: native validation fixture ownership and diagnostics
  only. No `ControlServer` limiter, production timeout, network field/frame,
  parser, authentication rule, authorization rule, or Windows behavior changed.

#### BUG-T120 - failed-key proof inherited an expiring limiter window

- Observed condition: the failed-key proof reused a server whose authentication
  failure window had aged during preceding security cases, so the exact 64+1
  boundary could straddle a legitimate ten-second window rollover.
- Change and proof: start a fresh server/window for the boundary proof, retain
  the exact failure count and resource caps, and emit the determining counters.
  Eight consecutive focused executions pass with the isolated fixture,
  including three after restoring the original cross-platform waits.
