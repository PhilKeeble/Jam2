# Tuning Profiles

Jam2 supports three named tuning profiles:

| Profile | Use |
| --- | --- |
| `fast` | Lowest-latency starting point for stable wired or very clean local network testing. |
| `moderate` | More forgiving general-purpose profile with a larger jitter buffer and lower packet rate. |
| `safe` | Safest Wi-Fi profile measured so far; higher latency, but best current dropout resistance. |

The CLI accepts `--profile fast`, `--profile moderate`, or `--profile safe` on `listen` and `connect`. The GUI exposes the profiles on **Start Jam** and sends the selected settings to the joining GUI before its engine starts. Explicit numeric CLI flags still override the selected profile.

## Fast Profile

The current low-latency default profile is:

```text
fast
```

Recommended values:

| Field | Value |
| --- | --- |
| Sample rate | `48000` |
| Audio buffer size | `32` |
| Frame size | `64` |
| Playback prefill frames | `256` |
| Playback max frames | `1536` |
| Capture ring frames | `4096` |
| Playback ring frames | `4096` |
| Stats warmup ms | `3000` |
| Drift correction | `on` |
| Drift smoothing | `0.02` |
| Drift deadband ppm | `25` |
| Drift max correction ppm | `500` |
| Sample-time playout | `on` |
| Playout delay frames | `256` |
| Jitter buffer frames | `512` |
| Jitter buffer max frames | `1024` |
| Adaptive cushion | `on` |
| Adaptive target frames | `256` |
| Adaptive min frames | `256` |
| Adaptive max frames | `1536` |
| Adaptive release ppm | `1000` |

This spends about `768` receive-side frames on jitter/playback cushion, around `16.0 ms` at `48000` Hz before device, driver, and network latency.

If this underruns or drops jitter-buffer packets on Wi-Fi, increase `Jitter buffer frames` first to `1024` and set `Jitter buffer max frames` to `3072`.

## Moderate Profile

The moderate profile uses a larger jitter buffer and `128` frame packets:

```text
moderate
```

Recommended values:

| Field | Value |
| --- | --- |
| Sample rate | `48000` |
| Audio buffer size | `64` |
| Frame size | `128` |
| Playback prefill frames | `512` |
| Playback max frames | `4096` |
| Capture ring frames | `4096` |
| Playback ring frames | `8192` |
| Drift correction | `on` |
| Drift smoothing | `0.02` |
| Drift deadband ppm | `25` |
| Drift max correction ppm | `500` |
| Sample-time playout | `on` |
| Playout delay frames | `512` |
| Jitter buffer frames | `2048` |
| Jitter buffer max frames | `3072` |
| Adaptive cushion | `on` |
| Adaptive target frames | `512` |
| Adaptive min frames | `512` |
| Adaptive max frames | `4096` |
| Adaptive release ppm | `1000` |

Use this when `fast` is close but shows occasional underruns or missing frames.

## Safe Profile

The safest Wi-Fi profile from the current two-machine benchmark data is:

```text
safe
```

Recommended values:

| Field | Value |
| --- | --- |
| Sample rate | `48000` |
| Audio buffer size | `64` |
| Frame size | `256` |
| Playback prefill frames | `1024` |
| Playback ring frames | `8192` |
| Playback max frames | `7168` |
| Drift correction | `on` |
| Drift smoothing | `0.02` |
| Drift deadband ppm | `25` |
| Drift max correction ppm | `500` |
| Sample-time playout | `on` |
| Playout delay frames | `1024` |
| Jitter buffer frames | `2048` |
| Jitter buffer max frames | `6144` |
| Adaptive cushion | `on` |
| Adaptive target frames | `1024` |
| Adaptive min frames | `1024` |
| Adaptive max frames | `7168` |
| Adaptive release ppm | `1000` |

At `48000` Hz this uses `256` frame packets, about `5.3 ms` per packet. The configured playout delay is `1024` frames, about `21.3 ms`, and the jitter buffer target is `2048` frames, about `42.7 ms`.

In the current Wi-Fi benchmark run this profile passed silence, tone, and pulse cases with no missing audio frames, no jitter-buffer drops, and no playback drops. Measured average playback depth was about `76-82 ms`, with average RTT around `34-37 ms`, so the practical one-way latency was roughly `95-105 ms` before audio device input and output latency.

Use this profile when Wi-Fi is unavoidable or when `fast` has dropouts that do not improve much from small prefill changes. It is not the lowest-latency profile, but it is currently the best measured Wi-Fi stability tradeoff.

## Network-Efficient Profile

This reduces packet rate at the cost of more packet interval latency:

```text
--sample-rate 44100
--audio-buffer-size 64
--frame-size 256
--playback-prefill-frames 1536
--playback-ring-frames 8192
--playback-max-frames 4096
--stats-warmup-ms 3000
```

## What To Change First

- If audio pops, clicks, or drops out, increase `playback-prefill-frames` first.
- If latency is too high and `playback_ring_underruns=0`, reduce `playback-prefill-frames`.
- If latency grows over time, keep `playback-max-frames` set so old audio is dropped instead of letting delay grow.
- If playback ring overruns are non-zero, use a larger playback ring than playback max.
- If `resampler_ratio` is pinned at the drift limit, inspect `drift_ppm` before changing correction limits.
- Lower `frame-size` and `audio-buffer-size` only after the current profile is stable.
