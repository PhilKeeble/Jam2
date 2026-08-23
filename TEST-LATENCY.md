# Jam2 latency review and tuning log

Last updated: 2026-08-23

## Objective and guardrails

Reduce two-peer live audio latency as far as the local headless path can sustain while preserving current audio quality, direct-mesh protocol behavior, split component ownership, and normal Jam2 functionality.

- Make only small, evidence-backed production changes during this exercise.
- Do not change the wire protocol or network audio quality to obtain latency improvements.
- Separate code-path improvements from fast-profile buffer tuning so their effects remain attributable.
- Validate iterations with repeatable two-peer headless jams, CSV statistics, and recorded WAV continuity rather than subjective scores.
- The original profile-sweep phase did not run CTests. This follow-up shared-path implementation pass runs only focused native owner and four-peer checks; the full suite remains deferred.
- Record major or structurally risky proposals here instead of implementing them.

## Starting evidence

The older stable jam was reported to feel nearly simultaneous. The later jam used the same configuration, network, and cabling but showed roughly 10 ms higher RTT and a peer playback queue that reached approximately 5,632 frames (117.3 ms at 48 kHz). The large playback backlog, rather than the RTT difference alone, is sufficient to explain the other musician sounding substantially behind.

The initial review found two concrete latency mechanisms:

1. The shared mixer preserved an all-peer-empty interval indefinitely until audio returned. Delayed catch-up packets could therefore be replayed instead of being classified as obsolete live audio.
2. The jitter buffer used a permanent first-packet wall-clock anchor and linear scans. Long-running clock differences could distort its release condition and add unnecessary work.

## Implemented before this tuning exercise

- The mixer advances a missed live deadline after an observed device underrun, accounts for late frames, and retains only a two-packet live tail when a delayed source recovers.
- The network runtime advances the mixer timeline before draining a queued UDP batch, preventing receive-thread stalls from making old catch-up audio current again.
- Jitter storage is now a preallocated FIFO with constant-time depth/release operations, local residence-time expiry, and explicit discontinuity rebasing.
- Ping timestamps are captured immediately before each peer send rather than at the start of a potentially busy network-loop iteration.
- CSV diagnostics now expose current/high-water peer queues, current peer-path depth, receive-processing duration, ping-reply turnaround, live-tail trimming, and jitter release/rebase causes.
- The wire protocol and audio encoding are unchanged.

## Validation already completed

Before the request to stop running CTests, focused native coverage, all 43 unit tests, and all 12 network-labelled tests passed. Four-peer clean, burst-loss, authentication, sequence-security, jitter drift, reordering, timeout, discontinuity, and stale-catch-up cases passed. A listener-compensated burst-loss run recovered with a 1,600-frame peer path (33.3 ms), far below the later jam's approximately 5,632-frame peer queue.

The performance-labelled run originally passed 31 of 33 cases. The listener-compensated latency assertion was corrected and its focused rerun passed. `jam2_four_performance_integration` still needs a later independent rerun; it timed out in the WAV recording-import worker-saturation scenario and was not an audio-path failure.

## Static review findings

### Initial high-value candidates

- At review start, the fast profile combined a 512-frame jitter target (10.7 ms), 256-frame playback prefill/cushion (5.3 ms), packetization, and device callback latency.
- Jam recording scanned every sample of all five stems in the real-time callback to count nonzero PCM16 frames. Its background writer then called `std::ofstream::write` once per 16-bit sample.
- A received audio packet was copied through multiple fixed 256-sample arrays on its direct, jitter, and reorder paths.
- The shared mixer always performs floating-point linear interpolation even while its resampler ratio is exactly 1.0. An exact-unity fast path is possible, but its state transitions must remain bit-identical and will only be attempted after lower-risk recording and packet-copy changes are measured.
- Periodic operational snapshots and CSV aggregation run on the network worker. They are outside the real-time callback but can delay packet draining; the new receive-processing and ping-turnaround measurements can quantify this before ownership is changed.
- Four-peer mixing waits for the slowest active contributor until a deadline. This matters for mesh scaling but is less relevant to the primary two-peer case.
- Kernel socket-queue residence is not currently observable. Kernel receive timestamps could improve attribution and drift estimation, but adding a cross-platform timestamp abstraction would be a larger change and is deferred.

### Low-probability causes

- Typed interfaces and split ownership do not themselves appear in the sample loop and have not changed packet shape.
- Packet storage, jitter storage, mixer queues, and audio callback scratch buffers are preallocated.
- `NetworkSession` already encodes into one preallocated bounded transmit packet and reuses it for every active peer. The suspected per-packet allocation is not present and no change is needed there.
- Linear peer lookup is negligible for the expected two-to-four-peer mesh and does not explain tens of milliseconds of delay.

## Iteration method

1. Build the normal optimized `release/jam2.exe` without invoking CTest.
2. Run two headless peers with the fast profile, 48 kHz/64-frame packets, statistics every 100 ms, and retained recordings.
3. Use a continuous 440 Hz run to inspect frequency, RMS stability, clipping, discontinuities, silence runs, and broadband/transient residuals.
4. Use a one-second pulse run to measure unambiguous delivery delay, missing/duplicate intervals, and stale replay.
5. Compare queue/path depth, jitter, playback-ring occupancy, RTT, ping turnaround, receive processing, callbacks, underruns, drops, deadlines, and WAV measurements.
6. Change one code path or one numeric profile control at a time, rebuild, repeat the same run, and keep a change only when latency improves without audio degradation or material functional cost.

## Iteration results

### Run 0: rejected 32-frame synthetic baseline

Artifacts: `latency-runs/baseline-current-full/`

Configuration: two loopback peers, fast profile, 48 kHz, 64-frame PCM24 network packets, requested 32-frame headless callback, 440 Hz input, high process/MMCSS priority, 1 ms Windows timer resolution, 100 ms CSV interval, 2 s stats warmup, and five-stem recording.

This run is rejected as a latency/audio-quality baseline because the Windows synthetic device cannot schedule a 0.667 ms callback with this timer-backed implementation. It delivered 7,625 callbacks in 15.1 s with a 1.984 ms average callback interval. Only 243,648 engine frames advanced (about 16.1 kframes/s rather than 48 kframes/s), producing a meaningless approximately -664,000 ppm peer drift estimate. The network itself had zero sequence loss, zero playback drops/underruns, zero mixer deadline releases, and zero live-tail trims. The peer playback path stayed small (145 frames final, 255 maximum observed periodic value), but those numbers are driven by the invalid synthetic clock rate.

Decision: do not use a 32-frame headless run to tune production values. Repeat code-path and network-profile comparisons with a 96-frame synthetic callback, whose 2 ms period is schedulable on this machine. Keep the public fast profile's 32-frame device request unchanged until it can be checked on real ASIO/CoreAudio hardware. Reworking the headless clock into a sub-millisecond high-resolution scheduler would be a distinct test-harness change and is deferred.

### Code-path iteration 1: recording callback and writer

Changes:

- Removed five full-stem nonzero-sample scans from `OutputRecorder::record`, which runs in the real-time audio callback.
- Moved signal-frame accounting to the existing background writer. A live signal-frame snapshot may now lag by the visible recorder queue depth; stopped/final statistics remain exact.
- Replaced one `std::ofstream::write` call per PCM16 sample with one contiguous block write on the little-endian Windows/macOS targets, retaining the portable per-sample fallback.

The local input WAV remained exact in shape: 440 Hz, RMS approximately 2,896, peak 4,096, maximum adjacent delta 236, and no large transitions. Recorder drops and writer errors remained zero. The 96-frame before/after runs showed that this materially reduced one source of host pressure, but their independent synthetic clocks still varied too much to attribute queue differences to this change alone.

### Code-path iteration 2: received packet ownership

`PeerStream` now passes pending packets by rvalue reference and processes a released jitter FIFO slot in place. This removes redundant moves of the fixed 256-sample packet array on the direct, reorder, and jitter-release paths. Storage remains bounded and preallocated; packet ordering, format, authentication, and PCM conversion are unchanged.

### Code-path iteration 3: default mixer gain

The mixer now has exact branches for muted and unity-gain peers. The normal default no longer performs a 64-bit multiply and divide for every remote sample. Non-unity gain retains the original scaled path, and the unity path adds the exact original signed sample, so it is bit-preserving.

### Establishing a stable synthetic comparison

Artifacts: `latency-runs/optimized-240-fast/` and `latency-runs/optimized-128-fast/`

A 240-frame callback ran at 5.0004 ms average with final peer drift around -5 ppm, proving the network path itself did not accumulate latency: the final peer queue was 2 frames and there was no packet loss or mixer deadline release. Its 256-frame prefill was only 16 frames larger than one callback, however, and the recording correctly exposed three playback underruns totaling 64 frames as three 16/32-frame zero gaps.

A 128-frame callback was both temporally stable and compatible with the fast profile's prefill. With the then-current 512-frame jitter target it produced:

- 2.667 ms callback average and approximately -4 ppm final peer drift.
- Zero playback underruns, mixer deadlines, packet loss, recording drops, or WAV discontinuities.
- 448-frame final peer path and 14.83 ms final estimated one-way latency.
- Remote tone 439.98 Hz, RMS 2,896.27, peak 4,096, maximum delta 236, and no multi-sample zero runs.

All subsequent numeric comparisons therefore use the 128-frame synthetic callback. This is an explicit validation override; the shipped fast profile continues to request 32 frames from a real device.

### Fast-profile sweep

All runs used 48 kHz, 64-frame PCM24 network packets, loopback UDP, 128-frame synthetic callbacks, continuous recording, high process/MMCSS priority, 100 ms CSV sampling, and the optimized code path. Emergency maxima remained at 1,024 jitter frames and 1,536 adaptive frames.

| Jitter target | Playback/adaptive boundary | Final estimate | Final peer path | WAV/counters | Decision |
|---:|---:|---:|---:|---|---|
| 512 | 256 | 14.83 ms | 448 frames | clean | old fast baseline |
| 256 | 256 | 9.58 ms | 193 frames | clean | safe intermediate |
| 128 | 256 | 8.05 ms | 127 frames | clean | clean lower target |
| 64 | 256 | 6.79 ms | 62 frames | clean | accepted jitter floor |
| 64 | 192 | 5.58 ms | 1 frame | first run clean; repeat produced one 64-frame underrun | rejected as not repeatable |
| 64 | 128 | 6.79 ms after underruns | 63 frames | three underruns/192 frames; three 64-frame WAV gaps and six large edges | rejected |

The 64-frame jitter target releases a one-packet loopback stream without retaining old jitter history. It is the measured local floor, not a claim that arbitrary internet paths need only one packet. The adaptive and maximum controls remain available as explicit user stability levers.

### Long tone and pulse evidence

`latency-runs/final-fast-tone-30s/` exercised the more aggressive 192-frame candidate for 30 seconds before the repeat rejected that boundary. It received 22,362 audio packets with zero loss, underruns, mixer deadlines, or live-tail trims. The remote WAV measured 440.024 Hz, expected RMS, maximum delta 236, and no gaps. Its clean result followed by a failed repeat is why a single clean run was not treated as sufficient.

`latency-runs/final-fast-pulse-15s/` used that same lower candidate to stress ordering. The remote stem contained 14 complete observed pulses, each 480 or 481 frames wide, with start intervals of 48,000 or 48,001 frames. CSV counters reported zero missing sample ranges, inserted frames, late drops, loss, duplicates, and out-of-order packets. The accepted profile has more playback protection and the same network/jitter behavior.

### Final accepted fast profile

The shipped fast profile now keeps its 32-frame device request, 256-frame prefill/playout/adaptive minimum, 1,024-frame jitter maximum, and 1,536-frame adaptive maximum, but lowers the baseline jitter target from 512 to 64 frames. The configured receive budget falls from 768 frames (16.0 ms) to 320 frames (6.67 ms), a 448-frame/9.33 ms reduction without reducing PCM24 quality or emergency headroom.

Final artifacts: `latency-runs/final-accepted-fast-15s/`

The final executable, including the unity mixer optimization, ran the accepted profile for 15 seconds with only the synthetic callback overridden to 128 frames:

- 11,288 received audio packets; zero sequence loss.
- Zero playback underruns, mixer deadlines, live-tail trims, recording drops, or writer errors.
- Final drift 1.39 ppm; callback average 2.6670 ms.
- Final peer path 0 frames; path maximum 2 frames after warmup; high-water queue 257 frames including startup cushion.
- Final estimated one-way latency 5.96 ms on loopback, versus 14.83 ms for the old profile in the stable 128-frame baseline.
- Remote WAV: 440.004 Hz, RMS 2,895.52, maximum adjacent delta 236, no delta over 1,000, and no multi-sample zero run.
- `release/jam2.exe` was rebuilt successfully after every retained code change. Per instruction, no CTest was run during this exercise.

Native coverage was updated for writer-owned final signal counters and the exact fast-profile contract. The previously added jitter, mixer recovery, four-peer burst, and diagnostics coverage remains in the working tree for later execution.

## Deferred major changes

- A dedicated single-active-peer output shortcut remains deferred. Keeping two- and multi-peer sessions on the same mixer path avoids a second behavior path; profile it only after the remaining shared path is exhausted.
- Moving telemetry formatting and delivery to a separate thread would alter runtime ownership and requires its own design/review if measurements prove it necessary.
- Kernel packet-arrival timestamps require platform-specific socket work and are not part of the current small-change iteration.
- A sub-millisecond Windows headless scheduler would make the requested 32-frame synthetic callback representative. Implementing it without busy-spinning four-peer tests needs a separate cross-platform harness design.
- The mixer resampler still uses floating-point interpolation after any non-unity correction leaves a fractional phase. Resetting that phase to enter a unity-copy path could create a sample discontinuity, so it was deliberately not changed.
- Per-peer isolated recording stems would expand the real-time recording surface and are unnecessary for the immediate two-peer investigation.
- A dedicated long-running tone/watermark CTest is intentionally not being implemented. Existing headless create/join commands are faster for the current iterative investigation.

## 2026-08-23 shared-path efficiency pass

The dedicated single-remote-peer mixer shortcut remains deferred. Keeping the same mixer path for two- and multi-peer sessions is the more stable default until the shared path has been exhausted; its cost can be profiled independently afterward.

### Fresh baseline

Artifacts: `latency-runs/20260823-baseline/`

The existing optimized Release executable was run for 15 seconds with the accepted fast profile, 48 kHz/64-frame PCM24 packets, a 128-frame synthetic callback, continuous 440 Hz input, five-stem recording, 100 ms CSV output, and high/MMCSS process priority.

- Estimated one-way latency: 5.45 ms.
- Playback depth: 1.437 ms average, 2.667 ms maximum.
- Audio packet gap: 1.333 ms average, 3.810 ms maximum.
- Receive-loop gap: 0.712 ms average, 2.988 ms maximum.
- Receive processing: 3.61 us average, 160 us maximum.
- Callback interval: 2.6669 ms average, 4.504 ms maximum.
- Zero packet loss, playback underruns, mixer deadline releases, live-tail trims, or recording drops.

The two synthetic clocks remained inside the 25 ppm drift deadband, so corrected-ratio resampler work was not active in this run. Resampler changes will therefore also use an explicit synthetic clock offset so the optimized path is actually measured.

### Iteration A: consolidate real-time metering scans

Artifacts: `latency-runs/20260823-peaks/`

Remote and metronome buffers now publish their live and GUI peaks from one scan. Final output peak and rail-clipping counts are also collected in one scan instead of three. This removes four complete callback-buffer passes without changing audio or meter semantics.

- Estimated one-way latency: 6.74 ms; playback depth averaged 2.716 ms.
- Callback interval: 2.6670 ms average, 4.527 ms maximum.
- Receive processing: 4.78 us average, 85 us maximum.
- Zero loss, underruns, mixer deadlines, recorder drops, or detected pops.
- Remote WAV retained the 440 Hz tone, 0.125 normalized peak, expected RMS, and no quality tags.

The latency estimate was worse than the fresh baseline because the independently scheduled synthetic queues retained roughly one additional 64-frame packet. There is no evidence that the scan consolidation added buffering, and interval-only callback timing is too noisy to quantify its CPU saving. The change is retained as a deterministic reduction in real-time work; direct callback execution-time counters are needed for finer attribution.

### Iteration B: contiguous recorder ring operations

Artifacts: `latency-runs/20260823-recorder-ring/`

Each of the five callback-side PCM16 stem queues now resolves wraparound once per block and converts into at most two contiguous regions instead of calculating a modulo for every sample. The writer similarly copies at most two contiguous regions rather than performing a modulo-indexed read per sample. Native coverage records beyond the queue capacity and verifies exact final counts for every stem.

- Estimated one-way latency: 5.50 ms; playback depth averaged 1.476 ms.
- Callback interval: 2.6670 ms average, 4.521 ms maximum; 91 intervals exceeded 1.5x.
- Receive processing: 4.87 us average, 128 us maximum.
- Zero loss, underruns, mixer deadlines, recorder drops, or detected pops.
- Remote WAV retained the expected tone/RMS/peak and produced no quality tags.

This returned the queue-derived latency estimate and callback-gap count to approximately the fresh baseline while preserving every recorded stem. As with metering, direct CPU attribution awaits callback work-duration counters, but the removed divisions are deterministic and occur only when recording is active.

### Iteration C: direct Q31 network PCM conversion

Artifacts: `latency-runs/20260823-pcm-q31/` and `latency-runs/20260823-pcm-q31-repeat/`

The default unity-gain send path now packs device-domain Q31 samples directly into the negotiated PCM16/PCM24 wire shape, avoiding the intermediate signed-24 array pass. Receive decoding similarly produces mixer-domain Q31 directly. Non-unity send gain and leader-audio click injection retain the original signed-24 processing path. Native boundary coverage proves that both negotiated formats produce the exact prior wire bytes and decoded samples, including integer rails and negative quantization edges.

The first run encountered one 64-frame playback underrun and adaptive raise. Its PCM/WAV content remained valid, but that run was rejected as clean evidence and repeated unchanged. The repeat produced:

- Estimated one-way latency: 5.45 ms; playback depth averaged 1.434 ms.
- Callback interval: 2.6668 ms average, 4.519 ms maximum.
- Receive processing: 4.15 us average, 129 us maximum.
- Zero loss, underruns, mixer deadlines, recorder drops, or detected pops.
- Remote WAV retained the expected tone/RMS/peak and produced no quality tags.

