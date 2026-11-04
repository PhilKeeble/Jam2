# macOS critical UDP wait experiment — 4 November 2026

The public Release build now uses a critical absolute `kqueue` timer for macOS
UDP waits. The previous select/poll path remains available as
`--udp-wait legacy`; `--udp-wait kqueue` selects the default explicitly. Both
Direct and Server use the new waiter. This is a measurable improvement to
ordinary deadline accuracy, **not a confirmed fix for the rare multi-millisecond
worker pauses**.

The same executable was used for every comparison below, SHA-256
`d2bb6dc91d8f4f753d6a4f49ff617dc2964d6aae13fda213e6602c8b8b4d14aa`.
It is the normal universal arm64/x86_64 Release app at
`release/Jam2.app/Contents/MacOS/jam2`. Windows retained its existing build.

## Implementation and measurements

The network thread owns a reusable RAII kqueue. It waits for UDP, stop/command
and applicable capture descriptors alongside a one-shot absolute Mach deadline,
using `NOTE_CRITICAL | NOTE_LEEWAY | NOTE_MACHTIME | NOTE_ABSOLUTE` and zero
explicit leeway. Early readiness cancels the timer. Descriptor interests are
refreshed in the wait syscall so closing and reusing a numeric descriptor cannot
leave the replacement unwatched. Storage grows with the peer set and imposes
no additional mesh cap. No spin loop is used.

CoreAudio still publishes capture readiness atomically, with the existing
250 us polling interval. The audio callback performs none of the new wait work.
Socket backend selection, QoS requests, playback recovery, drift correction,
radio settings and Windows behavior are unchanged by this experiment.

`UDP multiplex stats` now reports `udp_wait`, `udp_wait_runtime` and
`udp_wait_worker`. Each counter set includes requested/elapsed totals, maximum
elapsed time and overshoot, calls, readiness/deadline wakes, interruptions and
errors. `late_over_1ms` means elapsed time exceeded that call's requested budget
by **at least 1000 us**. It does not count every intentional wait above 1 ms.
In particular the background worker's normal 10 ms wait is separate from the
audio/runtime wait. Readiness can return early, so subtracting aggregate
requested time from aggregate elapsed time is not an overshoot distribution.

The [earlier kernel-wait study](KernelWaitInvestigation-2026-11-04.md) explains
the OS API rationale and its limits. This experiment has no new privileged
kernel capture of a long kqueue pause: low thread CPU identifies off-CPU time,
but does not distinguish late timer delivery, delayed dispatch or a kernel
wait/lock within the syscall.

## Production waiter timing

A native CTest exercised the actual production waiter, 1000 measured waits per
mode/interval/repetition after 100 warmups, with reversed order on repetition 2.
These are idle UDP socket waits, not end-to-end audio latency.

| Requested | Legacy median, repetitions (us) | Critical median, repetitions (us) |
|---|---:|---:|
| 100 us | 129.33 / 128.42 | 105.25 / 103.17 |
| 250 us | 317.33 / 319.83 | 257.42 / 259.33 |
| 1000 us | 1261.33 / 1261.12 | 1019.21 / 1019.96 |

The ordinary 250 us deadline overshoot fell from about 68 us to 8 us. This saves
tens of microseconds per normal timeout; it cannot directly explain several
milliseconds of audio queueing. Critical timers can also wake more often than
coalesced legacy waits; battery/power effects were not measured.

## Localhost audio comparisons

The native two-peer harness exercised both wait modes in Direct and Server,
then reversed the case order. Physical cases ran 60 seconds each with the
Focusrite Scarlett 2i2 and Aggregate Device, at 44.1 kHz, 32-frame callbacks and
64-frame PCM16 packets, Fast profile. Simulated cases ran 20 seconds each with
the same rate and packet/callback sizes. Simulated audio retains its normal
capture notifications; physical CoreAudio uses its normal atomic polling.
Receive timing instrumentation was enabled for both paths in these comparisons.

Pulse RTT is measured from recorded callback audio at both peers; opposite
clock origins cancel. It excludes analog converter latency. It is not a
physical analog round-trip measurement or a direct comparison to a particular
Windows hardware test.

| Physical pair | Legacy overall pulse RTT (ms) | Critical overall pulse RTT (ms) | Legacy late-window RTT (ms) | Critical late-window RTT (ms) |
|---|---:|---:|---:|---:|
| Direct A | 6.349 | 5.057 | 6.644 | 4.943 |
| Direct B | 8.844 | 4.399 | 5.986 | 8.073 |
| Server A | 6.349 | 6.417 | 6.417 | 6.984 |
| Server B | 10.952 | 2.925 | 7.823 | 2.948 |

