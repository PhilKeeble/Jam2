# Real ASIO delivery reproduction

The separate `jam2_udp_transport_comparison` CTest compares the current owned
UDP socket and the actual `UdpMultiplexTransport` through `NetworkSession`.
Run it with `compile.cmd --in-dev-shell --tests delivery --test-name
jam2_udp_transport_comparison`. It creates an authenticated four-peer direct
mesh, uses ABBA backend order and two creator selections, and covers PCM16/24,
64/128 frames, regular/8 ms batched sends and synthetic capture notifications.
Windows requests the same 1 ms timer period as the normal network runtime.
JSON rows in `build/Testing/Temporary/LastTest.log` report send lateness,
send-to-receive time, receive gaps, wait times, loss and multiplex counters.
Assertions check packet conformance; timing is observational. Each condition
lasts 1.5 seconds. This isolates loopback transport behavior and does not run
real devices, the mixer, the full CLI runtime or a failing wired/WAN path.

`asio-oct30-focusrite-tonex.json` explicitly selects the two ASIO drivers by
name, with 44.1 kHz, 32-frame device callbacks and 128-frame PCM24 packets.
The first device is the creator: TONEX creates and Focusrite joins, matching
the Focusrite's joiner role in the October 30 jam. Swap the names to reverse
the roles without changing any network settings.
Generated tone samples replace the input signal **inside the real device
callback**; both processes still open their physical ASIO device, use its clock,
and render received audio through it. This does not measure analog round-trip
latency or emulate CoreAudio. Playback is at 0.1 remote gain with a quiet click.

Run from the repository root in Windows PowerShell:

```powershell
$env:JAM2_ASIO_DELIVERY_PROFILE = (Resolve-Path tests/profiles/asio-oct30-focusrite-tonex.json).Path
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests delivery --test-name jam2_asio_oct30_delivery_hardware"
```

Close other Jam2 sessions using these devices before the hardware run. No system
network configuration, installed traffic shaper, live server or firewall change
is needed. Test UDP forwarding is loopback-only and outside the application.
The children are always the normal `release/jam2.exe`.

The run contains a 40-second unshaped baseline and a 230-second impaired case.
The latter has 7.5 ms fixed delay in each direction, seeded 0..3.6 ms forward
and 0..0.5 ms reverse jitter, and additional creator-to-joiner delay:

| Time since first packet in that direction | Additional delay |
| --- | --- |
| 0..18 seconds | 0 ms |
| 19 seconds | 200 ms |
| 19.001 seconds | 325 ms (approximately 128 ms arrival gap) |
| 20 seconds | 400 ms |
| 22 seconds | 650 ms |
| 24 seconds onward | 0 ms after linear catch-up |

The envelope repeats after 180 seconds. Audio, clock observations and reliable
control cross the **same** impairment scheduler. There is no injected loss,
corruption or background bandwidth flood. These settings approximate the
October 30 CSV counts/RTT/gap measurements; those logs do not contain the packet
timestamps needed for an exact replay. The scheduler preserves order and bounds
each queue to 2048 datagrams.

Artifacts remain under `artifacts/asio-delivery/<UTC>-two-asio/`: the exact
profile, tested executable SHA-256, per-peer scenarios/stdout/stderr/CSV, proxy
JSONL (requested delay, queue high-water and release lateness), and case summary
JSON. Summary rates exclude the first periodic sample and the final two seconds
to avoid counting process shutdown as starvation.
Raw timelines retain cumulative underruns, trimming, recovery events, RTT,
buffering and grid-target error. Scheduler delay/lateness must be inspected
alongside application results; the harness itself is not a perfect network.

The test asserts real ASIO selection, requested rate/callback/packet sizes,
capture-clock pacing, normal cleanup, sustained connected observations, no
proxy capacity loss/send failure, the requested delay excursion, and final
packet-rate recovery within 15% with buffering at most 40 ms over the final
five seconds. Injected disturbances may produce underruns; those are reported,
not converted into a subjective score or silently used to tune production code.
CTest's pass condition verifies execution and eventual recovery. The performance
counters provide the evidence for comparisons between builds.

Without a configured hardware profile, the hardware CTest skips. The focused
`--tests delivery` suite also includes interpolation/validation/queue tests and
an exactly-four-peer fake-audio workflow. That case uses 128-frame fake callbacks
and 128-frame packets, selecting capture-clock pacing without requiring the OS
sleep scheduler to emulate a submillisecond hardware callback. It delays audio
and control on one edge, with an explicitly
accelerated 18-second recurrence. It keeps the other mesh paths running and
checks recovery. Run only the named CTest when isolating a measurement.

