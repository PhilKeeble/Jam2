# Latency stability regression — 2026-11-03

Introducing commit **unproven**; rollback tests also failed. Oct 27's good-run
commit was inferred, not verified. Current fixes are committed in `c553a14`
but remain provisional; **user decides keep/revert**, no automatic decision.

## macOS handoff: Nov 3 Wi-Fi and local Direct/server comparison

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