The overall values are the native harness's phase medians, using pulses after
3 seconds and before the final recorded second. The additional late-window
values use the same nearest-pulse phase method on pulses at seconds 40–55.
These windows intentionally expose time variation; do not interpret the best
overall run as a sustained bound.

In Direct B critical, the pulse phase at both receivers stepped around
44 seconds. The worst socket waits were 3.047 / 2.624 ms, with only 7 / 18 us
thread CPU. Steady playback starvation was 93 / 66 frames, and late pulse RTT
rose to 8.073 ms even though the per-peer queue medians over the broader steady
window were just 0.771 / 0.726 ms. The measured small median did not describe
the post-pause tail. No formal receive-recovery event was triggered in that
case; playback padding/queue growth and thresholded receive recovery are
different counters.

Across the eight physical processes per wait mode:

| Measurement | Legacy | Critical |
|---|---:|---:|
| Runtime wait calls | 1,355,381 | 1,664,633 |
| Waits exceeding budget by at least 1 ms | 53 | 10 |
| Such waits per million calls | 39.10 | 6.01 |
| Maximum measured wait (ms) | 12.459 | 10.925 |
| Steady playback missing frames, summed | 112 | 238 |
| CoreAudio processor overloads | 0 | 0 |
| Capture overflows | 0 | 0 |

The long physical critical wait was simultaneously visible in both localhost
workers: 10.928 / 10.859 ms phase wall time, with only 14 / 30 us CPU. Therefore
**the large local pause survives the timer change**. Counts from two workers
can describe the same shared pause; they are not independent incidents.
Missing-frame counts use the native steady window, after 10 seconds and with
the final two seconds excluded. Lower latency and fewer late waits did not
produce fewer physical missing frames in this small sample.

| Simulated pair | Legacy overall pulse RTT (ms) | Critical overall pulse RTT (ms) |
|---|---:|---:|
| Direct A | 3.209 | 2.880 |
| Direct B | 5.624 | 4.229 |
| Server A | 2.937 | 4.206 |
| Server B | 8.186 | 6.224 |

The simulated runs also varied substantially. Legacy had 44 late waits across
221,074 calls; critical had 27 across 252,524. Their maximum waits were 18.622
and 14.486 ms respectively. Steady missing frames were 668 versus 80. These
results support tighter usual timing, not deterministic immunity to host
pauses. No synthetic network impairment was applied in this audio matrix;
deterministic impairment coverage is in the separate four-peer CTests.

## Live Windows jam over Wi-Fi

Eight sequential 60-second joins used the existing Windows Server jam at
44.1 kHz. Focusrite ran legacy/critical then critical/legacy; Aggregate ran
critical/legacy between those pairs. A final Focusrite legacy/default pair
disabled receive timestamp/phase tracing. The last default run omitted
`--udp-wait` and reported `kqueue-critical`, verifying the promoted default.
No radio settings or Windows binaries changed. Every join exited normally.

| Remote case | Maximum wait overshoot (us) | Waits late by at least 1 ms | Max kernel-to-app age (us) | Max gap between kernel packet stamps (ms) | CPU (% of one core) |
|---|---:|---:|---:|---:|---:|
| Focusrite legacy A | 764 | 0 | 683 | 126.162 | 10.03 |
| Focusrite critical A | 478 | 0 | 520 | 137.412 | 9.97 |
| Aggregate critical | 3544 | 3 | 1618 | 121.155 | 10.08 |
| Aggregate legacy | 2750 | 3 | 1136 | 154.775 | 9.80 |
| Focusrite critical B | 538 | 0 | 509 | 129.344 | 9.37 |
| Focusrite legacy B | 1610 | 1 | 518 | 113.142 | 9.23 |
| Focusrite legacy, tracing off | 745 | 0 | unmeasured | unmeasured | 7.45 |
| Focusrite default, tracing off | 577 | 0 | unmeasured | unmeasured | 7.71 |

CPU is supervised child user+system time divided by elapsed wall time; it
includes the same small one-shot process-inspection helper in every case.
It is not a power measurement. Tracing-disabled zero timestamp counters mean
**not measured**, not zero network delay. Wait counters remain enabled.