## Diagnostic matrices

Select a matrix by setting `JAM2_ASIO_DELIVERY_PROFILE` to its absolute path,
then use the same named hardware CTest command above. These matrices keep
TONEX as creator and Focusrite as joiner, at 44.1 kHz with 32-frame callbacks.

| Profile | Experiment |
| --- | --- |
| `asio-oct30-isolation.json` | Steady WAN control, then extra delay on beacons, audio, ping/pong, non-audio, and all packets separately. |
| `asio-delivery-batching.json` | Steady 64-frame control; delivery in 4/8 ms windows; 64/128-frame packets; release-disabled and leader-audio controls. |
| `asio-delivery-recurring-gaps.json` | 35 ms audio holds every 500 ms during seconds 15..40, followed by normal delivery; with and without buffer release. |
| `asio-clean-audio-baseline.json` | Unimpaired recorded tone and pulse cases at 64/128 frames. |
| `asio-regression-acceptance.json` | Clean recordings plus batching, recurring holds, the October 30 envelope and delayed beacons at 120/300 BPM. Add `reference_baseline_root` for clean comparisons. |

`delay_scope` chooses which datagrams receive the additional envelope/batching.
The other datagrams retain the fixed delay and jitter, using a separate queue
so they cannot be blocked behind the selected traffic. Proxy traces and case
summaries include selected/baseline counts for audio, beacons, ping/pong and
control. `batch_interval_ms` releases selected traffic at the next window
boundary. It does not drop packets or change source sample times.

Each matrix case saves `configuration.json` and `summary.json`; the root saves
`matrix-results.json`. Case failures retain their evidence and allow the other
cases to run. Rates cover the connected interval, excluding shutdown. For the
recurring-gap matrix, compare the explicit before/gaps/after windows rather
than averaging the disturbances across the full run.

The four-process workflow also checks audio-only and beacon-only delay. The
deterministic `jam2_audio_flow_coalescing` CTest exercises the production
NetworkSession/PeerStream/PeerMixer, callback resampler and rings with one local
device and three remote sources. It covers 0/4/8 ms delivery windows, perfect
clocks and 17 ppm sender skew, with the existing release setting at 5000 and
0 ppm. Normal release must have zero underrun, trim and padding frames during
the final simulated minute after two minutes of warmup, including an extended
30-minute skew case. With release deliberately disabled, the skew diagnostic
permits trimming within the source estimator's existing 10 ppm tolerance;
underruns and padding must still be zero. Clean average buffering is bounded
by the original implementation's measured 50.56/52.512 frames plus rounding.

`jam2_audio_flow_callback-phases` compares clean buffering exactly against the
old 50 ms gap threshold across callback phases, then exercises 4/8 ms batching.
`jam2_audio_flow_callback-runway` varies packet size, callback phase, clock skew
and worker service periods. `jam2_audio_flow_asio-cadence` repeats the clean
64/128-frame timing matrix at 44.1 kHz, including fractional callback periods.
`jam2_audio_flow_bounded-jitter` adds 7.5 ms fixed transit and seeded 0..3.6 ms
ordered arrival jitter at both packet sizes, clock skews and worker periods.
It passes on the current candidate; irregular real callback/wakeup behavior
remains outside that fixed-period fixture.
All use exactly four participants. The lower-level
`jam2_core_boundary_units` also reproduces a 32-frame callback / 96-frame worker
runway boundary: held interpolation history must reach playback before the
next worker wake, without adding padding, increasing the target or replaying
the sample in the next packet. Real queued audio takes precedence.
It also verifies that sustained surplus below two packet blocks can retire
when real callback quanta and worker runway are covered, and that release
stops when the worker runway is no longer available.

