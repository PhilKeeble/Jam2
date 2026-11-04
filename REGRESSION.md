# Latency stability regression — 2026-11-03

Introducing commit **unproven**; rollback tests also failed. Oct 27's good-run
commit was inferred, not verified. Earlier baseline fixes are committed in `c553a14`
but remain provisional; **user decides keep/revert**, no automatic decision.

## Network receive / telemetry CTest investigation — Nov 4

The follow-up isolated multiplex receive handling without changing WAV rate
limiting or scheduling. See [the full report](docs/NetworkIngressInvestigation-2026-11-04.md).
Candidates remain uncommitted for the user's keep/revert decision:

- A shared 64-socket-packet budget preserves classification exhaustion through
  recovery and telemetry decisions. The original caller consumed 72 socket
  packets in one mixed turn, and could declare a 64-control-packet prefix
  drained while PCM remained queued. Mixer probes avoid 1 or 64 extra silence
  frames when the yield state is retained; 64 frames is 1.333 ms at 48 kHz.
- Returning after draining prequeued control-only work removes a conditional
  timeout: 1264 us median becomes 17 us. Asynchronous arrival is 16 us with
  either caller. This is not an unconditional 1 ms audio-latency improvement.
- Periodic CSV serialization reuses storage and faster numeric formatting:
  matched fixed-snapshot medians fall from 33–35 us to 6–7 us; expanded final
  precision validation measures 11 us. Normal full-runtime telemetry averages
  9.8 us; stress sampling can still produce millisecond wall-time spikes.
- The explicit 1000-us cooperative receive-service guard is experimental;
  it never triggered in the final 96 peer-case observations. Zero disables it.
- Test-only arrival-timestamp substitution hides local stalls and produces
  underruns under recurring 6 ms pauses. No production timestamp change was kept.

All seven new CTests pass. Final focused Release validation is **14/15 passing**.
The core-input aggregate has 55 assertion failures; restoring HEAD's original
receive implementation reproduces the **identical 55 failures/messages/counts**.
No recovery assertion or threshold was weakened. Four-peer impairment,
mode-latency, receive-model and lifecycle checks also passed during this work.
The full suite, Windows build, real CoreAudio and WAN tests were not run.
The historical regression and Mac/Windows device-latency difference remain
unproven by these local tests. Evidence/provenance: `artifacts/network-ingress-20261104/`;
raw four-peer 32-frame runtime CSVs: `artifacts/network-ingress-native/`.

## A2 + B + C implemented for testing — Nov 4

The user selected all three candidates. They are now active together in the
working tree and optimized public Mac build, on top of the existing baseline:

- A2 retains the lifetime clock estimate and accounts for arrival-offset
  residuals of at least 2000 us. CSVs expose the threshold, adjustment count
  and signed offset; the packet format and audio callback are unchanged.
- B counts real audio received in the batch toward the absolute recovery goal
  before adding silence. Native tests now require this chosen behavior while
  checking remaining deficits, saturated batches, repeated idle completion and
  cancellation. They no longer require redundant post-arrival padding.
- C permits common real prefixes at larger configured minimums, retaining the
  output cap, callback, catch-up and recovery-debt guards.

The new drift-resume and established-empty-contributor checks pass with the
other focused tests. The Nov 3 isolated-candidate results remain historical evidence;
their statements that all candidates were reverted no longer describe the
current build. No Windows build or full suite has been run in this Mac session.

Combined optimized Release validation passes **15 of 19 targeted CTests**:
all seven focused drift/padding/prefix/boundary/CSV checks, clock and callback
flow, repeated/large recovery, and all four exactly-four-peer lifecycle,
impairment-delivery, mode-latency and receive-model workflows.
The mode test records Direct/Server medians **6.4/6.2 ms** and both p95 values
**8.1 ms**, inside the unchanged 1.333/2.667 ms comparison bounds. This run uses
synthetic 128-frame callbacks; it does not establish real-device or WAN latency.

Four known tests remain red, with no new failure category:

| Test | Remaining observed failure |
| --- | --- |
| `jam2_audio_flow_startup-recovery` | Six empty-batch retirement comparisons retain 34 frames beyond the control; same-batch comparisons now retain zero. The one-frame bound remains. |
| `jam2_core_low_skew_units` | 92 short-batch/learning assertions and four settled starvation/continuity assertions, matching the prior baseline counts. |
| `jam2_core_playback_recovery_units` | Three hold-clear and three return-to-baseline assertions, matching the prior baseline counts. |
| `jam2_core_release_return_units` | 18 short-batch/learning assertions, matching the prior baseline count. |

Failure interpretation: the four settled-continuity assertions in the low-skew
suite belong to two diagnostic cases with minimum-target release deliberately
disabled. The release-return suite repeats the short-batch/no-gap-learning
assertion; its 18 failures do not establish another independent failure mode.

The playback-recovery "actual buffered latency" assertion is compound: it
requires both a bounded target and a near-baseline measured queue. In its three
failing cases the target stays at 304/355/338 frames (allowed: 128), with the
learned hold active. Actual queue averages are already close to baseline:
3.37222 to 3.37453 ms, or 4.03889 to 4.04147 ms. The observed failure there is
stuck protection/target state; it should not be reported as a large measured
latency increase. The worker's recurring 3 ms stalls continue after the large
injected gaps stop, and the hold logic permits fresh gaps to extend protection
without a new underrun. Exact triggers across the wider short-batch matrix
remain to be isolated.

Their latency, continuity and hold-expiry assertions were not relaxed. B's
five old quantity/policy assertions were updated to the explicitly selected
absolute-goal contract, with stricter real-sample and remaining-padding checks.
The combined implementation has not repeated the Nov 3 physical-device matrices.

Public tested app: `release/Jam2.app/Contents/MacOS/jam2`, SHA-256
`b750f1962b01d5af98218f2ccd6524e58ec1c84414c9167c5f0eb8f548bf984b`.
[Build](artifacts/mac-combined-20261104/release-build.log),
[focused tests](artifacts/mac-combined-20261104/focused-unit-tests.log),
[combined regression tests](artifacts/mac-combined-20261104/combined-regression-tests.log),
[provenance and assertion counts](artifacts/mac-combined-20261104/validation.json).

## Windows validation and paired Wi-Fi repeat - Nov 4

