# macOS audio investigation — November 3, 2026

**Nov 4 implementation update:** the user selected A2, B and C together. They
are now implemented in the working tree for testing. The baseline-restoration
statements below describe the completed Nov 3 experiments. Current combined
validation is recorded in [REGRESSION.md](../REGRESSION.md#a2--b--c-implemented-for-testing--nov-4).

The connected Focusrite experiments are recorded in
[the candidate comparison](#connected-focusrite-controlled-candidate-experiments),
including real visible GUI sessions, separate hardware clocks and isolated
patches. See the final validation note for failing gates and the restored build.

## What is established

Two distinct problems need separate treatment:

1. **The original CoreAudio deadline misses were real.** macOS's retained HAL
   log confirms 45 skipped cycles in the connected Server Fast interval, seven
   in Server larger, and zero in the two Direct intervals. The affected events
   closely follow a 524.288 ms time grid. Current localhost tests have not
   reproduced those overloads, including when the same engines remain open
   through Direct/server/server/Direct. Their original cause is not established.
2. **The shared source-clock estimator mistakes a sustained arrival-delay step
   for clock drift.** A new deterministic native test reproduces this with
   identical clocks: +3 ms of delivery delay gives approximately −321 ppm at
   10 s and −102 ppm at 30 s. Zero-drift and genuine ±200 ppm controls pass.
   A real CoreAudio server run also exhibited the matching drift/queue-growth
   pattern and then trimmed 70 valid frames with one extra waveform step. A
   controlled real-CoreAudio +3 ms delay experiment reproduced false drift,
   264 trimmed frames, one extra large waveform step and retained queue growth.

This identifies an actionable weakness, not the historical regression's
introducing commit. The lifetime-baseline drift calculation in
`PeerStream::Impl::applyDriftObservation()` dates to `8d08ee0` in August,
before UDP multiplexing. Changed delivery timing can expose older weaknesses.
Experimental audio algorithms were restored after measurement; only diagnostics,
native tests, profiles and review patches remain in the working tree. Existing
acceptance thresholds were not relaxed. The installed `c553a14` candidate remains
provisional.

## Measurement scope of the initial Aggregate stage

- Production source: `0ad7fe3`, including the provisional recovery candidate.
  Release (`-O3 -DNDEBUG`), Apple Clang, staged
  `release/Jam2.app/Contents/MacOS/jam2`; no alternate Jam2 binary.
- macOS 26.4 / 25E246, arm64 execution. Matching local Go signaller from
  Jam2-Server `3f98b00`, built with verified official Go 1.26.10.
- Two localhost streams on existing CoreAudio UID `~:AMS2_Aggregate:0`:
  built-in speakers plus built-in microphone, microphone drift correction on.
  HAL composition was read without changing it.
- 48 kHz / PCM16. Fast: 32-frame callbacks / 64-frame packets. Larger:
  Moderate's complete tuning, 64-frame callbacks / 128-frame packets,
  jitter target 1536 and adaptive 512..4096 frames.
- Fresh mode tests use the public executable. Reuse tests use the GUI's actual
  `SharedSessionController` and `ApplicationRuntime` owners with real HAL
  streams and assert engine counts and capture/playback detach. They do not
  exercise GUI painting or dialogs.
- Tone input is generated inside real callbacks; pulse round trips are
  recovered from recorded callback audio. These are software render round
  trips, excluding physical input conversion, driver output paths, converters
  and acoustics. The device reports approximately 14.5–15.2 ms output and
  approximately 51.8–52.5 ms input latency; those properties are separate from
  the pulse measurement, not an analog latency validation.
- Both streams share one hardware clock. This is useful for detecting false
  drift, but does not validate independent-device drift or Wi-Fi/WAN behavior.

Full provenance, commands, CSVs, tone/pulse stems and analysis are retained in
[`artifacts/mac-investigation`](../artifacts/mac-investigation/),
[`artifacts/coreaudio-delivery`](../artifacts/coreaudio-delivery/) and
[`artifacts/coreaudio-reuse`](../artifacts/coreaudio-reuse/).
See [profile instructions](../tests/profiles/README.md#coreaudio-directserver-and-recovery-investigation)
for repeatable native CTest commands.

## Original overload evidence

The retained log reports `HALC_ProxyIOContext::IOWorkLoop: skipping cycle due
to overload`, with HAL classifications including
`ClientHALIODurationExceededBudget` and `SafetyViolationOccurred`.
These classifications confirm deadline failures; they do not identify the
offending Jam2 function or prove that the callback spent that elapsed time
executing CPU instructions. Jam2's callback-work metric uses elapsed time.
[Apple's CoreAudio explanation](https://developer.apple.com/library/archive/qa/qa1467/_index.html)
also describes preemption as a possible overload mechanism.

The CSV-connected windows align with 45 / 0 / 7 / 0 / 0 logged skipped cycles
for A/B/C/D/E. Using the first event of each affected session as phase zero,
the median absolute residual from a 524.288 ms grid is approximately 1.19 ms
for A and 0.40 ms for C; maximum residuals are 3.47 / 2.69 ms. This is a
periodic clue, not proof of cause. Published
[AWDL protocol research](https://arxiv.org/pdf/1808.03156) describes 16.384 ms
windows, 65.536 ms extended windows and a 1048.576 ms channel sequence:
524.288 ms is compatible with that schedule. Wireless scheduling is therefore
a hypothesis worth correlating with actual OS traces, not a reason to declare
Wi-Fi responsible for the historical regression. No interfaces, AirDrop or
power settings were changed.

A follow-up read found AWDL monitoring activity in the retained wireless log,
but none of the 55 skipped-cycle timestamps in the queried interval has a
logged wireless event within 5 ms. Those logs do not expose every radio or
kernel scheduling operation, so this neither proves nor excludes AWDL.
There is currently no direct trace correlation supporting that attribution.

Average callback work in the original Server Fast / Direct Fast files is
approximately 1.43 / 1.59 µs, despite very different maxima. All five original
sessions report the same active user-interactive network-worker QoS. The
original sessions also have different engine lifetimes: A begins with 85,789
callbacks already counted, C with 94,184, while D begins around 3,006. Maximum
callback counters must not be treated as per-session CPU profiles.

Evidence: [OS log](../artifacts/mac-investigation/original-coreaudio-system-log.log),
[cadence windows](../artifacts/mac-investigation/analysis/original-overload-cadence.json),
[original lifecycle analysis](../artifacts/mac-investigation/analysis/original-mac-lifecycle.json).

## Drift mechanism and real trim event

The production estimator selects a low-delay packet in each observation
window, then compares elapsed remote sample time with elapsed receive time
against a lifetime baseline. A sustained added transit/dequeue delay therefore
appears as a frequency error that slowly shrinks roughly as delay / elapsed
time. A bounded initial calibration protects startup bursts but does not remove
this later-delay-step ambiguity.

In fresh larger `server-tone-b`, receiver 1 reports about −600 ppm at 5 s,
−295 ppm at 10 s and −75 ppm at 38 s, despite sharing its sender's aggregate
clock. Its decoded peer queue grows from roughly 20 to 165 frames. Around
39.1 s it trims 69 frames, then one more; the recorded tone contains one extra
large step. Target stays 512, the adaptive hold is inactive, and overload,
missing-source and underrun counters remain zero. The measured callback-work
maximum around the event is only 22 µs. A later improvement in arrival phase
is consistent with the estimate returning toward zero, but the 250 ms CSV does
not establish a complete instruction-by-instruction causal trace.

The new `jam2_peer_drift_delay_step_units` tests the production `PeerStream`
and its actual resampler request. Controls accurately preserve 0 and ±200 ppm.
For ±3 ms delay steps from 3..40 seconds, the equal-clock cases incorrectly
sustain approximately ±321 ppm at 10 s and ±102 ppm at 30 s, returning to zero
after the step is removed. The desired 25 ppm settling assertion fails. This
test remains a visible unresolved failure, not an expected-failure pass.

Evidence: [native reproduction](../artifacts/mac-investigation/drift-native.log),
[trim interval](../artifacts/mac-investigation/analysis/larger-trim-events.json).

### Controlled real-device reproduction

The two-stream real-CoreAudio experiment uses identical clocks and the same
localhost proxy for both cases. The control adds no delay. The second case
adds 3 ms to one audio direction from 3..40 s, then removes it. Both use
64-frame callbacks / 128-frame packets and the larger profile.

| Receiver 1 | No-step control | +3 ms delay step |
|---|---:|---:|
| Drift correction at ~10 s | −4.47 ppm | −330.81 ppm |
| Drift correction at ~30 s | −2.86 ppm | −108.22 ppm |
| Steady median software queue | 38.750 ms | 42.000 ms |
| Steady valid frames trimmed | 0 | 264 |
| Recorded large waveform steps | 0 | 1 |
| Steady shortage frames | 0 | 0 |
| CoreAudio overloads | 0 | 0 |

After the step is removed, the estimator returns near zero but receiver 1
still holds approximately 42 ms through ~57 s, about 3.25 ms over the control.
The reverse receiver also retains about 3 ms more queue despite receiving no
injected delay; inspect both directions rather than assuming isolated startup
states are identical. The comparison fails latency and audio-quality gates.
The 264 trimmed frames represent 5.5 ms of source material, not 5.5 ms of
measured silence. Both cases have startup shortages before the steady window;
the claim of zero shortages here concerns steady counter deltas.

This device experiment is not a perfectly controlled scheduler: the delayed
case's proxy pump also has a 9.479 ms high-water gap by elapsed 3.5 s, versus
1.786 ms during the control's active measurement (a later 9.300 ms control
gap occurs during shutdown). The unchanged target 512, zero sequence loss
and zero hard-recovery count on the stressed receiver are recorded alongside
the result. The exact deterministic unit test separates the estimator's
delay-step defect from these additional scheduling variations. The device
comparison reproduces the failure pattern, not a uniquely apportioned count
of frames attributable to the intentional 3 ms alone.

The original spontaneous tone step is −237 PCM16 units with peak 410, at
absolute engine frame 1,866,816. It falls between CSV observations where
trimming rises from 0 to 69 frames. The controlled step is −401 units, at
absolute engine frame 1,924,738, between observations where trimming rises
from 133 to 262. Recording sidecars report zero dropped frames and writer
errors. This links the recorded discontinuities to the trim intervals, rather
than just associating a whole-file count with a run.

Evidence: [control/comparison](../artifacts/coreaudio-delivery/20261103-222719-169-two-coreaudio/),
[waveform event positions](../artifacts/mac-investigation/analysis/waveform-step-positions.json),
[hardware follow-up CTest log](../artifacts/mac-investigation/hardware-followup.log),
[wireless trace comparison](../artifacts/mac-investigation/analysis/overload-wireless-correlation.json).

## Fresh-engine mode measurements

Queues below are per-receiver median `total_current_buffered_ms`: jitter plus
peer plus output-ring storage, excluding transit and reported hardware latency.
Shortages are missing source frames counted by the output callback, not a
measurement of audible silence. Do not add padding and trimming to them.
The native runner uses the upper middle observation for its median, excludes
elapsed time before 10 s and the last two connected seconds, and computes p99
from sorted samples. WAV analysis excludes the first three recorded seconds
and the last second. CSV sampling is 250 ms and small-callback phase matters.

### Fast: 32 / 64 frames

| Case | Queue ms, receivers 0 / 1 | Steady shortage frames | Steady trimmed frames | Render round trip ms | Comparison |
|---|---:|---:|---:|---:|---|
| direct-tone-a | 4.000 / 4.000 | 0 / 0 | 0 / 0 | — | control |
| server-tone-a | 2.542 / 4.021 | 6 / 0 | 0 / 0 | — | fail |
| server-tone-b | 2.667 / 2.667 | 0 / 1 | 0 / 0 | — | fail |
| direct-tone-b | 2.062 / 2.062 | 0 / 0 | 0 / 0 | — | fail |
| direct-pulse-a | 0.688 / 0.708 | 0 / 0 | 0 / 0 | 2.729 | control |
| server-pulse-a | 0.688 / 0.729 | 0 / 0 | 0 / 0 | 4.083 | fail |
| server-pulse-b | 0.750 / 0.729 | 0 / 28 | 0 / 0 | 2.812 | fail |
| direct-pulse-b | 0.104 / 0.104 | 0 / 0 | 0 / 0 | 2.875 | pass |

[Retained matrix and per-case evidence](../artifacts/coreaudio-delivery/20261103-215902-900-two-coreaudio/).

### Larger: 64 / 128 frames

| Case | Queue ms, receivers 0 / 1 | Steady shortage frames | Steady trimmed frames | Render round trip ms | Comparison |
|---|---:|---:|---:|---:|---|
| direct-tone-a | 37.438 / 38.750 | 0 / 0 | 0 / 0 | — | control |
| server-tone-a | 38.750 / 38.812 | 0 / 0 | 0 / 0 | — | pass |
| server-tone-b | 38.750 / 41.833 | 0 / 0 | 0 / 70 | — | fail |
| direct-tone-b | 38.771 / 38.812 | 0 / 0 | 0 / 0 | — | fail |
| direct-pulse-a | 38.750 / 38.812 | 0 / 0 | 0 / 0 | 80.208 | control |
| server-pulse-a | 38.688 / 38.688 | 0 / 0 | 0 / 0 | 80.042 | fail |
| server-pulse-b | 40.021 / 38.729 | 0 / 0 | 0 / 0 | 80.083 | fail |
| direct-pulse-b | 38.729 / 38.729 | 0 / 0 | 0 / 0 | 80.125 | fail |

[Retained matrix and per-case evidence](../artifacts/coreaudio-delivery/20261103-220550-265-two-coreaudio/).

All 16 fresh cases report zero CoreAudio overloads. Each matrix fails five of
its six strict comparisons with the first Direct tone/pulse controls. This is
an equality gate: a latency decrease beyond tolerance also fails. Per-direction
median/p99 tolerance is one callback (Fast 0.667 ms, larger 1.333 ms); pulse
round-trip tolerance is two callbacks. Additional steady shortages, trimming or
tone discontinuities are rejected. These thresholds were not relaxed.

Interpret the failures individually. Fast includes changed startup depths and
steady shortages of 6 / 1 / 28 frames in three server receiver-runs. A Direct
repeat also differs substantially from its first Direct control. Larger
`server-tone-b` has the real 70-frame trim and extra waveform step described
above. The larger pulse round trips differ by only 0.167 ms at most; their
failures concern p99 software queues about 1–4 frames beyond the one-callback
equality tolerance. One p99 field in the larger tone comparison is additionally
a CSV-rounding boundary, but that case has independent quality failures.
Neither matrix is a passing golden baseline or proof of a uniform server tax.

## Persistent engines and recovery

Both complete two-contract reuse matrices passed lifecycle assertions:
unchanged engines start once per participant/contract, do not restart, detach
network capture/playback on close, and keep local callbacks advancing. No
CoreAudio overloads occurred. First matrix:
[`215457-682`](../artifacts/coreaudio-reuse/20261103-215457-682/).
The later matrix verifies the final harness's strategy metadata and ownership.
Its evidence is retained under
[`222314-668`](../artifacts/coreaudio-reuse/20261103-222314-668/).
A lifecycle pass does not substitute for an audio-quality comparison: the first
Fast Direct repeat had 63 / 160 steady shortage frames. Larger reuse queues
were about 39–40 ms, with zero steady shortages in that first matrix.

Both constructed hold cases pass packet-flow, tail-buffer and post-120-second
quiet gates. One direction gets 4/8/16 ms holds, followed by five 95 ms holds
at 22/24/26/28/30 s; delivery then remains clean for over two minutes.
These are synthetic delivery envelopes, not replayed Wi-Fi captures.

| Stressed receiver | Initial queue ms, 3..7 s | Settled queue ms, 90..120 s | Connected shortage frames | Trimmed valid frames | Hard recoveries | Largest observed arrival gap ms |
|---|---:|---:|---:|---:|---:|---:|
| fast-holds | 0.771 | 2.667 | 8545 | 3433 | 5 | 91.352 |
| larger-holds | 38.750 | 38.750 | 3876 | 21000 | 3 | 98.756 |

The larger profile trades fewer shortage frames for much more valid-audio
trimming during stress; these counts overlap different correction mechanisms
and must not be summed into an audible dropout duration. After stress, target
returns to 512, the hold expires and the actual queue returns near its earlier
39 ms baseline. Fast returns to target 64 with an expired hold, but retains
about 1.9 ms above its initial queue. No new shortage/trim/recovery events occur
after 120 s. Retention is bounded here; complete retirement is not established.

A large pending-recovery ledger is not itself measured queued latency: Fast's
stressed receiver retains roughly 510 credited frames while its sampled actual
queue is only about 128 frames. Disconnect cancellation must not be counted as
retirement through playback.

Partial releases are disabled by the existing larger-profile guard. Its clean
reuse measurements have about 1–125 decoded peer frames, 448–512 output-ring
frames and 1408 jitter frames. Those measurements do not support attributing
the old Wi-Fi run's ~65 ms output storage to partial-release suppression.
There is no guard relaxation in this change. CSV snapshots also cannot prove
how many decoded frames were available at an individual sub-millisecond
underrun; that remains a useful event-capture diagnostic before changing guards.

[Hold evidence](../artifacts/coreaudio-delivery/20261103-221238-391-two-coreaudio/),
[window analysis](../artifacts/mac-investigation/analysis/current-results.json).

## Native validation and remaining work

The focused Release runs pass callback-processing, CoreAudio device-setup and
recording-timing units, recovery-accounting units, repeated/large recovery,
core boundary checks and private-aggregate creation/input-off/cleanup/rollback.
Final native recheck passes the four existing selected unit tests; only the
new delay-step reproduction fails. Final Release compilation passes and the
public app's SHA-256 remains unchanged throughout the investigation.
The following exactly-four-peer workflows pass:

- `jam2_four_mode_audio_latency_integration`
- `jam2_udp_four_receive_models_performance`
- `jam2_oct30_four_peer_delivery_integration`
- `jam2_multiplex_four_session_lifecycle_integration`

The fake-audio mode test measures median total GUI breakdown 6.3 ms in both
modes; server minus Direct p95 is +1.1 ms, inside its 2.667 ms callback budget.
The receive-model comparison also passes. These use 128-frame synthetic
callbacks and are separate coverage from the small real CoreAudio callbacks.
Per-process CPU measurement is unavailable on this Mac test backend; do not
interpret its aggregate zero CPU field as measured zero CPU usage.

The documented `jam2_audio_flow_startup-recovery`,
`jam2_core_playback_recovery_units` and `jam2_core_release_return_units` still
fail. Startup retains about 34 frames over its equal-underrun control; the
other failures include non-clearing learned holds and short-batch drift/release
behavior. None was weakened. The new delay-step unit test additionally fails
its desired 25 ppm bound. Initial sandbox socket failures were rerun with real
localhost access and are not product failures. The initial incomplete hardware
attempt is retained separately and excluded from the matrices above.

No full suite or source coverage was run. The next optimization target supported
by new evidence is source-clock estimation across persistent arrival-offset
changes, while preserving genuine clock-skew, loss/reordering and four-peer
coverage. Startup retirement is a separate confirmed weakness. The periodic
original HAL misses still require an actual OS/radio/GUI correlation; the
localhost and controller-reuse tests do not resolve that cause. This work does
not accept/revert the provisional candidate or establish a verified historical
good/bad boundary.

Additional retained validation: [build/test ledger](../artifacts/mac-investigation/post-results.json),
[final native recheck](../artifacts/mac-investigation/final-native.log),
[four-peer output](../artifacts/mac-investigation/four-peer.log),
[provenance](../artifacts/mac-investigation/provenance.json).
Native CTest artifacts were also copied to
[`native-artifacts`](../artifacts/mac-investigation/native-artifacts/)
so a subsequent `compile.sh` workspace reset does not remove this evidence.

## Connected Focusrite: controlled candidate experiments

The subsequent user-authorized experiments use the connected Scarlett 2i2 USB,
UID `AppleUSBAudioEngine:Focusrite:Scarlett 2i2 USB:Y82G60M961344A:1,2`, and the
existing Aggregate UID `~:AMS2_Aggregate:0`. Device indexes changed on insertion;
all hardware profiles resolve stable UIDs. Raw evidence and isolated candidate
patches are under `artifacts/mac-candidates-20261103/`. These experiments do not
accept a production optimization automatically.

### Baseline drift controls

Eight 60-second cases use two processes on the same device clock, 48 kHz,
64-frame callbacks, 128-frame PCM16 packets, Moderate tuning, explicit
1536-frame jitter target and 512-frame adaptive minimum/target. The four cases
per device are correction on/off, each with clean forwarding or a +3 ms forward
arrival-delay step from 3 to 40 seconds. A 251 ms stats interval avoids sampling
only at integer callback multiples. This switches Jam2's **network** drift
correction, not Apple's aggregate-subdevice compensation. HAL configuration was
read separately: Aggregate speaker drift is off; microphone drift is on, quality
127. No system device composition or drift property was changed.

The baseline public executable SHA-256 is
`119916d91bda2fd25cbb7c46f7fe00069093619c2e271db68c8149792f12b23a`.
Each row below is the delayed receiver; steady metrics begin at 10 seconds and
exclude the last two seconds before shutdown.

| Device | Network correction | Drift near 10 s / 30 s (ppm) | Median buffered (ms) | Steady trimmed frames | Recorded waveform large steps |
| --- | --- | --- | --- | --- | --- |
| Aggregate | on | -674.89 / -212.59 | 42.0000 | 338 | 3 |
| Aggregate | off | -348.44 / -112.93 | 36.0000 | 0 | 0 |
| Focusrite | on | -353.46 / -116.13 | 41.9375 | 155 | 1 |
| Focusrite | off | -359.59 / -111.18 | 39.8542 | 0 | 0 |

Drift still appears in stats with correction off; the correction option controls
the resampler request. All eight cases report zero HAL processor overloads,
invalid callback timestamps and packet sequence loss. The Focusrite on-clean
control independently has 192/64 steady playback-shortage frames and 1/2 waveform
steps across its peers, with zero HAL overloads. The stepped on cases have no
steady playback-shortage frames. These counters describe different failure
layers and must not be conflated. Proxy pump stalls and initial receive offsets
vary between runs, so hardware differences alone do not isolate a pure 3 ms
step; the deterministic native delay-step test provides that isolation.

The matched comparison gate fails some baseline cases. Its assertions were
retained, and failing CTest status is recorded in `baseline-ledger.json` rather
than presented as a passing performance claim. Turning correction off is a
shared-clock diagnostic; these runs do not justify disabling it for independent
oscillators.

[Machine-readable measurements](../artifacts/mac-candidates-20261103/measurements.csv),
[baseline provenance](../artifacts/mac-candidates-20261103/baseline-ledger.json),
[HAL readback](../artifacts/mac-candidates-20261103/hal-focusrite-connected.json).

### Real visible GUI sessions, both devices

`jam2_coreaudio_real_gui_hardware` now drives the actual Start/Join modal actions
with physical audio, two visible windows, isolated storage, diagnostics, a quiet
-60 dB master level, and a localhost-only signaller override. Unlike the earlier
controller-only reuse test, this includes painting and the real dialog path.
Each device runs Direct/server/server/Direct at Fast (32/64 callback/packet
frames) and Moderate (64/128), at 48 kHz PCM16, holding each session 30 seconds.

Both eight-case native CTests pass. Across 32 peer/case measurements: zero HAL
overloads, zero invalid callback timestamps, zero callback gaps above twice the
expected period, and zero steady playback-shortage or live-tail-trim growth.
The saved CSVs verify actual callback sizes, not just requested engine settings.
Maximum observed callback work is 50 us on Aggregate and 47 us on Focusrite.
These maxima are cumulative within each reused physical stream. Each peer has
one engine start across Fast mode changes; switching to Moderate adds one
restart, as required by the changed callback contract. Later mode changes reuse
that engine. Stable queue medians range from 0..0.667 ms in Fast and
38.646..38.688 ms in Moderate across modes/devices. A near-zero sampled queue
is not zero audio latency: packet collection, callbacks and driver latency are
separate stages.

The OS unified-log query from 23:05:20..23:13:56 UTC also contains no Jam2
CoreAudio overload message. The original Wi-Fi overloads remain real and
unresolved; these results do not establish a radio or scheduling cause. They do
show that GUI painting, ordinary mode changes, the physical Aggregate and the
Focusrite are insufficient to reproduce those misses on localhost.

The audio implementation is baseline, with only the new debug/test surface;
public app SHA-256 is
`3da49474dd221f768f886dc53c8c26bf17dc59db42d699b5f1b763d92f9b9e6d`.
Early harness setup attempts are excluded. Fixes to the test reader, format
label and in-memory Start-dialog rendezvous override were completed before the
accepted matrices; no production audio correction was applied to obtain these
results. In particular, the first server pilot used the default rendezvous
rather than the intended local override and could not connect; the accepted
matrix uses the corrected local override.

[GUI provenance](../artifacts/mac-candidates-20261103/baseline-gui-ledger.json),
[GUI measurements](../artifacts/mac-candidates-20261103/gui-measurements.csv),
[OS overload query](../artifacts/mac-candidates-20261103/gui-unified-overloads.log).

### Focusrite fresh-engine tone/pulse mode matrix

The fresh-engine ABBA comparison was repeated on Focusrite for both contracts:
30-second tones and 20-second pulses, 251 ms stats, unchanged mode-equivalence
and recorded-audio assertions. The audio implementation remains baseline with
the GUI test surface. This adds 16 recorded hardware cases, separate from the
16 visible GUI sessions above.

| Focusrite contract | Direct pulse RTTs (ms) | Server pulse RTTs (ms) |
| --- | --- | --- |
| Fast, 32 callback / 64 packet | 10.7292, 3.3542 | 8.0833, 10.7500 |
| Moderate, 64 callback / 128 packet | 83.0104, 83.4479 | 82.3542, 83.6354 |

These are recorded render round trips, excluding analog converters. Reported
Focusrite output-driver paths total 3.833 ms across both peers and are saved
separately. Both profile CTests fail the strict comparison gate: four of six
comparisons per profile exceed at least one bound. Fast uses a 0.667 ms queue
bound and 1.333 ms round-trip bound; Moderate uses 1.333/2.667 ms. Improvements
also fail an equivalence assertion; these failures are not all regressions.
Direct's own repeat differs strongly in Fast. No fixed mode penalty follows
from this small, startup-sensitive matrix.

The Fast 10.729/10.750 ms cases hold approximately 4 ms per peer, while the
3.354 ms Direct repeat holds 1.333/0.688 ms. The slower cases also have startup
padding and learned gap holds: near the end of the 20-second recordings, about
14 seconds of their 30-second hold remain after 9.7..12.5 ms arrival gaps.
They do not establish that a hold never expires. The existing longer injected
recovery experiment and deterministic startup tests address longer retention.
All Fast cases have zero steady shortage/trim growth.

Moderate server tone B has 25 steady trimmed frames, despite zero steady
shortages and zero HAL overloads. The affected peer falsely reports approximately
-125..-103 ppm while trimming, although both processes share the Focusrite
clock. This reproduces the drift/queue lead without an injected delay. All
16 mode cases report zero HAL processor overloads and invalid timestamps.

[Mode run provenance](../artifacts/mac-candidates-20261103/modes-baseline-ledger.json),
[measurements](../artifacts/mac-candidates-20261103/measurements.csv).

### Candidate B: absolute post-batch padding goal

This isolated patch records a recovery runway goal rather than adding the old
deficit to whatever real audio arrives before `finishReceiveBatch()`. Across
all six deterministic same-batch startup comparisons, extra retained latency
falls from 34 frames (0.771 ms) to zero; unnecessary same-batch padding falls
from 64 frames to zero. An empty drained batch still inserts the required
64-frame cushion, but subsequently retains the same 34 extra frames. This is
therefore not a complete padding-retirement fix.

Clock-flow, callback-flow, repeated/large recovery, recovery accounting and
core-boundary passing controls remain passing. The low-skew and release-return
failure counts match baseline (96 and 18); learned-hold recovery failures also
remain. Five additional assertions fail: three in the callback-runway test
expect the old preserved post-arrival silence/depth, and two isolated-shortage
checks expect the old 1/64 padding quantities. These are observable contract
disagreements, not five independently demonstrated audible dropouts. Existing
assertions were not rewritten to accept the candidate. The unchanged settled
clock test still has its four pre-existing starvation/continuity failures.

Candidate B was reverted after native comparison and was not promoted to a
hardware/default fix. Its measured benefit and unresolved retirement/policy
tradeoff are retained for the user's decision.

[Candidate B patch](../artifacts/mac-candidates-20261103/patches/B-absolute-startup-padding.patch),
[native log](../artifacts/mac-candidates-20261103/B-native-tests.log),
[result](../artifacts/mac-candidates-20261103/B-native-result.json).

### Candidate C: real partial packets with a larger minimum

The isolated one-line patch removes the `adaptive_min_frames > block_size`
exclusion from real common-prefix release. The new native four-peer test passes
all nine 127/128/129-frame, output-cap and full-output cases, including completion
by the later 128th frame. For example, with 384 real frames already in playback,
127 available common source frames produce 511 playback frames instead of staying
at 384; with 450 preceding frames, only the available 62-frame output capacity
is filled. No padding, source/timeline debt, drops or sample changes are allowed.

All seven baseline passing native controls remain passing; the existing startup,
low-skew, learned-hold and release-return failure counts are unchanged. This is
an improvement in access to already-buffered real runway. Moving frames between
queues does not itself lower total buffering, and no hardware latency benefit
has been demonstrated for C alone. The candidate was reverted for isolation
before testing the drift changes.

[Candidate C patch](../artifacts/mac-candidates-20261103/patches/C-larger-partial-release.patch),
[native log](../artifacts/mac-candidates-20261103/C-native-tests.log).

### Candidate A: recent-window drift estimator rejected

A fixed 16-observation median of recent slopes passes the new 64/128-frame
arrival-step tests, including known ±200/±500/±1000 ppm controls. However, it
fails the previously passing callback-flow clock test: 18 genuine-rate and 18
relative-resampling assertions fail, including a true 17 ppm skew observed as
zero with quantized worker timing. Eight additional clean/startup assertions
also fail. A short robust window can discard timing changes that carry real
slow-clock information. This version was reverted and received no hardware
promotion; passing the isolated delay-step test was insufficient.

[Rejected candidate A patch](../artifacts/mac-candidates-20261103/patches/A-recent-drift.patch),
[native log](../artifacts/mac-candidates-20261103/A-native-tests.log).

### Candidate A2: lifetime drift with explicit arrival-offset accounting

This alternative retains the original long-term frequency baseline. Between
quarter-second low-delay observations, it subtracts the expected interval at
the previous clock estimate; an abrupt residual of at least 2000 us is accounted
as an arrival-offset change. The threshold is a typed `PeerStreamConfig` value
(zero disables accounting). CSVs append the applied threshold, adjustment count
and signed accumulated offset. Aggregate CSV counts sum peer events and retain
the signed largest-magnitude peer offset. Packet headers and RT callbacks do
not change; the bounded arithmetic runs on the network thread.

The expanded native test covers 64/128-frame packets, zero and known clock
rates, ±3 ms steps, deterministic jitter, correction caps, offset counters and
an explicitly disabled legacy negative control. It passes. The quantized
17 ppm callback-flow control that rejected A also passes. In the ten-test native
unit/clock comparison, the failure set matches baseline: startup's 18 assertions, low-skew's 96,
learned-hold recovery's six and release-return's 18 remain. Strict CSV
periodic/final widths, values and signed field copying also pass. This is a
prototype with a 2 ms threshold, not a demonstrated solution to arbitrary delay
ramps, all startup offsets or the original HAL misses.

Matched 60-second Moderate cases use the same profiles and correction-on
settings as baseline, changing only this algorithm and its diagnostic columns:

| Delayed receiver | Baseline | A2 |
| --- | --- | --- |
| Aggregate drift near 10 s (ppm) | -674.893 | -19.908 |
| Aggregate steady trimmed frames | 338 | 0 |
| Aggregate recorded large steps | 3 | 0 |
| Aggregate median queued (ms) | 42.0000 | 36.3542 |
| Focusrite drift near 10 s (ppm) | -353.456 | -5.850 |
| Focusrite steady trimmed frames | 155 | 0 |
| Focusrite recorded large steps | 1 | 0 |
| Focusrite median queued (ms) | 41.9375 | 41.3125 |

All four A2 shared-device cases have zero steady shortage growth and HAL
overloads. The clean Focusrite receiver still has one recorded waveform step
and a 43.313 ms steady median; startup/recovery retention remains. Initial
arrival offsets and proxy scheduling vary between runs, so individual queue
differences are not precise algorithm-only latency effect estimates. The
native known-clock experiment isolates the offset/frequency distinction.

Both A2 hardware profile CTests still fail their unchanged absolute queue
comparison bounds. Aggregate's delayed queue is lower than its clean control
by more than one callback; Focusrite's delayed/control queues also differ by
more than that bound. Neither A2 failure reports increased steady underruns,
trimming or waveform discontinuities against its clean control. Those failing
statuses were retained rather than redefined as passing performance tests.

Both independent-device direction controls pass on A2, as on baseline. Their
four peer measurements have zero steady shortages, trims, waveform large steps
and HAL overloads. Individual queue medians still vary, from 38.708 to 42 ms on
A2 versus 37.938 to 41.938 ms on baseline. This checks real separate device
clocks; it does not prove arbitrary oscillator behavior from only these clocks.

An additional matched Focusrite Fast pulse ABBA on A2 measures Direct
2.7708/9.4167 ms and Server 6.7500/6.7292 ms. All three strict comparisons to its
first Direct control fail. Baseline measured Direct 10.7292/3.3542 and Server
8.0833/10.7500 ms. These short runs still show substantial startup dependence;
A2's trimming improvement is not evidence that Fast latency is now stable or
that one mode has a fixed overhead.

Public A2 app SHA-256:
`1bb71777333e008f9abbee0b87bf4f6994e9b9b931efcc24222a69bee631095a`.

[A2 isolated patch including counter tests](experiments/mac-audio-2026-11-03/A2-arrival-offset-accounting.patch),
[native comparison](../artifacts/mac-candidates-20261103/A2-native-tests.log),
[CSV/counter checks](../artifacts/mac-candidates-20261103/A2-csv-counter-checks.log),
[matched hardware provenance](../artifacts/mac-candidates-20261103/A2-ledger.json),
[independent-clock provenance](../artifacts/mac-candidates-20261103/mixed-A2-ledger.json),
[Fast pulse provenance](../artifacts/mac-candidates-20261103/latency-A2-ledger.json).

### Final validation and review state

The final A2 validation group passes seven of eight CTests: the four-peer
lifecycle, receive-model and impairment-delivery workflows, repeated/large
recovery, drift/counter units and CLI/CSV units. The four-peer mode-latency
comparison fails its unchanged symmetric p95 bound. A restored-baseline repeat
also fails that gate:

| Build | Direct median / p95 (ms) | Server median / p95 (ms) | Server minus Direct p95 |
| --- | --- | --- | --- |
| A2 | 6.7 / 11.5 | 6.5 / 8.0 | -3.5 ms |
| Restored baseline | 6.7 / 10.7 | 5.4 / 7.7 | -3.0 ms |

There are 12 directed paths and 192 samples per mode. The allowed absolute
median difference is 1.333 ms; the p95 bound is 2.667 ms. Both fail because
Server's measured tail is lower by more than that bound. This is a real failed
equivalence assertion; it neither demonstrates a Server regression nor isolates
A2 as its cause. Startup/scheduling variance and the gate's sampling contract
remain unresolved. No assertion or threshold was changed to make it pass.
The four-peer tests use synthetic 128-frame callbacks and do not profile HAL.

The restored Release build and CoreAudio setup check pass. Final CSV units pass.
The desired native delay-step and larger-prefix reproductions fail again on
baseline, as expected; their failed conditions were not hidden with `WILL_FAIL`.
The selected startup, learned-hold and skew issues described above also remain.
No full CTest suite or source coverage was run.

All six saved production audio/CLI source files compare byte-for-byte with the
pre-candidate baseline, including the Mac backend, mixer, stream and stats code.
The staged public app is rebuilt as baseline audio plus the retained diagnostic
GUI surface. Final SHA-256 is
`239fe16ddb84fbdb9a26f1557c68846916a5da39e3cd5f16d0e499cabbe448ae`.
Builds embed `__TIME__` in automation build information, so a rebuild hash is
not a substitute for the saved source-equality checks. The temporary experiment
profile cache is cleared. A2, A, B and C are all unapplied review artifacts;
the pre-existing provisional `c553a14` behavior is unchanged.

| Candidate | Demonstrated result | Review conclusion |
| --- | --- | --- |
| [A: recent slopes](experiments/mac-audio-2026-11-03/A-recent-drift.patch) | Fixes step fixture, loses genuine small skew and fails new controls | Rejected version |
| [B: absolute padding](experiments/mac-audio-2026-11-03/B-absolute-startup-padding.patch) | Removes 34-frame same-batch penalty; empty-batch retention and five policy assertions remain | Partial result; recovery contract needs a decision |
| [C: larger prefixes](experiments/mac-audio-2026-11-03/C-larger-partial-release.patch) | Nine prefix/cap cases pass; baseline passing controls retained | Correctness candidate; no measured total-latency benefit |
| [A2: offset accounting](experiments/mac-audio-2026-11-03/A2-arrival-offset-accounting.patch) | Controlled-step trimming 338/155 to zero on Aggregate/Focusrite; known-clock controls retained | Strongest false-drift lead; startup and latency stability still unresolved |

Patches apply on top of the retained diagnostics/native-test working tree;
apply one candidate at a time for the comparisons above. Combined candidates
were not tested. A2 includes its CSV and native counter-control test changes.
All patches pass `git apply --check` against the restored working tree.
No candidate has Windows or full-suite distribution validation here.

[Final source/build provenance](../artifacts/mac-candidates-20261103/final-baseline-provenance.json),
[A2 four-peer/recovery validation](../artifacts/mac-candidates-20261103/A2-four-peer-and-recovery.log),
[restored-baseline checks](../artifacts/mac-candidates-20261103/final-baseline-checks.log),
[retained native data](../artifacts/mac-candidates-20261103/native-artifacts/).

Cleanup audit: all 52 hardware cases (104 peer/case rows across the GUI and CLI
measurements) report zero HAL overloads. Final HAL readback confirms unchanged
device compositions and aggregate-subdevice drift settings. No test client or
local signaller remains running. Final native data is preserved separately in
[`final-native-artifacts`](../artifacts/mac-candidates-20261103/final-native-artifacts/).
