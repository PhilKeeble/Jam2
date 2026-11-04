# CoreAudio polling default and local receive delays — 4 November 2026

CoreAudio now defaults to atomic capture-readiness publication with a **250 us
network-worker polling ceiling**. This applies through the shared runtime to
normal GUI and CLI sessions. Backend selection uses the actual opened audio
stream; Windows and synthetic audio retain native notifications. Explicit
`--capture-wake auto|on|poll|off` modes remain available for investigation.
The numeric polling range remains 50–1000 us. No protocol, drift-controller,
capture-pacing or recovery-buffer policy was changed.

The previous report describes the pre-promotion comparison:
[physical Wi-Fi investigation](PhysicalWifiCoreAudioInvestigation-2026-11-04.md).

## What the new timing establishes

The earlier `socket_residence_us` name was broader than literal queue residence:
it measured from the macOS kernel timestamp to an observation immediately
after `recvmsg`, including a possible pause around or after the read. The
new records separate:

1. Kernel timestamp → immediately before `recvmsg`.
2. Immediately before → immediately after that call.
3. Return → ancillary-data observation.
4. Observation → multiplex classification trace point.

All timestamps use the same Mach absolute-time domain. Existing delivery time
still controls buffering/recovery. Kernel timestamps are additional evidence,
not replacements for actual application delivery time.

In the first complete traced 250 us Focusrite run, **111 of 144 recorded steady
slow episodes** were dominated by the receive-call stage; 30 by the socket wait,
two by outgoing audio send and one by decode/mix. These are representative
episodes, not a distribution over every packet. The source records are in
[`events.csv`](../artifacts/receive-service-20261104/events.csv) and
[`events.json`](../artifacts/receive-service-20261104/events.json).

One packet was read **21 us after its kernel timestamp**, then the receive call
took **2746 us**. The largest kernel-to-observation event was **4108 us**:
**1292 us before the read + 2816 us inside it**, with zero whole microseconds
between return and the observation. Its dominant full receive span accumulated
**1528 us of thread CPU time in 2816 us wall time**. It was the first delivered
datagram after an empty read. Subsequent classification was approximately a
microsecond, rather than the cause of that multi-millisecond interval.

This locates an elapsed-time problem at the OS-call boundary. It does not
identify an exact kernel lock or prove that all elapsed time was active kernel
work. OS preemption during the call remains possible. The CPU values describe
the complete enclosing worker span; they are not invented per-packet CPU
measurements for partially overlapping spans.

The user-space stack sample also caught the runtime worker in `__recvmsg`.
It does not reveal the kernel's internal stack. Sampling interrupted the audio
thread: all six skipped cycles in that diagnostic run occurred around the
five-second sampling window. That run's overload/max-delay/CPU figures are
excluded from performance comparisons. A clean 100 us repeat was run.

## Kernel timestamp is not a socket-readiness guarantee

Apple's published XNU
[`udp_input` implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet/udp_usrreq.c)
collects ancillary timestamps before appending data to the receive buffer and
waking readers. Its
[`soreceive` implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/uipc_socket.c)
takes a socket lock before checking data/nonblocking conditions. These sources
explain why a timestamp does not guarantee that a subsequent read is immediately
available or that a nonblocking call has a bounded wall time. They are supporting
implementation evidence, not an identification of this Mac's exact kernel wait.

The diagnostic therefore records actual timed `EAGAIN`/`EWOULDBLOCK` attempts
and the number of delivered datagrams since the last such attempt. An empty
read beginning *after* a packet's kernel stamp would be evidence against
assuming that packet was already readable then. None of the first traced
250 us run's recorded steady episodes provided that evidence. Its absence
does not prove data was readable throughout the interval.

## Diagnostics and overhead

`--receive-timestamps on` enables the optional trace. The default remains off.
It records a bounded history of 512 worker spans and retains 32 slow events.
The trigger is kernel-to-runtime delay of at least 1000 us. Packets within a
5000 us episode are coalesced, retaining the worst observed delay under the
same event ID. Logs can contain updated/repeated IDs; analysis deduplicates
them by retaining the worst record. Overwritten history, coalesced packets,
invalid clock samples and event-publication time are exposed as counters.

Worker phases include advance, maintenance, capture/packing, audio send,
control send, socket wait/read, classification, decode/mix, receive completion,
runtime commands, telemetry and shutdown. Phase overlap and full-span thread
CPU time accompany each event. Incomplete history is visible as uncovered time.
The trace uses preallocated storage and performs no per-packet logging or
allocation. A small non-real-time mutex protects only the cold event copy;
no trace work occurs in the CoreAudio callback.

Artifacts and executable provenance are under
[`artifacts/receive-service-20261104`](../artifacts/receive-service-20261104).
Python supervises/analyzes the actual Release process; native CTests validate
the implementation. Source coverage and the full suite were not run.

## Validation