The accepted repeat matches the fresh 5.45 ms baseline. This removes one full 64-frame conversion pass on each side but does not produce a resolvable millisecond-level gain on the synthetic scheduler.

### Iteration D: phase-safe peer-resampler unity path

Artifacts: `latency-runs/20260823-peer-unity/` and `latency-runs/20260823-peer-correction/`

An active peer whose resampler is both exactly unity and sample-aligned now enqueues exact source samples without scalar floating-point interpolation. A peer with fractional history continues through the original interpolator even after its target returns to unity; no phase reset or discontinuity is introduced. Per-peer stats count frames that use the exact path, and native coverage verifies unity, correction, and correction-to-unity ownership.

The normal run remained clean at 5.50 ms estimated one-way, 1.480 ms average playback depth, 4.24 us average receive processing, and zero loss/underruns/artifacts. A deliberate +200 ppm clock-offset/zero-deadband run held peer drift correction active for 100% of samples with a 1.000026-1.000263 observed ratio range. It remained pop-free but incurred one 64-frame underrun at the aggressive profile floor; that stress run uses the unchanged interpolation branch and is retained as continuity evidence rather than a clean latency baseline.

### Iteration E: batched device playback resampler

Artifacts: `latency-runs/20260823-device-resampler/`

When adaptive playback correction requires interpolation, the callback now calculates its exact source-frame requirement and obtains that block from the SPSC playback ring in one operation. Fractional phase, ratio smoothing, and the 32-frame continuity fade remain unchanged. This replaces per-source-sample acquire/release atomics, modulo, drop checks, and diagnostic updates with callback-level ring work. The scratch capacity is allocated when each ASIO, CoreAudio, or synthetic stream is created; the callback does not allocate.

- Estimated one-way latency: 5.50 ms; playback depth averaged 1.475 ms.
- Callback interval: 2.6670 ms average, 4.547 ms maximum.
- Zero loss, underruns, mixer deadlines, recorder drops, or WAV quality tags.

The normal headless run stayed on its initial exact-unity block-copy path, so it validates compatibility rather than the CPU gain of corrected playback. Native coverage directly exercises half-rate, double-rate, ratio ramps, isolated shortage, sustained shortage, and reset state; a sustained callback shortage is now represented by one batched ring event while retaining the same fade.

### Measurement instrumentation

Artifacts: `latency-runs/20260823-work-metrics/` and `latency-runs/20260823-pre-receive-baseline/`

CSV and final CLI diagnostics now expose callback execution work separately from callback scheduling interval, and expose the work performed by the network loop before it starts receiving. Instrumentation adds two monotonic timestamps per callback and loop, with relaxed atomic aggregation outside all sample loops.

- Callback work measured 7.88 us average/37 us maximum in the first run and 7.65 us average/43 us maximum in the pre-receive baseline.
- Pre-receive work measured 16.52 us average but reached 3,828 us maximum.
- The latter provided direct evidence that occasional control/transport work could delay an already queued packet by several milliseconds.

### Iteration F: receive-priority maintenance and bounded telemetry

Artifacts: `latency-runs/20260823-receive-priority/`

Runtime commands and membership changes are now applied after each receive batch. Transport state takes its mutex only when its release-published revision changes, rather than on every network wake. Operational/CSV telemetry prefers a wake whose bounded receive wait found no packet, cannot be deferred more than 10 ms, and no longer writes catch-up CSV rows after a delayed sample. Replaceable GUI snapshots use a non-blocking handoff and update at 5 Hz.

Compared with the immediately preceding instrumented 10-second baseline:

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Pre-receive work average | 16.52 us | 15.68 us | -5.1% |
| Pre-receive work maximum | 3,828 us | 2,058 us | -46.2% |
| Receive-loop gap maximum | 5.594 ms | 3.497 ms | -37.5% |
| Estimated one-way latency | 5.459 ms | 5.469 ms | effectively unchanged |

The run had zero loss, underruns, mixer deadlines, recorder drops, or WAV quality tags. Callback work was 8.12 us average/29 us maximum. This is a measured reduction in latency spikes rather than fixed buffering.

### Focused native validation for this pass

The Release build succeeded after every retained iteration. The following focused CTest cases passed against the normal build and public executable layout:

- `jam2_audio_device_processing_units`
- `jam2_core_boundary_units`
- `jam2_core_input_units`
- `jam2_output_recorder_units`
- `jam2_cli_boundary_units`
- `jam2_application_boundary_units`
- `jam2_four_session_command_integration`
- `jam2_metronome_shared_grid_clean`

The two four-peer checks exercised the retained shared path rather than a two-peer shortcut. The full CTest suite was intentionally not run because the user requested that it remain deferred.

## 2026-08-23 additional shared-path review

This follow-on pass implements the remaining small shared-path efficiencies found by static review. The dedicated two-peer shortcut and larger ownership/threading changes remain deferred. Every iteration uses the same 10-second two-process loopback workload: PCM24, 48 kHz, 64-frame packets, 128-frame synthetic callbacks, continuous 440 Hz input, five recorded stems, 100 ms CSV samples after a two-second warmup, and high/MMCSS scheduling.

### Additional baseline and GUI fast-profile correction

Artifacts: `latency-runs/20260823-more-efficiency-baseline/` and `latency-runs/20260823-gui-profile-512-control/`

Static review found that the core `fast` profile had already been tuned to a 64-frame jitter target, but new GUI preferences and saved schema-5 GUI preferences still supplied 512 frames after applying that profile. Real jam logs marked `fast` also carried the 512-frame target, so the GUI silently restored 9.33 ms of receive buffering.

The GUI default now matches the 64-frame core profile. Schema-5 migration changes only the exact old `fast`/512 combination; other explicit numeric values and other profiles remain untouched. Native preference coverage checks both cases.

- Explicit 512-frame control estimated one-way latency: 14.827 ms.
- Corrected 64-frame fast baseline: 6.749 ms.
- Observed reduction: 8.078 ms; configured jitter budget reduction: 448 frames/9.333 ms.
- Both runs had zero loss, underruns, mixer deadlines, recording drops, or writer errors.

The estimate is the measured average playback depth plus average callback interval, configured jitter target, and half average RTT. The observed reduction is slightly less than the configured reduction because the independently scheduled playback queue averaged one additional 64-frame packet in the fast-64 baseline.

### Iteration G: batched peer-mixer queues

Artifacts: `latency-runs/20260823-mixer-batched/`

Peer mixer enqueue now checks the output once per input span, handles late-prefix discard and capacity retention once, and copies at most two contiguous queue regions. Corrected-ratio output renders into preallocated peer scratch and is committed in batches. Slot release similarly consumes at most two readable spans and publishes queue depth once instead of performing modulo and stats writes for every sample. Unity gain remains an exact integer path, and both two- and multi-peer sessions retain the same mixer implementation.

Native coverage supplies an oversized numbered burst, verifies exact discarded-frame diagnostics, and proves that wrapped dequeue emits the newest 512 frames in order.

| Metric | Fast-64 baseline | Batched mixer | Change |
|---|---:|---:|---:|
| Callback work average | 8.050 us | 6.808 us | -15.4% |
| Receive processing average | 5.072 us | 4.499 us | -11.3% |
| Pre-receive work average | 15.148 us | 12.273 us | -19.0% |
| Pre-receive work maximum | 4,025 us | 3,689 us | -8.3% |
| Estimated one-way latency | 6.749 ms | 5.534 ms | -1.215 ms observed |

The queue-derived latency movement is useful repeat evidence but is not attributed as fixed latency saved: this change adds or removes no frames, and the baseline playback queue happened to average one additional packet. The CPU reductions directly match the removed hot-path operations.

The run completed with zero sequence loss, playback underruns, mixer deadlines, capacity drops, recording drops, or writer errors. Offline analysis found the remote 440 Hz tone at 440.37 Hz, 0.125 peak, 0.0878 RMS, zero pop events, and no quality tags.

### Iteration H: audio-deadline-aware network wakes

Artifacts: `latency-runs/20260823-network-scheduled/`, `latency-runs/20260823-network-listener-before/`, and `latency-runs/20260823-network-listener-after/`

The bounded UDP receive wait now ends at the next future audio-send deadline instead of always permitting a full 1 ms sleep. If a due send is explicitly blocked waiting for the next captured block, the existing 1 ms wait remains to avoid busy-spinning. Native schedule coverage checks an early wait, a sub-millisecond future deadline, an exact due deadline, and the fractional 48 kHz/64-frame interval.

Authenticated peer-liveness scanning now runs every 100 ms rather than every network wake; this adds at most 100 ms to an existing three-second expiry. Listener-compensated peer-phase calculation now runs at its actual 10 ms correction cadence rather than every roughly 0.7 ms wake. Finally, one arrival timestamp is shared by liveness, audio receive, RTT, and phase handling for each datagram.

Normal shared-grid comparison against the immediately preceding mixer run:

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Receive-loop gap average | 0.721 ms | 0.611 ms | -15.3% |
| Receive-loop gap maximum | 3.974 ms | 2.997 ms | -24.6% |
| Receive processing average | 4.499 us | 4.147 us | -7.8% |
| Pre-receive work average | 12.273 us | 12.608 us | +2.7% noise |
| Pre-receive work maximum | 3,689 us | 2,056 us | -44.3% |
| Estimated one-way latency | 5.534 ms | 5.354 ms | -0.180 ms observed |

A dedicated listener-compensated before/after pair exercised the gated phase calculation:

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Receive-loop gap average | 0.730 ms | 0.621 ms | -15.0% |
| Receive processing average | 4.896 us | 4.223 us | -13.8% |
| Pre-receive work average | 16.179 us | 13.196 us | -18.4% |
| Pre-receive work maximum | 2,030 us | 2,015 us | -0.7% |
| Estimated one-way latency | 5.549 ms | 5.357 ms | -0.192 ms observed |

Both after-runs had zero sequence loss, playback underruns, mixer deadlines, recording drops, or writer errors. Remote WAVs retained the 440.37 Hz tone and zero pop events. The listener metronome already clipped 15 click samples in the before-run and clipped the same 15 after; that pre-existing click shape is not caused by this scheduling change.

### Iteration I: inactive recorder gating and contiguous ASIO fan-out

Artifacts: `latency-runs/20260823-inactive-recorder-before/`, `latency-runs/20260823-inactive-recorder-after/`, `latency-runs/20260823-active-recorder-after/`, and `latency-runs/20260823-active-recorder-after-repeat/`

Output and track-take recorders now publish their existing atomic active/armed state through lock-free callback queries. Headless, ASIO, and CoreAudio callbacks use those states to skip inactive recording-source clearing, copies, and inputs-mix construction. Active recording retains the same five-stem path. Native coverage verifies inactive, started/armed, stopped, and canceled transitions without taking recorder stats mutexes.

The ASIO output fan-out now performs one contiguous copy per selected output channel rather than a frame-by-channel nested scalar loop. This is a deterministic reduction for the real Windows device path but is not executed by the headless harness; it remains subject to real-ASIO validation.

Recording-disabled comparison:

| Metric | Before | After | Change |
|---|---:|---:|---:|
| Callback work average | 6.889 us | 5.303 us | -23.0% |
| Estimated one-way latency | 5.371 ms | 5.357 ms | -0.014 ms / unchanged |
| Playback underruns | 0 | 0 | unchanged |

The first active-recording verification encountered one 64-frame playback underrun and was rejected as clean evidence. Its WAV/recorder output remained structurally valid. The unchanged repeat completed at 5.468 ms estimated one-way and 5.628 us average callback work with zero loss, underruns, mixer deadlines, recording drops, or writer errors. The remote WAV retained the 440.37 Hz tone, 0.125 peak, 0.0878 RMS, zero pops, and no quality tags. Active-run timing is retained as compatibility evidence rather than attributed gain because the active recording work is intentionally unchanged.

### Iteration J: bounded decoded-packet handoff

Artifacts: `latency-runs/20260823-packet-handoff-clean/` and `latency-runs/20260823-packet-handoff-clean-repeat/`

The jitter and reorder slots use a fixed 256-frame maximum packet shape. Default array move-assignment copied all 1,024 sample bytes even for the fast profile's 64-frame packet. Handoff now copies metadata plus only `sample_count`: 256 bytes for the common packet, saving 768 bytes per ordinary jitter handoff. A reordered packet crosses both reorder and jitter slots, saving 1,536 copied bytes. The storage remains fixed-capacity and allocation-free.

Native marker coverage already drives an out-of-order packet through both handoffs and verifies the exact four-packet output order. It now also requires the reorder-recovered counter, making that ownership path explicit.

Both valid runs had zero loss, playback underruns, mixer deadlines, jitter drops, recording drops, or writer errors. The repeat estimated one-way latency was 5.365 ms versus 5.468 ms immediately before. Callback and receive work were host-load outliers in both runs—even callback work, which this network-thread-only change cannot affect, more than doubled—so no measured CPU gain or regression is attributed. The byte reduction above is deterministic. Both remote WAVs retained the 440.37 Hz tone, zero pops, and no quality tags.

Two earlier attempts used ports that had entered Windows' dynamic UDP exclusion range and failed at bind before producing stats or audio. They are not product failures and are excluded from evidence.

### Iteration K: block-coherent metronome state and unity local monitoring

Artifacts: `latency-runs/20260823-metronome-unity-before/`, `latency-runs/20260823-metronome-unity-after/`, and `latency-runs/20260823-metronome-unity-after-repeat/`

The metronome renderer now snapshots count-in, epoch, render offset, and pattern-origin state once per callback. A scheduled pattern origin is still applied at the exact sample where its raw-frame boundary crosses the block and is published back to the shared control state at that point. This removes five relaxed atomic loads per rendered sample in the normal shared-grid case, or 640 loads per 128-frame callback; an active stored pattern origin previously added another 128 loads. Native coverage verifies a scheduled origin crossing at frame 32 inside a 64-frame callback and the resulting exact shared state.

Unity local monitoring now mixes the exact input integer directly instead of converting every sample to `double`, multiplying by 1.0, clamping, and converting back. The tested 128-frame callback therefore avoids another 128 floating-point scaling operations while retaining saturating output mixing and identical peak metering. Non-unity monitor gain retains the existing numeric path.

The paired workload explicitly enabled both shared-grid metronome and unity local monitoring:

| Metric, two-peer mean | Before | After run 1 | Unchanged repeat |
|---|---:|---:|---:|
| Callback work average | 15.907 us | 25.060 us | 25.838 us |
| Receive processing average | 5.481 us | 8.711 us | 8.443 us |
| Pre-receive work average | 12.018 us | 18.909 us | 19.720 us |
| Estimated one-way latency | 5.357 ms | 5.401 ms | 6.569 ms |

Neither after-run is accepted as CPU-gain evidence. Work rose by roughly the same proportion in the independent network loop, which this callback-only edit cannot affect, and remained elevated in the unchanged repeat. The repeat also happened to carry about one extra 64-frame packet of average playback depth. No fixed buffer or scheduling delay changed, so these timings are host-load and queue-placement noise rather than an attributed code regression or gain. The deterministic removed operations above are the evidence retained for this iteration.

All three runs had zero sequence loss, playback underruns, mixer deadlines, jitter drops, recorder drops, or writer errors. Offline analysis found zero pops in every recorded stem. Before and after remote tones remained at 0.125 peak with matching RMS and no clipped samples. The generated click retained the same pre-existing 18 creator/15 joiner clipped samples before and after; this optimization neither introduced nor hid that click-shape issue.

### Focused native validation for the additional pass

The optimized Release build succeeded and these focused CTest cases passed:

- `jam2_audio_device_processing_units`
- `jam2_core_input_units`
- `jam2_core_boundary_units`
- `jam2_output_recorder_units`
- `jam2_user_preferences_units`
- `jam2_four_session_command_integration`
- `jam2_metronome_shared_grid_clean`

The integration cases exercise exactly four peers through the common direct-mesh and shared-grid paths. The full suite was intentionally not run, per the user's request.

## 2026-08-23 final low-risk hot-path pass

This pass implements the remaining small efficiencies that do not change configured buffer depth, protocol shape, direct-mesh ownership, or the common two-to-four-peer playback design. The same 10-second, two-process workload described above was retained: fast profile, PCM24, 48 kHz, 64-frame network packets, 128-frame synthetic callbacks, continuous 440 Hz input, five recorded stems, 100 ms CSV samples after a two-second warmup, and high/MMCSS scheduling.

Some creator processes accumulated playback underruns before the joiner completed bootstrap. Those runs are retained for bounded CPU-counter and offline-WAV compatibility evidence but rejected for clean end-to-end latency comparison. The final mixer run used a fixed test session and near-simultaneous process launch and had no startup underrun.

### Iteration L: single-pass input routing and peak metering

Artifacts: `latency-runs/20260823-input-router-clean/`

The input router now skips stable unconfigured or excluded slots before entering their revision-copy loop, uses the dry mono input span directly instead of copying it to scratch, has an exact unity-level integer path, and avoids division when only one source contributes. The router publishes the peak already calculated while rendering, allowing headless, ASIO, and CoreAudio callbacks to avoid a second full-block input scan.

The existing native input CTest now verifies exact routed samples and peak state. It also reports a non-gating 200,000-block microbenchmark; seven-run medians before and after were:

| Native metric, 128 frames | Before | After | Change |
|---|---:|---:|---:|
| Single mono route | 0.3644 us | 0.1704 us | -53.2% |
| Route plus peak observation | 0.4293 us | 0.1750 us | -59.2% |

The clean public-executable run averaged 6.802 us callback work and 5.965 ms estimated one-way latency across the two peers. It had zero loss, underruns, mixer deadlines, or recording drops. Both remote stems measured 440.37 Hz, 0.125 peak, zero pops/clipping, and no quality tags.

### Iteration M: direct engine-frame query and transport lock gating

Artifacts: `latency-runs/20260823-engine-frame/`

Network timing now reads a dedicated relaxed engine-frame atomic instead of requesting a complete engine snapshot, which also read many unrelated counters and locked scheduled-command state. Transport commit now returns before its mutex when the release-published pending flag is clear, while retaining the required check again under the lock.

Mean pre-receive work moved from 12.104 us in the immediately preceding clean run to 10.236 us, a 15.4% reduction. This is useful evidence for the network-thread change, but no fixed latency reduction is claimed: the creator had a bootstrap-time startup underrun and the two runs placed the average playback queue differently. Native coverage checks that the direct frame query is zero before start and agrees with the stopped engine snapshot.

### Iteration N: idle prepared/stem gating and inline metronome peak