## Startup-recovery latency regression

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests delivery --test-name jam2_audio_flow_startup-recovery"
```

This deterministic four-participant CTest uses production NetworkSession,
PeerStream, PeerMixer, callback playback/resampling and rings at 44.1 kHz,
64-frame packets and 32-frame callbacks. It covers PCM16/24 and three worker/
callback timing combinations. It needs no real devices or rendezvous service.

Each combination compares a clean startup with three identical 32-frame startup
underruns. The next real packet arrives before the recovery decision, after the
decision but before the receive batch ends, or immediately after that batch.
The latter case requires genuine recovery padding. Subsequent timing is matched.
It measures source-plus-output depth during seconds 10..60, allowing at most one
extra frame against the equal-underrun control, with no later underruns, padding,
trimming, packet loss, missing-source debt or queued-audio drops.

Before the recovery-accounting candidate, 64 extra frames (1.451 ms) remained
against the equal-underrun control after the target returned to 64. The original reproduction is retained under
`artifacts/startup-recovery-20261103/`; attempted fixes and device evidence are
under `artifacts/startup-recovery-fix-20261103/`. None of those playback fixes
was accepted. The application changes from this experiment were reverted.

The archived worker-state probe reproduces blocked release at a 96-frame worker
peak with 64/128-frame buffer snapshots. It is diagnostic evidence only: those
snapshots do not prove that reducing the buffer is safe. It is not registered as
an acceptance CTest. The production callback/clock tests and real-ASIO recordings
remain the safety gates; relaxing the guard broke those tests. The expanded
startup regression remains an ordinary failing CTest, not WILL_FAIL. The current
recovery-accounting candidate reduces that difference to about 34 samples; it
still fails the unchanged one-sample retirement criterion.

`jam2_audio_flow_repeated-recovery` separately checks for accumulating latency:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests delivery --test-name jam2_audio_flow_repeated-recovery"
```

It uses the same production four-participant callback/session boundary, PCM16/24
and three timing combinations. Each case lasts 496 simulated seconds: a clean
reference, then thirty identical 1.6 or 4 ms packet-delivery pauses 16 seconds
apart; separate clean controls receive no pauses. Packet and callback phase repeat
exactly. During seconds 6..16 after every pause, actual source-plus-output depth
must remain within one frame of the first recovery in average, minimum and maximum.
Targets return to 64; no continuing corrections, packet loss, trimming, audio
drops or skipped source time are allowed. The longer pauses must exercise further
padding. Cumulative padding is reported separately from actual buffered depth.
All 18 cases pass; evidence is in `artifacts/repeated-recovery-20261103/`.
This establishes bounded retention for these isolated pauses, while the startup
retirement failure remains unresolved. It does not cover irregular device clocks,
socket scheduling or persistent GUI stop/rejoin, and is not a new ASIO run.

