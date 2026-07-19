# Jam2 Two-Peer Benchmarks

`benchmark` measures the normal direct Jam2 path between two peers. The peers
may run on separate machines or as two processes on one machine. Python
coordinates the run and transfers artifacts after each case; it never relays
audio. The benchmark has two fixed, versioned suites:

- `core`: nine cases covering the main profile and protocol decisions.
- `full`: the core cases plus sixteen focused diagnostic cases.

The coordinator always runs the Fast PCM24 control first. Every primary case
runs once for 30 seconds. If its RTT, jitter, or loss is materially different
from the control, the coordinator runs one labelled confirmation. It retains
both measurements and never selects the better result.

Run commands from the repository root. They use `release\jam2.exe` by default;
use `--jam2 PATH` to select another build.

## Requirements

Both peers need:

- the same compatible Jam2 build and benchmark catalog;
- Python 3;
- distinct `--machine-id` values;
- the same `--sample-rate` value;
- either `--headless-audio` or one machine-local `--audio-device ID`.

The coordinator needs TCP `49000` for benchmark control and TCP/UDP `49001`
for Jam2 create/join. The agent needs outbound access to those ports. Start the
coordinator first and then the agent.

Use `--network-profile wired`, `wifi`, or `unknown` to label the connection.
This is recorded metadata and never changes Jam2 tuning.

### Physical audio

List device IDs and test the selected device before benchmarking:

```powershell
.\release\jam2.exe list-devices
.\release\jam2.exe test-device 5
```

`test-device` checks 44100/48000 Hz and 32/64/128/256-frame buffers. A physical
core run needs the requested sample rate plus 32- and 64-frame callbacks. A
physical full run also uses a 128-frame callback. Device IDs are local and may
differ between the machines.

The benchmark injects deterministic test signals while still using the real
device callback. It measures the driver, device clock, callback timing, xruns,
and their interaction with the network path. It does not measure analog
round-trip latency.

### Headless audio

A prebuilt Jam2 executable can run headlessly without an installed ASIO driver
or audio device:

```powershell
--headless-audio
```

The synthetic callback uses each case's requested audio-buffer size. It is not
fixed at 256 frames. Headless runs exercise packetization, UDP, jitter and
playback buffers, prefill, adaptive buffering, resampling, mixing, metronome,
recording, and stats. They do not claim ASIO-driver, physical clock, hardware
latency, or device-xrun coverage. Building Jam2 on Windows still requires the
ASIO SDK.

Results are classified as `headless-headless`, `physical-physical`, or `mixed`.
Do not pool these coverage classes when comparing callback, drift, scheduling,
or xrun behavior.

## Running Core and Full

Replace the example addresses and device IDs with values for the two machines.
The suite name must match on both commands.

### Core with headless audio

Coordinator at `192.168.1.50`:

```powershell
python tools\jam2_test.py benchmark coordinator core --machine-id studio-a --sample-rate 48000 --headless-audio --network-profile wifi --public-audio-host 192.168.1.50
```

Agent:

```powershell
python tools\jam2_test.py benchmark agent core --machine-id studio-b --sample-rate 48000 --headless-audio --network-profile wifi --coordinator 192.168.1.50:49000
```

### Localhost headless baseline

Run these in two terminals. This keeps separate local peer roots while the
coordinator also receives the agent files needed for a self-contained result:

Coordinator:

```powershell
python tools\jam2_test.py benchmark coordinator core --machine-id coord --sample-rate 48000 --headless-audio --network-profile unknown --public-audio-host 127.0.0.1 --output artifacts\localhost_headless_coord
```

Agent:

```powershell
python tools\jam2_test.py benchmark agent core --machine-id agent --sample-rate 48000 --headless-audio --network-profile unknown --coordinator 127.0.0.1:49000 --output artifacts\localhost_headless_agent
```

When both peers run on one machine, give them different custom output roots as
shown. They intentionally share the coordinator-issued timestamp, so one
common root would collide.

### Core with physical devices

Coordinator:

```powershell
python tools\jam2_test.py benchmark coordinator core --machine-id studio-a --sample-rate 44100 --audio-device 5 --network-profile wired --public-audio-host 192.168.1.50
```

Agent:

```powershell
python tools\jam2_test.py benchmark agent core --machine-id studio-b --sample-rate 44100 --audio-device 16 --network-profile wired --coordinator 192.168.1.50:49000
```

### Full

Use the same commands with `full` in place of `core`. Full uses the same first
nine cases as core with identical settings.

### Mixed coverage

One machine may use `--audio-device` while the other uses `--headless-audio`.
This is useful for diagnosing one physical endpoint, but the result is labelled
`mixed` and is not equivalent to either a two-device or two-headless run.

