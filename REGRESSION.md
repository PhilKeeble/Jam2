# Latency stability regression — 2026-11-03

Introducing commit **unproven**; rollback tests also failed. Oct 27's good-run
commit was inferred, not verified. Earlier baseline fixes are committed in `c553a14`
but remain provisional; **user decides keep/revert**, no automatic decision.

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