Artifacts: `latency-runs/20260823-idle-stems/`

The prepared-track source exposes a lock-free `needsProcessing` state, so an idle source is no longer mixed on every callback. Scratch stems are requested only when active jam or track-take recording actually needs them; a requested idle prepared stem is still explicitly cleared. Metronome peak is calculated inside the renderer's existing sample loop rather than by scanning the completed stem again.

Native coverage verifies idle, queued, and completed prepared-source states, exact zeroing when an idle stem is requested, and inline metronome peak publication. The headless work counters were worse than the preceding run across independent callback and network paths, so no measured gain or regression is attributed. The implementation is retained because it deterministically removes inactive work and every recording remained structurally and audibly clean.

### Iteration O: direct nonblocking UDP drain

Artifacts: `latency-runs/20260823-udp-drain/`

The UDP socket is now made nonblocking once at construction. The first bounded receive still uses the existing timed readiness wait, but subsequent zero-time batch drains call `recvfrom`/`recvmsg` directly instead of performing a zero-time `select` before every datagram. Explicit constructor cleanup preserves the previous failure behavior on both platforms.

The native loopback test queues three datagrams, receives the first through the timed path, drains the next two in exact order through the direct path, and verifies the empty socket reports would-block. Mean receive processing fell from 4.118 us to 3.386 us, a 17.8% reduction. The configuration and all buffering were unchanged; fixed end-to-end latency is therefore not attributed.

### Iteration P: word-oriented authenticated packet hashing

Artifacts: `latency-runs/20260823-auth/`

SipHash now reads each key word once and specializes encode versus parse at compile time. Parsing masks only the two 64-bit words that overlap the authentication tag, replacing an authentication-range branch for every hashed byte. Header-only and partial-final-word handling remain explicit.

Native coverage includes an independent fixed packet vector with expected tag `0xaf2b7d1a445b9350`, in addition to the existing authentication and rejection cases. Mean receive processing fell from 3.386 us to 3.015 us, another 11.0%. Both peers reported zero authentication failures, parse rejects, sequence loss, and recording drops; remote tones remained clean.

### Iteration Q: decode directly into fixed packet ownership

Artifacts: `latency-runs/20260823-direct-decode/` and `latency-runs/20260823-direct-decode-repeat/`

An expected packet now decodes directly into its jitter slot rather than first filling a local 256-sample packet and copying its live samples into that slot. An out-of-order packet similarly decodes directly into its reorder slot. This saves a 256-byte sample copy for the normal 64-frame PCM packet and for the initial out-of-order handoff. Reorder-to-jitter transfer remains bounded to the live samples; unifying those two ownership pools would be a larger change and is deferred.

The existing marker test now also requires exactly four jitter-buffer queue/release events while proving the exact out-of-order output sequence. Mean receive processing fell from 3.015 us to 2.783 us in the first run (-7.7%) and was 2.924 us in an unchanged repeat (-3.0%). From the pre-UDP 4.118 us measurement to the unchanged repeat, the three packet/receive iterations reduced mean receive processing by approximately 29.0%.

The first run had one 64-frame creator underrun and the repeat had a creator bootstrap-time underrun, so neither is used to claim fixed queue latency. Both had zero sequence loss, jitter drops, parse/authentication failures, or recording drops. Offline analysis retained the 440.37 Hz remote tone at 0.125 peak with zero pops or clipped samples.

### Iteration R: consolidated mixer readiness observation

Artifacts: `latency-runs/20260823-mixer-readiness/` and `latency-runs/20260823-mixer-readiness-clean/`

Peer readiness and occupancy diagnostics are now gathered in one traversal whenever release changes queue state. A release previously walked every peer separately up to four times to calculate all-ready, any-ready, and diagnostic occupancy. The adaptive-release decisions, deadline rules, statistics, and common two-to-four-peer mixer are unchanged.

The first run is rejected as timing evidence because the creator waited for the joiner before beginning its workload. The clean repeat averaged 13.168 us pre-receive work, effectively identical to the first direct-decode run's 13.164 us and 4.4% below its unchanged repeat. A two-peer process contains only one remote contributor per mixer, so the headless measurements do not resolve this small saving. It is recorded as deterministic traversal removal whose benefit grows for three/four peers, not as a measured latency win.

The clean repeat averaged 5.424 ms estimated one-way latency and 6.903 us callback work. Both peers had zero sequence loss, underruns, mixer deadlines, work-budget yields, authentication failures, or recording drops. Every analyzed WAV passed: the remote tone was 440.37 Hz at 0.125 peak, and no stem contained a pop or clipped sample.

### Focused final validation

The normal optimized Release build succeeded. After the final retained change, these focused native CTest cases passed:

- `jam2_core_input_units`
- `jam2_audio_device_processing_units`
- `jam2_core_boundary_units`
- `jam2_four_session_command_integration`
- `jam2_metronome_shared_grid_clean`

The last two cases exercise exactly four peers through the common direct-mesh, transport, and impaired shared-grid paths. One attempted shared-grid invocation did not start because the still-running session test owned the common artifact directory; it was rerun after that process completed and passed. No full suite was run, per the user's instruction.

## Remaining validation outside this exercise

- Rerun `jam2_four_performance_integration` independently.
- Run the optimized full CTest suite and Windows coverage audit when requested.
- Validate the final fast profile with real Windows ASIO and macOS CoreAudio devices; the local headless result is a software-path lower bound, not a real-jam latency guarantee.
- Validate the callback changes on macOS; the CoreAudio source was updated but is not compiled by the Windows build.
- Compare a dedicated two-peer mixer shortcut only after all shared-path work is exhausted. It remains deliberately deferred because the common path is simpler and more stable.
- Consider unifying reorder and jitter packet ownership only as a separately reviewed larger change; the remaining bounded live-sample copy is not worth expanding this pass.

## 2026-08-23 final shared-path efficiency and scheduler-attribution pass

This pass implements the last small shared-path changes identified in review. It does not add a two-peer shortcut, alter packet shape, change PCM quality, or lower another buffer. Each retained change was built into the public `release/jam2.exe` and exercised with the same two-process 48 kHz/64-frame PCM24 tone workload. Focused owner tests and exactly-four-peer checks were run; the full suite remains deferred at the user's request.

### Iteration S: pre-receive stage attribution and Windows scheduling evidence

Artifacts: `latency-runs/20260823-final2-baseline-{1,2,3}/`, `latency-runs/20260823-prerecv-stages-baseline/`, and `latency-runs/20260823-prerecv-thread-cycles/`

Three unchanged baselines reproduced highly variable pre-receive maxima on both processes: 1.52-3.96 ms, while receive processing itself peaked at only 61-243 us. Permanent CSV diagnostics now split pre-receive wall time into advance, maintenance, and send/control averages and maxima, and retain the three stage durations belonging to the largest total sample. This adds only one clock read per network wake because the existing loop-top timestamp is reused.

The staged baseline localized the largest samples:

- Creator: 4,006 us total = 1 us advance + 4,004 us maintenance + 1 us send.
- Joiner: 2,020 us total = 1 us advance + 2,000 us maintenance + 19 us send.
- Ordinary maintenance averaged less than 0.8 us on both peers.

A temporary Windows-only `QueryThreadCycleTime` probe was then built for one diagnostic run and removed before the retained build. At a 2.003 ms creator maintenance wall-time spike the thread consumed only 14,376 cycles; at the joiner's 2.008 ms spike it consumed 56,006 cycles. The largest maintenance CPU burst anywhere in the run was 91,947 cycles. Those are only a few to a few tens of microseconds on the test CPU, so the approximately 2 ms missing interval is Windows/host descheduling, not Jam2 executing maintenance for 2 ms. The final clean run reproduced the result: its total maxima were 2,031/2,007 us and the corresponding maintenance stages were 2,001/1,990 us.

Decision: retain the cheap stage counters so real-device logs can distinguish recurrence, but do not retain a per-wake OS cycle query or add buffering to hide an occasional scheduler quantum. MMCSS/high priority is already requested and the spike caused no clean-run underrun.

### Iteration T: idle stream and mixer advancement

Artifacts: `latency-runs/20260823-idle-advance/`, `latency-runs/20260823-idle-advance-repeat/`, and the final clean run below.

`PeerStream::advance` now returns without traversing reorder/jitter work when neither queue nor a sequence-gap decision is pending. `PeerMixer` caches readiness and marks it dirty only when queue state changes; repeated network wakes return early while still checking deadline-ready sources, active adaptive release, output limits, and asynchronous playback underruns. Native coverage proves that idle calls manufacture no output and that a device underrun still wakes deadline/adaptive recovery.

Mean pre-receive advance work fell from 0.274 us across the staged baseline peers to 0.071 us in the final clean run, a 74.2% reduction. The first run had a creator bootstrap/shutdown underrun burst and the unchanged repeat had one creator startup packet underrun, so neither is used as clean end-to-end evidence.

### Iteration U: single-writer callback diagnostics

Artifacts: `latency-runs/20260823-callback-single-writer/`

The audio callback is the sole writer of callback timing diagnostics. It now updates reader-visible atomics with relaxed load/store operations and keeps its previous timestamp in writer-local state instead of using locked read-modify-write instructions. The callback timeline seqlock similarly uses a writer-local generation followed by release stores. This removes seven steady locked RMW operations per callback and up to seven additional conditional min/max/gap RMW operations while retaining lock-free readers.

Native coverage verifies exact interval/work aggregates and odd/in-progress then even/complete timeline generations. Against the unchanged immediately preceding run, two-peer mean callback work moved from 14.612 us to 14.226 us (-2.6%). The before-run contained one creator startup underrun, so this is CPU-counter evidence rather than an end-to-end latency claim; the after-run was fully clean.

### Iteration V: block-oriented cached metronome rendering

Artifacts: `latency-runs/20260823-metronome-wave-bank/`

Each device stream now prepares the four click sounds for normal/count-in and accented/unaccented voices before its callback starts. The callback performs bounded indexed reads and applies the current pattern/level; it no longer evaluates sine, cosine, or exponential functions. Within each callback the grid quotient, remainder, and pattern step are calculated at the first valid frame or a real origin/count-in discontinuity, then incremented across the block instead of dividing and taking modulo for every sample.

Native coverage renders the direct and cached paths and requires every output/stem sample and beat index to be identical. The metronome-on, shared-grid, unity-local-monitor headless run averaged 13.891 us callback work with zero loss, underruns, deadlines, or mixer clipping. The older comparable pre-cache run averaged 15.907 us, but several callback improvements lie between those artifacts; the 12.7% combined reduction is compatibility evidence, not an isolated cache attribution.

### Iteration W: consolidated peer access and owned audio-send accounting

Artifacts: `latency-runs/20260823-peer-access-accounting/`

The receive loop now obtains one typed, non-owning `NetworkPeerAccess` from the datagram endpoint and reuses its descriptor and stream for authentication state, replay, RTT, audio decode, metronome, transport, and departure handling. A normal audio datagram previously performed four linear session peer scans (endpoint identity, active state, endpoint acceptance, and stream lookup); it now performs one.

`NetworkPeerSendStats` now categorizes audio packets and bytes at the actual send owner. The runtime no longer traverses its peer map and performs two more session lookups per active peer after every audio fan-out merely to mirror counters. Native UDP coverage verifies identity/endpoint access resolves the same owned stream and that ping/control packets do not enter audio totals.

Mean receive work was 3.196 us versus 5.303 us immediately before, but callback work independently halved in the same run, proving host load changed. No CPU percentage is attributed; the removed searches and exact retained counters are the evidence.

### Iteration X: exact capture-ring packet pop

Artifacts: `latency-runs/20260823-exact-capture-pop/` and the final clean run below.

The network capture consumer now performs one exact ring pop. If a complete fixed packet is unavailable, the ring and destination remain untouched and no false underrun is recorded. When available, the operation copies and commits from one read/write index snapshot. This replaces a separate availability query followed by a second index load/copy pass.

Native coverage verifies partial data remains queued and the caller buffer remains unchanged, then verifies the completed packet is consumed exactly in order. The first live run had one unrelated creator playback underrun and is rejected as the final baseline; capture underruns remained zero.

### Final clean result

Artifacts: `latency-runs/20260823-final-shared-path-clean/`

Two-peer means:

| Metric | Final |
|---|---:|
| Estimated one-way latency | 5.332 ms |
| Callback work | 7.978 us |
| Pre-receive work | 12.273 us |
| Pre-receive advance | 0.071 us |
| Pre-receive maintenance | 0.526 us |
| Pre-receive send/control | 11.677 us |
| Receive processing | 3.353 us |

Both peers had zero sequence loss, capture/playback underruns, mixer deadlines, capacity drops, authentication failures, recording drops, or writer errors. Both remote WAVs were 48 kHz mono PCM16 with a measured 440.000 Hz tone, 4,096 peak, approximately 2,896.3 RMS, maximum adjacent delta 236, no delta over 1,000, no multi-sample internal zero run, and no clipped sample.

Focused native validation passed:

- `jam2_cli_boundary_units`
- `jam2_core_boundary_units`
- `jam2_audio_device_processing_units`
- `jam2_four_session_command_integration`
- `jam2_metronome_shared_grid_clean`

The last two tests exercise exactly four peers on the common direct-mesh path. No full suite was run.

### Remaining deferred work after this pass

- Real Windows ASIO and macOS CoreAudio validation remains required; headless loopback cannot reproduce driver interrupt/DPC behavior or physical converter latency.
- A dedicated two-peer mixer path remains deliberately deferred until it is independently profiled against the now-optimized shared path.
- Kernel receive timestamps and a unified reorder/jitter ownership pool remain larger cross-platform/ownership changes and were not justified by the remaining few-microsecond work counters.
- The approximately 2 ms maintenance maxima are confirmed host descheduling. Further work there belongs to Windows scheduler/DPC investigation with real hardware traces, not another Jam2 audio-path rewrite.

## 2026-08-23 Windows realtime/MMCSS priority experiment

Artifacts: `latency-runs/20260823-realtime-priority-run2/`

This experimental build temporarily defaulted new runtime and GUI preferences to `os-priority=realtime`. The result is superseded by the elevated A/B experiment below. Its Windows mapping was:

- `off`: leave the process/thread unchanged and do not join MMCSS.
- `high`: High process, Highest packet worker, and MMCSS Pro Audio/High.
- `realtime`: Realtime process request, Time Critical packet worker, and MMCSS Pro Audio/Critical.

The process priority and any temporarily enabled token privilege are scoped to the network session and restored when the session ends. CSV and console diagnostics expose the requested/active MMCSS relative priority, actual process class, normalization error, and `SeIncreaseBasePriorityPrivilege` state.

On this machine, a normal Jam2 launch does not possess `SeIncreaseBasePriorityPrivilege`. Windows returned error 1300 (`ERROR_NOT_ALL_ASSIGNED`) when Jam2 attempted to enable it, and normalized the Realtime process request to an active High process. The retained run therefore proves Time Critical plus MMCSS Critical is active, but it is not evidence for a whole-process Realtime class. Actually granting that class requires launching with a token that owns the privilege or changing Windows policy/elevation behavior. A `requireAdministrator` executable manifest was not added because it would impose UAC elevation on every GUI and command launch; that is a separate application-wide behavior decision.

The two-peer 48 kHz/64-frame PCM24 tone run used the 128-frame stable synthetic callback override. Means across the two peers, using only CSV rows with one active remote peer, were:

| Metric | Previous final clean | MMCSS Critical run |
|---|---:|---:|
| Estimated one-way latency | 5.332 ms | 5.815 ms |
| RTT | not separately retained | 0.068 ms |
| Callback work | 7.978 us | 9.304 us |
| Pre-receive work | 12.273 us | 9.489 us |
| Pre-receive advance | 0.071 us | 0.081 us |
| Pre-receive maintenance | 0.526 us | 0.519 us |
| Pre-receive send/control | 11.677 us | 8.889 us |
| Receive processing | 3.353 us | 3.909 us |

The differences are within the host variation already observed and are not attributed as CPU or latency gains. Most importantly, the scheduler spike remained: peer pre-receive maxima were 2,041/2,030 us and maintenance maxima were 1,996/2,011 us. MMCSS Critical did not eliminate the approximately 2 ms involuntary descheduling interval.

Both peers reported zero sequence loss, capture/playback underruns, mixer deadline releases, capacity drops, authentication failures, recording drops, or writer errors. Offline analysis accepted both recording folders with no tags. Every live tone stem was 48 kHz mono PCM16, peak 0.125, with no pop-threshold event or clipped sample. The only long zero spans were the creator's expected pre-join silence and the normal shutdown tail; there was no internal live-audio dropout.

Focused validation passed:

- optimized Release build of the public `release/jam2.exe`
- `jam2_cli_boundary_units`
- `jam2_user_preferences_units`

No full CTest suite was run.

## 2026-08-23 elevated High versus Realtime A/B and removal

Paired elevated artifacts: `latency-runs/20260823-admin-priority-pair/`

The second run used an administrator token and confirmed that the requested mappings were genuinely active on both peers. The High case reported High process, Highest packet worker, and MMCSS Pro Audio/High. The Realtime case reported Realtime process, Time Critical packet worker, MMCSS Pro Audio/Critical, and active `SeIncreaseBasePriorityPrivilege`, with no priority-request error. This removes the ambiguity in the earlier non-admin experiment.

Both cases used the same executable, local loopback topology, 48 kHz/64-frame PCM24 stream, 128-frame synthetic callback, 15-second tone, recording settings, startup order, and stats cadence. Means combine both peers and include only periodic rows with one active remote peer:

| Metric | Elevated High | Elevated Realtime | Result |
|---|---:|---:|---|
| Estimated one-way latency | 5.234 ms | 5.256 ms | no improvement (+0.022 ms) |
| RTT | 0.041 ms | 0.064 ms | worse under Realtime |
| Callback work | 6.752 us | 10.039 us | worse under Realtime |
| Pre-receive work | 10.049 us | 13.286 us | worse under Realtime |
| Pre-receive advance | 0.059 us | 0.082 us | worse under Realtime |
| Pre-receive maintenance | 0.358 us | 0.451 us | worse under Realtime |
| Pre-receive send/control | 9.632 us | 12.753 us | worse under Realtime |
| Receive processing | 2.985 us | 4.139 us | worse under Realtime |
| Callback maximum | 143 us | 191 us | worse under Realtime |
| Pre-receive maximum | 1,584 us | 2,609 us | worse under Realtime |
| Maintenance maximum | 1,567 us | 2,527 us | worse under Realtime |

