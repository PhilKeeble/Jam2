# Automated Stress and Network-Impairment Tests

Repeatable product stress is part of Jam2's native C++/CTest catalogue. It uses
bounded virtual-time or real-process cases rather than an independent Python
product runner.

## Focused commands

On Windows:

```powershell
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests performance"
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests network"
cmd.exe /d /c "call compile.cmd --in-dev-shell --tests shared-content"
```

On macOS:

```bash
bash ./compile.sh --tests performance
bash ./compile.sh --tests network
bash ./compile.sh --tests shared-content
```

Use `--test-name <registered-name>` after a suite to isolate one CTest while
developing. On both platforms, `--tests-full` runs the complete normal Release
test catalogue once. Windows source coverage is the separate, optional
`--coverage` audit used after major feature work; it is not part of the baseline
full-test command.

## What the native stress matrix proves

The automated real-jam cases always launch exactly four isolated Jam2
instances over the universal direct-mesh engine. They combine headless fake
audio and injected PCM with real process, socket, authentication, control,
asset-transfer, persistence, GUI-agent, and event-loop boundaries.

The network matrix includes deterministic delay, jitter, loss, duplication,
reordering, corruption, bounded burst pauses, malformed packets, replay,
sequence wrap/gaps, source interruption, and bounded floods. Every case checks
recovery and continued authenticated traffic rather than treating a rejection
counter alone as success.

The performance matrix covers every maintained metronome model and epoch
transition under the impairment grid. It asserts exact shared musical targets,
bounded clock mapping, correct count-in and recording schedules, peer-local
render compensation, callback/queue limits, and convergence across all four
instances.

The shared-content matrix covers simultaneous same/different WAV offers,
deduplication, conflicts, policy changes, interruption/rejoin, stale and partial
transfers, source handoff, repeated sharing, session reset, exact bytes/hashes,
relative JamJar paths, and isolated per-peer storage roots.

## Deliberate exclusions

Cross-machine campaigns and long real-time soak runs are manual usage tests.
They are not hidden inside `--tests-full`: environment coordination would make
the gate slow and nondeterministic, while accelerated native cases cover the
repeatable state transitions.

Real ASIO/CoreAudio checks are opt-in because they require an attached device
and, for the existing plugin callback path, an explicit plugin fixture. They
must never make the default hardware-independent gate environment-specific.

## Python impairment utility

`tools/jam2test/impairment.py` remains a small reusable UDP impairment library
for benchmarking/tool experiments. It can inject loss, corruption, duplicate,
jitter, reordering, fixed delay, and bounded burst pauses. The authoritative
automated product cases use the native C++ impairment support under `tests/`,
so Python timing is not part of a distribution pass/fail decision.