`7749606` builds in Windows MSVC Release; eight focused drift/padding/prefix/
callback/CSV native checks pass. TONEX is disconnected, so no physical ASIO
matrix ran. Two 30 s localhost synthetic pairs (48 kHz/128 callback/64 PCM16)
record median queues Direct 2.67/5.33 ms, Server 3.56/3.88 ms; Direct has four
steady missing frames and 25 trimmed frames, Server zero. No loss/hard recovery.
[Synthetic baseline](artifacts/windows-baseline-20261104/RESULTS.md).

Real Server Fast repeat: Windows `*20261104_0024*pid28140*`, uploaded Mac
`*20261104_0025*pid33379*`, same devices/48 kHz/32 callbacks/64 PCM16 as Nov 3 A.
Missing audio ms/min Windows **3247 -> 3250**, Mac **3801 -> 3812**; trimming
**4328 -> 4326 / 4922 -> 4969**, hard recoveries ~114-116/min on both.
Windows A2 records eight offset adjustments (net +15.5 ms), zero source-ratio
clamp observations; main audio failure remains. Mac has **38 HAL overloads**,
2.90 ms max callback work and 3.69 ms max interval against a 0.667 ms budget;
source correction often clamps despite remote drift ~-23 ppm: inspect the local
device-clock/relative-drift inputs missing from CSV. New catch-up starts zero;
Fast does not exercise C's newly enabled larger-minimum case. Build/tests ended
before the jam; overlapping localhost simulations do not explain it, since the
intervening quiet-PC slice has the same recovery/shortage rates. No acceptance
or revert decision. [Paired review](artifacts/windows-mac-jam-review-20261104/RESULTS.md).

## Connected Focusrite and isolated Mac candidates — Nov 3