Realtime therefore did not reduce transport latency or the occasional pre-receive spike. It increased every measured audio/packet-work mean and produced a larger scheduler/maintenance maximum. The approximately 2 ms spike persists even with genuine Realtime activation, so whole-process priority is not a solution to it.

All four elevated recording folders passed offline analysis with no tags, pop-threshold events, clipped samples, dropped recording frames, or writer errors. Remote tones peaked at 0.125. Long zero spans were confined to startup and shutdown tails, with no internal live-audio dropout. Both High peers reported zero sequence loss, playback/capture underruns, mixer deadlines, capacity drops, authentication failures, or work-budget yields. One Realtime creator reported 7,575 playback-underrun frames only after the 15-second send window, during its extended shutdown tail; this is not counted as a live-stream audio dropout, but is consistent with less predictable teardown scheduling.

Decision: remove the public Realtime mode and its Windows privilege adjustment and macOS time-constraint branches. Retain `off` and make `high` the default, mapping to High process, Highest packet worker, and MMCSS Pro Audio/High on Windows, and user-interactive QoS on macOS. Persisted `realtime` GUI preferences normalize to `high`. The three historical macOS realtime CSV column positions remain empty so existing comparison scripts retain stable indexes.

Post-removal artifact: `latency-runs/20260823-default-high-post-removal/`

A five-second two-peer run omitted `--os-priority` to exercise the shipped default. Both peers reported requested/active High process, Highest worker, and MMCSS High, with no priority error. Across 101 active-peer periodic rows it measured 5.642 ms estimated one-way latency, 0.066 ms RTT, 10.849 us callback work, 12.092 us pre-receive work, and a 2,036 us pre-receive maximum. The short run is a functional confirmation rather than a new performance baseline. It had zero sequence loss, playback underruns, mixer deadlines, recording drops, and writer errors. Both recording folders passed offline WAV analysis with no tags, pops, or clipping; the local and remote tones were present at approximately 440.367 Hz and peak 0.125. Startup/shutdown silence was confined to the recording boundaries.

Focused validation after removal passed:

- optimized Release build of the public `release/jam2.exe`
- `jam2_cli_boundary_units`
- `jam2_user_preferences_units`
- `jam2_gui_widget_boundary_units`

The GUI boundary exercises the actual create/join priority controls and verifies that only High and Off are available and returned through typed dialog state. No full CTest suite was run.

## 2026-08-23 demand-armed capture-ready wake

Accepted quality artifact: `latency-runs/20260823-capture-ready-wake-final3/`

Wake-path diagnostic: `latency-runs/20260823-capture-ready-wake-64frame/`

Rejected implementation iterations: `latency-runs/20260823-capture-ready-wake/`, `latency-runs/20260823-capture-ready-wake-final/`, and `latency-runs/20260823-capture-ready-wake-final2/`

The network worker previously waited only on UDP readiness for as much as 1 ms after an exact capture-ring pop found fewer than one packet of local audio. A device callback could complete the 64-frame packet during that wait but could not wake the worker. The retained implementation uses a pre-created cross-platform signal: a Windows event included alongside socket readiness, or a nonblocking pipe included in the POSIX `select` set. The callback still performs no allocation, locking, logging, exception work, or blocking operation.

The signal is demand-armed, not callback-period polling. After a capture pop fails, the network worker arms the signal and immediately retries the exact pop to close the arm/publication race. Only if the retry also fails does it block on UDP-or-capture readiness. A successful pop disarms the signal. The callback signals only after publishing enough ring frames for a complete network packet and only while the worker has requested a wake. This keeps the OS notification syscall out of ordinary callbacks.

Permanent CSV/console evidence now includes capture-ready wake transitions, consumptions, and callback-publication-to-network-dispatch min/average/max/sample counters. The first implementation run found and rejected a Windows simultaneous-readiness defect: when UDP was already readable, the manual-reset capture event was not consumed. Native coverage now requires a queued UDP datagram and simultaneous capture notification to preserve the datagram and consume the wake. A second rejected iteration signalled every callback; it proved thousands of transitions worked but added unnecessary callback/network wake activity. Demand arming replaced it.

### Accepted 128-frame quality run

The matched workload remained the Fast 48 kHz/64-frame PCM24 session with the established 128-frame Windows synthetic callback override. That callback supplies two packets at once, so neither peer exhausted capture during this run: wake transitions/consumptions were `0/0`. This is useful evidence that the demand-armed path stays dormant when capture is already ready, but it cannot measure the wake's dispatch improvement.

Two-peer means were 5.371 ms estimated one-way latency, 0.070 ms RTT, 11.643 us callback work, and 16.000 us pre-receive work. The previous clean shared-path means were 5.332 ms, 7.978 us, and 12.273 us respectively. The 0.039 ms latency difference is normal run variation; because no wake transition occurred, no notification-path latency or CPU gain is attributed from this quality run. Both peers had zero sequence loss, capture underruns, playback underruns, recording drops, or writer errors.

Both remote WAVs were 48 kHz mono PCM16 with 4,096 peak and approximately 2,896.29 RMS. Measured tones were 439.986/440.003 Hz. Maximum adjacent delta was 236, there were no deltas over 1,000, no multi-sample internal zero run, and no clipped sample.

### Forced wake-path diagnostic

The separate diagnostic set both network packet and synthetic callback size to 64 frames. It forced 3,771/4,210 real wake transitions and measured callback-publication-to-network-dispatch averages of 33.93/32.36 us, minima of 2 us, and maxima of 1,095/1,163 us. The ordinary case therefore dispatches tens of microseconds after capture publication instead of remaining in the old receive wait for up to 1 ms.

This diagnostic is rejected as audio-quality or latency-profile evidence. Windows delivered the nominal 1.333 ms synthetic callback only every 2.63-2.65 ms, leading to 64/128 playback-underrun frames and a 2,828-frame creator playback average. That is the known Windows headless clock limit, not a viable Fast setting. The validated 128-frame workload remains the quality baseline; real Fast-profile validation requires a 32-frame ASIO device.

Focused native validation passed after the final implementation:

- `jam2_core_boundary_units`
- `jam2_cli_boundary_units`
- `jam2_four_session_command_integration`

The four-session case exercises exactly four peers on the common direct-mesh/fake-audio path. No full suite was run.

### Full composition of the 5.371 ms software one-way estimate

The accepted run's two-peer mean is exactly the sum currently implemented by `estimated_one_way_ms`: `RTT / 2 + jitter target + observed playback depth average + one active audio callback buffer`. At 48 kHz:

| Estimated section | Mean | Share | Source / Fast-profile lever |
|---|---:|---:|---|
| Active audio callback buffer | 2.667 ms | 49.65% | 128-frame headless validation override. Fast profile `audio_buffer_size=32` is 0.667 ms on a device that actually supports it. |
| Observed playback depth | 1.336 ms | 24.88% | Measured 64.13-frame mean, not a fixed constant. It is influenced by `playout_delay_frames=256`, adaptive target/min/max `256/256/1536`, playback prefill `256`, and live mixer release timing. |
| Jitter target | 1.333 ms | 24.82% | Fast profile `jitter_buffer_frames=64`, exactly one 64-frame network packet. |
| Network propagation | 0.035 ms | 0.65% | Half the measured 0.070 ms loopback RTT; not a profile setting. |
| **Total** | **5.371 ms** | **100%** | Software estimate used by Jam2 stats. |

The largest reported component is therefore the 128-frame headless callback override. Substituting the shipped Fast request of 32 frames while holding the other measured terms constant would make this formula approximately 3.371 ms. That is only a projection: the Windows synthetic clock cannot validate it, and an ASIO device may report additional driver/converter latency.

Several profile values are intentionally not added as independent steady-state milliseconds. `jitter_buffer_max_frames=1024`, `playback_max_frames=1536`, and the 4,096-frame rings are bounded recovery/capacity limits, not always-resident delay. The 256-frame prefill applies at playback startup/restart. The adaptive 256-frame target and 192 startup padding govern timeline/cushion recovery, while the estimator uses the playback depth actually observed at each release; adding all of them again would double count queue state.

The estimate also does not claim physical end-to-end instrument latency. It omits converter/ASIO input latency, the sender's wait to physically collect a full 64-frame packet, wire serialization, receiver converter/ASIO output latency beyond the one callback term, and any acoustic path. Packing, sending, receive parsing, mixing, and resampling CPU work are generally microseconds and do not appear as separate configured buffers. The new capture-ready wake removes only the avoidable worker sleep after a complete sender packet already exists; it cannot remove the physical time needed to capture those samples.

## 2026-08-23 capture-synchronised packet pacing

Accepted artifact: `latency-runs/20260823-capture-synchronized-final/`

Rejected eager-drain artifact: `latency-runs/20260823-capture-paced-sender/`

The sender previously applied an independent wall-clock packet schedule to audio already paced by the device callback. Small clock differences therefore accumulated captured frames: in the preceding accepted headless baseline the two periodic capture-ring means were 136.35 and 218.86 frames, with maxima of 256 and 320. The latest real 44.1 kHz ASIO jam showed the same defect at 131.56/160 frames on the 32-frame Focusrite side and 254.23/384 frames on the 64-frame TONEX side. Older remote jams had accumulated thousands of frames.

The first implementation drained every complete capture packet immediately. It reduced periodic capture depth to 43.10/36.35 frames, but a 128-frame callback then emitted its two 64-frame packets together. Interarrival jitter rose from approximately 0.43 ms to 1.332 ms and receiver queue depth increased. The WAV remained clean, but this iteration was rejected because it exchanged sender backlog for packet bursts.

The retained implementation keeps normal 64-frame packet spacing and re-anchors the schedule to device publication whenever capture was empty. It permits an immediate catch-up only when more than one complete packet would otherwise remain after a send. Thus a callback larger than one packet retains at most one deliberately spaced packet, while real clock drift cannot grow the ring without bound. Synthetic silence without a live device retains ordinary wall-clock pacing. Packet layout, sequence, sample-time ownership, and the common two/four-peer mesh path are unchanged.

At the established 48 kHz, 128-callback, 64-packet workload, periodic capture means fell to 82.29 and 81.38 frames. A periodic sample can observe 192 frames in the short interval after a 128-frame callback publishes on top of the one retained 64-frame packet; the worker then drains it back to the one-packet bound. Interarrival jitter was 0.576/0.590 ms, much closer to the prior 0.454/0.426 ms baseline than the rejected eager drain's 1.332 ms.

Jam2's current one-way estimate does not include the remote sender's capture ring. Cross-pairing each receiver estimate with its sender's measured capture queue shows the useful total:

| Directional measurement | Prior software + sender capture | Retained pacing | Reduction |
|---|---:|---:|---:|
| creator receives joiner | 9.938 ms | 8.187 ms | 1.751 ms |
| joiner receives creator | 8.220 ms | 7.267 ms | 0.953 ms |

These are software-path comparisons, not acoustic round-trip measurements. They also show why looking only at the existing receiver estimate would hide this fix: its playback component moved between runs while the omitted sender queue fell substantially.

Both accepted remote tones were continuous between their startup/shutdown boundaries, peaked at 0.125, contained no pop-threshold event or clipped sample, and measured approximately 440.367 Hz. There was no sequence loss, capture overrun, jitter forced release, recording drop, or writer error. One creator observed a single 64-frame playback underrun during initial activation; the recorded live interval had no internal dropout.

Native coverage now includes typed schedule override/rebase behavior and an exactly-four-peer workflow in which one 64-frame fake device runs at +200 ppm. Every peer must keep capture at the live edge with zero overrun. Focused validation passed:

- `jam2_core_boundary_units`
- `jam2_four_session_command_integration`

No full CTest suite was run.

## 2026-08-23 optional ASIO output-ready notification

Real-device quality artifact: `latency-runs/20260823-asio-output-ready/`

Exact latency-reporting artifact: `latency-runs/20260823-asio-output-ready-exact/`

The Windows backend now probes `IASIO::outputReady()` after buffers are created and before the final latency query. A driver returning `ASE_OK` is notified immediately after Jam2 has copied every output channel for each callback. `ASE_NotPresent` is a supported no-op and prevents further calls; any other probe or later callback error disables the optional path without stopping audio. The callback adds no allocation, lock, log, exception, or diagnostic atomic store on successful notifications.

This follows the vendored ASIO SDK contract: a supporting driver can move/convert the completed host output into the next DMA buffer instead of waiting another block. Jam2 records the probe state, error, driver input/output latency frames, and the before/after reported output-latency reduction in console and CSV output.

The matched 44.1 kHz real-device jam produced:

| Device | Buffer | Output-ready | Input latency | Output latency | Reported before/after reduction |
|---|---:|---|---:|---:|---:|
| Focusrite USB ASIO | 32 frames / 0.726 ms | active | 105 frames / 2.381 ms | 149 frames / 3.379 ms | 0 frames |
| TONEX | 64 frames / 1.451 ms | unsupported | 130 frames / 2.948 ms | 154 frames / 3.492 ms | 0 frames |

Focusrite explicitly accepted the mechanism, so Jam2 retains the notification path there; TONEX is unchanged after its single probe. However, Focusrite returned the same 149-frame output latency immediately before and after the accepted probe in the exact diagnostic run. Therefore no numeric latency gain is claimed yet. The SDK says a supporting driver should adjust `getLatencies()` according to use, but this driver did not expose a change in this run. A physical loopback A/B would be needed to prove whether the accepted mechanism removes its possible 32-frame/0.726 ms block in practice.

The real-device tone run used a deliberately quiet 0.01 remote level. Both remote recordings contained one continuous live tone between boundary silence, no internal dropout, no pop-threshold event, no clipping, no sequence loss, no capture overrun, no recording drop, and no writer error. Focusrite callback work averaged 5.20 us with recording and tone injection; TONEX averaged 8.21 us. The separate silent Focusrite diagnostic averaged 2.33 us. Workloads differ from the preceding manual jam, so these are safety measurements rather than an attributed CPU delta.

Focused validation passed after the final implementation:

- optimized Release build of the public `release/jam2.exe`
- `jam2_audio_device_processing_units`
- `jam2_cli_boundary_units`
- `jam2_four_session_command_integration`

No full CTest suite was run. Packet-size and recovery/profile tuning remain deferred until the code-path refinement sweep is complete.

## 2026-08-23 post-ASIO and Axe-Fx remaining-opportunity review

No production code or profile value was changed in this review. The older Axe-Fx logs were compared with the real 44.1 kHz Focusrite/TONEX validation produced after capture-synchronised pacing.

The old receive backlog is no longer present. The Axe-Fx-era active rows reported average playback depths of approximately 2,142-2,978 frames on the Axe-Fx side and 3,522-4,186 frames on the Focusrite side, or roughly 44.6-87.2 ms at 48 kHz, with a 5,632-frame/117.3 ms maximum. They also used the former 512-frame/10.67 ms jitter target. The current real-ASIO run used the corrected 64-frame/1.45 ms target and averaged 77.64/111.23 playback frames, or 1.76/2.52 ms. This confirms that another hidden receive queue comparable to the jam regression is not surviving in the current path.

One shared-path code candidate remains worth profiling before numeric profile tuning: let a real device callback no larger than one network packet drive immediate packet dispatch. The retained pacing currently leaves one complete 64-frame packet deliberately spaced so a 128-frame synthetic callback cannot emit two back-to-back datagrams. That anti-burst rule is required for the headless validation workload, but a 32-frame Focusrite callback or 64-frame TONEX/Axe-Fx callback cannot publish more than one complete 64-frame packet at once. In the post-fix ASIO run the periodic capture depths still averaged 86.63 Focusrite frames and 103.04 TONEX frames, equivalent to 1.96/2.34 ms of sender queue occupancy at 44.1 kHz. A callback-size-aware capture-clock mode could reuse the existing capture-ready wake and preserve spacing only when a callback is larger than a packet. Its gain must be measured rather than assumed, and its packet-gap distribution must remain clean.

The next largest numerical levers remain deliberately deferred to the settings sweep:

- A 32-frame network packet could halve the 1.45 ms packet-collection interval at 44.1 kHz, permit a 32-frame jitter floor, and better match the Focusrite's 32-frame callback, but it doubles packet rate and header/authentication work.
- Removing or conditionally bypassing the one-packet jitter target could remove another 1.45 ms on localhost, but the current ten-second ASIO evidence contained 2-5 packet gaps over twice the nominal interval, approximately 3.0 ms packet-gap maxima, and approximately 3.0-3.4 ms receive-loop maxima. The older remote Axe-Fx logs also contained loss and 100-267 ms jitter maxima. An unconditional zero-jitter default would therefore trade directly against clean-audio robustness.
- Playback prefill, adaptive target/max, and recovery thresholds remain stability controls. They no longer form a large steady queue in the clean ASIO run, so lowering them cannot be credited as steady-state latency until an impaired repeat proves an actual occupancy reduction.

Driver/hardware delay is now at least as important as Jam2's remaining software queues. The reported directional input-plus-output latency is 259 frames/5.87 ms for Focusrite input to TONEX output and 279 frames/6.33 ms for TONEX input to Focusrite output. These figures are not simply added to Jam2's estimator because the driver's output report may overlap the estimator's callback term. The exact acoustic result needs a physical loopback measurement. Axe-Fx driver latency was not available in the older CSV schema and should be captured by the current build in the next real remote jam.

Further packet parsing, mixing, or callback micro-optimization is not expected to remove another frame of latency: the real-device run averaged approximately 4.8-4.9 us receive processing and 5.1/8.2 us callback work with zero live loss, drops, or underruns. A separate send/receive worker, kernel timestamps, unified packet ownership, or a two-peer-only mixer would be larger changes for microsecond-scale work or better attribution, not evidence-backed solutions to the old tens-of-milliseconds regression. The occasional approximately 2 ms pre-receive maximum also remained Windows descheduling rather than Jam2 CPU work.

## 2026-08-23 callback-sized capture-clock dispatch

Primary real-device artifacts:

- baseline: `latency-runs/20260823-asio-output-ready/`
- first implementation: `latency-runs/20260823-capture-clock-asio/`
- unchanged repeat: `latency-runs/20260823-capture-clock-asio-repeat/`
- final typed-pacer validation: `latency-runs/20260823-capture-clock-asio-final/`

Control/diagnostic artifacts:

- `latency-runs/20260823-capture-clock-headless-128/`
- `latency-runs/20260823-capture-clock-headless-128-repeat/`
- `latency-runs/20260823-capture-clock-headless-64/`