Use `--control`, `--audio-bind`, and `--output` only when the default ports or
artifact location are unsuitable. Without `--output`, `--clean` removes the
default `tools\benchmark_logs` contents. With `--output PATH`, it clears that
exact custom artifact root before allocating the invocation.

List the exact catalog without starting a run:

```powershell
python tools\jam2_test.py benchmark list core
python tools\jam2_test.py benchmark list full
```

## Cases

All cases record their purpose, comparator, base native profile, sparse
overrides, signal direction, wire format, and interpretation.

### Core cases

| Case | What it tests |
|---|---|
| `control-fast-tone-pcm24` | Fast bidirectional PCM24 control for all main tuning comparisons. |
| `signal-fast-pulse-pcm24` | Transient continuity, missing frames, dropouts, pops, and timing. |
| `profile-moderate-tone-pcm24` | Complete Moderate profile latency/resilience tradeoff. |
| `profile-safe-tone-pcm24` | Complete Safe profile latency/resilience tradeoff. |
| `adaptive-fast-off-tone-pcm24` | Fast with adaptive playback cushioning disabled. |
| `buffer-fast-prefill-only-768-adaptive-on-tone-pcm24` | Latency-matched prefill-only buffering against Fast's jitter-buffer strategy. |
| `frame-fast-128-tone-pcm24` | 128-frame packet interval, bitrate, CPU, and loss sensitivity against Fast's 64 frames. |
| `format-fast-tone-pcm16` | Matched PCM16 against the PCM24 control. |
| `metronome-moderate-shared-grid-pcm24` | Shared-grid mapping, click timing, and transport baseline. |

### Additional full cases

| Case | What it tests |
|---|---|
| `signal-fast-silence-pcm24` | Unexpected signal, noise, clipping, and recording integrity. |
| `direction-coordinator-to-agent-tone-pcm24` | Coordinator-to-agent audio and route asymmetry. |
| `direction-agent-to-coordinator-tone-pcm24` | Agent-to-coordinator audio and route asymmetry. |
| `frame-fast-32-tone-pcm24` | Lower packetization latency against higher packet rate and CPU. |
| `frame-fast-256-tone-pcm24` | Lower packet rate against higher packetization latency. |
| `callback-fast-64-frame-64-tone-pcm24` | 64-frame physical or synthetic callback against Fast's 32 frames. |
| `callback-fast-128-frame-64-tone-pcm24` | 128-frame physical or synthetic callback against Fast's 32 frames. |
| `buffer-fast-prefill-only-768-adaptive-off-tone-pcm24` | Fixed versus adaptive latency-matched prefill. |
| `buffer-fast-jitter-2048-tail-256-adaptive-on-tone-pcm24` | Larger jitter absorption without changing Fast packet sizing. |
| `sample-time-fast-off-tone-pcm24` | Sample-time playout effects on delay error and late handling. |
| `drift-correction-fast-off-tone-pcm24` | Drift correction effects; physical-device evidence is most useful. |
| `socket-buffer-fast-1m-tone-pcm24` | 1 MiB socket buffers against default loss and receive work. |
| `os-priority-fast-off-tone-pcm24` | Jam2 without process/thread priority elevation. |
| `os-priority-fast-realtime-tone-pcm24` | Realtime scheduling request against the default high priority. |
| `metronome-moderate-leader-audio-pcm24` | Leader selection, injected click delivery, and click integrity. |
| `metronome-moderate-listener-compensated-pcm24` | Compensation target, convergence, clamping, and alignment. |

## Outlier Confirmations

The first control supplies the network reference. A completed primary case gets
one immediate `outlier-confirmation` when the absolute difference exceeds:

- RTT average: the greater of 10 ms or 50% of control;
- jitter average: the greater of 5 ms or 100% of control;
- loss: the greater of 0.5 percentage points or the control loss.

Confirmations cannot trigger further confirmations. Failed process attempts are
separate bounded infrastructure retries. The original result, confirmation,
thresholds, measurements, and reasons all remain in the dataset.

## Results and WAV Retention

The coordinator output is:

```text
tools/benchmark_logs/<invocation-id>/
```

`<invocation-id>` is the UTC minute in `YYYYMMDDTHHMMZ` form. If another
invocation already exists for that minute, `_1`, `_2`, and so on avoid the
collision. A custom output is used directly:

```text
PATH/<YYYYMMDDTHHMMZ>/
```

Benchmark executions use:

```text
<case-id>/<execution-number>/
  scenario.json
  peer-result.json
  stats.csv
  agent_scenario.json
  agent_peer-result.json
  agent_stats.csv
  benchmark-result.json
```

Important root files are:

- `invocation-manifest.json`: invocation state, identities, catalog, hashes,
  build/profile information, and case outcomes;
- `analysis.json` and `analysis.csv`: complete normalized results and one
  aggregate row per attempt;
- `peer-analysis.csv`: per-machine metrics and effective configuration;
- `comparisons.json` and `comparisons.csv`: raw numeric deltas against each
  case's declared comparator;
