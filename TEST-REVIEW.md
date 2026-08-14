# Jam2 Manual Review Register

This file contains only items requiring user judgment: possible feature
removals, changes that may break intended functionality, ambiguous legacy
behavior, coverage exemptions, or manual checks that automation cannot settle.
Items are never actioned merely because they appear here.

| ID | Area | Observation | Risk or decision needed | Status |
|---|---|---|---|---|
| REVIEW-001 | Join Jam | Cancel retains the typed invite draft. | Decide whether convenience or erasure is intended. | Open |
| REVIEW-002 | Looper widget ownership | Assigned bank callback, placeholder-lane mouse branch, and six MainWindow editing/file/seek wrappers are dormant. | Decide whether old UI remnants should be removed or missing behavior restored. | Open |
| REVIEW-003 | Loopback recording | A stopped or fully trimmed capture can complete successfully with a valid zero-frame WAV. | Decide whether an empty WAV is useful evidence or should be a visible capture failure. | Open |
| REVIEW-004 | Transport/metronome slew | An adopted transport keeps its raw target while listener render offset may continue slewing. | Decide whether fixed execution ownership is intended or future schedules should be reconciled until commit. | Open |
| REVIEW-005 | Legacy track-processing control | `SharedTrackController` still contains unreferenced `track.processing` JSON construction/application helpers. | Approve removal as an obsolete control schema, or identify the workflow that must own and test it. | Open |
| REVIEW-006 | Bank-transition message validation | `bank.request` does not bound its optional target, and `bank.switch` matches the prepared switch ID without also matching its bank. | Approve tighter acceptance/handling, or retain the trusted-creator behavior as protocol policy. | Open |
| REVIEW-007 | Bank-switch commit atomicity | Peers acknowledge prepared audio before their future Engine scheduling commands are submitted; a later local queue rejection can still diverge. | Decide whether this rare boundary warrants an additional protocol acknowledgement/reservation phase. | Open |
| REVIEW-008 | Section/render duration | The maintained Section model can describe substantially more audio than `PreparedMixRenderer` will render. | Choose streaming/full-duration rendering, a visibly smaller Section limit, or an explicit product error. | Open |
| REVIEW-010 | Legacy session defaults | Three `SessionController` default helpers and their sibling-path resolver have no caller in the unified executable. | Approve removal as obsolete bootstrap API, or identify the intended owner. | Open |
| REVIEW-011 | Physical MIDI and driver callbacks | The Windows profile contains a real ASIO device and VST3 but no physical WinMM MIDI input; unsolicited driver-change/error callbacks cannot be forced safely. | Run the listed manual hardware checks when suitable MIDI/driver hardware is available. | Open |
# Jam2 Test Review

## REVIEW-001 - should Join Jam Cancel preserve the typed invite draft?

- Current behavior: cancelling Join Jam does not start a session or network
  engine, but the typed invite URL is retained in `SessionRuntimeDraft` and is
  shown when the owned dialog is reopened. Bind, port, device, profile, and
  tuning drafts are discarded. Saved Join defaults intentionally do not own an
  invite URL.
- Current automated contract: preserve the unsubmitted draft and prove the jam
  role remains inactive with networking stopped.
- Manual decision requested: confirm whether preserving the draft is desirable
  convenience, or whether Cancel should erase it. No product behavior was
  changed because either choice is user-visible and discarding a long invite
  could be disruptive.

## REVIEW-002 - are dormant LooperLaneStackWidget ownership paths obsolete?

- `MainWindowPages` assigns `LooperLaneStackWidget::onBankSelected`, but the
  widget has no invocation path for that callback. Section/bank selection is
  currently owned by separately laid-out buttons that call
  `MainWindow::selectViewedBank` directly.
- `LooperLaneStackWidget::mousePressEvent` also handles a visual lane index
  beyond `lanes_.size()` by adding a lane and optionally arming or renaming it.
  That branch cannot currently execute because `visualLaneCount()` returns
  exactly `lanes_.size()`. The separate plus row remains functional and is
  covered for both empty-track and import-audio actions.
- Repository-wide call inspection in Iteration 38 also found
  `MainWindow::moveSelectedLooperLane`, `setSelectedLooperLaneGain`, and
  `editSelectedLooperLaneRegion` have declarations and implementations but no
  callers. The painted widget owns movement and gain directly, while its
  region callback reaches `applySelectedLooperLaneRegion`. Tests therefore do
  not fabricate calls that would turn the three wrappers into maintained UI.
- Iteration 41's file/action tail audit found three more unreachable wrappers.
  `loadWavIntoLooperLane` and `loadTrackMetadata` have no caller and the old
  `loadWavButton_` is explicitly null in the current Track-page build; painted
  lane drop/import and Add WAV are the maintained workflows instead.
  `seekPreparedTrack` is reachable only through the callback guarded by
  `trackWaveform_`, while that old waveform widget is explicitly null in the
  current layout. Prepared restart, loop, and transport controls remain live
  and automated. These wrappers were not invoked artificially or removed.