The network runtime now chooses its sender pacing from the audio callback size reported by the running device, not only from the requested profile size. If a valid callback is no larger than one network packet, the callback publication clock may release a newly completed packet before the old wall deadline through the existing demand-armed capture-ready wake. After a send, one remaining complete packet is still deliberately spaced; immediate catch-up is permitted only while more than one packet remains. A callback larger than one packet stays on the previous capture-synchronised anti-burst behavior. This preserves the common two/four-peer transport, packet format, ownership split, sequence/sample-time rules, and bounded recovery path.

The rule is owned by the typed `NetworkCapturePacketPacer` state object. Native tests prove that a newly published single packet may lead the old deadline, one deliberately retained packet cannot be sent immediately, catch-up requires more than one retained packet, draining capture re-enables the device clock, and a larger callback retains scheduled pacing. CSV and console stats expose `capture_clock_packet_pacing_active`; both periodic and final CSV rows now retain the same schema width.

### Direct sender-queue result

All values below are live periodic capture-ring occupancy at 44.1 kHz. This is the queue the existing one-way estimate omits.

| Run | Focusrite mean | TONEX mean | Pair mean | Pair mean time |
|---|---:|---:|---:|---:|
| preceding real-ASIO baseline | 86.63 frames | 103.04 frames | 94.84 frames | 2.150 ms |
| first capture-clock run | 40.34 frames | 42.93 frames | 41.64 frames | 0.944 ms |
| unchanged repeat | 44.10 frames | 37.07 frames | 40.59 frames | 0.920 ms |
| final typed-pacer run | 47.86 frames | 24.71 frames | 36.29 frames | 0.823 ms |

The repeated directly attributable reduction is therefore approximately 53.2-54.3 frames, or 1.21-1.23 ms of pair-mean sender queue. The final run corroborates the result but is not used to enlarge that claim because both drivers selected 64-frame callbacks, whereas the baseline used 32 Focusrite/64 TONEX and the first two after-runs selected 32 on both. The same-buffer Focusrite comparison alone fell from 86.63 frames at 32 to 40.34/44.10 frames at 32, a 42.5-46.3 frame or 0.96-1.05 ms reduction.

The receiver packet cadence did not turn into the rejected eager-drain burst pattern. The two first after-runs retained 1.4512-1.4513 ms average packet gaps with 2.798-3.561 ms maxima, essentially the same average as the baseline. The final validation measured 1.4512/1.4511 ms averages and 3.090/3.502 ms maxima. Its callback-publication-to-dispatch averages were 19.45/19.78 us. Capture-clock pacing was reported active by every affected peer.

The receiver-side one-way estimate moved with normal playback-queue placement and is not used as the attribution metric: the omitted capture queue is the component this change directly removes. Cross-pairing receiver estimates with the opposite sender queue showed approximately 0.95 ms improvement in the first after-run and 1.63 ms in the repeat, but the direct 1.21-1.23 ms queue result is the stable conclusion.

### Audio integrity and validation

Both repeated 32-frame ASIO runs had zero sequence loss, capture overrun, jitter drop, playback drop, recording drop, or writer error. Their quiet remote WAVs measured approximately 440 Hz, peak 41, active RMS approximately 28.93, maximum adjacent delta at most 27, no delta over 1,000, no multi-sample internal zero run, and no clipped sample.

The final source refactor was validated again on the Focusrite USB ASIO and TONEX devices. During the live peer interval both sides had zero sequence loss, capture/playback underruns, playback or jitter drops, and mixer deadline releases. Focusrite's 3,968 playback-underrun frames appeared only after the peer had disconnected during shutdown, not during live audio. Both recording writers reported zero dropped frames and zero errors. The remote WAVs measured 440.001/440.029 Hz, peak 41, active RMS 28.931/28.926, maximum adjacent delta 3, no delta over 1,000, no multi-sample internal zero run, and no clipping.

The normal optimized public `release/jam2.exe` build succeeded. Focused native validation passed:

- `jam2_core_boundary_units`
- `jam2_cli_boundary_units`
- `jam2_four_session_command_integration`

The exactly-four-peer integration uses the shared 64-frame fake-audio path, includes a +200 ppm peer, and requires capture to remain at the live edge without overrun. Per instruction, no full CTest suite was run.

## 2026-08-23 post-change real GUI jam

Latest logs:

- `release/logs/jam2_stats_20260823_162005_482_pid39676.csv` (Focusrite USB ASIO)
- `release/logs/jam2_stats_20260823_162011_599_pid44488.csv` (TONEX)
- matching GUI logs `jam2_gui_20260823_151941_649_pid39676.log` and `jam2_gui_20260823_151947_244_pid44488.log`

Immediately preceding matched GUI logs:

- `release/logs/jam2_stats_20260823_142016_401_pid43976.csv` (Focusrite USB ASIO)
- `release/logs/jam2_stats_20260823_142024_643_pid35228.csv` (TONEX)

Both sessions used the Fast profile, 44.1 kHz PCM16, 64-frame network packets, a 32-frame Focusrite callback, a 64-frame TONEX callback, and the same public-address hairpin route on the local machine. The preceding live interval was approximately 288 seconds and the latest was approximately 156 seconds. This is a stronger matched GUI comparison than the shorter PCM24 diagnostic runs.

| Live measurement | Previous Focusrite | Latest Focusrite | Previous TONEX | Latest TONEX |
|---|---:|---:|---:|---:|
| Capture-ring mean | 131.86 frames | 43.09 frames | 256.45 frames | 24.75 frames |
| Capture-ring min/max | 64/160 | 0/96 | 128/384 | 0/64 |
| Existing receiver one-way estimate | 5.370 ms | 5.386 ms | 4.699 ms | 6.069 ms |
| Playback-depth average | 126.66 frames | 126.59 frames | 64.99 frames | 124.81 frames |
| RTT average | 0.642 ms | 0.676 ms | 0.645 ms | 0.672 ms |
| Packet-gap average/max | 1.4512/2.984 ms | 1.4512/2.583 ms | 1.4512/2.989 ms | 1.4512/2.808 ms |
| Capture publication-to-dispatch average/max | unavailable | 7.16/233 us | unavailable | 8.15/620 us |
| Callback work average/max | 0.99/217 us | 1.90/155 us | 1.47/273 us | 3.02/253 us |
| Pre-receive work average/max | 5.05/381 us | 3.24/539 us | 4.77/597 us | 2.91/560 us |

The pair-mean sender capture queue fell from 194.16 frames/4.403 ms to 33.92 frames/0.769 ms, a measured reduction of 160.24 frames/3.633 ms. The final values also agree with the controlled typed-pacer run's 47.86/24.71-frame result, showing that the gain survives the longer normal GUI lifecycle.

The current receiver estimate omits the remote sender capture queue. Adding the opposite sender's measured capture occupancy gives:

| Direction | Previous receiver + sender capture | Latest | Reduction |
|---|---:|---:|---:|
| Focusrite receives TONEX | 11.185 ms | 5.947 ms | 5.238 ms |
| TONEX receives Focusrite | 7.689 ms | 7.046 ms | 0.643 ms |
| Directional mean | 9.437 ms | 6.497 ms | 2.940 ms |

The smaller second-direction result is caused by receiver queue placement: TONEX happened to hold approximately one additional 64-frame packet in this run, raising its reported receiver estimate by 1.370 ms. The controlled after-runs already alternated between approximately 64 and 121 playback frames on TONEX without a setting change, so this is not evidence of a new fixed buffer. It consumes part of this run's sender gain but does not reverse it. Focusrite playback depth was unchanged to 0.07 frame.

RTT rose only approximately 0.03 ms and packet-gap averages remained exactly 1.4512 ms. Packet-gap maxima improved on both peers. The new path reported no send catch-up events, confirming that the lower capture depth did not produce eager-drain bursts. Callback work increased by approximately 0.9/1.6 us, consistent with the now-active demand-armed wake path, but remained only approximately 0.21-0.26% of each device's callback interval. Pre-receive means fell by approximately 36-39%.

During both actual live intervals there was zero sequence loss, duplication, missing/inserted audio, late audio, capture/playback underrun, playback or jitter drop, mixer deadline/missing contribution, UDP send drop, or authentication failure. The preceding TONEX log's 4,928 underrun frames and 70 mixer deadlines appeared only in its final sample after the control disconnect, not in live audio. The newest GUI run produced no WAV recording, so audio integrity is supported by the exact runtime counters rather than offline waveform analysis.

No code or profile setting was changed during this comparison.

## 2026-08-23 remaining latency opportunities after capture-clock validation

The latest GUI jam changes the remaining priority from code-path micro-optimization to explicit buffering/profile measurement. Callback work is only 1.90/3.02 us, receive processing 1.85/1.80 us, and pre-receive work 3.24/2.91 us. Their maxima remained 155/253 us, 297/645 us, and 539/560 us respectively. These are all well below the 1.451 ms packet interval and caused no deadline, drop, or underrun. Removing another microsecond of CPU work would not remove an audio frame from the current steady path.

The remaining resident state is packet/buffer sized:

| Layer observed in latest GUI jam | Focusrite | TONEX | Current control |
|---|---:|---:|---|
| Sender capture queue mean | 43.09 frames / 0.977 ms | 24.75 / 0.561 ms | 64-frame packet size; most remaining time is physical packet formation |
| Jitter target | 64 frames / 1.451 ms | 64 / 1.451 ms | `jitter_buffer_frames=64` |
| Per-peer mixer queue | 63 frames / 1.429 ms | 61 / 1.383 ms | 64-frame common mixer block plus per-peer drift resampling |
| Audio playback ring mean | 203.95 frames / 4.625 ms | 150.19 / 3.406 ms | 256-frame playout/prefill/adaptive floor |
| Total current peer path mean | 266.95 frames / 6.053 ms | 211.19 / 4.789 ms | overlapping mixer/playback state; not added independently to the existing estimator |

The next evidence-based work should therefore be a settings sweep, in this order:

1. Sweep the coupled `playout_delay_frames`, `playback_prefill_frames`, `adaptive_playback_target_frames`, and `adaptive_playback_min_frames` from 256 to 192, 128, and finally 64. Each 64-frame step is 1.451 ms at 44.1 kHz. Relative to 256, the configured receiver floor falls by 1.451/2.902/4.354 ms respectively. The latest run had no adaptive raises/releases and no recovery event, so the 256-frame floor was unused recovery margin. Packet gaps nevertheless reached 2.583/2.808 ms: 64 frames plus the unchanged 64-frame jitter target gives only 128 frames/2.902 ms of total scheduled protection, leaving almost no margin over the measured maximum. The 64 candidate is therefore a useful local floor probe, not an assumed shippable setting. Every peer must receive all four matching overrides; changing only playout delay leaves another 256-frame owner in place.
2. Test 32-frame network packets with a 32-frame jitter target. This halves packet collection to 0.726 ms and should remove approximately 0.726 ms of fixed jitter target plus roughly 0.36 ms of average packet formation, about 1.09 ms before receiver queue effects. It doubles packet and authentication rate, but current receive work is only about 1.8 us per packet. A 64-frame device callback must remain on anti-burst pacing and send its two 32-frame packets spaced.
3. Test a 32-frame TONEX callback if the driver continues to support it cleanly. The current 64-frame callback contributes 1.451 ms to TONEX's estimator; 32 frames would remove 0.726 ms. Focusrite is already at 32.
4. Only after those tests, assess a zero-target jitter mode. It could remove the current 1.451 ms target, but the observed approximately 2.8 ms packet gaps prove that it has no full-packet protection even on this local route. It is a less robust experiment than a 32-frame one-packet target.

Two code changes remain possible but are deliberately deferred. Decoupling the common mixer quantum from the 64-frame packet might reduce the approximately 61-63-frame per-peer residue, but it changes multi-peer deadline cadence and can create partial-block releases; a two-peer bypass would violate the earlier decision to keep one common stable path. Exchanging/selecting a loopback or LAN candidate for two instances on the same host could reduce the current public-hairpin RTT by approximately 0.6 ms round trip (about 0.3 ms one way), but it is endpoint-discovery work with no benefit to a normal remote jam.

No further safe static code-path change is presently supported by the measurements. The profile sweep above is the next useful iteration; larger mixer or endpoint changes should remain documented rather than implemented unless the numeric settings have been exhausted.

## 2026-08-23 Fast-profile delay ledger before the settings sweep

The built-in Fast profile is nominally 48 kHz, but the latest real GUI session negotiated 44.1 kHz. A fixed frame count therefore lasts 8.84% longer in the measured jam. Reference conversions are:

| Frames | 48 kHz | 44.1 kHz |
|---:|---:|---:|
| 32 | 0.667 ms | 0.726 ms |
| 64 | 1.333 ms | 1.451 ms |
| 256 | 5.333 ms | 5.805 ms |
| 1024 | 21.333 ms | 23.220 ms |
| 1536 | 32.000 ms | 34.830 ms |
| 4096 | 85.333 ms | 92.880 ms |

### Steady clean-path controls

`frame_size=64` controls packet formation. A randomly timed note waits approximately half a packet on average: 32 frames, or 0.667 ms nominal/0.726 ms in the measured 44.1 kHz jam. Its position-dependent range is approximately 0 to one packet, 1.333/1.451 ms. The latest measured sender capture queues were 0.561/0.977 ms and averaged 0.769 ms, consistent with this floor plus callback/dispatch phase.

`jitter_buffer_frames=64` adds one fixed packet of release protection: 1.333 ms nominal/1.451 ms measured-rate. The latest run released every packet by this target, with zero timeout or forced release.

`sample_time_playout=true`, `playout_delay_frames=256`, `playback_prefill_frames=256`, `adaptive_playback_cushion=true`, `adaptive_playback_target_frames=256`, and `adaptive_playback_min_frames=256` jointly establish the receiver's 256-frame clean floor. They are overlapping controls and must not be added six times. Their configured duration is 5.333 ms nominal/5.805 ms at 44.1 kHz. The latest observed combined peer-mixer plus playback-ring path averaged 4.789/6.053 ms because callbacks consume this target in blocks.

`audio_buffer_size=32` requests a 0.667/0.726 ms device block. It affects packet publication granularity and receiver output scheduling, but sender input callback time is already substantially represented by packet formation and must not be independently added again. Jam2's existing receiver estimator conservatively adds one full active callback. Focusrite used 32 frames/0.726 ms; TONEX was locally overridden or negotiated to 64 frames/1.451 ms.

`sample_rate=48000` does not add a buffer; it determines the duration of every frame-based control. Falling to 44.1 kHz makes all of the above frame counts 8.84% longer.

Using non-overlapping profile terms, the nominal Fast software budget is approximately `0.667 packet formation + 1.333 jitter + 5.333 receiver floor + 0.667 receiver callback = 8.000 ms`, before network propagation and hardware/driver conversion. At 44.1 kHz with a 32-frame receiver the equivalent is approximately 8.708 ms. Adding the latest approximately 0.34 ms one-way network path gives about 9.05 ms. Direct queue measurements produced approximately 9.00-9.13 ms for the two actual directions, close to this profile ledger.

### Recovery and capacity controls

The following Fast fields add zero deliberate steady delay in a clean run:

- `jitter_buffer_max_frames=1024`: 21.333/23.220 ms reorder/capacity bound, not a wait target.
- `adaptive_playback_max_frames=1536`: recovery target may rise as high as 32.000/34.830 ms. The additional range above the 256-frame floor is 1,280 frames, 26.667/29.025 ms. No raise occurred in the latest run.
- `playback_max_frames=1536`: maximum retained playback depth before excess is dropped; zero clean contribution.
- `playback_ring_frames=4096`: 85.333/92.880 ms storage capacity. The 1,536-frame maximum prevents normal use of the full capacity as latency.
- `capture_ring_frames=4096`: sender storage capacity for stalls, not a target. Capture-clock pacing held the latest mean below one packet.

`adaptive_playback_release_ppm=5000` and `adaptive_playback_ratio_ramp_ms=250` affect how long recovery delay persists, not clean delay. A 0.5% release removes one 64-frame recovery step in approximately 0.267 seconds at 48 kHz/0.290 seconds at 44.1 kHz. Removing the full possible 1,280-frame extra cushion takes approximately 5.33/5.80 seconds, plus the gradual 250 ms ratio transition.

`drift_correction=true`, `drift_smoothing=0.02`, `drift_deadband_ppm=25`, and `drift_max_correction_ppm=500` add no fixed buffer. They control long-term queue movement and playback ratio. The deadband permits up to 25 microseconds of relative clock movement per second, or 1.5 ms per minute, before correction is requested; the maximum correction is a 0.05% rate adjustment. The latest 10.94/0.85 ppm estimates remained inside the deadband and used a unity ratio.

### Outside the profile ledger

Network propagation, driver-reported input/output latency, converter delay, operating-system scheduling, and acoustic travel are not Fast-profile settings. The latest local public-hairpin path contributed approximately 0.34 ms one way. The drivers separately reported 105/149 Focusrite input/output frames and 130/154 TONEX input/output frames, approximately 5.87-6.33 ms for directional input-plus-output reports. Those figures cannot simply be added to the software ledger because driver output reporting may include the callback block Jam2 already counts. A physical electrical/acoustic loopback measurement is required for the exact mouth-to-ear total.

## 2026-08-23 coupled playback-floor real-ASIO sweep

Artifacts:

- `latency-runs/20260823-fast-floor-192-asio/`
- `latency-runs/20260823-fast-floor-128-asio/`
- `latency-runs/20260823-fast-floor-64-asio/`

This is a measurement sweep, not a pass/fail test. Every run and every observed fault is retained. Each setting ran for 30 seconds with the connected Focusrite USB ASIO and TONEX devices. The Focusrite requested and obtained a 32-frame callback; TONEX requested and obtained 64 frames. The session used 44.1 kHz, 64-frame PCM16 packets, the Fast profile, explicit localhost endpoints with STUN disabled, a 440 Hz native input tone on both peers, unity remote level, no local monitor, and no metronome. Stats were sampled every 100 ms with a 2-second warm-up. Queue means below use active-peer periodic samples from 5 seconds onward so startup is still retained in the artifacts and counters but does not dominate resident-queue averages.

Only these four coupled controls changed between cases:

| Case | `playout_delay_frames` | `playback_prefill_frames` | adaptive target | adaptive minimum | Configured floor at 44.1 kHz |
|---:|---:|---:|---:|---:|---:|
| 192 | 192 | 192 | 192 | 192 | 4.354 ms |
| 128 | 128 | 128 | 128 | 128 | 2.902 ms |
| 64 | 64 | 64 | 64 | 64 | 1.451 ms |

