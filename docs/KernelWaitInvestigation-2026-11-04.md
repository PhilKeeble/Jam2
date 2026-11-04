# macOS kernel waits — 4 November 2026

This records the baseline investigation before the application wait change.
The subsequent [critical-wait experiment](CriticalWaitExperiment-2026-11-04.md)
implements the candidate by default, retains legacy comparison, and reports
native, physical localhost and remote measurements. Rare long waits survive.

There is a repeatable OS-wait optimization worth testing in Jam2. Normal
`select()` timeouts overshot the requested interval considerably more than
absolute critical `kqueue` timers in the standalone measurement below. This
does not establish the cause of the rare 4–8 ms local pauses. No Jam2 runtime
code, defaults, recovery policy, radio settings or audio callback behavior
changed during this follow-up.

## What the new kernel capture established

The unchanged public Release executable ran two localhost clients with
Focusrite + Aggregate, 44.1 kHz, 32-frame callbacks, 64-frame PCM16 and automatic
connected BSD. The native profiled hardware CTest passed its 180-second case.
Executable SHA-256 remained
`30134397ca0ab70ebe4d2125d717136704ac5d64b4e128ebf552ff785bff0bdb`.

The user ran a 30-second administrator binary capture. It recorded syscall,
scheduler and lock events across processes. Binary capture avoided formatting
JSON during audio. The largest application wait spans, **3664 / 4769 us** with
only **10 / 15 us CPU**, occurred about **3.13 seconds after launch**, before
the kernel capture began. They remain unclassified at the kernel level.

The capture contains the following runtime socket calls:

| Receiver | `select` calls | `select` max (us) | Reads | Read max (us) | Send max (us) |
|---|---:|---:|---:|---:|---:|
| Focusrite | 114184 | 1559.375 | 251348 | 51.292 | 97.500 |
| Aggregate | 113781 | 1567.750 | 250537 | 52.291 | 97.667 |

The ~1.56 ms select outliers occurred immediately at trace startup. Both HAL
overload entries also occurred at capture startup, at
15:34:35.333931 / 15:34:35.333961 UTC. This profiled run is excluded from clean
overload and latency comparisons. Neither receiver had a >1 ms `recvmsg`.

Other captured waits distinguish sleep from time waiting for a CPU. One
Aggregate `select` took **1262.625 us**: its matched off-CPU cycle contained
**1248.375 us blocked before wakeup**, then **12.625 us runnable before
dispatch**. Another took 1258.291 us, with **1252.041 us blocked / 3.667 us
runnable**. These wakes had `THREAD_TIMED_OUT` result 1. Thus those particular
tails primarily involved the timer wait, rather than a millisecond-scale
queue of runnable work. The requested userspace timeout was not recorded, so
these durations alone do not prove a missed 250 us deadline: Jam2's callsite
can also request waits up to 1000 us.

Ordinary Focusrite waits in the selected tail included a **385.584 us** call
with **309.250 us blocked / 74.209 us runnable**. The remaining time was syscall
and context-switch work. The waker PID identifies the thread/interrupt context
that emitted a wake; it does not establish that process as the cause of delay.

The older ordinary-BSD trace was also reanalysed: 38,104 select calls, with a
2.596 ms maximum containing about 35 blocking/switch cycles. This is different
from the almost CPU-free long local wait. There may be multiple mechanisms.

The local ktrace manual says scheduler events are preserved despite capture
PID filters. The old PID-filtered JSON nevertheless contained only Jam2-tagged
records, so its visibility into external wakers is limited. The new binary
capture retained system-wide context. We should not infer a missing wake event
or lock owner solely from the older filtered output.

Thread priority inspection was also checked against a control. BSD priority
fields remained 31 despite successful QoS requests in the control; those fields
alone do not prove that Jam2's user-interactive QoS was lost. Stored thread
handles are userspace pthread handles, not kernel trace thread IDs.

## Why a different timer is a concrete candidate