- Manual decision requested: confirm these are remnants of an old inline bank/
  placeholder/file/waveform design and may be removed, or identify the intended
  missing UI behavior that should invoke them. No product behavior was changed
  because either decision affects visible looper interaction ownership.

## REVIEW-003 - should an empty loopback capture be success or failure?

- Current behavior: stopping internal loopback capture before a frame arrives,
  or trimming a completely silent take, writes a valid mono PCM16 WAV with a
  44-byte header and zero data frames. The capture callback reports success;
  later import/analysis code decides whether the empty asset is useful.
- Current automated contract: the writer's zero-frame RIFF shape is valid, but
  iteration 30 does not change the product-level success decision. Nonempty
  injected captures, processing, atomic commit, failures, stop, and reuse are
  covered independently.
- Manual decision requested: confirm whether preserving a technically valid
  empty WAV is useful for diagnosis, or whether loopback capture should report
  a visible `no audio captured` failure and avoid creating/importing the asset.
  No behavior was changed because either answer affects an existing user flow.

## REVIEW-004 - should an adopted transport target follow later render-offset slew?

- Current behavior: a received or local transport schedule adopts one raw
  countdown frame, one raw target frame, and one musical target. Later
  listener-compensation changes can move the current raw-to-musical mapping,
  but the already prepared playback/recording target remains fixed. The stored
  musical target itself remains on the shared bar.
- Evidence: Iteration 33's clean four-peer fake-audio runs retained exact
  96,000-frame one-bar countdowns and bar-aligned musical targets on all peers.
  During the several-second contended fake-clock wait, applying the later
  render offset to the already adopted raw target differed from the stored
  musical target by observed values from 5 through 143 frames. This is separate
  from the 21-case impairment suite's live click/dynamic-epoch assertions.
- Decision requested: confirm that deterministic fixed raw execution after
  adoption is intended, or request a design for reconciling uncommitted
  transport and prepared-source targets while the render mapping slews. The
  latter affects recording/playback timing ownership and needs its own focused
  design and tests; it was not inferred as safe during coverage work.
- Protocol status: no wire field or payload change is required merely to
  review this behavior, and Iteration 33 made none. Any proposed behavioral or
  protocol adjustment must be presented before implementation.

## REVIEW-005 - may the unused `track.processing` control schema be removed?

- Current code: `SharedTrackController::processingMessage()` creates a
  `track.processing` JSON object and `applyProcessingMessage()` applies that
  shape to the old single-track model. Repository-wide reference inspection
  found no caller, receiver, router entry, validator, or test for either
  helper or the message type. Current track/looper sharing uses the maintained
  song-set, batch, and asset-transfer workflows instead.
- Coverage decision: Iteration 34 directly covers every live shared-track
  playback state and status path, but deliberately does not add a test that
  would turn this apparently obsolete schema into a maintained contract.
- Manual decision requested: approve deletion of the two dead helpers as old
  compatibility code, or identify an intended user/network workflow that must
  restore routing and receive behavioral coverage. No helper, message name,
  field, parser, or network behavior was changed during Iteration 34 because
  protocol-surface removal requires explicit approval.

## REVIEW-006 - should bank-transition acceptance be tightened?

- Current behavior: `bank.request` validates the bank but does not validate its
  optional `target_abs_beat`. The creator handler accepts any unsigned 64-bit
  string, while `bank.switch` validation later limits that same field to
  signed-64 maximum. A request above that limit can therefore be accepted by
  the creator and emitted as a switch that joiners reject. Separately, a
  joiner accepts `bank.switch` when `switch_id` matches its prepare state but
  does not also require the message's bank to match the prepared bank.
- Current safety: Iteration 37 makes all local frame arithmetic saturating and
  keeps ordinary valid transitions exact. It deliberately does not change a
  validator, accepted message set, or receiver rule. Authenticated session
  authority and current GUI requests generate bounded ordinary targets.
- Manual decision requested: approve requiring an optional request target to
  parse within the same signed-64 bound as `bank.switch`, and requiring both
  switch ID and bank to match at commit. These are small safety checks but
  change network-message acceptance, so they were not inferred as protocol
  changes. The redundant/unused target field on `bank.prepare` should likewise
  remain until its compatibility status is explicitly decided.

## REVIEW-007 - should bank commit add a post-scheduling acknowledgement?

- Current behavior: `bank.ready` proves that each peer has no render work left
  for the requested Section. The creator then broadcasts `bank.switch`, and
  each process independently submits its prepared-stop/seek/play and transport
  schedule commands. If an Engine command queue rejects after readiness, the
  other peers may still commit while that process remains on the prior bank.