The 64-frame jitter target, 1,024-frame jitter maximum, 1,536-frame adaptive maximum, 1,536-frame playback maximum, drift controls, sample rate, packet size, device buffers, and all other profile controls stayed fixed.

### Resident-path and latency measurements

`Total current peer path` is the directly sampled overlapping peer mixer plus playback-ring state. The directional path estimate below is deliberately composed from non-overlapping live measurements:

`opposite sender capture queue + 64-frame jitter target + receiver total-current peer path + receiver callback + RTT / 2`

It is the same queue ledger used for the preceding GUI jam. It includes software-resident delay but not driver/converter or acoustic latency.

| Floor | Focusrite capture mean | TONEX capture mean | Focusrite total peer path | TONEX total peer path | Focusrite receives TONEX | TONEX receives Focusrite | Directional mean |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 192 | 45.71 frames | 27.90 frames | 161.59 frames | 106.75 frames | 6.503 ms | 6.387 ms | 6.445 ms |
| 128 | 43.74 frames | 30.98 frames | 83.89 frames | 228.38 frames | 4.812 ms | 9.103 ms | 6.958 ms |
| 64 | 43.12 frames | 28.93 frames | 137.63 frames | 96.26 frames | 5.982 ms | 6.103 ms | 6.043 ms |

The sender queue stayed consistent across the sweep, averaging 36.81/37.36/36.03 frames for 192/128/64. The result therefore comes from receiver queue placement and recovery rather than capture-pacing variation. The built-in receiver estimate plus the opposite sender capture queue likewise had nearly invariant directional means of 5.547/5.561/5.529 ms. That estimator only includes average playback depth, not the full directly sampled peer path, so the direct ledger above is the more complete setting comparison.

In this single sweep, 64 had the lowest directional mean, but it saved only 0.402 ms relative to 192 even though its configured floor was 2.902 ms lower. The 128 case was 0.513 ms slower than 192 in the two-direction mean: Focusrite's receive direction fell to 4.812 ms while TONEX's rose to 9.103 ms. This is not evidence that 128 intrinsically costs delay. It is evidence that a single 30-second run can place an extra packet and adaptive recovery on one direction, so the configured subtraction cannot be assumed to become an equal acoustic saving.

For reference, the preceding 256-frame GUI jam's direct ledger was approximately 9.00-9.13 ms by direction. It used the public-address hairpin route and ordinary GUI audio rather than this explicit-loopback tone workload, so its roughly 2-3 ms difference is contextual evidence, not a matched 256-to-candidate attribution.

### Adaptive state and queue placement

| Floor | Focusrite adaptive target, mean/range | TONEX adaptive target, mean/range | Focusrite total path range | TONEX total path range |
|---:|---:|---:|---:|---:|
| 192 | 192.17 / 192-236 | 192.00 / 192-192 | 95-447 frames | 0-192 frames |
| 128 | 128.00 / 128-128 | 128.26 / 128-171 | 32-160 frames | 63-319 frames |
| 64 | 64.20 / 64-116 | 64.00 / 64-64 | 95-191 frames | 0-192 frames |

Final adaptive counters, including shutdown, were:

| Floor | Focusrite raises/releases/bursts/padding frames | TONEX raises/releases/bursts/padding frames |
|---:|---:|---:|
| 192 | 4 / 21 / 4 / 1,217 | 0 / 0 / 0 / 128 |
| 128 | 2 / 0 / 2 / 384 | 1 / 64 / 1 / 192 |
| 64 | 5 / 85 / 5 / 641 | 1 / 64 / 1 / 64 |

Focusrite's last live/final targets were 236/427, 128/256, and 116/299 frames for the 192/128/64 cases. The large final values occurred after TONEX disconnected and must not be presented as steady-state latency, but they are retained because teardown behavior is part of the result. During the live interval, Focusrite raised 1/0/2 times and TONEX 0/1/1 times respectively.

### Network, scheduling, and callback measurements

Every case had zero sequence loss, duplication, out-of-order or late packets, missing/inserted audio, jitter drops, forced or timeout releases, playback drops, capture overruns/underruns, mixer deadline releases, missing mixer contributions, mixer capacity drops, UDP send drops, authentication failures, and clipped mixer samples.

| Floor | Focusrite packet gap avg/max | TONEX packet gap avg/max | Focusrite callback interval max | TONEX callback interval max | Focusrite pre-receive avg/max | TONEX pre-receive avg/max |
|---:|---:|---:|---:|---:|---:|---:|
| 192 | 1.4512 / 4.040 ms | 1.4512 / 3.171 ms | 1.346 ms | 3.986 ms | 6.52 / 2,006 us | 5.46 / 2,044 us |
| 128 | 1.4512 / 3.692 ms | 1.4513 / 3.061 ms | 1.403 ms | 4.279 ms | 7.01 / 2,001 us | 6.63 / 2,017 us |
| 64 | 1.4513 / 4.296 ms | 1.4512 / 4.273 ms | 1.464 ms | 4.042 ms | 6.49 / 1,999 us | 7.42 / 3,374 us |

The 192/128/64 Focusrite packet-gap-over-2x counts were 1/3/1 and TONEX counts were 3/1/1; every over-4x count was zero. The unchanged 1.451 ms mean confirms that no setting caused packet bursts. The 64-floor plus unchanged jitter target supplies 128 frames/2.902 ms of scheduled protection, below both observed 64-case packet-gap maxima. The 128 floor supplies 192 frames/4.354 ms including jitter, only narrowly above the 4.279 ms TONEX callback maximum. The 192 floor supplies 256 frames/5.805 ms including jitter and retained the largest measured timing margin.

Hot-path averages did not materially change with the floor:

| Floor | Focusrite receive avg/max | TONEX receive avg/max | Focusrite callback work avg/max | TONEX callback work avg/max | Capture wake max, Focusrite/TONEX |
|---:|---:|---:|---:|---:|---:|
| 192 | 2.77 / 130 us | 2.56 / 159 us | 4.57 / 281 us | 7.34 / 321 us | 918 / 1,192 us |
| 128 | 2.70 / 155 us | 3.18 / 219 us | 5.10 / 323 us | 8.31 / 330 us | 1,046 / 762 us |
| 64 | 3.13 / 384 us | 3.14 / 104 us | 5.02 / 386 us | 8.52 / 298 us | 725 / 1,233 us |

The occasional pre-receive spike remains dominated by the approximately 2 ms maintenance/scheduling interval, with one 3.374 ms TONEX maximum at 64. Average receive and callback work remains only a few microseconds and does not explain the millisecond queue changes.

### Playback underruns, retained without pass/fail filtering

The final counters and their active/teardown split were:

| Floor | Focusrite live frames/events | Focusrite after peer inactive | Focusrite final total | TONEX live/final frames/events |
|---:|---:|---:|---:|---:|
| 192 | 32 / 1 | 6,623 / 208 | 6,655 / 209 | 0 / 0 |
| 128 | 0 / 0 | 4,992 / 156 | 4,992 / 156 | 64 / 1 |
| 64 | 64 / 2 | 5,663 / 178 | 5,727 / 180 | 64 / 1 |

The large Focusrite totals are shutdown behavior after TONEX's active peer disappeared, not deleted or rejected measurements. The live counters are more relevant to sustained listening, while the final totals remain relevant to teardown quality and recorder tails. Playback overruns and explicit playback-dropped frames were zero in all six peers.

### Sample-level WAV analysis

All 30 WAV stems were parsed sample by sample. All were mono PCM16 at 44.1 kHz, all six metronome stems were exactly silent as configured, and all six local `my-input.wav` tones were continuous: 439.99987-440.00018 Hz, peak 4,096, 64-frame-block median RMS 2,896.31, maximum adjacent delta 257, no delta of 1,000 or more, no multi-sample internal zero run, and no clipping. With local monitoring off, `mix.wav` equals the received `their-input.wav`; that received stem is therefore the direct artifact measurement.

| Floor | Receiver | Measured remote frequency | Median active RMS | Maximum adjacent delta | Deltas >= 1,000 | Internal zero-filled segments |
|---:|---|---:|---:|---:|---:|---|
| 192 | Focusrite | 439.94656 Hz | 2,895.08 | 3,236 | 2 | 224 frames/5.079 ms at 30.820 s, teardown boundary |
| 192 | TONEX | 439.99994 Hz | 2,896.31 | 257 | 0 | none |
| 128 | Focusrite | 439.99976 Hz | 2,896.31 | 257 | 0 | none |
| 128 | TONEX | 439.93593 Hz | 2,895.24 | 2,544 | 2 | 192 frames/4.354 ms at 10.942 s, steady interval |
| 64 | Focusrite | 439.93570 Hz | 2,894.97 | 2,538 | 2 | 96 frames/2.177 ms at 3.000 s, steady; another 96 frames/2.177 ms at 30.853 s, teardown boundary |
| 64 | TONEX | 439.95716 Hz | 2,895.39 | 1,841 | 2 | 128 frames/2.902 ms at 0.242 s, startup boundary |

Every remote stem retained peak 4,096 and had zero clipped samples. The small apparent frequency reductions are caused by counting crossings across a zero-filled segment; the uninterrupted local references confirm the tone generator itself stayed at 440 Hz. Large deltas occur at the entry/exit boundaries of the listed gaps. Thus the 192 case had no mid-run received-WAV zero gap, 128 had one 4.354 ms mid-run gap on TONEX, and 64 had one 2.177 ms mid-run gap on Focusrite plus the retained startup and teardown gaps.

The six `inputs-mix.wav` stems were also scanned. None clipped; their peaks ranged from 4,096 to 8,190. Because both peers intentionally sent the same 440 Hz tone, this local-plus-remote stem shows phase cancellation and reinforcement and is not a stable degradation metric. Its large deltas coincide with the received-tone discontinuities above. Future tone comparison should use different peer frequencies if the CLI gains another deterministic tone choice.

All recording manifests reported zero dropped frames, zero drop events, and zero writer errors. Five manifests had `frames_queued == frames_written`. The 128-frame Focusrite manifest queued 1,375,840 frames but wrote 1,375,776, leaving one 64-frame tail difference despite reporting zero drops/errors. Its WAVs are internally consistent at the written length. This is retained as a recorder-tail metadata anomaly for follow-up rather than hidden by the sweep result.

### Interpretation for manual listening

- 192 is the strongest first manual candidate from this run: its live audio had only one 32-frame underrun counter on Focusrite, neither direction had a mid-run zero-filled WAV segment, and its direct software-path mean was 6.445 ms.
- 128 is the most asymmetric result. Focusrite's receive path was the fastest measured direction at 4.812 ms, but TONEX rose to 9.103 ms and contains a measurable 4.354 ms mid-run gap. Manual listening can establish how noticeable that single event is, but this trace does not support making it the Fast default yet.
- 64 produced the lowest single-run directional mean at 6.043 ms and the most balanced directions, only 0.402 ms below 192. It also exhausted the measured gap margin, triggered live recovery on both peers, and contains a 2.177 ms mid-run Focusrite gap plus a TONEX startup gap. It remains useful as the most aggressive manual setting, not as a clean assumed winner.

No profile value or production code was changed from this sweep, and no CTest was run. Repetition and manual real-input listening are needed before selecting a shipped Fast floor; these retained traces provide the exact numerical and waveform comparison for that decision.

## 2026-08-23 latest GUI partial-64 profile comparison

Latest successful paired GUI logs:

- `release/logs/jam2_stats_20260823_171911_220_pid39600.csv` (Focusrite USB ASIO)
- `release/logs/jam2_stats_20260823_171935_776_pid42756.csv` (TONEX)
- matching GUI logs `jam2_gui_20260823_161617_702_pid39600.log` and `jam2_gui_20260823_161622_506_pid42756.log`

The first 48 kHz creation attempt in `jam2_stats_20260823_171804_939_pid39600.csv` never attached an active peer because TONEX rejected the unsupported sample rate. It contains no jam latency evidence. The successful session negotiated 44.1 kHz PCM16, 64-frame packets, a 32-frame Focusrite callback, and a 64-frame TONEX callback. Its live interval was approximately 244 seconds. The two GUI instances selected the public-address hairpin endpoints `81.86.171.138:49000/49001`; the immediately preceding automated 64-floor run used explicit `127.0.0.1` loopback.

### Actual profile controls

The latest GUI profile was not the fully coupled 64-frame case from the preceding sweep:

| Control | Latest GUI partial-64 | Preceding full-64 loopback | Previous matched GUI-256 |
|---|---:|---:|---:|
| Playback prefill | 256 | 64 | 256 |
| Playout delay | 64 | 64 | 256 |
| Adaptive initial target/minimum | 64 / 64 | 64 / 64 | 256 / 256 |
| Adaptive maximum | 528 | 1,536 | 1,536 |
| Jitter target | 64 | 64 | 64 |
| Jitter maximum | 528 | 1,024 | 1,024 |
| Playback maximum | 1,536 | 1,536 | 1,536 |

Sample-time playout, adaptive cushion, drift correction and its 0.02/25/500 controls, 250 ms ratio ramp, capture/playback ring capacities, packet size, format, and device buffers were otherwise matched. Both latest peers held an adaptive target of exactly 64 frames throughout every live periodic sample and reported zero live adaptive raises/releases. TONEX's final teardown row rose to the 528-frame cap only after the creator stopped; that is not live latency.

Reducing adaptive and jitter maxima to 528 did not reduce steady latency because neither maximum was reached during live audio. Those settings only reduced recovery/capacity headroom. The setting with a direct steady-state consequence was leaving playback prefill at 256.

### Latency and resident queue comparison

Queue means use active-peer periodic rows beginning five seconds after connection. The complete path again uses:

`opposite sender capture + 64-frame jitter target + receiver total-current peer path + receiver callback + RTT / 2`

| Run | Focusrite total peer path | TONEX total peer path | Focusrite receives TONEX | TONEX receives Focusrite | Directional mean |
|---|---:|---:|---:|---:|---:|
| Latest partial-64 GUI | 257.13 frames | 152.27 frames | 8.804 ms | 7.691 ms | 8.248 ms |
| Previous matched GUI-256 | 266.95 frames | 211.19 frames | 9.129 ms | 9.005 ms | 9.067 ms |
| Preceding full-64 loopback | 138.10 frames | 96.39 frames | 5.990 ms | 6.109 ms | 6.049 ms |

Against the previous GUI-256 session, which best matches lifecycle and route, the new profile reduced the directional mean by 0.819 ms. The decomposition is:

- Pair-mean receiver path fell 34.37 frames, from 239.07 to 204.70: 0.779 ms.
- Pair-mean sender capture fell 1.12 frames: 0.025 ms.
- Mean one-way RTT contribution fell only 0.015 ms.

Approximately 95% of the measured improvement therefore came from queue reduction, not a faster network. The directional effect was uneven: Focusrite receive improved by 0.325 ms and TONEX receive by 1.313 ms. TONEX's peer mixer residue fell from 61 to 4 frames, while Focusrite remained at 63 frames; this packet-phase placement explains most of the asymmetry.

The latest profile remained 2.198 ms slower than the immediately preceding fully coupled 64-frame loopback run. That difference decomposes into approximately 1.983 ms more receiver-resident queue, 0.288 ms more one-way public-hairpin RTT, and 0.073 ms less sender capture queue. The receiver increase was almost entirely playback-ring occupancy:

| Run | Focusrite playback ring mean | TONEX playback ring mean | Pair mean |
|---|---:|---:|---:|
| Latest partial-64 GUI | 194.13 frames | 148.27 frames | 171.20 frames |
| Preceding full-64 loopback | 75.10 frames | 96.39 frames | 85.75 frames |
| Difference | +119.03 frames | +51.88 frames | +85.46 frames / +1.938 ms |

Adding the two-frame pair-mean mixer-queue difference gives the observed 87.46-frame/1.983 ms receiver-path difference. This is direct evidence that `playback_prefill_frames=256` was not merely a momentary startup wait: once the global playback ring filled, balanced production/consumption retained much of that initial occupancy throughout the four-minute jam even though the per-peer adaptive target remained 64. The 192-frame configured prefill difference is 4.354 ms, but only approximately 1.98 ms remained as extra measured resident state because callbacks consume the initial fill in quantized blocks.

Jam2's existing `estimated_one_way_ms` reported 5.357/4.764 ms in the latest session versus 5.036/4.389 ms in the full-64 loopback run. Most of that approximately 0.35 ms change is RTT. This estimator does not expose the extra global playback-ring occupancy demonstrated by `playback_ring_readable_frames` and `total_current_buffered_frames`; relying on it alone would miss most of the prefill effect.

### Stability result

Throughout the latest 244-second live interval, both peers had zero sequence loss, missing/inserted audio, jitter drops, playback drops, capture faults, playback underruns, mixer deadlines, missing mixer contributions, and adaptive raises. Packet-gap averages remained 1.4512/1.4513 ms and maxima were 2.525/2.984 ms, substantially below the preceding full-64 run's 4.296/4.273 ms maxima. No WAV was recorded, so this run has counter evidence rather than sample-level artifact evidence.

After the creator stopped, TONEX's final 1.2-second teardown interval accumulated 23,104 playback-underrun frames, 361 underrun events, 358 mixer deadlines, 22,908 missing mixer frames, and eight adaptive raises to the 528-frame maximum. All were zero in its last live periodic row. They are retained as teardown faults and are not included in the live latency or stability claim.

The profile therefore delivered a real but much smaller reduction than the nominal 256-to-64 playout change suggests: approximately 0.82 ms against the matched GUI-256 run. Leaving prefill at 256 preserved about 1.98 ms more resident receiver delay than the full-64 run and also coincided with a clean, longer live interval. The next controlled profile comparison should change only prefill to 192 or 128 while retaining this exact GUI route and all other settings; that isolates how much of this stability margin can be removed without returning to the short full-64 run's discontinuities.

## 2026-08-23 GUI follow-up with playback prefill also at 64

New logs:

- `release/logs/jam2_stats_20260823_173319_008_pid24844.csv` (Focusrite USB ASIO)
- `release/logs/jam2_stats_20260823_173347_449_pid37704.csv` (TONEX)
- matching GUI logs `jam2_gui_20260823_163221_627_pid24844.log` and `jam2_gui_20260823_163231_217_pid37704.log`