`jam2_audio_flow_large-recovery` tests whether a larger past disturbance leaves
a higher floor after delivery becomes clean:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests delivery --test-name jam2_audio_flow_large-recovery"
```

The same four-participant fixture runs 42 cases: PCM16/24, three timing pairs,
and a single 0/1.6/4/8/12/20/80 ms pause at second 16, ending at second 496.
Late quiet windows must settle within one sample of the matched 1.6 ms recovery;
the earlier startup test retains the stricter clean-baseline requirement. The
larger-pause baseline fails 24 of 30 comparisons: 4..20 ms pauses retain
about 128 samples (2.9 ms) above clean, versus about 95 for the small recovery.
Targets return to 64, while worker-observed output touches the 128-frame surplus
floor and the one-second release-stability interval keeps resetting. There are
no further quiet-window underruns, padding, trimming or hard recoveries. Severe
pauses explicitly permit obsolete-audio disposal during recovery and record it;
the 80 ms cases settle lower after hard recovery, without proving clean audio
during the outage. This is an ordinary acceptance CTest, not WILL_FAIL.
Baseline evidence is retained under `artifacts/large-recovery-20261103/`.

The current recovery-accounting candidate passes all 30 larger-pause comparisons
and the 18 repeated-pause cases without changing their gates. The 4..20 ms pauses
settle near 66 extra samples (1.5 ms), matching the small recovery. Playback
credits only actual extra source advances on fully supplied callbacks, excluding
interpolation prefetch and concealed shortages. Outstanding credit survives a
later empty output ring and stays bounded by the configured adaptive maximum.
Release at the configured 5000 ppm requires clean delivery and measured output
low water across the current/previous stability windows, preserving the profile
minimum, measured worker runway, another callback, two interpolation frames and
the actual stopping ramp. Initial prefill creates no recovery credit.

CSV appends seven `mix_recovery_*` fields: pending, retired and cancelled frames,
safe depth, measured low water, release event count and active state (542 fields
total). Credit can remain protected by the safety floor; it never requests more
padding. Native coverage includes counter deltas/reset, source lifecycle, invalid
peer commands, prefill and larger profile minimums. The strict startup test and
other pre-existing core failures remain. Native evidence is under
`artifacts/recovery-accounting-fix-20261103/`.

The real-device follow-up is recorded in `artifacts/asio-mode-review-20261103/`:
all eight saved pre-fix and eight candidate Direct/server tone/pulse cases
completed. Native paired comparison flags seven cases; one is solely a rounded
milliseconds boundary (exact TONEX p99 138 -> 170 frames), while six have other
buffering/quality differences. The new release path has no events in 14 of 16
candidate device-runs. Both pre-fix and candidate mode matrices have five failed
comparisons out of six; fresh Direct repeats also vary. Keep/revert is explicitly
the user's decision. No playback code or test gate was adjusted from these
hardware results; the tested candidate remains installed pending that decision.

## Direct/server ASIO equivalence

`asio-mode-equivalence.json` runs unproxied PCM16/44.1-kHz/64-frame/32-callback
Direct/server/server/Direct ordering for both tones and pulses, using TONEX as
joiner and Focusrite as creator, matching the recent manual Direct run. Set `JAM2_ASIO_DELIVERY_PROFILE` to that file
and `JAM2_RENDEZVOUS_TEST_SERVER` to the matching local Go server executable;
then run `jam2_asio_oct30_delivery_hardware` using the hardware CTest command above.
The local server is started and cleaned up by the native runner; no live-server
or network setting changes are involved. Both audio paths use the public
`release/jam2.exe network create/join` runtime and real ASIO devices.

Earlier experimental tone comparisons failed acceptance; the two interrupted
runs have explicit run-status.json files. Their pulse/stress acceptance was not reached.
The later fresh before/candidate matrices above completed all tones and pulses;
their completed status and separate mode/before-after comparator files are retained.
Mode comparisons retain the clean waveform/underrun/trim gates and check
absolute latency differences against one callback per direction, or two callbacks
for recorded round trips. Commands, server logs, CSV, recordings and comparisons
are retained. These are fresh CLI processes; GUI stop/rejoin lifecycle is separate.

## Recorded clean-audio acceptance

`asio-clean-audio-baseline.json` records four unimpaired localhost cases:
64/128-frame packets with a 440 Hz tone (60 seconds each) and one pulse per
second (40 seconds each). Set the hardware profile and run the named hardware
CTest as above. The real ASIO callback supplies the test signal and consumes
the received audio; per-peer `my-input.wav` and `their-input.wav` stems remain
alongside the CSV and recording sidecar. Writer errors or dropped recording
frames fail the case.

To compare a candidate, copy that profile into an artifact directory and add
`reference_baseline_root` pointing to the unchanged-product run's absolute
root. Without that reference, clean cases collect a baseline and do not prove
no regression. Device roles, format, rate, callback and signal must match. Comparison
files retain every failure. The gates allow at most one callback of median or
p99 buffered-latency variation per direction (0.726 ms at these settings), and
two callbacks of rendered pulse round-trip variation. They permit no increase
in steady underrun/trim counts or tone discontinuities; near-zero run length
has a one-sample measurement tolerance. Tone curvature (RMS second difference)
may increase by at most one PCM16 unit, catching short held/skipped samples
that do not cross the large-step threshold. Deterministic clean tests separately
require unchanged buffering rather than a hardware scheduling tolerance.

Steady counters and buffer distributions exclude the first 10 seconds and the
final two seconds; WAV analysis excludes the first three recorded seconds and
the final second. Tone frequency uses interpolated zero crossings. Opposite
pulse phase measurements cancel independent device clock origins to measure
rendered round-trip latency; driver output paths are reported separately.
The pulse metric ends at the software render callbacks; it excludes those
driver output paths. This is real callback audio, not an analog cable-loopback
measurement, and does not establish converter or acoustic latency.

For older retained recordings, the native runner supports
`--review-recorded <case-directory> <merged-case-configuration.json>` to write
`audio-review.json` with the current analysis. It preserves the original
summary and records any comparison failure. These unit-tested measurements
do not require another device run.

Stress profiles can set `maximum_mapping_error_ms` (for delayed beacons) and
`assert_quiet_after_ms` (no further underruns, trimming or recovery events
after an impairment ends). Optional `maximum_underrun_ms_per_minute` and
`maximum_trim_ms_per_minute` bound correction rates over the connected interval.
The acceptance matrix uses 5 ms/minute underrun and 10 ms/minute trim limits
for 4/8 ms batching; these reject the original implementation's roughly
57/598 ms/minute underrun/trim result at 8 ms. Severe holds may exhaust audio;
their gate requires quiet recovery rather than claiming impossible concealment.
The four-peer delayed-beacon simulation covers
one hour of drift, reordering, short bars and authority/epoch transitions.
Its original unfiltered control is a separate expected-failure CTest; a green
control test means the old behavior still violates the explicit error bound.
