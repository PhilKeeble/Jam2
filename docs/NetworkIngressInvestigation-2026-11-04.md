# Network ingress investigation — 4 November 2026

The receive path has reproducible bookkeeping and scheduling problems. These
tests do **not** establish that multiplexing caused the reported historical
regression, the Mac/Windows latency difference, or the CoreAudio overloads.
WAV rate limiting and asset scheduling were left unchanged.

All validation uses Apple Clang, optimized Release, and native CTests. Baseline
source is `774960642e120c0a10143aa64922fd12e4b87057` with A2/B/C already applied.
The candidates remain uncommitted for review. The full suite was not run.

## Candidates

| Candidate | Implementation | Evidence / limits |
|---|---|---|
| D1: shared receive budget | Count every socket datagram across a runtime turn; preserve exhaustion through recovery completion and idle-telemetry decisions | Deterministic reproduction and four-peer socket tests. Correctness improvement; no general transport-latency improvement established. |
| D2: return after draining control-only work | Return to audio sending/maintenance once the available classified work is drained; retain event-based waits for initially idle sockets | Separate old/new caller probe measures the otherwise unnecessary timeout. |
| D3: cheaper periodic CSV | Reuse field/row storage, use numeric `to_chars` with the existing six-digit general precision, and write the assembled row | Fixed-snapshot native benchmark and CSV schema/precision tests. No new thread or queue. |
| D4: elapsed receive-service guard | Optional cooperative limit after the first dequeue, default 1000 us | Packet processing can yield between datagrams; this cannot bound OS preemption or interrupt a packet handler. Not established as a steady-state latency improvement. |

Numeric CLI controls are `--receive-work-packets 1..4096` (default 64) and
`--receive-service-us 0..10000` (default 1000; zero disables the time limit).
The typed runtime options expose the same controls. CSVs append socket-packet
counts/maxima, packet/time yields, configured limits, and telemetry service
counts/sum/max. Existing CSV columns retain their positions.

## Receive-budget reproduction

`jam2_udp_four_ingress_boundary` and `jam2_udp_four_ingress_mixed_budget` create
a real authenticated four-peer mesh, pass real PCM through `NetworkSession`,
and put authenticated duplicate control ACKs ahead of audio. They check that
later PCM remains ordered and deliverable. This is deliberate burst pressure,
not a measurement of typical two-player control traffic.

Before D1, 64 control ACKs produced no native packet and were reported as a
drained runtime batch. An interleaved 64-ACK/eight-PCM sequence consumed **72
socket packets in one turn**, despite the nominal 64-native-packet budget.
Both new acceptance CTests failed on that baseline. With D1, the mixed first
turn consumes **64 socket packets/seven PCM packets**, reports exhaustion, and
retains the eighth PCM packet for the next turn. Control-only work cannot
enable idle telemetry.

The lower-level four-participant mixer test measures the consequence of the
wrong exhaustion bit without depending on OS timing:

| Observed shortage, frames | Extra silence with original drained flag | Extra silence with exhaustion preserved |
|---:|---:|---:|
| 1 | 1 | 0 |
| 16 | 64 | 0 |
| 32 | 64 | 0 |
| 64 | 64 | 0 |

Each case supplies the same subsequent 64 real frames from all three remote
contributors. A wrongly retained packet of silence is **1.333 ms at 48 kHz**.
This is a conditional mechanism, not a measured reduction in every real jam.
It extends B's same-batch recovery protection across classification yields.

The D2 control-only probe also distinguishes asynchronous arrival from a
packet already waiting in the socket. Asynchronous calls measured **16 us
median with either caller**. For a prequeued packet, the original caller took
**1264 us median / 1305 us sampled tail**, versus **17 / 23 us** after D2. The
probe waits before the measured call to establish the prequeued condition.
Thus D2 removes a conditional timeout; it is not a universal 1 ms audio gain.

## Transport timing

`jam2_udp_four_ingress_performance` compares old and shared-budget callers in
ABBA order, changes the creator, and uses regular/8 ms batched PCM sends, with
and without four duplicate control ACKs per packet on the authenticated
control edges. It uses actual UDP sockets and authentication; there is no
device or mixer. All conditions require complete, uncorrupted delivery.

The final comparison produced these medians of per-peer timing summaries:

| Delivery / control prefix | Old p50 / p99, us | Shared-budget p50 / p99, us |
|---|---:|---:|
| Regular / none | 47 / 99.5 | 48.5 / 104.5 |
| Regular / four | 47.5 / 113.5 | 51 / 119 |
| 8 ms batches / none | 142.5 / 306 | 150 / 352.5 |
| 8 ms batches / four | 214 / 562 | 208.5 / 554 |

There were no audio losses, corruption, duplicates, or multiplex queue drops.
These results support D1's bounded-work semantics, not a general speedup.
They do not explain a persistent extra 2–3 ms by classification CPU cost.
Burst timing and sustained control pressure remain distinct from a WAN or
Wi-Fi capture. The original comparison harness also had an extra receive at
the end of a saturated 64-packet turn; the investigation avoids consuming an
unprocessed 65th packet in that **test harness**.

## Local stalls versus network timing

