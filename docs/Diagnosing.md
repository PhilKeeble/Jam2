# Diagnosing Jam2

Jam2 exposes raw technical data so tuning can be based on measured behavior instead of guesswork. Enable stats while debugging and compare CSV files when testing changes.

## Before A Jam

1. Confirm both players can see their audio devices with `jam2 list-devices`.
2. Run `test-device` or `meter-device` locally if device setup is uncertain.
3. Use [Connection Test](ConnectionTest.md) to check UDP reachability before debugging Jam2 itself.
4. Start with a known profile from [Profiles](Profiles.md).
5. Keep sample rate and frame size identical on both peers.

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
.\release\jam2.exe listen --stats enabled --stats-interval-ms 1000 --log-stats logs
```

CSV rows are useful for comparing one change at a time: frame size, audio buffer size, playback prefill, jitter-buffer frames, adaptive cushion, network path, or audio device.

For automated stress and two-host benchmark runs, use [Test Cases](TestCases.md).
