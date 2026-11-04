# macOS UDP paths and localhost buffering — 4 November 2026

Connected BSD is now the automatic macOS **Server-mode peer-socket** backend.
Direct mode keeps ordinary BSD because it shares its bootstrap socket; other
platforms keep ordinary BSD. `--udp-backend auto|bsd|connected|network` retains
explicit comparisons. Automatic selection is resolved by the multiplexer, so
GUI and CLI sessions use the same policy. Bootstrap discovery/authentication
still uses BSD before a proven peer socket is connected. Network.framework,
voice service class and the queued reader remain optional experiments.

The existing CoreAudio atomic readiness / 250 us polling default is retained.
No Wi-Fi, Bluetooth, AWDL or other radio settings were changed. Buffer growth,
the 30-second delivery-gap hold, drift correction and recovery policy were
not changed in this investigation.

## Remote measurements

These are actual joins to the supplied, still-running Windows jam over the
Mac's Wi-Fi, not localhost substitutes. Windows kept its existing build.
Primary cases used 44,100 Hz, 32-frame CoreAudio callbacks, 64-frame PCM16
packets and Fast. Both Scarlett 2i2 and Aggregate were exercised. Case JSON
files record commands, device UIDs, PIDs, times, executable hashes and CPU.

“Kernel-to-app” below is the optional kernel receive timestamp through the
post-read observation. The stamp can precede socket readiness; it is not proof
that the packet could have been read throughout that interval. It includes
time inside the receive call and possible descheduling, not just queueing.

| Focusrite remote case | Seconds | Mean / max kernel-to-app (us) | Audio packets >1 ms | CPU, % of one core |
|---|---:|---:|---:|---:|
| Ordinary BSD, matched A | 60 | 223 / 2949 | 3215 / 40655 (7.91%) | 12.06 |
| Connected BSD, matched B | 90 | 52 / 605 | 0 / 61473 | 9.73 |
| Ordinary BSD, matched A2 | 60 | 251 / 3185 | 3851 / 40997 (9.39%) | 12.36 |
| Network.framework, reverse | 90 | 37 / 942 | 0 / 61413 | 15.81 |
| Network.framework, confirmation | 90 | 39 / 708 | 0 / 61541 | 16.08 |

The A/B/A cases used the same public Release executable. This favors the
connected socket as a useful independent control: much of Network.framework's
improvement was also present with BSD `connect`, without its higher CPU cost.
However, an earlier ordinary-BSD reverse run also had no >1 ms observations
and a 689 us maximum. Ordinary BSD does not stall on every run; the matched
results are evidence of a context-dependent improvement, not a universal bound.

With optional receive timing disabled, measured child CPU was 11.05% for
ordinary BSD, 7.91% for connected BSD and 14.13% for Network.framework.
Those runs lasted 60, 90 and 180 seconds respectively. These are supervised
process-resource measurements, not an isolated CPU microbenchmark.

Voice service class did not remove the tails: repeated maxima were approximately
4.0–4.1 ms. A separate receive worker also retained approximately 3.1–3.6 ms
kernel-to-reader tails and introduced an additional measurable handoff. Moving
queued-reader sends onto the existing immutable active routes reduced mean
send service but did not eliminate the OS tails. Neither experiment is selected
by default.

All clean physical remote runs in this matrix had zero CoreAudio overloads and
zero capture-ring overflows. Incoming gaps before the local kernel timestamp
still reached approximately 115–145 ms across backends. These observations do
not isolate Windows sending, radio/AP delay and the lower Mac network stack.
Connected BSD does not remove that upstream component or the resulting underruns.

After promotion, another 60-second Focusrite join used **no backend override**.
The actual backend was connected BSD, with **534 us maximum kernel-to-app**,
**0 / 40,888 audio packets over 1 ms**, zero overloads, zero capture overflows,
13 us maximum callback work and 10.27% of one core measured child CPU. Its
public executable SHA-256 was
`30134397ca0ab70ebe4d2125d717136704ac5d64b4e128ebf552ff785bff0bdb`.

Machine-readable evidence: [remote summary](../artifacts/udp-paths-20261104/summary.csv),
[per-case detail](../artifacts/udp-paths-20261104/summary.json),
[slow packet events](../artifacts/udp-paths-20261104/events.csv).

## Kernel trace and stack evidence