This run retained the matched GUI/public-hairpin setup: 44.1 kHz PCM16, 64-frame packets, Focusrite 32-frame callback, TONEX 64-frame callback, and public endpoints `81.86.171.138:49000/49001`. The shared live interval was approximately 94 seconds. Playback prefill, playout delay, adaptive target, and adaptive minimum were all 64. Jitter target remained 64. Adaptive and jitter maxima were 512 rather than the preceding partial-64 run's 528; neither maximum was reached during live audio, so that 16-frame capacity difference did not contribute steady delay.

### Updated comparison

| Run | Focusrite playback ring | TONEX playback ring | Focusrite total peer path | TONEX total peer path | Focusrite receives TONEX | TONEX receives Focusrite | Directional mean |
|---|---:|---:|---:|---:|---:|---:|---:|
| New full-64 GUI | 132.27 frames | 78.22 frames | 133.27 frames | 78.22 frames | 5.924 ms | 5.946 ms | 5.935 ms |
| Prior partial-64 GUI, prefill 256 | 194.13 frames | 148.27 frames | 257.13 frames | 152.27 frames | 8.804 ms | 7.691 ms | 8.248 ms |
| Previous matched GUI-256 | 203.95 frames | 150.19 frames | 266.95 frames | 211.19 frames | 9.129 ms | 9.005 ms | 9.067 ms |
| Automated full-64 explicit loopback | 75.10 frames | 96.39 frames | 138.10 frames | 96.39 frames | 5.990 ms | 6.109 ms | 6.049 ms |

Lowering prefill from 256 to 64 in the matched GUI setup reduced the measured mean by 2.313 ms and balanced the directions from 8.804/7.691 to 5.924/5.946 ms. The decomposition versus the prior partial-64 GUI run is:

- Pair-mean playback-ring occupancy fell 65.96 frames, from 171.20 to 105.25: 1.496 ms.
- Pair-mean per-peer mixer residue fell from 33.5 to 0.5 frames: another 0.748 ms of run-specific packet-phase placement.
- Pair-mean total receiver state therefore fell 98.96 frames: 2.244 ms.
- Sender capture fell 3.65 frames: 0.083 ms.
- The slightly higher RTT added 0.014 ms one way.

The global playback-ring reduction is the direct evidence for lowering prefill. The additional 33-frame mixer reduction is valuable observed latency but should not all be attributed to prefill: that queue can move by roughly one packet with startup/callback phase even when its settings are unchanged. The observed run-to-run improvement is 2.313 ms; approximately 1.50 ms of it is directly visible in the queue owned by the changed setting.

Against the original matched GUI-256 profile, the fully coupled 64 settings reduced the complete path mean by 3.132 ms. Pair-mean total receiver state fell 133.33 frames/3.023 ms, sender capture saved 0.108 ms, and mean one-way RTT was effectively unchanged. This is therefore an almost entirely profile/queue result, not a route improvement.

The new public-hairpin run also reproduced the preceding automated full-64 result: 5.935 versus 6.049 ms directional mean. It overcame approximately 0.302 ms more one-way RTT by carrying 11.5 fewer pair-mean receiver frames and 6.85 fewer sender frames. The close result across different runtimes is stronger evidence that a fully coupled 64 profile puts this device pair near a 6 ms software-resident path.

Jam2's built-in receiver estimates were 3.988/4.688 ms, averaging 4.338 ms. They remained lower than the complete 5.935 ms ledger because they do not contain all global playback-ring and sender-capture occupancy.

### Live fault evidence

Both peers held a 64-frame adaptive target in every live periodic sample. Focusrite accumulated one live 32-frame playback underrun, one adaptive raise, one 64-frame padding event, and 64 releases back to the minimum; the target rise and release occurred between the two-second samples. TONEX had one 64-frame underrun and adaptive event before its first active periodic sample, then zero additional live faults. There was zero sequence loss, missing/inserted audio, jitter drop, playback drop, capture fault, mixer deadline, or mixer missing frame during the shared live interval.

Packet-gap means remained 1.4512 ms. Maxima were 2.865 ms on Focusrite and 3.018 ms on TONEX. The latter is just above the 128-frame/2.902 ms combined 64-frame floor plus 64-frame jitter target, consistent with the profile operating at the edge and the retained underrun/adaptive evidence. No WAV was recorded, so sample-level pops or zero runs cannot be accepted or excluded for this run.

After the creator stopped, TONEX's final teardown interval added 17,664 underrun frames, 276 underrun events, 273 mixer deadlines, 17,472 missing mixer frames, and seven raises to its 512-frame maximum. These values were zero in the last live row and remain classified as teardown behavior rather than live latency.

This follow-up confirms that prefill was materially limiting the previous partial-64 profile. The fully coupled 64 GUI settings removed approximately 2.31 ms in this matched comparison and approximately 3.13 ms relative to the original 256 profile, while retaining only a marginal one-event live underrun signal and no packet loss. Manual listening and a recorded repeat remain necessary to determine whether that margin is audibly clean enough for the aggressive Fast profile.

## 2026-08-23 fully coupled 64-frame settings promoted to Fast

Following the measured GUI comparison, the public Fast profile now owns this playback tuple for CLI, Start Jam, and Join Jam:

| Control | Fast default |
|---|---:|
| Playback prefill | 64 frames |
| Playout delay | 64 frames |
| Jitter target | 64 frames |
| Jitter maximum | 512 frames |
| Adaptive target | 64 frames |
| Adaptive minimum | 64 frames |
| Adaptive maximum | 512 frames |

The unchanged Fast controls retain a 32-frame requested callback, 64-frame network packet, 1,536-frame playback maximum, 4,096-frame capture/playback rings, adaptive cushion, 5,000 ppm release, 250 ms ratio ramp, and the existing drift controls. Create continues to own the session's 48 kHz nominal sample rate and 64-frame packetization; a device-constrained session can still negotiate 44.1 kHz as in the TONEX measurements.

The core `JoinProfile` is the single profile source used by the Fast join selection and by the Fast creator's local pointer. GUI `LocalTuningPreference` defaults now match it for both create and join. Preference schema 7 migrates only the exact former Fast latency tuple (`256/256/64/1024/256/256/1536`) to the new values. A difference in any changed numeric field is treated as an explicit user choice and is preserved; unrelated device tuning such as a 64-frame TONEX callback is also preserved.

The normal optimized public `release/jam2.exe` build succeeded. Focused native validation passed:

- `jam2_core_boundary_units`: the shared create/join Fast profile exposes the exact 64/512 tuple.
- `jam2_user_preferences_units`: new create/join defaults, exact old-default migration, custom-tuple preservation, and device-buffer preservation.
- `jam2_gui_widget_boundary_units`: both Start Jam and Join Jam open with the tuple and reapply every value when Fast is selected after another profile.
- `jam2_cli_boundary_units`: CLI profile/default parsing remains valid.
- `jam2_four_session_command_integration`: exactly four peers retain the shared direct-mesh/fake-audio workflow with the new Fast preset.

No full CTest suite was run.

## macOS CoreAudio latency-efficiency handoff

This is the continuation point for the same investigation on macOS. The objective is to validate that the shared Windows-tested refinements are actually active and clean under CoreAudio, then optimize only the remaining CoreAudio device boundary. Do not reopen protocol ownership, add a two-peer shortcut, or change the new Fast values until the current macOS path has been measured. Preserve every run, including underrun or artifact runs, and append each before/after result to this document.

### How much of the Windows work macOS already shares

By functional ownership, approximately 85-90% of the retained work is cross-platform and approximately 10-15% is device/backend-specific. This is an engineering classification, not a source-line calculation. Nearly all changes capable of removing resident network or mixer frames are shared; the remaining CoreAudio-specific opportunities are primarily callback CPU, scheduling evidence, buffer negotiation/reporting, and hardware latency.

| Area | macOS status | Notes |
|---|---|---|
| Protocol packing/authentication and direct Q31 PCM encode/decode | Shared now | CoreAudio converts Float32 device samples to/from Jam2's Q31 domain only at the device boundary. |
| UDP receive batching, nonblocking drains, one peer lookup, bounded telemetry and receive-priority maintenance | Shared now | The POSIX socket path is used on macOS. |
| Reorder/jitter handoff, peer-stream idle advance, peer-mixer batching/readiness, unity resampler path and common two-to-four-peer mixer | Shared now | These own most transport/mixer CPU and all peers retain one stable path. |
| Playback-ring batching, adaptive playback, recorder-ring changes, inactive recorder gating, metering consolidation, input-router work and cached metronome waves | Shared or already mirrored in the CoreAudio callback | The macOS source contains the required scratch allocation and helper calls, but the Windows build could not compile or execute it. |
| Demand-armed capture-ready wake | Shared design, POSIX implementation | macOS uses the nonblocking-pipe wake in the socket `select` set instead of the Windows event. It must be proven on macOS. |
| Capture-synchronised and callback-sized packet pacing | Shared now | It uses the active device callback size. A 32-frame CoreAudio callback with a 64-frame network packet should activate capture-clock pacing and keep capture close to the live edge. |
| Fast profile and create/join defaults | Shared now | Current Fast owns 64-frame prefill/playout/jitter/adaptive target/minimum and 512-frame jitter/adaptive maxima. |
| Process/packet-worker priority | Partly platform-specific | Current High maps to user-interactive QoS on the macOS packet worker. Windows High/MMCSS and timer behavior do not transfer. |
| ASIO contiguous output fan-out | Windows-only | CoreAudio still has its own Float32 `AudioBufferList` conversion/fan-out loops and needs an equivalent review. |
| `IASIO::outputReady()` | ASIO-only | CoreAudio's IOProc is already the driver render contract; do not invent an `outputReady` analogue. |
| Windows Realtime/MMCSS and scheduler/DPC experiments | Windows-only evidence | Do not port whole-process Realtime. Measure macOS first. CoreAudio supplies its IOProc real-time thread; Apple says framework-provided audio threads are automatically associated with the device audio workgroup. |

The practical consequence is that a CoreAudio pass should not rediscover the network protocol or mixer work. Start by verifying those shared gains in current macOS stats. The likely new code wins are concentrated in `libs/jam2-core/src/audio_device_macos.cpp`.

### Historical Robbie/Axe-Fx CoreAudio evidence carried into the handoff

The macOS machine may not have the copied logs, so the useful evidence is reproduced here. The original Windows-side copies were:

- `release/logs/jam2_gui_20260821_112703_749_pid12139-robbie.log`
- `release/logs/jam2_stats_20260821_122711_636_pid12139-robbie.csv`
- `release/logs/jam2_stats_20260821_125309_331_pid12139-robbie.csv`
- `release/logs/jam2_stats_20260821_131738_798_pid12139-robbie.csv`

All three CSVs were real macOS/CoreAudio sessions using an Axe-Fx III over USB. The device exposed eight packed/interleaved Float32 input and output channels at 48 kHz. Jam2 selected inputs 1-4 and outputs 1-2, requested and obtained a 32-frame callback, sent 64-frame PCM16 packets, used a 256-frame playback prefill, and still used the old 512-frame jitter target. High priority reported user-interactive QoS. These are especially relevant to CoreAudio conversion work because the callback traversed four physical input channels, performed the smoothed mono downmix, and fanned the mono result to two output channels every 0.667 ms.

The following values are from the last periodic row with one active remote peer, before teardown. `Capture periodic mean` is the mean of the instantaneous capture-ring field across all active periodic rows. The logs predate callback-work, pre-receive-stage, capture-ready-dispatch, driver-latency, mixer-queue and total-current-path fields.

| Robbie run | Active span | Capture periodic mean / last | Playback average / maximum | Reported one-way | RTT average | Packet-gap / jitter maximum | Callback average / maximum |
|---|---:|---:|---:|---:|---:|---:|---:|
| `122711` | 1,520 s | 3,087 / 4,000 frames | 2,371.53 / 3,937 frames = 49.407 / 82.021 ms | 73.272 ms | 25.064 ms | 106.394 / 105.061 ms | 0.666599 / 1.348 ms |
| `125309` | 1,464 s | 2,462 / 2,624 frames | 2,977.98 / 5,632 frames = 62.041 / 117.333 ms | 86.708 ms | 26.667 ms | 268.320 / 266.987 ms | 0.666618 / 30.658 ms |
| `131738` | 588 s | 1,274 / 2,304 frames | 2,141.06 / 3,838 frames = 44.605 / 79.958 ms | 68.774 ms | 25.670 ms | 101.398 / 100.065 ms | 0.666617 / 30.658 ms |

Additional last-active-row fault evidence was:

| Robbie run | Missing ranges / inserted frames | Jitter dropped packets | Playback-underrun frames/events | Adaptive target and raises/releases |
|---|---:|---:|---:|---:|
| `122711` | 12 / 3,264 | 18 | 15,833 / 15,430 | 1,536 frames; 308 / 18,276 |
| `125309` | 6 / 1,600 | 5 | 519,177 / 518,371 | 256 frames; 247 / 15,668 |
| `131738` | 0 / 0 | 0 | 524,860 / 523,713 | 256 frames; 107 / 6,814 |

Sequence-loss event counters were zero, while the first two final percentage fields were only 0.0029% and 0.0018%; the audible-risk evidence instead lies in the enormous packet/jitter gaps, inserted frames, jitter drops, and playback-underrun frames. Old underrun events were effectively counted per missing sample and are not directly comparable with the newer batched-event semantics. Compare frame totals and WAV discontinuities, not only event totals.

These sessions are a regression reference, not a current baseline. They used the old 512-frame/10.667 ms jitter target and predated capture-ready wake, callback-sized capture-clock pacing, mixer/packet batching, the current callback instrumentation, and the fully coupled 64-frame Fast floor. The old estimated one-way value decomposes consistently: approximately 44.6-62.0 ms playback depth + 10.667 ms jitter + 0.667 ms callback + 12.5-13.3 ms RTT/2. The playback and capture rings, rather than CoreAudio's healthy 0.667 ms average callback cadence, caused most of the software delay.

Current shared-path expectations on the same Axe-Fx configuration are therefore concrete:

- `capture_clock_packet_pacing_active=yes` with a 32-frame callback and 64-frame packet;
- capture publication-to-dispatch normally in microseconds and capture-ring residency close to one packet, not 1,274-4,000 frames;
- no persistent 2,141-2,978-frame playback average in a clean run with the current 64-frame receiver floor;
- 64-frame packet gaps averaging approximately 1.333 ms at 48 kHz;
- callback intervals still averaging approximately 0.667 ms when the Axe-Fx accepts 32 frames;
- callback work, current mixer/playback occupancy, and the existing device/safety-offset latency terms recorded by the current schema; add CoreAudio overload, abnormal-stop, timestamp, and stream-latency fields where the OS exposes them;
- remote internet packet gaps may still be far larger than the new 64-frame floor. The historical 100-267 ms spikes are network/host events, not evidence that CoreAudio conversion itself was slow, and must not be hidden when judging audio robustness.

The GUI log also records the separate known bug `internal loopback recording is currently implemented on Windows only`, followed by a failed loopback take. That remains the macOS follow-up in `PLAN.md`. Do not confuse it with Jam recording used for latency waveform validation; fix and test it separately unless explicitly included in the task.

### macOS evaluation workflow

1. Read the 2026-08-23 sections above before changing code. Build the normal optimized public macOS executable with Apple tooling. Run the existing focused native tests that compile the shared callback helpers and CoreAudio source before trusting the port. Do not make a Windows-only build result stand in for this check.
2. Inventory the real device before each run: macOS version and hardware, device/UID/transport, selected channels, physical/virtual `AudioBufferList` shape, requested and active sample rate, requested and active buffer frames, variable-buffer-size flag, device latency, safety offset, and any stream latency. Keep raw terms separate so the callback block is not double-counted in a physical ledger.
3. Establish an unchanged current-build baseline before editing. Prefer two real CoreAudio peers. If only one real device is available, use one real CoreAudio peer against one local headless/fake peer so CoreAudio callback cost is still real; do not present the fake peer's device latency as hardware evidence. Run at least 30 seconds after a five-second warm-up, use the current Fast 48 kHz/64-frame/64-floor profile where supported, and make two unchanged repeats when queue placement differs by a packet.
4. Keep route and hardware stable within every A/B. Record whether the peers use loopback, LAN, public hairpin, or remote internet. Never attribute RTT movement to a callback optimization. For an Axe-Fx historical comparison, retain 48 kHz, 32 callback frames, inputs 1-4 and outputs 1-2 if that configuration is still available.
5. Capture the full current CSV evidence: callback interval and callback work; CoreAudio overload/abnormal-stop counts if added; capture wake transitions and publication-to-dispatch min/average/max; capture-ring current/min/max; packet gaps; receive-loop and pre-receive stage times; receive work; RTT; jitter target/depth/drops/late/forced releases; adaptive target/raises/releases/padding; peer mixer and global playback-ring occupancy; total current peer path; underruns/overruns/deadlines/missing frames; recorder drops/errors; and the complete receiver-plus-opposite-sender latency ledger.
6. Record the jam and inspect each remote stem sample-by-sample for sample rate/length, frequency, peak, active RMS, maximum adjacent delta, large-delta count, internal zero-filled spans, clipping, and recorder manifest agreement. Distinct peer tones are preferable if the available input seam supports them; otherwise analyze each isolated remote stem and do not use a same-frequency `inputs-mix.wav` as a degradation metric.
7. Make one small change per iteration, rerun the same workload, retain rejected runs, and record both CPU and queue results here. A callback CPU saving is valid even when packet-phase noise moves the estimated milliseconds, but do not claim fixed latency unless a resident frame count or hardware timestamp actually falls.
8. Add or update the lowest-owned native C++/CTest case with every production change. CoreAudio layout/conversion helpers should have deterministic native tests for interleaved and noninterleaved Float32 buffers, selected-channel mapping, rails, silence, and fan-out. Run the focused owner tests after every change. If shared transport, mixer, packet timing, or synchronization behavior changes, also retain the exactly-four-peer workflow. Leave the full suite for the normal distribution checkpoint unless explicitly requested earlier.

### Prioritized CoreAudio-specific review

