# Diagnosing Jam2

Jam2 exposes raw technical data so tuning can be based on measured behavior instead of guesswork. Enable stats while debugging and compare CSV files when testing changes.

## Before A Jam

1. Confirm both players can see their audio devices with `jam2 list-devices`.
2. Run `test-device <id>` locally, or use **Test Device** in the GUI, if device setup is uncertain.
3. Use [Connection Test](ConnectionTest.md) to check UDP reachability before debugging Jam2 itself.
4. Start with a known profile from [Profiles](Profiles.md).
5. Let the creator choose the session sample rate and frame size; joiners receive both through the authenticated session contract.

## Common Symptoms

| Symptom | First stats to inspect | Usual next step |
| --- | --- | --- |
| Pops or dropouts | `playback_ring_underruns`, jitter max, jitter-buffer drops | Increase playback prefill or jitter-buffer frames. |
| Latency grows over time | playback depth, `playback-max-frames`, drift ppm | Set or lower playback max frames; inspect drift correction. |
| One side cannot connect | connection test verdict, bind host, public endpoint, STUN result | Check firewall, LAN IP, port forwarding, or NAT behavior. |
| Click feels unstable | metronome mode, RTT, jitter, callback gaps | Compare metronome modes with recordings and CSV logs. |
| Remote level is wrong | local remote level, sender input level | Adjust local remote level or sender gain outside Jam2. |
| CPU/device instability | audio callback gaps, xruns, underruns | Increase audio buffer size or use a better native driver. |

## Stats Worth Watching

- `sequence_lost`, `sequence_loss_events`, `sequence_loss_max_gap`: UDP packet loss shape.
- `jitter_min_ms`, `jitter_avg_ms`, `jitter_max_ms`: packet arrival variation.
- `rtt_min_ms`, `rtt_avg_ms`, `rtt_max_ms`: network round-trip time.
- `playback_depth_*`: how much audio is queued for playback.
- `playback_ring_underruns`: playback ran out of audio.
- `playback_ring_overruns`: playback queue overflowed.
- `jitter_buffer_late_packets` and `jitter_buffer_dropped_packets`: packets missed the jitter-buffer deadline or span limit.
- `drift_ppm` and `resampler_ratio`: device clock mismatch and correction.
- `receive_loop_gap_max_ms`: local stream loop stalls.
- `audio_callback_gap_over_*`: host audio callback timing gaps.

## CSV Logs

Enable CSV logging during repeatable tests:

```powershell
.\release\jam2.exe network create --stats enabled --stats-interval-ms 1000 --log-stats logs
```

CSV rows are useful for comparing one change at a time: frame size, audio buffer size, playback prefill, jitter-buffer frames, adaptive cushion, network path, or audio device.

GUI jams write a periodic row every two seconds and a final aggregate row when
stats are enabled. Use the periodic rows and `network_active_peer_count` when
analysing a particular connection interval. In particular, separate rows where
a peer is connected from time after the last peer leaves. A final aggregate row
spans the whole runtime and by itself cannot attribute increasing counters to
the connected portion of the jam. Disabling GUI diagnostics also disables this
collection and CSV path.

For clean post-build checks use [Validation](Validation.md). For controlled
impairment and recovery use [Stress Tests](StressTests.md), and for real
two-peer measurements use [Benchmark](Benchmark.md).

Bounded native parser fuzzing uses the same compact artifact convention:

```powershell
python tools\jam2_test.py fuzz all
```

Its default root is `tools/fuzz_logs/<YYYYMMDDTHHMMZ>`. With
`--output PATH`, it is `PATH/<YYYYMMDDTHHMMZ>`. Retained failures are direct
children named `<target>-<index>-<digest>`; there is no redundant `failures`
directory. As with the other tool families, a small numeric timestamp suffix is
used only for a same-minute collision.