`jam2_audio_flow_ingress-timing` injects known 0/3/6 ms local worker pauses every
70 ms, separately from a deterministic 0/3.6 ms network-jitter parameter. Each
case simulates 180 seconds with exactly four participants, real packet
authentication/stream/mixer code, and the production callback ring/resampler.
The reported late window is the last 60 seconds.

With no injected network jitter, average buffered path grows from **50.56
frames (1.053 ms)** without pauses to **174.904 (3.644 ms)** with 3 ms pauses,
and **349.193 (7.275 ms)** with 6 ms pauses. Drift remains zero. Thus local
service pauses alone can cause a larger buffering requirement on an unchanged
network. These are sensitivity experiments, not measured costs of the current
multiplexer.

A test-only ablation supplies the known arrival timestamp instead of delivery
time. With 3 ms local pauses and no network jitter, it changes the final target
from 268 to 64 frames but leaves average actual buffering **identical**.
With 6 ms pauses it creates **3781 late underrun frames and 10587 late padding
frames**, versus zero in the real-delivery-time case. With the network-jitter
parameter enabled, the same ablation creates **5208 underrun frames and 5789
padding frames**. Cleaner timestamp statistics do not imply better playback.

Arrival timing is useful additional diagnostic evidence. Replacing delivery
time throughout the existing buffer controller is a dead end in these tests.
No production timestamp or drift-controller change was made.

## Diagnostics and CSV

`jam2_four_ingress_runtime_investigation` runs the real controller, application
runtime, multiplex transport, engine, and full packet loop in Direct and Jam2
Server modes. There are exactly four participants and synthetic **32-frame
callbacks / 64-frame PCM24 packets at 48 kHz**. A real local Go signaller is
used. Each matrix has 24 conditions: both modes, service limits disabled/1000
us, diagnostics disabled/normal 2 s CSV/stress 10 ms CSV, and forward/reverse
condition ordering. Each condition streams for six seconds after convergence.

Before D3, normal telemetry averaged approximately **9–15 us** per service,
with observed per-run maxima below **235 us**. Stress telemetry averaged
approximately **73 us** overall and occasionally reached **2–3.6 ms**.
Wall-time counters include OS descheduling; they are not exclusive CPU time.

The fixed-snapshot four-writer CTest measured D3 independently: median per-row
time **33–35 us → 6–7 us**, p99 **41–44 us → 10–15 us**, across three runs each.
The later benchmark additionally checks zero, negative zero, rounded/scientific
values, infinity, NaN, and changing rows against established formatting.
That expanded final validation measured 11 us median and 21 us p99.

One intermediate full runtime matrix with D3 averaged approximately **40 us**
for stress telemetry, but still recorded a **3.2 ms** maximum. Neither that
sequential comparison nor the benchmark establishes removal of scheduling
spikes. In the final combined D2/D3 matrix, normal telemetry averaged **9.8 us**
and stress telemetry **23.2 us**, but a **2.83 ms** stress maximum remained.
There is no demonstrated need for an asynchronous telemetry worker
from the normal-rate results. Raw CSVs retain startup and shutdown; aggregate
underrun totals must not be presented as steady-state regression counts.

The elapsed service guard triggered **zero times across all 96 final peer-case
observations**. Its deadline unit tests pass, but its benefit in a normal jam
remains unproven. `--receive-service-us 0` isolates packet-budget-only behavior.

## Existing failure audit

The core-input aggregate CTest has **55 failing assertions** in seven groups:
five lean two-peer latency-budget checks; 21 short-batch/no-gap-learning checks;
21 four-peer retained-cushion checks; one Fast stress quality check; three
hold-expiry checks; three compound latency-return checks; and one sub-50 ms
batch-learning check.

The controlled baseline audit restores HEAD's original `network_session.cpp`
object and its declarations in an arm64 Release copy of the core archive,
using the unchanged core-input test object and other unchanged core objects.
Its native CTest produces **the identical 55 failures with identical messages
and counts**. No second public Jam2 executable is created. None of those
assertions or controller behaviors was changed to pass this investigation.

## Validation outcome

All **seven newly registered CTests pass**. The final focused run is **14/15
passing**, with only the unchanged core-input aggregate failing as audited
above. A final asynchronous/prequeued control probe also passes. Earlier
extended checks additionally passed native multiplex conformance, four-peer
impaired delivery, the four-peer pressure/conformance workflow, mode latency,
and receive-model comparisons. The mode comparison reported Direct/server
median totals **6.6/6.4 ms**, p95 **8.4/8.0 ms**; it uses synthetic 128-frame
callbacks and is separate from the new 32-frame runtime matrix.

The tested public universal Release binary is
`release/Jam2.app/Contents/MacOS/jam2`, SHA-256
`11faac78350e8770c9bbb6854bad70922374bf506f85b9298d56c8bbe8fe7884`.
No Windows build, physical-device retest, source coverage, or full suite was run.

## Evidence

Detailed JSON summaries, provenance, baseline/candidate CTest logs, compiler
logs, and the baseline-audit script are under
[`artifacts/network-ingress-20261104`](../artifacts/network-ingress-20261104/).
The full runtime CSV matrices are under
[`artifacts/network-ingress-native`](../artifacts/network-ingress-native/).
These artifacts are local and ignored by Git; this report and native tests
remain reviewable in the repository. Windows and real-device/CoreAudio/WAN
validation were not performed in this investigation.