- Current evidence: direct tests cover all barrier state and numeric scheduling
  boundaries, Track Recording units cover command rejection, and the real
  four-peer path completed two consecutive Section transitions with exact
  epoch preservation. There is no deterministic end-to-end queue-rejection
  handshake because the current protocol has no scheduled/failed response.
- Manual decision requested: decide whether the bounded command queue and
  existing failure diagnostics are sufficient for this rare local-overload
  case, or request a design for reservation plus a second acknowledgement (or
  explicit abort) before commit. That would add network state/messages and
  should not be implemented silently during coverage refactoring.

## REVIEW-008 - how should long Sections interact with prepared-mix limits?

- Current model: a Section may contain up to 512 beats. At the maintained
  minimum tempo of 20 BPM this represents 25.6 minutes, before considering
  alternate pulse units.
- Current renderer: `PreparedMixRenderer` bounds one prepared bank to five
  minutes and also applies a 512 MiB working-memory bound. Audio after that
  render limit cannot be represented by the prepared buffer even though the
  Section and lane timeline models accept it.
- Risk: silently treating the renderer's shorter buffer as the complete
  Section could truncate playback or make Section-extension/trim information
  disagree with audible output. Increasing the fixed buffer to the full model
  limit can consume excessive memory; silently lowering the Section limit
  changes a visible feature.
- Manual decision requested: choose a streaming/chunked renderer that supports
  the full maintained Section domain, reduce the visible Section-duration
  contract, or require an explicit render-limit error in the UI. Iteration 38
  changes neither limit and does not claim long-Section playback coverage.

## REVIEW-009 - confirm the current-format protocol cleanup scope

- The transport wire encoder was already emitting the fixed 28-byte version-2
  payload. Slice 4 corrected the packet validator from the unreachable obsolete
  20-byte size to 28, removed the unreachable version-1 decoder branch, and
  centralized the 28/56-byte constants. This changed validation/legacy
  acceptance, not bytes emitted by current Jam2. The user explicitly queried
  this distinction during the initiative; it is retained here for final review.
- Slice 3 made the existing `jam.sync.set` current format require its positive
  authoritative revision, made `jam.sync.request` reject a revision, and made
  both reject fields outside the current format. Emitted field names and values
  did not change, but malformed/extended messages that the old generic check
  accepted now fail closed.
- Slice 4 stopped placing machine-local `asset_path` in shared looper JSON.
  Persisted JamJar JSON still writes that field. Receivers already discarded
  the sender's unusable filesystem path and reconstruct their hash-addressed
  local path, so current peers retain the same content behavior with fewer
  bytes, but the shared JSON shape changed.
- No later slice changed a network message name, field, header, payload,
  encoding, parser, version, authentication rule, or accepted input. The final
  diff audit found no additional wire-shape change.
- Manual confirmation requested: confirm these three already-tested
  current-format cleanups are approved under the stated no-backward-
  compatibility requirement. If any must be reverted, identify it explicitly;
  changing it now would require a focused protocol regression and another
  distribution gate.

## REVIEW-010 - may the unused SessionController default helpers be removed?

- `SessionController::defaultJam2Path`, `defaultBindHost`, and
  `defaultPublicHost` have no repository caller. `siblingToolPath` exists only
  to implement the unused executable-path default. The unified application,
  startup dialogs, and public `network create`/`network join` parsing own their
  current defaults elsewhere.
- Coverage decision: these exact functions are reviewed exemptions. Tests do
  not fabricate calls that would preserve an obsolete bootstrap API as a
  product contract.
- Manual decision requested: approve deletion of these four helpers, or name a
  workflow that still needs them. Their removal would not alter a network
  message or emitted protocol byte, but it is deferred because it changes a
  public C++ surface.

## REVIEW-011 - physical MIDI and unsolicited driver callback validation

- The explicit Windows hardware profile validates ASIO device 5, one real
  input channel, a 32-frame callback, Surge XT Effects VST3 discovery/worker/
  editor/processing, and deliberate out-of-range channel rejection followed by
  a successful reopen. It does not name a physical WinMM MIDI input.
- The exact `WinMidiInput` constructor, live counters, callback, and destructor
  therefore remain hardware-bound reviewed exemptions. The production backend
  still enumerates WinMM and rejects malformed, nonexistent, and injected
  system-device operations in automated tests. Synthetic MIDI routing and
  publication-order behavior are covered independently.
- An unsolicited ASIO sample-rate-change or post-start kernel accept/select
  failure is likewise not manufactured against the user's driver or OS. Normal
  callbacks, channel errors, bind failures, transport failures, and cleanup are
  automated.
- Manual check requested when suitable hardware is available: open a physical
  MIDI input, send supported short messages and at least one unsupported/SysEx
  message, verify routed events and exact counters, close/reopen it, then change
  the ASIO driver's sample rate while idle and confirm the visible diagnostic
  and safe restart behavior.