The user ran the prepared administrator trace commands. The first trace recorded
15 seconds of BSD syscalls, scheduler switches and lock events for the Jam2 PID.
It contains 87,291 main-worker `recvmsg` calls, 30 longer than 1 ms, with a
2.199 ms maximum. That worst call spans 45 scheduler switches. The bootstrap
reader's maximum was 34.8 us over 1,442 calls.

Sixteen application slow events matched syscall entries to within 1 us after
clock alignment. For example, a 1.743 ms application read corresponded to a
1.742 ms syscall containing 38 scheduler switches and about 1.169 ms descheduled.
Its separate application thread-CPU measurement was 662 us. These are different
probe intervals; their durations should not be added as exact accounting.

A second, target-only kernel stack sample caught `__recvmsg` through Sandbox
`cred_check_socket`, `sb_evaluate_internal`, `eval`, `sock_gettype` and mutex /
turnstile machinery. The installed XNU source's `sock_gettype` takes the socket
lock. This identifies a real blocking path in a captured sample, not the owner
of the lock or proof that every slow receive has that cause. Private kernel
symbols and the other thread's identity remain unresolved. Live process checks
did not identify Jam2 as application-sandboxed.

Both connected and unconnected UDP still use socket locks; connecting does not
make them lock-free. Primary source:
[installed XNU kpi_socket.c](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.101.15/bsd/kern/kpi_socket.c#L1226),
[installed XNU UDP implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.101.15/bsd/netinet/udp_usrreq.c#L2577).

The syscall-traced audio run had one overload and is excluded from the clean
performance comparisons. Trace analysis:
[syscalls](../artifacts/udp-paths-20261104/kernel-trace-44132-analysis.json),
[application correlation](../artifacts/udp-paths-20261104/kernel-app-correlation.json),
[kernel stacks](../artifacts/udp-paths-20261104/kernel-stacks-44186.txt).

## Local audio measurements and the 5 ms ring

Native CTest runs exercised two simulated devices, Focusrite + Aggregate, and
two sessions on the same Focusrite. These used the actual public executable,
44,100 Hz / 32 callbacks / 64-frame PCM16, with recorded synthetic pulses and
actual callback playback. The first two matrices used ten 20-second cases with
the order reversed for the second pass. No network impairment was configured.

The pulse figure is a recorded callback round trip: opposite clock origins
cancel, but analog converter delays are excluded. Queue medians use the CSV
window after 10 seconds, excluding the last two seconds. Pulse analysis uses
the native helper's recording window after three seconds through one second
before recording end. Those windows differ and are not interchangeable.

| Path | Simulated pulse RTT, two passes (ms) | Focusrite + Aggregate pulse RTT, two passes (ms) |
|---|---:|---:|
| Server, ordinary BSD | 5.102 / 3.560 | 6.961 / 5.964 |
| Server, connected BSD | 3.016 / 3.322 | 5.964 / 12.426 |
| Server, Network.framework | 5.011 / 5.057 | 4.490 / 5.181 |
| Server, queued receive | 3.798 / 6.531 | 9.592 / 5.011 |

Direct simulated results were 5.249 / 4.819 ms. With both local sessions using
the same Focusrite, ordinary BSD produced 4.229 / 8.764 ms and connected BSD
10.499 / 5.998 ms. Variation is therefore not specific to combining Aggregate
and Focusrite. These short runs do not establish a repeatable transport-only
advantage in overall audio latency.

The 12.426 ms connected-BSD outlier retained median software queues of
5.102 / 5.079 ms per receiver. Its jitter and mixer queues were empty in the
steady snapshots: the audio was in the **playback ring**.

At about two seconds, after earlier startup shortages, the peers recorded
6.061 / 5.992 ms receive-loop gaps and 6.401 / 6.256 ms audio delivery gaps.
Callbacks stayed regular: maximum intervals 0.787 / 0.849 ms, with zero HAL
overloads. Capture-ready dispatch reached 5.459 / 5.323 ms. Measured receive
processing maxima were only 67 / 52 us for the complete runs. Thus the audio
threads continued while network-worker service was delayed; localhost removes
the physical network but does not provide a scheduling deadline for that worker.

The resulting targets were raised to 411 / 404 frames, with a 30-second gap
hold. Padding counters reached 533 / 464 frames. The ring first grew to about
600 frames, then smooth release reduced it to approximately 192–226 frames.
At 44.1 kHz this is 4.35–5.12 ms. In `PeerMixer::updateAdaptiveReleaseState`,
the raised-cushion minimum uses `max(2 * callbackFrames, min + batchTolerance)`.
Here the effective callback quantum is the 64-frame packet block, minimum is
64 and batch tolerance is 192, yielding 256 frames. The stop threshold subtracts
one block, leaving roughly 192 frames plus the measured return-ramp margin.
The raised target also keeps the separate minimum-target recovery-credit
release path inactive. This explains the observed level during the hold.

The 20-second run ended with approximately 12 seconds of that hold remaining.
It could not establish whether latency returned afterward. The user accepted
waiting to confirm clean conditions, so this behavior has been retained.

The new native `jam2_core_short_startup_gap_units` runs the production packet
receiver, mixer, ring and callback resampler for 60 simulated seconds, with
both one and three remote peers. It compares clean delivery, controlled local
worker pauses of 4 / 4 / 6 ms at 0.6 / 1.2 / 2 seconds, and an explicit 64-frame
target cap. This is an analogue of aggregate observations, not a packet replay.
The pauses allow normal short-gap qualification; one isolated short gap need
not select a held target.

The default impaired case retained a mean **5.143 ms in the playback ring**
over seconds 10–20 for both peer counts. The hold then expired and ring depth
returned to the smaller range (two-peer total software path mean **2.422 ms**
over seconds 50–60). No new underruns, padding or sample cuts occurred after
four seconds. The four-participant total includes about another packet in
source alignment; it must not be confused with the ring alone.

A hard 64-frame target cap was not a general cure: that control retained about
3.7–3.8 ms in the ring despite a minimum target, due to real catch-up audio and
the minimum-target release guards. It also preserved qualified gap evidence.
Initial diagnostic assertions predicting that the cap would eliminate the
retained depth were rejected by measurements; the initial log is preserved.
No product behavior or existing test threshold was changed to hide that result.

The GUI estimate is a different measurement: RTT/2 + actual jitter, mixer and
playback-ring depth + reported driver output latency (callback fallback if
unreported). Aggregate reported about 14.8 ms output latency versus Focusrite's
1.34 ms. That device contribution is not extra network buffering. Reconstructed
GUI estimates, pulse results and queue measurements are separate columns in
[the local summary](../artifacts/udp-paths-20261104/local-summary.csv).

## Locating the localhost pause

The first longer timestamp-enabled matrix used three 60-second physical local
runs. Connected BSD had steady median software queues 1.474 / 1.791 ms and
4.376 ms pulse RTT. Ordinary BSD reproduced **10.887 / 11.024 ms audio gaps**
at about two seconds; receive loops paused 10.058 / 9.921 ms and capture-ready
dispatch reached 9.767 / 9.529 ms. Measured send work was at most 143 / 218 us,
while receive processing was at most 50 / 67 us. Network.framework's pulse RTT
was 2.880 ms, with 0.726 ms median software queues in both receivers. These
are individual runs, not a randomized proof that either transport prevents
local pauses. Small steady callback-ring underruns still occurred in some runs.

The ordinary-BSD gap was also present between kernel receive timestamps,
while kernel-to-app residence stayed below a millisecond. Packets were not
sitting in the receiving socket for the whole 11 ms. On localhost this is
consistent with delayed sending/servicing; it does not identify the cause of
the worker pause. In particular, existing packet-residence events can miss an
empty `recvmsg` that stalls before a packet exists.

Optional receive timing now also exports the worst completed >=1 ms span for
each worker phase, including empty reads and waits, with wall time, separate
thread CPU and common-clock endpoints. Storage is fixed-size and retains an
early worst span through the end of the run. No tracing or new work is added
to the CoreAudio callback. Focused native tests cover empty-read/wait timing
without any received packet, shorter later spans, missing CPU evidence, and
the production JSON boundary.

The follow-up used that same promoted Release executable for three further
60-second Focusrite + Aggregate localhost cases:

| Backend | Recorded pulse RTT (ms) | Median software queues (ms) | Steady underrun frames | HAL overloads |
|---|---:|---:|---:|---:|
| Explicit ordinary BSD | 3.673 | 1.429 / 1.497 | 2 / 0 | 0 / 0 |
| Automatic connected BSD | 3.583 | 1.429 / 1.429 | 65 / 66 | 0 / 0 |
| Network.framework | 5.986 | 2.222 / 2.222 | 221 / 222 | 0 / 0 |

Ordinary BSD was clean in this repetition. At about 43.9 seconds after process
start, both connected-BSD workers recorded matching **2299 / 2303 us socket-wait
spans**, with only **7 / 8 us thread CPU**. Network.framework then recorded
matching **7934 / 7968 us socket-wait spans**, beginning only 57 us apart, at
about 14.4 seconds after process start. Those spans used **17 / 11 us CPU**.
The runtime wait budget at this callsite is at most 1000 us; the 250 us capture
poll setting applies while waiting for more captured audio. Elapsed wait phases
can exceed both requested budgets.

In the Network.framework event, receive-loop maxima reached 8.014 / 8.019 ms,
delivery gaps 8.425 / 8.319 ms, and capture-ready dispatch 6.565 / 6.721 ms.
Callbacks continued with maximum intervals 0.762 / 0.854 ms and work 16 / 12 us.
The corresponding CSV observations show **221 / 222 newly missing playback
frames**, 64 added padding frames per peer and targets moving from 64 to 128.
There were zero HAL overloads. Thus a shared local worker wait delay really
can cause starvation and recovery padding, independently of a physical network.

This places the large pause inside the wait phase, not active packet-processing
CPU. That phase surrounds the select-based wait. It does not distinguish a
delayed kernel wait from descheduling around the call; another scheduler trace
would be needed to identify that cause. It also demonstrates that connecting
BSD or using Network.framework does not remove every local scheduling pause.
Raw results: [phase comparisons](../artifacts/udp-paths-20261104/local-gap-summary.json),
[comparison CSV](../artifacts/udp-paths-20261104/local-gap-summary.csv),
[original low/high queue timelines](../artifacts/udp-paths-20261104/queue-outlier-timeline.csv).

## Are post-arrival delays present remotely too?

Yes. For example, an unprofiled ordinary-BSD remote run recorded **2804 us from
kernel timestamp to observation**, including **2793 us before the read** and
only **11 us in the read**. A socket-wait span overlapped 2792 us of that delay.
Other remote cases also contain receive-call stalls and pre-read wait delays.
The timestamp is evidence of OS receipt, not proof of continuous readability.

The latest connected-BSD remote run had a 1275 us worst wait phase but no audio
packet with >1 ms kernel-to-app delay. It therefore does not reproduce the
8 ms localhost event with already-timestamped incoming audio. Nor have the
existing remote captures established a one-to-one causal match between a
particular wait pause and a particular recovery. The local event and native
injection demonstrate the mechanism; the exact remote recovery contribution
still requires correlation, distinct from the much longer pre-timestamp gaps.

Connected BSD cannot simply be applied to the existing Direct creator socket:
that socket is shared by multiple peer links and pending joins. UDP `connect`
filters reception to one endpoint, so doing it globally would break other
participants and admission. Extending the default to Direct requires a design
that preserves shared bootstrap while moving each media link to its own proven
socket. That refactor is not part of this measured default promotion.

## Validation and remaining limits

All builds are optimized universal macOS Release builds using Apple tooling.
Only the public `release/Jam2.app/Contents/MacOS/jam2` was used as the product.
The full CTest suite was not run. Earlier focused checks passed for backend
socket exchange/error paths, CLI controls, receive clocks and traces,
exactly-four-peer runtime paths, and direct ordering/impairment. The local
simulated, four-peer delivery and explicitly profiled CoreAudio CTests passed.

Final validation passed all six focused native CTests: CLI boundaries, backend
socket exchange, receive diagnostics, short startup gaps, exactly-four-peer
runtime backend paths (including automatic Direct and Server choices), and
exactly-four-peer lifecycle. The expanded runtime test uses ten conditions.
Three additional focused CTests passed for Direct impairment, Server key
rotation/ordering and Server receive pressure. The profiled physical CTest
passed its three final phase-traced cases. The full suite was not run.

macOS log correlation found zero overload entries for all 40 physical local
processes and all clean remote cases in this study. The sole matching remote
entry was the one overload in the kernel-traced run, already excluded above:
[OS log validation](../artifacts/udp-paths-20261104/coreaudio-os-validation.json).
Raw artifacts are under
[`artifacts/udp-paths-20261104`](../artifacts/udp-paths-20261104/).

The kernel lock owner, an exact trace of the original local 6 ms episode, and
a controlled same-hardware Windows/macOS scheduling comparison remain unknown.
Neither clean localhost tests nor connected BSD's remote improvement establish
that all real-jam underruns are solely caused by Wi-Fi.