Kernel-to-app age stayed below 1 ms for every timestamped Focusrite packet,
in both paths. Aggregate retained small tails: 2/41,231 packets above 1 ms with
critical versus 1/41,324 legacy. The largest kernel-stamped delivery gaps were
113–155 ms; the first packet after each case's largest gap was serviced in
only 25–51 us. Thus those particular large gaps were already present before
application routing. They cannot be attributed to a long Jam2 software queue,
but this alone does not distinguish Wi-Fi/network delay from sender or driver
behavior upstream of the receiving timestamp.

| Remote case | Steady queue median / p99 (ms) | Steady missing playback frames | Steady receive-recovery events |
|---|---:|---:|---:|
| Focusrite legacy A | 2.902 / 15.964 | 131616 | 132 |
| Focusrite critical A | 2.902 / 14.512 | 124608 | 119 |
| Aggregate critical | 3.628 / 15.964 | 126336 | 121 |
| Aggregate legacy | 3.628 / 16.689 | 141248 | 114 |
| Focusrite critical B | 3.628 / 15.964 | 126784 | 121 |
| Focusrite legacy B | 3.628 / 13.787 | 128640 | 126 |
| Focusrite legacy, tracing off | 4.354 / 13.787 | 127040 | 122 |
| Focusrite default, tracing off | 3.628 / 14.512 | 127168 | 126 |

These use fully connected periodic samples after 10 seconds and exclude the
last two seconds. Windows audio was not an automated pulse-return source, so
there is **no remote audio RTT measurement** here. Network ping RTT averaged
12.18–12.99 ms across cases; it must not be substituted for audio latency.
The remote data shows no consistent recovery/queue-tail reduction attributable
to the new timer. All eight runs had zero capture overflows, CoreAudio processor
overloads, ingress queue drops and audio queue drops. macOS unified logs also
contained zero Jam2 `skipping cycle due to overload` entries over the full
comparison interval, 16:20:00–16:52:11 UTC.

The tests support keeping the new default for tighter ordinary deadlines,
with legacy retained for comparison. They do not establish that it removes
multi-millisecond sleep/descheduling or cures network starvation. A prelaunch
kernel capture that actually includes one of the remaining long critical waits
is the next attribution step; the existing userspace spans cannot prove its
kernel cause.

## Native coverage and artifacts

Twelve directly relevant Release CTests passed:

- Wait ownership/readiness/deadline/error/cancellation tests; production wait
  timing; UDP backend tests; multiplex tests; CLI boundary tests.
- Exactly-four-peer Direct impairment, session lifecycle, UDP runtime paths,
  receive pressure, and rotation/key ordering. The runtime path test covers
  twelve combinations, including explicit legacy Direct and Server controls,
  and checks that the requested wait mode actually reached runtime snapshots.
- The eight-case simulated comparison and eight-case physical comparison.

The wait tests cover zero budgets, early input without consuming payload,
stale-timer cancellation, descriptor replacement and numeric reuse, larger
descriptor sets, registration failures and subsequent recovery, nested mode
scope cleanup, simultaneous wake/UDP readiness, stop wakeup and descriptor
cleanup at object/thread destruction. An initial fixture assumed UDP send
completion implied receive readiness; it was corrected to establish receive
readiness before testing simultaneous UDP and capture wake. The final tests
pass without changing the behavior or weakening their assertions.

The full suite was not requested and was not run. No Windows rebuild was run.

- [Focused CTest log](../artifacts/critical-wait-20261104/ctest-focused.log)
- [Four-peer CTest log](../artifacts/critical-wait-20261104/ctest-four-peer.log)
- [Audio matrix CTest log](../artifacts/critical-wait-20261104/ctest-latency-matrix.log)
- [Local per-peer CSV summary](../artifacts/critical-wait-20261104/local-summary.csv)
- [Local raw-summary index](../artifacts/critical-wait-20261104/local-summary.json)
- [Physical profile](../artifacts/critical-wait-20261104/local-profile.json)
- [Simulated CTest profile](../tests/profiles/macos-udp-wait-local-simulated.json)
- [Offline analysis](../artifacts/critical-wait-20261104/analyze_results.py)
- [Remote per-case CSV summary](../artifacts/critical-wait-20261104/remote-summary.csv)
- [Remote raw-summary index](../artifacts/critical-wait-20261104/remote-summary.json)
- [Remote comparison runner](../artifacts/critical-wait-20261104/run_remote_matrix.py)
- [macOS overload log query](../artifacts/critical-wait-20261104/hal-overloads.json)

Keep the legacy comparison while investigating the residual pauses. The data
supports the new path for normal deadline accuracy; it does not support claiming
that recovery or CoreAudio overloads are fixed. No clean overload occurred in
either physical local path, so this matrix cannot establish an overload cure.