- `format-comparison.json` and `.csv`: matched PCM16/PCM24 measurements.

Each execution keeps coordinator and uploaded agent data together. Coordinator
filenames are unchanged; each uploaded filename receives an `agent_` prefix.
Machine IDs, suite identity, attempt UUID, run kind, and network labels stay in
the structured JSON/CSV data rather than lengthening directory names. The
numeric execution folder is `1` for the primary attempt and increases for a
retry or objective outlier confirmation.

After every WAV is parsed successfully, the tooling deletes the WAVs and keeps
their byte counts and SHA-256 hashes plus structured measurements: frames,
duration, peak, RMS, noise floor, tone frequency, clipping spans, bounded pop
locations/severity, bounded dropout spans, pulses, and metronome timing.
Clipping, pops, dropouts, or a missing expected tone are findings, not analyzer
failures, so those WAVs are also deleted after their findings are recorded.
WAVs remain only when required audio or metadata was missing, malformed, or
unreadable.

The agent uploads a validated attempt to the coordinator. Its local copy remains
unless `--delete-after-upload` is used.

## Offline Analysis and Packaging

Analyze one invocation or a directory containing several:

```powershell
python tools\jam2_test.py benchmark analyze tools\benchmark_logs\<invocation-id>
```

This writes a new analysis invocation. Do not combine `--clean` with a source
inside the same default `benchmark_logs` family.

Create a compact local submission:

```powershell
python tools\jam2_test.py benchmark package tools\benchmark_logs\<invocation-id>
```

Use `--output C:\path\result.zip` to select the archive path. Packaging never
uploads data. The ZIP contains normalized JSON/CSV, comparator deltas, raw stats
CSVs, build/protocol/configuration metadata, and a hashed submission manifest.
It excludes invite URLs, session keys, absolute paths, and process/control logs.
Only WAVs from incomplete analyses are included.

## Comparing Runs Over Time

Compare data only when the following are known:

- benchmark catalog version and exact case ID;
- Jam2 build hash and control/UDP protocol versions;
- complete effective configuration, not just the profile name;
- sample rate and negotiated frame/format contract;
- coverage class and device/backend identity;
- machine IDs and network profile;
- primary versus confirmation status.

Keep the same two machines, devices, sample rate, placement, and network path
when measuring network change over time. Record a new run after Jam2, driver,
operating-system, router, interface, or physical placement changes. Cross-build
data remains useful for protocol development, but it is not a like-for-like
network trend unless effective settings and protocol versions also match.

## Reading the Measurements

Start by checking that the control completed, both peers have CSV and complete
audio analysis, negotiated settings match, and any outlier confirmation agrees
with its primary. Then compare hard measurements:

- **Profiles:** prefer the least-buffered profile that does not introduce
  missing/late frames, underrun time, playback drops, jitter-buffer drops,
  capacity drops, or audio discontinuities. Account for its playback depth and
  device-buffer latency.
- **Adaptive buffering:** compare Fast control with `adaptive-fast-off`.
  Raise/burst events show whether adaptation was needed; target and recovery
  fields show its added depth and whether it released afterward.
- **Prefill and jitter buffering:** compare missing/late frames and underruns
  against playback depth. Prefill-only and jitter-buffer cases have similar
  intended buffering but react differently to bursty arrival.
- **Frame size:** smaller frames reduce packetization time but increase packets,
  header bitrate, CPU, and exposure to packet loss. Larger frames do the
  opposite.
- **Callback size:** on physical coverage, compare active buffer time, callback
  interval/gaps, xruns, CPU, and underruns. Headless callback cases measure only
  the synthetic scheduler and Jam2 algorithms.
- **Drift correction:** physical runs should show raw drift, resampler ratio,
  active/clamped percentage, and whether buffer depth remains bounded.
  Zero-drift headless results are not evidence that correction is unnecessary.
- **Sample-time playout:** inspect playout-delay error, stale/future rejects,
  late frames, drops, and depth stability.
- **Wire format:** PCM16 should reduce payload, bitrate, and possibly CPU.
  Compare its audio measurements with PCM24 rather than assuming the bandwidth
  saving is free.
- **Scheduling and sockets:** verify the requested scheduling mode was active,
  then compare callback gaps, work-budget yields, loss, and drops. A request
  that the operating system did not grant is not a valid comparison.
- **Metronome:** inspect authority identity, mapped epoch, beat delta,
  compensation target/offset, convergence, stale/clamp events, and structured
  click analysis.

The tooling deliberately does not produce a subjective playability score or
choose a profile. The result should support a decision with measured latency,
loss, buffering, callback, drift, and audio-integrity costs visible.

For controlled local impairment use [Stress Tests](StressTests.md). For the
deterministic post-build baseline use [Validation](Validation.md).