1. **Compile and behavior audit first.** The current CoreAudio callback was updated in parallel with the ASIO callback but has not been compiled by the Windows validation. Resolve only genuine Apple compile/runtime defects, then capture an unchanged hardware baseline before optimizing.
2. **Resolve `AudioBufferList` channel views once per callback.** The current `read_float_channel` and `write_float_channel` helpers rescan the buffer list to map a global channel for every sample. The ordinary four-input downmix reads every selected input once for peak analysis and again for mixing. Output loops frame-by-frame, then selected-channel-by-selected-channel, and remaps the buffer on every write. Build allocation-free typed views from the actual callback lists once, retaining arbitrary interleaved/noninterleaved layouts, null stream buffers, channel selection, and bounds. Then use contiguous or strided loops per channel. This is the direct CoreAudio analogue of the retained ASIO fan-out improvement and is the strongest static backend candidate.
3. **Calculate input/output frame counts once.** `buffer_frames(input/output)` currently walks every buffer several times per callback. Snapshot the validated input and output frame counts at callback entry and reuse them. Keep the real returned size visible; do not assume it always equals the requested buffer when the device reports variable frame sizes.
4. **Profile Float32/Q31 conversion after channel mapping.** CoreAudio uniquely pays Float32-to-Q31 input and Q31-to-Float32 output conversion. The current helpers promote through `double` and clamp every sample. Only replace them if an isolated native benchmark and real callback-work counters show value; preserve exact clipping/rail behavior and add exhaustive boundary tests before accepting a faster scalar or vectorized path.
5. **Use CoreAudio's own timing and fault evidence.** The IOProc already receives `inNow`, input time, and output time, but Jam2 ignores them. Add bounded lock-free stats for valid host/sample timestamps so scheduling wake delay and hardware input/output timing can be separated from callback execution. Register a non-real-time property listener/counter for `kAudioDeviceProcessorOverload` and, where supported, abnormal IO stop. Never log or allocate in the IOProc. Apple's IOProc documentation explicitly says the `inNow` time includes scheduling latency, making this better macOS attribution evidence than importing the Windows pre-receive-spike conclusion.
6. **Audit the physical latency ledger.** Jam2 currently publishes CoreAudio device latency + safety offset for each direction. Enumerate active streams and record `kAudioStreamPropertyLatency` separately where available, along with active buffer frames and IOProc timestamps. Do not silently add every term to `estimated_one_way_ms`; publish raw values first and use a physical electrical/acoustic loopback to establish which terms overlap.
7. **Verify buffer negotiation rather than assuming it.** The existing code sets `kAudioDevicePropertyBufferFrameSize` and reads it back before stream creation. Confirm requested/active values and property-change behavior on real devices at 32/64/128 frames, including devices that report variable buffer sizes. A smaller active callback is a direct scheduling lever; a rejected request must stay visible rather than being described as a Jam2 latency regression.
8. **Check stream usage only if the device exposes multiple streams.** `kAudioDevicePropertyIOProcStreamUsage` may allow unused device streams to be disabled. It cannot remove unused channels inside one interleaved eight-channel Axe-Fx stream, so inspect the topology before attempting it. Do not add hog/exclusive mode merely for tuning; it conflicts with simple multi-application use and has no measured latency benefit here.
9. **Treat Audio Workgroups as a later, evidence-gated design.** The CoreAudio IOProc thread is already managed with the device workgroup. The packet worker is asynchronous and currently receives user-interactive QoS. Only investigate a narrow asynchronous Audio Workgroup if macOS capture-publication-to-dispatch measurements show scheduling delay after the channel/conversion work is reduced. That would require a separately reviewed worker boundary with no unsafe real-time operations; do not make the whole process real-time.

### Completion criteria for the macOS pass

- The current shared capture-clock path is proven active and the old multi-thousand-frame sender/receiver backlogs are absent in matched clean runs.
- CoreAudio callback average and maximum work are measured before/after each backend change and remain comfortably below the active callback interval.
- Requested versus active buffer size, timestamp timing, overload/abnormal-stop evidence, device/safety/stream latency terms, and callback xruns are visible in structured stats where the OS exposes them.
- No retained optimization introduces sequence loss, capture/playback faults, jitter drops, mixer deadlines, recording drops, clipped samples, new internal zero spans, pops, or material tone/RMS change.
- Any claimed latency reduction is tied to fewer resident frames or a CoreAudio/hardware timestamp/loopback measurement; pure CPU savings are reported as CPU savings.
- Larger ideas such as a separate packet real-time worker, new mixer path, protocol change, aggregate-device architecture, or wholesale callback ownership change remain documented for review instead of being implemented without evidence.

## 2026-08-23 macOS CoreAudio implementation and validation

This pass used the public Fast profile throughout. No Fast or Moderate profile
number was changed and no profile sweep was performed. All comparative device
runs were local direct-mesh sessions at 48 kHz, 64-frame PCM24 packets and a
requested 32-frame callback. One peer opened the real CoreAudio device while a
local headless peer supplied a deterministic 440 Hz tone. Five-stem recording
was enabled so callback-work comparisons include the recording workload and so
the delivered waveform can be checked independently of the counters.

### Real Wi-Fi Fast jam retained as a separate profile baseline

The user-reported artifact run is
`release/logs/jam2_stats_20260823_180836_757_pid52098.csv`, with lifecycle
context in `release/logs/jam2_gui_20260823_170736_918_pid52098.log`. It used the
Aggregate Device, 48 kHz, a requested and active 32-frame callback, 64-frame
PCM16 packets, and the current Fast 64-frame prefill/playout/jitter/adaptive
floor with 512-frame maxima. The active CSV covers 61.577 seconds.

- CoreAudio cadence was healthy: 0.666760 ms callback average, 1.894 ms
  maximum, nine callbacks over 1.5x, and 1.301 us average callback work.
- Sequence loss was zero, but packet delivery was bursty: packet-gap average
  1.3334 ms, maximum 96.36 ms, 4,123 gaps over 2x, jitter average 1.284 ms and
  jitter maximum 95.027 ms.
- The mixer released 4,143 deadlines with 264,834 missing peer frames
  (5.517 seconds of absent peer contribution). The playback ring reported
  151,777 underrun frames in 4,771 events (3.162 seconds).
- Adaptive playback reached 415 frames in the final row after 240 raises,
  12,005 releases and 1,356 inserted padding frames. The periodic playback
  depth frequently held hundreds of frames even while isolated deadlines and
  drains continued.

This is consistent with the severe, persistent artifacts reported for that
Wi-Fi jam. It is not evidence of slow Float32 conversion or a late CoreAudio
IOProc: device cadence and callback work were healthy while network arrival
had repeated roughly 95 ms stalls. It remains the real-connection baseline for
the later Moderate-profile tuning. Per instruction, Fast remains the proven
Ethernet profile and this CoreAudio pass does not try to make Fast absorb Wi-Fi
burst loss.

### Current device inventory and unchanged baselines

The attached devices used for this pass were:

| Device | Current topology at 48 kHz/32 frames | Selected channels |
|---|---|---|
| Focusrite Scarlett 2i2 USB | one two-channel packed/interleaved Float32 input stream and one two-channel packed/interleaved Float32 output stream | input 1,2; output 1,2 |
| Aggregate Device (`~:AMS2_Aggregate:0`) | one mono packed Float32 input stream and one stereo packed/interleaved Float32 output stream | input 1; output 1,2 |

Both devices accepted 32, 64, 128 and 256 frames in the device diagnostic.
Both reported a fixed 32-frame callback during the actual runs. The Aggregate
Device available for this pass exposes only one input, so it cannot reproduce
the historical four-input Axe-Fx downmix workload.

Unchanged Release baselines were captured before editing:

| Baseline artifact | Device-side callback result | Fault result |
|---|---|---|
| `latency-runs/20260823-coreaudio-headless-baseline/` | synthetic 32-frame callback averaged 0.666669 ms; 4.081 us work; 0.844 ms maximum interval | zero loss/deadlines/missing frames; one 32-frame startup drain |
| `latency-runs/20260823-coreaudio-hardware-baseline/focusrite/` | Focusrite live-window work 4.842 us; 0.666665 ms interval average; 0.744 ms maximum | no callback deadlines or missing frames; isolated 64 startup/low-floor underrun frames |
| `latency-runs/20260823-coreaudio-hardware-baseline/aggregate/` | Aggregate live-window work 4.162 us; 0.666665 ms interval average; 0.793 ms maximum | no callback deadlines, loss or missing frames |

The headless figure is a shared-engine regression reference, not a measurement
of CoreAudio conversion. Its run-to-run microsecond work value moves with host
scheduling and was never used to claim a CoreAudio saving.

### Retained CoreAudio changes

1. `AudioBufferList` channel resolution now happens once per selected channel
   per callback. Allocation-free typed views retain the real interleaved or
   noninterleaved stride and frame limit. The input peak/downmix passes and
   output fan-out then perform direct strided access instead of rescanning the
   buffer list for every sample. The Q31-to-Float32 conversion is also done
   once per source frame before writing all selected outputs.
2. Input and output callback frame counts are each scanned once at callback
   entry and reused. Null data for a disabled CoreAudio stream no longer
   collapses the entire callback to zero frames or shifts later global channel
   numbers. Unselected output memory is left alone because the CoreAudio
   `AudioDeviceIOProc` contract explicitly supplies zeroed output memory.
3. Format validation and device descriptions enumerate every device stream and
   query each stream's virtual format. Jam2 now rejects layouts that are not
   exact packed Float32 in the shape the callback understands instead of
   consulting only the deprecated device-level first-stream property.
4. `kAudioDevicePropertyUsesVariableBufferFrameSizes` is read before stream
   construction. Every callback scratch buffer is allocated to the reported
   maximum rather than only the nominal minimum. Structured statistics expose
   nominal/maximum/variable frames, actual callback min/max/sample count and
   any callback that exceeds scratch capacity.
5. Device latency, safety offset and maximum stream latency are published as
   separate raw input/output fields. The existing driver-latency estimate
   remains device latency plus safety offset; stream latency is deliberately
   not silently added because the physical overlap has not been established.
6. The IOProc uses the supplied CoreAudio host timestamps to expose sampled
   cycle-to-callback-entry delay, input-to-cycle offset, cycle-to-output offset
   and absolute cycle-interval jitter. Valid/invalid timestamp counts,
   callback-size observations, callback gaps and fault counts are exact. The
   conversion-heavy timestamp aggregates sample one callback in 16 (about
   94 samples/second at 32 frames); their CSV sample counts make this explicit.
7. A minimal property listener counts `kAudioDeviceProcessorOverload` and,
   when the property exists, abnormal IO stops. The listener only performs a
   relaxed atomic increment and is removed after the device is stopped and
   before the IOProc is destroyed. No callback path allocates, locks, logs,
   throws or blocks.
8. CoreAudio timing aggregation and callback-owned counters use the existing
   Jam2 single-writer relaxed load/store pattern rather than locked atomic
   read-modify-write operations. This recovered the telemetry overhead seen in
   the first instrumented repeat while preserving exact counter values.

The CSV schema now has 488 consistently shaped columns in periodic and final
rows. The new CoreAudio fields include raw latency terms, stream counts,
listener registration/error state, fault counts, actual frame bounds and
capacity faults, timestamp validity, timing min/sum/max/sample counts, and the
equivalent human final summary.

### Iterations and callback-work evidence

`latency-runs/20260823-coreaudio-channel-views-headless/` confirmed that the
shared headless path remained functional after extracting the typed helper.
Because it does not execute the CoreAudio helper, its 4.433 us work result is
host noise rather than an A/B value. The matching first Focusrite hardware run,
`latency-runs/20260823-coreaudio-channel-views-focusrite/`, measured 4.594 us
live-window callback work versus the 4.842 us unchanged baseline (5.1% lower),
with the same fixed 32-frame cadence and no callback gaps or missing frames.

The fully instrumented repeats showed enough normal variation that a larger
claim would not be defensible: Focusrite live-window results ranged from about
3.8 to 4.84 us before telemetry was tightened. The final retained build gave:

| Final artifact and active window | Callback work | Cadence/fault result | CoreAudio timestamp averages |
|---|---:|---|---|
| `latency-runs/20260823-coreaudio-telemetry-sampled-focusrite/`, 16.504-27.477 s | 4.739 us across 16,460 callbacks; 2.1% below the unchanged 4.842 us run | fixed 32 frames; zero live underruns, callback gaps over 1.1x, overloads, abnormal stops, invalid timestamps, capacity faults, missing frames or deadlines | callback entry 28.815 us; input-to-cycle 2,199.121 us; cycle-to-output 967.556 us; absolute cycle jitter 4.660 us (1,029 timing samples) |
| `latency-runs/20260823-coreaudio-final-aggregate/`, 12.983-23.454 s | 3.866 us across 15,706 callbacks; 7.1% below the unchanged 4.162 us run | fixed 32 frames; zero post-startup underruns, gaps over 1.5x, overloads, abnormal stops, invalid timestamps, capacity faults, missing frames or deadlines | callback entry 27.942 us; input-to-cycle 1,551.876 us; cycle-to-output 2,906.476 us; absolute cycle jitter 5.758 us (about 1,000 timing samples) |

The final cumulative interval averages were 0.666666 ms on both devices. Their
maximum callback intervals were 0.718 ms on Focusrite and 0.785 ms on the
Aggregate Device. CPU savings are therefore reported only as a small backend
work reduction: no resident frame or physical latency reduction is claimed.
The callback used less than 1% of its 666.7 us period in these recorded local
sessions.

The raw latency ledgers explain why CoreAudio properties must remain separate:

| Device | Input device + safety + stream | Output device + safety + stream | Timestamp observation |
|---|---:|---:|---|
| Focusrite | 74 + 74 + 0 frames | 14 + 14 + 0 frames | input-to-cycle about 2.20 ms; cycle-to-output about 0.97 ms |
| Aggregate | 14 + 43 + 2,399 frames | 1 + 107 + 556 frames | input-to-cycle about 1.55 ms; cycle-to-output about 2.91 ms |

The Aggregate stream properties would imply roughly 50.0 ms input and 11.6 ms
output stream latency by themselves, which plainly does not match the IOProc
timestamp offsets. Those properties likely represent aggregate/subdevice HAL
compensation or overlapping pipeline terms. They are useful raw diagnostics,
but must not be summed into `estimated_one_way_ms` without an electrical or
acoustic loopback measurement.

### Waveform and shared-path checks

The final Focusrite and Aggregate recordings had zero recorder drops, writer
errors, clipped frames or pop detections. Their deterministic local/headless
440 Hz stems measured 440.367 Hz with the expected 0.125 peak. All long silence
spans were at session start or after peer teardown. The Aggregate hardware
input's short approximately 131 ms startup silence and the remote stems'
approximately 101 ms teardown tails were boundary conditions, not internal
live gaps.

A final two-headless-peer regression is retained at
`latency-runs/20260823-coreaudio-final-headless/`. It had zero sequence loss,
missing-frame insertion, late drops or mixer deadlines, and the recorded tones
had zero clipping/pops and no internal silence span of 2,048 frames or more.
The joiner nevertheless drained four isolated 32-frame playback blocks and
both synthetic callbacks had several scheduling gaps over 2x. This matches the
general Fast-floor sensitivity and the user's Wi-Fi observation; it is not a
CoreAudio regression and is intentionally left for the subsequent Moderate
profile exercise.

Capture-clock packet pacing was active throughout the real-device sessions.
The current local runs did not reproduce the historical multi-thousand-frame
capture backlog, persistent playback backlog, packet loss or missing-frame
behavior from the old Axe-Fx sessions.

### Candidates measured and not retained

- Float32-to-Q31 alternatives were slower when exact existing rail/truncation
  behavior was preserved: 1.377 ns/sample versus 0.793 ns/sample for the
  current conversion in an optimized isolated benchmark. A bit-identical
  Q31-to-Float32 power-of-two form measured 0.265 ns/sample versus
  0.603 ns/sample, but saves only about 11 ns for a 32-frame mono callback
  (approximately 0.24% of measured callback work). Neither change is material
  enough to justify a new numerical path.
- Both current devices expose exactly one input and one output stream. IOProc
  stream-usage restriction therefore cannot save work; it would only matter on
  a future device with multiple independent streams and would not disable
  unused channels inside one interleaved stream.
- No Audio Workgroup or whole-process scheduling change is justified. The
  IOProc had zero processor-overload notifications and tens-of-microseconds
  entry delay against a 666.7 us period. The packet worker remains the separate
  place to investigate only if future macOS capture-dispatch evidence is poor.
- No `kAudioDevicePropertyIOCycleUsage`, hog mode, aggregate-device ownership,
  protocol, mixer, or profile change was made. There is no measured latency or
  reliability evidence for those expansions in this local pass.

### Validation and remaining reconciliation notes

The optimized public app built successfully with Apple tooling, and the built
and released executables had the same SHA-256
`6902b0fc40bc0ea869220daa8cd44e0b60de4988e1cd5532a78afea9d81702fd`.
Focused native validation passed:

- `jam2_audio_device_processing_units`: packed/interleaved and planar channel
  views, null disabled-stream numbering, selected-channel conversion, silence,
  Float32/Q31 rails and duplicated output fan-out;
- `jam2_cli_boundary_units`: the 488-column final/periodic schema and new
  CoreAudio values.

No new CTest executable was added; the lowest-owned existing tests were
extended. The broader `jam2_core_boundary_units` executable was also tried
outside the sandbox. It still fails its pre-existing simultaneous pipe-wake/
UDP-readiness assertion; that first timing race leaves its datagram queued and
cascades into the following zero-timeout ordering assertion. No UDP source was
changed in this pass. The full suite was not run, as requested.

The following non-CoreAudio-path items should be reconciled later rather than
folded into this optimization:

- `jam2 local --log-stats` accepts the option but the local-only runtime does
  not currently produce the same CSV artifact as a network session. Hardware
  tuning therefore used a real device peer against a headless local peer.
- Periodic CSV rows retain blanks for some cold device/session context columns
  that are populated in the final row. The new callback/latency fields are
  present and row width is consistent, but repeating immutable context would
  make ad-hoc interval analysis easier. This is a cross-platform reporting
  cleanup, not an audio-path optimization.
- The GUI still reports that internal loopback recording is Windows-only on
  macOS. That remains the separate `PLAN.md` feature and was not confused with
  the working five-stem Jam recording used here.
- A future four-input CoreAudio device should rerun the channel-view workload
  because the current Aggregate exposes only one input. The deterministic
  native layouts already cover arbitrary selected-channel mapping until that
  hardware is available.

CoreAudio-specific work is complete for the available hardware: the callback
boundary is allocation-free, fixed-shape for the observed devices, measured,
fault-visible, comfortably inside its deadline and no longer performs
per-sample buffer-list discovery. The next phase is the explicitly separate
Moderate-profile Wi-Fi tuning using the retained real-connection baseline.