The installed XNU version's
[`select` implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.101.15/bsd/kern/sys_generic.c#L1441)
uses a normal-urgency timeout. Its
[timer code](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.101.15/osfmk/kern/timer_call.c#L1920)
computes timer slack and adds it to deadlines; zero caller-supplied leeway does
not disable all system coalescing. The
[`kqueue` timer implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.101.15/bsd/kern/kern_event.c#L1639)
maps `NOTE_CRITICAL` to user-critical timer urgency. Apple demonstrates an
absolute Mach-time timer with `NOTE_CRITICAL | NOTE_LEEWAY | NOTE_MACHTIME |
NOTE_ABSOLUTE` in its
[deadline-timer example](https://developer.apple.com/videos/play/wwdc2025/211/).
This is a supported API pattern, not evidence that it guarantees network/audio
deadlines or eliminates every source of descheduling.

## Standalone OS experiment

An Apple Clang `-O3 -DNDEBUG` native-arm64 helper compared three OS waits with
two concurrent workers. Each worker requested user-interactive QoS and waited
on idle loopback UDP sockets. Interest registration happened outside timing;
each method received a 200-iteration warmup. Methods ran in forward then
reverse order. There were 20,000 measured waits per worker per repetition at
100 / 250 us, and 5,000 at 1000 us: **540,000 measured waits** in total.

This is an OS characterization probe. It does not exercise Jam2 audio,
recovery, live packet readiness, disconnect/reconnect or route changes, and
is not a substitute for native product CTests or physical A/B runs.

The values below are medians of the four worker/repetition medians:

| Requested (us) | `select` (us) | `kqueue` timeout (us) | Critical absolute timer (us) |
|---|---:|---:|---:|
| 100 | 128.19 | 115.83 | 104.06 |
| 250 | 319.17 | 286.96 | 257.12 |
| 1000 | 1260.23 | 1135.98 | 1020.62 |

At 250 us, the critical timer reduced median overshoot from about **69 to 7 us**.
The largest per-batch p99.9 was **362 us** for select versus **295 us** critical.
At 1000 us, median overshoot fell from about **260 to 21 us**. No rare 4–8 ms
pause occurred in the probe. CPU cost was not uniformly lower: critical timers
used less CPU at 250 us, but about 1.0% of a core per worker versus select's
0.7% at 1000 us. Actual Jam2 CPU cost needs separate measurement.

Raw data and reproducibility:
[probe source](../artifacts/kernel-waits-20261104/wait_probe.cpp),
[250 us results](../artifacts/kernel-waits-20261104/wait-probe-250.csv),
[all-deadline summary](../artifacts/kernel-waits-20261104/wait-probe-summary.json),
[kernel call and wake analysis](../artifacts/kernel-waits-20261104/capture-analysis.json),
[analysis source](../artifacts/kernel-waits-20261104/analyze_capture.py).

## Application-level tests proposed at this stage

The initial proposal was to test an opt-in `kqueue` receive wait with an absolute critical timer, retaining
the existing atomic CoreAudio capture publication and numeric polling interval.
The user subsequently requested making it the default with the previous path
retained alongside for comparison. Both Direct and Server can use it: it does
not pin a UDP socket to one peer. The linked follow-up reports its effect on
packet residence, capture dispatch, callback overloads, queue depth and recovery.
Required native coverage includes readiness,
timeout, wake/cancellation, route/socket replacement, cleanup and four-peer
ordering/impairment, not just this idle-wait probe.

For the rare large pause, begin the next kernel capture **before launching the
clients**. The prepared script now has `--before-launch` mode and emits a
recording-ready file. It also adds timer-interrupt and turnstile event classes.
Those changes have shell syntax validation but have not had a second privileged
capture yet. Capture startup itself should finish before CoreAudio begins.
The next trace should match a large wait to timer expiry, the runnable
transition, dispatch and any lock owner. Avoid changing timer behavior, QoS and
socket backend simultaneously, which would obscure attribution.