The [extended report](docs/MacAudioInvestigation-2026-11-03.md#connected-focusrite-controlled-candidate-experiments)
now includes Focusrite and Aggregate, 16 actual visible GUI sessions, 36 paired
CLI hardware cases, separate-device clocks and individually tested patches.
All experimental audio changes are **reverted to baseline**; patches and native
diagnostics remain for review. The existing provisional `c553a14` is unchanged.

- **HAL overloads remain unexplained.** Both devices pass the real GUI lifecycle
  matrices with zero overloads, invalid timestamps or steady shortage/trim growth.
  The accepted GUI interval also has no OS-log overload messages. Local tests
  do not reproduce the original connected Wi-Fi misses.
- **Source drift is independently actionable.** In matched +3 ms arrival-step
  recordings, the offset-accounting A2 prototype reduces Aggregate/Focusrite
  steady trims **338/155 to 0/0**, and waveform large steps **3/1 to 0/0**.
  Known-clock and quantized-worker controls remain passing. A clean waveform
  discontinuity and startup queue retention still occur; it is not a complete
  recovery or latency-stability fix.
- **A short recent-slope estimator is rejected:** it loses real 17 ppm skew.
  **Absolute padding is partial:** its deterministic same-batch penalty drops
  34 frames to zero, but empty-batch retention and five existing policy
  assertions remain. **Larger partial release passes its nine new native cases**
  without new selected-control failures; no total-latency benefit is established.
- Focusrite Fast recorded pulse RTTs span **3.35..10.73 ms Direct** and
  **8.08..10.75 ms Server** on baseline. A2 still varies: **2.77..9.42 ms Direct**,
  **6.73..6.75 ms Server**. Moderate baseline is **82.35..83.64 ms** across modes.
  These exclude analog converters and separate reported driver paths.
- A2 passes seven of eight final four-peer/recovery/CSV/drift checks. The
  symmetric mode-p95 gate fails on **both A2 and a restored-baseline repeat**:
  Server minus Direct is -3.5/-3.0 ms against a 2.667 ms absolute bound.
  Thresholds are unchanged; this does not prove a Server regression or an A2 cause.

Reviewable patches: [A2 offset accounting](docs/experiments/mac-audio-2026-11-03/A2-arrival-offset-accounting.patch),
[C partial prefixes](docs/experiments/mac-audio-2026-11-03/C-larger-partial-release.patch),
[B padding](docs/experiments/mac-audio-2026-11-03/B-absolute-startup-padding.patch),
[rejected A](docs/experiments/mac-audio-2026-11-03/A-recent-drift.patch).
Raw CSVs, WAVs, profiles, build hashes and failed assertions are linked in the
report. Final Release build/setup/CSV checks pass; desired baseline reproductions
remain red. No full suite, Windows validation or analog-loopback measurement ran.

## Initial Aggregate-only macOS controlled follow-up — Nov 3

Full measurements, causes versus hypotheses, native test results and repeat
instructions: [Mac audio investigation](docs/MacAudioInvestigation-2026-11-03.md).
Production code and existing acceptance thresholds are unchanged.

Current Release, 48 kHz/PCM16, two localhost CoreAudio streams on the existing
Aggregate Device, matching local Go signaller. Fresh Direct/server/server/Direct
tone/pulse matrices plus persistent-engine stop/rejoin and controlled holds.
The CoreAudio hardware-harness gap described in the earlier handoff is now
covered by explicit UID profiles and native CTests; it is not full GUI-painting
or independent-device/WAN coverage.

| Contract | Recorded render round trip, Direct | Jam2 Server |
|---|---:|---:|
| Fast, 32 callbacks / 64 packets | 2.729–2.875 ms | 2.813–4.083 ms |
| Larger, 64 callbacks / 128 packets | 80.125–80.208 ms | 80.042–80.083 ms |

These exclude physical input/converters and reported driver output paths.
All 16 fresh cases have zero CoreAudio overloads. Nevertheless, each matrix
fails 5/6 strict comparisons, including Direct repeats: Fast startup/queue
variation, and a larger server tone run with **70 valid frames trimmed and one
extra waveform step**. Several larger pulse failures concern p99 queue
differences just beyond a callback, despite nearly identical recorded round
trips. Do not describe these matrices as accepted or treat every failure as a
consistent server penalty.

**New reproducible estimator weakness:** a sustained 3 ms arrival-delay step
with identical clocks produces about −321 ppm correction at 10 s and −102 ppm
at 30 s. Zero and genuine ±200 ppm controls pass. The new
`jam2_peer_drift_delay_step_units` intentionally remains red at its desired
25 ppm settling bound. The real larger trim case has the matching false-drift
and queue-growth pattern, without overloads or underruns. The lifetime-baseline
formula dates to August `8d08ee0`; this is not proof of a UDP introducing commit.
A controlled real-CoreAudio +3 ms delay case also reproduces false drift,
**264 trimmed frames and one extra waveform step**, versus zero in its
no-delay control, and retains ~3.25 ms more software queue after delay removal.
Both have zero overloads and zero steady shortages. The corresponding hardware
comparison remains a visible failing CTest alongside the new estimator unit.

**Original CoreAudio events confirmed by OS logs:** 45 / 0 / 7 / 0 / 0 skipped
cycles across A/B/C/D/E connected windows. A/C events follow a near-524.288 ms
grid. Periodic wireless/OS scheduling is a hypothesis to trace, not a confirmed
cause; no wireless settings were changed. Local fresh and reused-engine runs
have not reproduced that deadline failure.

Five constructed 95 ms holds followed by over two minutes clean pass both
hardware recovery gates. Larger buffering returns near its ~39 ms baseline;
Fast settles around 2.67 ms versus ~0.77 ms initially, despite target 64 and an
expired hold. Target statistics alone do not establish latency retirement.
Four-peer mode/receive-model/delivery/lifecycle checks and private-aggregate
cleanup pass. The documented startup/recovery native failures remain.

## macOS handoff: Nov 3 Wi-Fi and local Direct/server comparison

This handoff records the investigation state before the controlled follow-up
above; its harness coverage gaps and test-configuration notes are historical.

User reports the same Wi-Fi setup worked better before UDP multiplexing; expected
Wi-Fi/Fast underruns do not settle that regression question. Investigate Mac
callback/device lifecycle and mode differences as well as network recovery.
This summary is self-contained: Windows logs and ignored `artifacts/` reports may
not exist on the Mac. Latest review changed no product code or test gates.

Paired runs below use 48 kHz/PCM16, Windows **Focusrite USB ASIO creator** and Mac
**CoreAudio Aggregate Device joiner**. Both GUIs persist across sessions. Fast:
32-frame callbacks, 64-frame packets, jitter target 64, adaptive min/max 64/512.
Larger: 64-frame callbacks, 128-frame packets, jitter target 1536, adaptive min/max
512/4096. B keeps 64-frame packets and the server's Fast contract label, but the
Mac uses larger local buffers. All successful audio endpoints are LAN:
Windows `192.168.4.63`, Mac `192.168.4.57`; no server audio relay/WAN detour.

CSV prefix `jam2_stats_20261103_`, extension `.csv`; Windows in `release/logs`,
Mac originals in its `release/logs` (Windows uploads: `tools/incoming_uploads`).

| Run / approximate start | Windows CSV suffix | Mac CSV suffix |
|---|---|---|
| A Server Fast, 18:17 | 181531_887_pid9148, first peer join | 181715_359_pid23894 |
| B Server mixed, 18:19 | Same CSV, second peer join | 181930_720_pid23894 |
| C Server larger, 18:21 | 182120_395_pid9148 | 182134_296_pid23894 |
| D Direct Fast, 18:24 | 182346_634_pid9148 | 182413_151_pid23894 |
| E Direct larger, 18:26 | 182613_251_pid9148 | 182625_568_pid23894 |

GUI files: Windows `jam2_gui_20261103_181520_455_pid9148.log`,
Mac `jam2_gui_20261103_181602_636_pid23894.log`. Pair by endpoints/lifecycle;
host clocks differ. Use connected periodic counter **deltas**, excluding first
10 s and final 2 s for steady comparisons; raw totals include earlier/idle runs.

| Run | Median software queue ms, Windows / Mac | Missing source audio ms/min, Windows / Mac |
|---|---:|---:|
| A Server Fast | 4.08 / 3.32 | 3247 / 3801 |
| B Server mixed | 2.67 / 96.32 | 3297 / 0.93 |
| C Server larger | 97.85 / 95.15 | 0 / 8.16 |
| D Direct Fast | 4.67 / 3.67 | 3226 / 3330 |
| E Direct larger | 92.85 / 96.04 | 5.50 / 3.20 |

Queue = jitter + peer + output storage, excluding transit and hardware latency.
Shortage = underrun source frames / sample rate / duration, not measured audible
silence; do not add cumulative padding/trimming as independent dropout time.
Fast starts roughly two hard receive recoveries/sec in both modes. Larger buffers
largely prevent that, retaining about 29-31 ms jitter + 61-67 ms output storage.

**Mac-specific difference to reproduce**, not yet proven caused by server mode:

| Mac measurement | Server Fast | Direct Fast | Server larger | Direct larger |
|---|---:|---:|---:|---:|
| Additional overload notifications in connected sampled interval | 45 | 0 | 7 | 0 |
| Reported max callback work, ms | 1.545 | 0.081 | 1.868 | 0.028 |
| Reported max callback interval, ms | 2.335 | 0.899 | 3.393 | 1.563 |
| Median reported mixer service-gap high water, ms | 4.28 | 1.40 | 2.58 | 1.40 |

Windows has no consistent corresponding callback penalty; Fast max work is
0.304 ms server / 0.577 ms Direct, larger 0.416 / 0.314 ms. RTT averages are
roughly 12-13 ms in both modes. Mac C reuses B's 64-frame engine; D reconfigures
the engine. Sequential Wi-Fi variation and engine reuse confound mode causality.
Direct's severe Fast shortages despite zero Mac overloads remain a shared issue.

Recovery observations: audio arrival gaps reach ~93-101 ms while local receive-loop
maxima are ~2-5 ms. C Windows' 91.838 ms gap calculates exactly its 3129-frame
target: ceil(91.838*48) - (1536-128) + 128. Repeated evidence renews 30 s holds;
no sustained clean hold-expired phase is captured. Mac C target reaches 4096;
sampled total queues peak ~100.69 ms across these runs. **New catch-up starts = 0
on all ten receiver-runs**: raised targets/holds and insufficient output low water
block it. Mac A's 14 retired frames are older-controller consumption accounting,
not new-path activation. Pending credit is capped bookkeeping, not actual retained
latency; disconnect cancellation is not playback retirement. Delayed-beacon filter
records 86 rejected observations; raw sampled errors reach ~89 ms, mapping-target
errors stay below 3.90 ms. Zero sequence loss/mux drops does not imply good audio.

### Follow-up: partial transfers where currently disabled

Test `PeerMixer::Impl::partialReleaseFrames()` in
`libs/jam2-core/src/peer_mixer.cpp`. It moves already decoded/resampled peer audio
to the output ring before a full mixer block is available: e.g. 127 usable frames
can supply a 64-frame callback while the mixer normally handles 128-frame blocks.
The `adaptive_min_frames > mixed_output.size()` guard disables this for the
larger profile (512 > 128); Fast already permits it subject to other guards.

Add focused native CTests comparing current behavior with a narrowly enabled
candidate: 48 kHz, 128-frame packets/64-frame callbacks, resampler remainders,
small worker delays, returning bursts and raised targets. Cover active hard
recovery/debt, insufficient callback runway and output-cap conditions separately;
establish why those guards are needed before relaxing any. Preserve four-peer
coverage and compare Direct/server with real CoreAudio and Windows ASIO follow-up.
Measure peer/output depth and valid frames available **at the underrun**, partial
releases, padding, trimming and subsequent target growth. Moving audio must respect
the existing shared buffer budget, with no additional clean latency or quality
failures. Two-second Wi-Fi snapshots do not establish benefit: larger-profile
Windows had 0 steady underrun frames over 98 s server / 229 over 52 s Direct.
Do not attribute the ~65 ms cushion to this restriction without reproducing it.

### macOS investigation / native test entry points

- Read `AGENTS.md` and applicable macOS test instructions; use optimized Release
  via `compile.sh`, the staged public app and a matching **local** Go signaller.
  Keep clean latency/quality gates; no full suite unless requested. Inspect
  `libs/jam2-core/src/audio_device_macos.cpp`, `audio_device_processing.cpp`,
  `engine.cpp`, `peer_mixer.cpp`, `peer_stream.cpp`, and
  `app/cli/NetworkRuntime.cpp` (`jam2_engine_restart_required`). Compare callback
  work/intervals, overloads, timestamp validity/cycle timing, actual callback sizes,
  worker runway, queue components and engine create/reuse/restart in each mode.
- Existing `jam2_four_mode_audio_latency_integration` and
  `jam2_udp_four_receive_models_performance` are **four-peer fake-audio** tests
  (`tests/integration/network/ModeAudioLatencyIntegration.cpp`), 48 kHz/PCM24,
  64-frame packets/**128-frame headless callbacks**, with Direct/server ABBA
  ordering. They do not validate 32/64-frame real CoreAudio callbacks. Their
  local Go binary requires CMake cache `JAM2_RENDEZVOUS_TEST_SERVER`; current
  CMake does not import that environment variable on macOS. Ensure registration,
  not a missing-test/skip pass. Then use, for example:
  `bash ./compile.sh --tests network --test-name jam2_four_mode_audio_latency_integration`.
- CoreAudio entry points: `jam2_coreaudio_aggregate_integration` (explicit
  `JAM2_COREAUDIO_INPUT_UID`/`JAM2_COREAUDIO_OUTPUT_UID`; device lifecycle, not a
  two-peer mode comparison), `jam2_audio_device_processing_units`,
  `jam2_coreaudio_recording_timing_units`, and
  `jam2_multiplex_four_session_lifecycle_integration`. Recovery tests:
  `jam2_audio_flow_large-recovery`, `jam2_audio_flow_repeated-recovery`,
  `jam2_audio_flow_startup-recovery`. Windows large/repeated pass; startup,
  `jam2_core_input_units`, `jam2_core_playback_recovery_units` and
  `jam2_core_release_return_units` have known remaining failures, not Mac-only
  regressions. Preserve measurements/assertions rather than weakening them.
- Coverage gap: real-device selection in
  `tests/integration/network/AsioDeliveryImpairmentIntegration.cpp` is ASIO-only;
  `jam2_asio_oct30_delivery_hardware` is Windows-only. Use its
  `tests/profiles/asio-mode-equivalence.json` as the small **two-instance localhost
  CoreAudio CTest** model; it is not an existing Mac hardware test. Profile real
  device UIDs/channels/callbacks, compare Direct/server/server/Direct with a local
  signaller, tone 60 s + pulse 40 s, identical settings in both modes. Separately
  compare fresh engines against GUI stop/rejoin with engine reuse; retain the
  existing four-peer coverage. Record callback audio, not just a GUI estimate.
  Test both 48 kHz contracts above; only compare absolute Windows numbers at
  matching supported rates/settings, without silently changing the device setup.
- Windows reference without its files: fresh candidate Focusrite/TONEX localhost,
  44.1 kHz/32 callbacks/64 PCM16, eight tone/pulse cases completed; receiver median
  software queues span **1.59-3.47 ms**, recorded pulse round trips **6.33-7.19 ms**
  excluding converters. Earlier reused-GUI queues: Direct **1.46/1.54 ms**,
  server **2.86/2.88 ms**. These are references, **not a passing golden baseline**:
  fresh pre-fix and candidate matrices each failed 5/6 mode comparisons. Compare
  matched software/frame measurements on Mac, accounting separately for driver
  latency (Wi-Fi Mac reports ~14.5-15.2 ms output, Windows ~3.25-3.92 ms).
  Hardware gates allow one callback per-direction median/p99 variation, two for
  recorded round trip, and no extra steady underruns/trims/tone discontinuities.
- After the clean local comparison, replay repeated ~90-100 ms holds/bursts plus
  shorter gaps, then remove impairment and follow real queues through hold expiry,
  target decay and catch-up. Current Wi-Fi runs cannot prove post-stress clearing
  or before/after benefit. Report results for the user to decide, not automatic
  acceptance/revert or an assumption that Wi-Fi explains the historical regression.

Optional Windows-local reports, if available:
[Wi-Fi review](artifacts/wifi-recovery-review-20261103/RESULTS.md),
[ASIO comparison](artifacts/asio-mode-review-20261103/RESULTS.md).

## Inputs / directions

- Good Oct 23: `release/logs/jam2_stats_20261023_120503_496_pid33228.csv`
  and `jam2_gui_20261023_110410_830_pid33228.log`.
- Good Oct 27 LAN: `release/logs/jam2_stats_20261027_181624_332_pid68908.csv`.
- Bad Oct 30 Robbie: `C:\Users\Phil\Documents\Jam2-windows\logs`;
  `*20261030*pid24448*` = Windows joiner, `*pid7609*` = Mac creator.
- Bad LAN: `release/logs/jam2_stats_20261029_151420_795_pid37968.csv`.
- Paired captures: `captures/`, `tools/incoming_uploads/`, `release/logs/`;
  [packet review](artifacts/packet-regression-20261102/packet-review.json).
- New evidence/patches: `artifacts/regression-fixes-20261103/`;
  [results review](artifacts/regression-fixes-20261103/results-review.json),
  [test profiles / commands](tests/profiles/README.md).

## Findings / latest experiment

- 64-frame 4/8 ms batching reproduces repeated underruns and valid-audio trimming.
  Fix experiments cover short-gap learning, release/target decay and interpolation
  runway. The original mixer/stream logic was unchanged from pre-UDP `21a45d3`.
- Delayed beacons independently cause ~427 ms mapping error. Candidate filters
  delayed/reordered **unwrapped** observations, preserves epochs and exposes CSV
  diagnostics. Latest delayed-beacon errors remain below **2.59 ms**.
- Baseline: `artifacts/asio-delivery/20261103-095809-018-two-asio`.
  Latest J: `artifacts/asio-delivery/20261103-113622-699-two-asio`.
  Real **TONEX creator / Focusrite joiner, 44.1 kHz / 32-frame ASIO, PCM24**.
- All four clean cases pass unchanged gates: no added steady underruns/trimming
  or tone discontinuities. Callback audio round trips (64/128 packets):
  **7.91 / 10.11 ms**, baseline **7.39 / 12.52 ms**. The +0.52 ms difference is
  within the declared callback tolerance; driver output/converters are separate.
- 8 ms batching: Focusrite underruns/trimming improve from **56.52 / 598.04**
  to **2.84 / 4.83 ms/min**. Recurring holds and the Oct 30 envelope recover.
- **Full J matrix: 9/10 pass.** At 300 BPM, 16 TONEX / 10 Focusrite underrun
  frames occur in seconds 45..63 after extra delay ends; mapping remains bounded.
  Identical isolated repeat passes: `20261103-114802-999-two-asio`.
  **Do not call the intermittent issue fixed or the candidate fully accepted.**
- Twenty focused native checks pass, including four peers, 44.1 kHz cadence,
  bounded jitter, ordering/loss and hour-long clock simulations. One clean runtime
  check failed zero-padding with a 24 ms proxy-pump stall, then passed isolated;
  both logs retained, causation unproven.

Next: capture callback/worker/buffer state at the intermittent underrun and add
irregular-wakeup coverage. Keep clean latency/quality gates unchanged.
Nov 3 manual Direct/Jam2-server GUI review: [results](artifacts/manual-local-review-20261103/review.json).
Server playback queues are ~1.3–1.4 ms higher, with 64/63 recovery-padding
frames retained while targets return to 64; RTT remains ~0.04 ms. No loss,
trimming or recovery loops; small underruns remain. Add startup/rejoin and
padding-retirement coverage for the persistent GUI engine.
Native startup regression retains 64 frames in PCM16/24; an archived worker
state probe explains the blocked release but does not prove safe headroom. Proposed fixes were **rejected**:
real-ASIO mode comparisons failed; more aggressive release introduced clock-skew
underruns. Playback changes from this experiment were reverted; tests/harness
and [evidence](artifacts/startup-recovery-fix-20261103/review.json) remain.
No-growth check: `jam2_audio_flow_repeated-recovery` passes 18 deterministic
four-participant cases, thirty identical 1.6/4 ms pauses over 496 seconds each.
Actual settled depth stays within one frame of the first recovery despite growing
cumulative padding; no loss/trimming/drops. Retention still need not clear.
[Evidence](artifacts/repeated-recovery-20261103/review.json); production unchanged.
Larger single-pause baseline: `jam2_audio_flow_large-recovery` fails 24/30 comparisons.
4..20 ms pauses retain ~128 samples / 2.9 ms above clean for ~8 minutes. Target
returns to 64, but touching the 128-frame surplus floor resets release's 1 s
stability timer. This is the baseline for the following candidate.
[Evidence](artifacts/large-recovery-20261103/review.json).
Recovery-accounting candidate: larger-pause comparisons now pass 30/30, with
~66 samples / 1.5 ms retained instead of ~128 / 2.9 ms; repeated pauses stay
bounded. Uses measured extra source consumption and recent low-water headroom.
Strict startup retirement still fails (~34 extra samples vs equal-underrun
control); other pre-existing native failures remain.
[Current experiment](artifacts/recovery-accounting-fix-20261103/review.json).
Real ASIO Direct/server follow-up: 8 fresh pre-fix + 8 candidate cases completed.
Native paired checks flag 7/8; one is an exact-32-frame CSV-rounding boundary,
six have other buffer/quality differences. New release path inactive in 14/16
device-runs; both mode matrices already vary. Candidate is still installed;
**user decides keep/revert**, no automatic decision.
[Hardware results](artifacts/asio-mode-review-20261103/review.json).
Nov 2 Wi-Fi receive pauses do **not** establish Robbie's Ethernet/WAN cause;
the short [transport A/B](artifacts/udp-transport-comparison-20261102/summary.json)
passed without devices/mixer/full runtime. Wired captures are follow-up, not a
prerequisite; user cannot arrange that run alone. Oct 30's ~200 ms GUI estimate
can follow RTT/2; ~417 ms mapping error is not an OS clock jump:
[GUI review](artifacts/gui-latency-review-20261103.json).

### Nov 4: physical Mac Wi-Fi / 44.1 kHz follow-up

Twelve real CoreAudio sessions (19 minutes of requested audio) joined the
provided Windows jam, keeping Windows on its existing build. Both Aggregate
and Focusrite were tested. The callback's native capture-wake pipe notification
is a reproducible Mac contributor: Focusrite off/on/off 90-second cases gave
**0 / 28 / 0 overloads**, with **15 / 2910 / 15 us** maximum callback time.
Atomic-only wake publication also removed the repeated overloads, narrowing
the contributor to native notification/scheduling rather than all wake state.

Two Focusrite 32-frame cases with **250 us polling** gave **zero overloads over
90 + 60 seconds**, **13 us** callback maxima, **163–170 us** mean dispatch and
about **10.9% of one core** CPU, versus **34.9 us / 10.7%** in a 60-second native
control with **15 overloads**. Aggregate polling had **zero / 8 us** over 60
seconds; its 180-second native baseline had one startup/page-fault overload
and none afterward. This does not erase last night's Aggregate/48 kHz evidence.

Kernel timing locates a **291.150 ms incoming gap before application dequeue**,
with only **0.683 ms** residence for that packet. Smaller local socket delays
still exist. Polling does not cure these upstream gaps or ensuing underruns;
the Windows sender, radio/AP and Mac lower network stack remain to separate.
The 64-frame native test also had zero overloads but exercised **zero native
wakes** and sampled more queued capture audio. Its retained backlog is a
separate latency candidate, not proof that a larger callback fixes the call.

Nine focused Release CTests pass, including exactly-four-peer runtime and
impaired ordering. New behavior is exposed through diagnostic CLI controls;
normal GUI/Windows defaults remain unchanged pending user review.
[Full report](docs/PhysicalWifiCoreAudioInvestigation-2026-11-04.md),
[measurements](artifacts/remote-jam-20261104/measurements.md),
[raw summary](artifacts/remote-jam-20261104/summary.json).

### Nov 4: CoreAudio polling promoted; receive-call attribution

User accepted the polling change. Normal CoreAudio GUI/CLI sessions now
automatically use atomic readiness with a **250 us polling ceiling**. Other
backends retain native notifications; Windows policy is unchanged. Explicit
diagnostic modes remain available. Real Focusrite and Aggregate default runs
both had zero overloads. A clean 100 us trial reduced capture dispatch from
about **171 to 91 us**, but did not remove receive stalls; 250 us remains the
default.

New timing separates kernel stamp, receive entry/return and post-read
observation, and pairs slow events with worker phase and full-span thread CPU.
One **4.108 ms** incoming delay consisted of **1.292 ms before `recvmsg` and
2.816 ms during it**, with microsecond classification afterward. In the first
traced run, receive-call time dominated **111/144** recorded steady slow
episodes. This locates most observed delay at the OS-call boundary, not in
application packet routing; it does not identify the exact kernel lock or
exclude OS preemption. None of the recorded empty-read attempts began after
the affected packet's kernel stamp; socket availability throughout the
interval remains unproven.

A per-call `MSG_DONTWAIT` experiment and disabling the network-worker priority
request both left millisecond receive stalls. The flag experiment was reverted;
priority defaults remain unchanged. Ten focused Release CTests pass; six also
pass after restoration of the trial. The restored executable exactly matches
the main validated binary. One stack-sampled diagnostic run had six HAL skips
coincident with profiling and is explicitly excluded from clean comparisons.
[Follow-up report](docs/ReceiveServiceInvestigation-2026-11-04.md),
[case summary](artifacts/receive-service-20261104/summary.csv),
[per-event evidence](artifacts/receive-service-20261104/events.csv).

### Nov 4: connected BSD default and localhost wait gaps

Connected BSD is now automatic for **macOS Server per-peer media sockets**;
Direct keeps its shared BSD bootstrap socket and other platforms keep BSD.
Explicit `--udp-backend auto|bsd|connected|network` comparisons remain available.
The remote same-build A/B/A produced ordinary-BSD maxima **2.949 / 3.185 ms**
and **7.91 / 9.39%** audio packets over 1 ms, versus connected BSD **0.605 ms /
zero over 1 ms**. Network.framework also reduced these tails but used more CPU;
voice service and a separate reader did not consistently remove them. An
ordinary-BSD reverse run was also fast, so this remains context-dependent.
The promoted public default then joined the live Windows jam for another 60 s:
**0.534 ms maximum, zero of 40,888 packets over 1 ms, zero HAL overloads and
capture overflows**. Windows retained its existing build.

Local native pulse tests included simulated devices, Focusrite + Aggregate,
and two sessions on the Focusrite. Several runs reached roughly 3–6 ms recorded
callback RTT, but short-run variation remained substantial. The 12.426 ms
outlier's ~5 ms software queue was entirely the playback ring. Startup delivery
gaps raised a 30-second held target; its raised-cushion release stopped near
192–226 frames. The 20-second run ended before the hold expired. A native
production-pipeline test reproduced a **5.143 ms mean ring**, then return to
a smaller depth after expiry without new clean-period starvation. Recovery
policy is retained as requested; a hard 64-frame target cap was not a cure.

Longer localhost runs traced matching **~8 ms socket-wait phases in both
workers**, using only **11–17 us CPU**, while CoreAudio callbacks stayed regular.
This occurred with Network.framework too and caused 221 / 222 missing playback
frames plus recovery padding. Ordinary BSD separately showed ~11 ms gaps
between kernel receive timestamps. These are local servicing/wait delays, not
time travelling over Wi-Fi. Remote captures also contain post-timestamp waits
and read stalls, but not yet a causal match of a particular wait to recovery.
The exact cause of the large wait remains unresolved; connecting BSD does not
guarantee worker scheduling deadlines.

Nine final focused native CTests and the three-case physical comparison passed.
The full suite was not run. OS logs confirm zero overload entries for 40 local
hardware processes and clean remote cases; the kernel-traced run's one overload
is excluded from performance comparisons.
[Report](docs/UdpPathInvestigation-2026-11-04.md),
[remote results](artifacts/udp-paths-20261104/summary.csv),
[local results](artifacts/udp-paths-20261104/local-summary.csv),
[local phase timing](artifacts/udp-paths-20261104/local-gap-summary.json).

### Nov 4: timer-wait attribution and a critical-deadline candidate

A further 180-second profiled Focusrite + Aggregate localhost CTest passed on
the unchanged connected-BSD Release build. Its 3.7–4.8 ms startup wait preceded
the administrator trace and remains unclassified. During the capture, one
1.263 ms wait split into **1.248 ms blocked / 0.013 ms runnable**, with a timer
expiry wake. Both HAL overloads coincided exactly with trace startup and are
excluded from clean comparisons. Across the two runtime workers, over 500,000
`recvmsg` calls had maxima of **51 / 52 us**, with none above 1 ms.

An optimized standalone OS probe found repeatable normal-timeout overshoot.
Requested **100 / 250 / 1000 us** waits had select medians **128 / 319 / 1260 us**,
versus absolute critical kqueue timer medians **104 / 257 / 1021 us**. Two
concurrent workers, reversed order and 540,000 total waits were measured.
This supports an application-level wait experiment for both Direct and Server;
it does not establish that the rare 4–8 ms pauses are cured. No runtime defaults
or recovery policy changed. The next large-pause trace must start before the
clients, and the actual Jam2 candidate still needs native and physical A/B tests.
[Kernel-wait report](docs/KernelWaitInvestigation-2026-11-04.md),
[probe measurements](artifacts/kernel-waits-20261104/wait-probe-summary.json).

### Nov 4: critical kqueue waits implemented with legacy comparison

macOS Direct and Server now default to absolute critical kqueue waits. The
previous select/poll path remains available through `--udp-wait legacy` for
same-build comparisons. CoreAudio retains atomic capture publication and
250 us polling; playback recovery and radio settings were not changed.
Runtime and background-worker stats separately expose wait counts, requested
and elapsed time, overshoot, readiness/deadline wakes, interruptions and errors.

Production-waiter CTests measured 250 us request medians of **257–259 us** with
critical versus **317–320 us** legacy. Eight physical Focusrite + Aggregate
localhost cases at 44.1 kHz / 32-frame callbacks / 64-frame packets gave overall
callback pulse RTTs of **4.40–5.06 ms Direct** and **2.93–6.42 ms Server** with
critical, versus **6.35–8.84 ms Direct** and **6.35–10.95 ms Server** legacy.
These are per-run phase medians, excluding analog conversion, not sustained
latency bounds. Critical Direct B rose to **8.07 ms** in its late 40–55 s window
after a pause, despite its 4.40 ms overall result.

Physical runtime waits exceeding their requested budget by at least 1 ms fell
from **53/1,355,381** legacy to **10/1,664,633** critical. However a **10.9 ms**
critical wait remained in both localhost workers with only **14/30 us CPU**.
Steady missing playback frames were **112 legacy / 238 critical**, so fewer
late waits did not establish fewer underruns. Eight simulated Direct/Server
cases also varied; three matched pairs improved and one regressed.

Eight 60-second joins to the unchanged live Windows jam passed. Focusrite
critical maximum overshoot was **478/538 us** versus **764/1610 us** legacy;
Aggregate retained >1 ms tails with both. Timestamp-disabled Focusrite CPU was
**7.71% critical / 7.45% legacy** of one core, including common inspection
overhead. Remote recovery and queue tails did not improve consistently.
Largest gaps between receive-kernel packet stamps remained **113–155 ms**;
the packet following each maximum was serviced in **25–51 us**. Those gaps
preceded application routing, but these data do not identify Wi-Fi versus
sender/driver behavior upstream of the receive timestamp.

Twelve relevant Release CTests passed, including exactly-four-peer impairment,
runtime paths, lifecycle, pressure and rotation. All 16 local hardware
processes and eight remote processes had zero HAL overloads/capture overflows;
macOS logs contained zero overload entries over the comparison interval.
The full suite was not run. The new waiter improves normal deadlines; the
rare long pause remains unresolved and requires a kernel capture that includes
the event. Both comparison paths are retained.
[Implementation and measurements](docs/CriticalWaitExperiment-2026-11-04.md),
[local CSV](artifacts/critical-wait-20261104/local-summary.csv),
[remote CSV](artifacts/critical-wait-20261104/remote-summary.csv).

## Windows 250 microsecond wait comparison — Nov 4

This experiment was removed after the physical ASIO comparison below. Windows
again uses its original native waits; the macOS changes from `d4d04bf` remain.
The removed experimental combination was:
`--udp-wait high-resolution --receive-wait-us 250`. The runtime wait ceiling
and timer selection are independent controls. Defaults remain native waits
with a 1000 us ceiling. The optional thread-owned high-resolution waitable
timer rounds logical budgets upward in 250 us quanta, retains early UDP and
capture wakes, and cancels its timer after early readiness. No callback work,
spin loop, new dependency or application-wide peer limit was added. Larger
socket sets poll readiness at the configured timer quantum rather than imposing
Windows' event-array capacity as a peer limit. Both Windows wait paths now
populate the existing requested/elapsed/overshoot diagnostics; startup logs
also state the runtime ceiling. These counters measure elapsed wall time,
including scheduling, rather than identifying a kernel cause.

The optimized MSVC Release timing CTest ran 2000 measured idle waits per
condition after 100 warmups, reversing native/timer order on repetition two.
It requested the production 1 ms timer resolution. Results below are the two
repetitions, in microseconds; timing is observational, not a pass threshold:

| Wait / requested budget | Median us | p99 us |
|---|---:|---:|
| Native / 1000 us | 1036 / 1035 | 2265 / 2227 |
| Native / 250 us | 1030 / 1040 | 2199 / 2196 |
| High-resolution / 1000 us | 1562 / 1562 | 2007 / 2042 |
| High-resolution / 250 us | 538 / 542 | 888 / 916 |

Thus lowering the request alone does not remove native timeout rounding, and
changing timer type alone does not improve the median here. The combined
250 us option improves normal wake timing but does not produce actual 250 us
wakes on this machine. Its observed 250 us maxima were 1078 / 1225 us. Thread
CPU and wall totals are retained in the raw results; the Windows CPU counters
are coarse for these short samples and are not a physical-session CPU verdict.

The exactly-four-peer production runtime matrix compared all four combinations
in forward/reverse order, with native capture notifications and fake
32-frame/48 kHz audio. Per-peer maximum loop gaps were 2.421–2.668 ms for native
1000 versus 1.504–1.596 ms for high-resolution 250; median per-peer mean capture
dispatch fell from 40.69 to 24.32 us. This did not establish an audio benefit:
sampled queue medians were 2.47 versus 5.08 ms, and steady underruns were
234/516384 versus 751/904272 observed frames. Synthetic callback intervals also
changed from about 1.98 to 1.13 ms despite the nominal 0.667 ms callback period.
Those unequal synthetic loads limit comparisons to real ASIO behavior.

The separate four-peer authenticated delivery CTest received all **108000**
expected audio packets, with zero invalid packets, duplicates, queue drops or
send errors, including control prefixes, creator rotation and 8 ms batches.
Eight 32-second public `release/jam2.exe` two-peer pulse cases also passed their
existing recording/transport checks. Recorded synthetic pulse RTT was
9.66 / 9.50 ms for native 1000 and 10.02 / 11.49 ms for high-resolution 250;
it does not show a consistent audio latency improvement. No physical ASIO or
Wi-Fi retest was performed, and the real jam's 3.271 ms Windows event remains
unattributed. Keep the option explicit while investigating actual device timing.

Seven focused Release CTests passed, followed by another passing ownership
unit run after adding thread-error reporting and invalid-mode rollback coverage.
The full suite was not run. The first attempted audio CTest lacked the Qt DLL
search path; its registration was corrected before the passing matrix. The
initial timer comparison also enforced a second native timeout on some early
returns; that prototype was corrected to preserve the native single-timeout
behavior, and all measurements above come from the corrected build.

Tested public executable SHA-256:
`005ed58630272d286c75b6f739fae64e4d85ad80ae37ce7cb594c8aceb73a9b7`.
[Focused CTest log](artifacts/windows-wait-20261104/ctest-focused.log),
[final build/unit log](artifacts/windows-wait-20261104/build-final.log),
[raw measurements and per-peer evidence](artifacts/windows-wait-20261104/measurements.json),
[analysis](artifacts/windows-wait-20261104/analyze_results.py), and
[archived pulse profile](artifacts/windows-wait-20261104/retired-source/tests/profiles/windows-udp-wait-local-simulated.json).

## Ten physical ASIO sessions per wait configuration — Nov 4

The follow-up used the connected **TONEX creator and Focusrite USB ASIO joiner**,
with real device callbacks in both public Release processes. All 40 sessions
used 44.1 kHz, 32-frame callbacks, 64-frame PCM16 packets, generated one-second
pulses, an unshaped localhost Direct connection and 32 seconds per session.
Five eight-case blocks ran native-1000, native-250, high-resolution-1000,
high-resolution-250, then the reverse order. This gives ten fresh two-process
sessions per condition, rather than counting pulses within one session as
independent runs. Device roles and the executable remained fixed; no rebuilds
or profiling occurred during the measurements. These hardware results are a
separate dataset from the earlier synthetic recordings.

Recorded callback-audio pulse RTT, in milliseconds (analog converter latency
excluded):

| Wait / runtime ceiling | Sessions | Mean ± sample SD | Median | Min–max |
|---|---:|---:|---:|---:|
| Native / 1000 us | 10 | 5.41 ± 0.67 | 5.77 | 4.42–5.98 |
| Native / 250 us | 10 | 6.60 ± 1.33 | 6.63 | 4.41–8.76 |
| High-resolution / 1000 us | 10 | 6.15 ± 1.19 | 6.25 | 4.44–7.99 |
| High-resolution / 250 us | 10 | 5.81 ± 1.03 | 5.78 | 4.32–7.30 |

The primary comparison pairs high-resolution 250 with native 1000 in each
matching forward/reverse set. The candidate was faster in **6/10** sets, with
median paired difference **−0.125 ms**, but mean paired difference **+0.401 ms**
(positive means slower). Individual paired differences ranged **−1.440 to
+2.676 ms**. The mean differences of the five balanced blocks were −0.130,
−0.283, +0.113, +0.635 and +1.672 ms. A descriptive 95% block-bootstrap interval
for the mean difference was **−0.143 to +1.074 ms** (20,000 fixed-seed resamples,
keeping the two orderings within a block together). These sessions do not
establish a consistent audio-latency benefit or regression. The interval is
about this machine/device pair, not other networks or ASIO drivers.

The wake-timing improvement survives the physical test: across 20 peer streams
per main configuration, the median of per-stream receive-loop gap maxima fell
from **1.962 to 1.396 ms**; the largest recorded maxima were **2.087 versus
1.729 ms**. Mean actual callback intervals remained approximately **0.72562 ms**
for all four configurations. This removes the synthetic callback-cadence
confound from the earlier comparison, while showing that shorter worker gaps
do not necessarily lower end-to-end callback-audio latency. Steady underrun
totals were 83 versus 69 source frames across the respective 20 peer streams;
steady live-tail trimming was zero in both main configurations.

All five focused hardware CTest blocks passed (40/40 sessions, 80 measured ASIO
peer streams). All sessions recorded zero sequence loss; native checks also
passed capture-overflow, authentication, send, recording, packet-flow and
cleanup validation. The full suite was not run. Defaults remain unchanged.
The same public executable hash as the previous experiment was verified in
all five provenance records. The reusable test is `jam2_two_windows_wait_asio`,
enabled by an explicit `JAM2_WINDOWS_ASIO_WAIT_PROFILE`.

[Session plot](artifacts/windows-asio-wait-10runs-20261104/comparison.png),
[all session results and matched differences](artifacts/windows-asio-wait-10runs-20261104/results.json),
[frozen study manifest and raw artifact roots](artifacts/windows-asio-wait-10runs-20261104/study-manifest.json),
[first build/CTest block](artifacts/windows-asio-wait-10runs-20261104/block-1-build-and-ctest.log),
[remaining four CTest blocks](artifacts/windows-asio-wait-10runs-20261104/blocks-2-to-5-ctest.log),
[analysis](artifacts/windows-asio-wait-10runs-20261104/analyze_results.py), and
[archived hardware profile](artifacts/windows-wait-20261104/retired-source/tests/profiles/windows-udp-wait-asio-repeated.json).

## Windows wait experiment removal ? Nov 4

At the user's request, the Windows high-resolution timer experiment and its
`--receive-wait-us` option were removed after the ten-run ASIO comparison.
Windows socket/event waits and the 1000 us runtime ceiling are restored to
`d4d04bf`; macOS critical kqueue deadlines, atomic capture publication and
250 us CoreAudio polling remain unchanged. Experimental native tests and
profiles are removed from active suites. The existing Windows ASIO harness
retains its necessary Winsock include-order fix. CLI regression coverage
rejects the retired flags. Measurements, recordings, plots, profiles and the
experiment patch remain archived under `artifacts/` for reference. Historical
commands above describe the measured build and are not current options.

The restored `release/jam2.exe` was rebuilt with MSVC Release through
`compile.cmd`. Five focused native CTests passed: CLI boundaries (including
retired-option rejection), ingress timing, four-peer ingress boundaries,
mixed socket-work budgets and the four-peer capture/batched-delivery matrix.
CTest discovery confirms zero remaining Windows wait experiment tests.
Public executable SHA-256 after restoration:
`897e82d032549feb0b161fcff0c824c8f08b47d9bfa0290aba36e4fc808eea7d`.
[Build and CLI validation](artifacts/windows-wait-20261104/rollback/build-and-cli.log),
[four-peer validation](artifacts/windows-wait-20261104/rollback/ctest-focused.log),
and [archived experiment patch](artifacts/windows-wait-20261104/retired-source/experiment.patch).