All **ten focused Release CTests passed** after the polling-default and trace
changes, including exactly-four-peer runtime modes and impaired ordering.
[`ctest-final.log`](../artifacts/receive-service-20261104/ctest-final.log)
contains that run. Deterministic native tests distinguish waiting before a
read, time within it, post-read pauses, busy CPU spans, incomplete history,
episode coalescing/worst-event replacement and bounded event storage. Real
loopback probes verify timestamp ordering, timed empty reads and attribution
of an injected local service delay. Existing unrelated core-input assertions
were not changed.

## Physical comparison and decisions

All runs below used the provided live Windows jam, real CoreAudio at
44.1 kHz / 32-frame callbacks, and 64-frame PCM16 packets over the Mac's Wi-Fi.
The Windows build stayed unchanged. Builds, CTests and profiling were kept
outside the clean measurement runs. The separately marked sampled run is
excluded from this table.

| Device / setting | Stream, s | HAL overloads | Callback max, us | Mean capture dispatch, us | Kernel-to-observation max, us | Representative receive-call max, us |
|---|---:|---:|---:|---:|---:|---:|
| Focusrite, automatic default, trace off | 60 | 0 | 16 | 198.5 | Not measured | Not measured |
| Focusrite, default 250 us, trace on | 90 | 0 | 13 | 170.6 | 4108 | 2855 |
| Focusrite, 100 us, clean repeat | 60 | 0 | 14 | 91.5 | 3178 | 2716 |
| Aggregate, automatic 250 us default | 60 | 0 | 13 | 198.9 | 2891 | 2882 |
| Focusrite, per-call `MSG_DONTWAIT` trial | 90 | 0 | 25 | 194.8 | 2882 | 2645 |
| Focusrite, 250 us, priority request disabled | 90 | 0 | 24 | 169.7 | 2838 | 2863 |

The table's receive-call maxima come from retained representative slow events,
not an independently sampled maximum over every syscall. Kernel-to-observation
maxima use all measured native audio packets. Natural network conditions vary
between runs; a smaller isolated maximum is not evidence that a candidate
eliminates the underlying stall.

**Kept:** automatic CoreAudio polling, the 250 us default and optional receive
diagnostics. Real default runs required no `--capture-wake` override. The
Windows/synthetic automatic policy still resolves to native notifications.
The shorter 100 us poll reduced measured capture dispatch by approximately
79 us in the clean comparison, while millisecond receive stalls remained.
It remains an explicit numeric tuning option rather than a new default.

**Per-call nonblocking trial reverted:** Apple's published `soreceive` uses
`SBLOCKWAIT(flags)`, whose selection depends on `MSG_DONTWAIT`. This justified
testing the flag independently of descriptor `O_NONBLOCK`. It did not eliminate
the receive-call stalls or produce late `EAGAIN` evidence, so it is not retained
as a demonstrated latency fix. Six focused native tests passed for the trial,
and six passed again after restoration. The restored public executable is
bit-for-bit identical to the main ten-CTest build.

**Priority default retained:** `--os-priority off` changed the measured network
thread class from `user-interactive` to `default`. Receive-call stalls still
reached 2863 us. This run does not demonstrate a reason to alter the existing
scheduling policy.

Across all clean runs, the app's HAL overload count exactly matched zero
per-PID skipped cycles in the OS logs. The sampled diagnostic run had six
skipped cycles at 12:30:02.454–12:30:03.908 UTC, coinciding with the stack
sampler; it also had a 17.603 ms receive observation. Those figures are kept
in the raw artifacts with `profiled: true`, not presented as clean polling
results. The clean 100 us repeat had zero overloads.

The trace increased observed process CPU in the 250 us diagnostic run from
about 10.8% to 12.5% of one core compared with the untraced default check.
These short live runs are not a controlled CPU benchmark. Timing remains
opt-in and no tracing clock/syscall work runs in the real-time callback.

## Remaining boundary

The common multi-millisecond component is inside the timed OS receive call,
with additional socket-wait delay in some events. Packet classification and
decode/mix were not the dominant stage in most recorded episodes. The largest
examples were the first datagram after an empty read; 62 of the first run's
144 steady representative events were first datagrams. Later queued packets
can inherit the delay, so first-packet examples must not be generalized to
every episode.

Kernel socket/input work, locking and OS scheduling remain possible underlying
mechanisms. The current data does not identify the specific lock or establish
that one kernel mechanism explains every event. It does establish where most
elapsed time occurred. Moving parsing between application queues or making
capture polling faster does not directly address a receive call that itself
takes milliseconds. The cooperative receive-service guard also cannot
interrupt that call.

The retained default is ready for review at
`release/Jam2.app/Contents/MacOS/jam2`, SHA-256
`3bdbb836fe21c5235b29cedaed7165dc94ef72c4c7d78e2ff1aee565e0622d96`.
Only documentation/analysis and a timestamp-comment correction followed its
behavioral validation. All supervised clients exited normally.

[Case summary](../artifacts/receive-service-20261104/summary.csv) ·
[Detailed summary](../artifacts/receive-service-20261104/summary.json) ·
[Source/build provenance](../artifacts/receive-service-20261104/provenance.json)
