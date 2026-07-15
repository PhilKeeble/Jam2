"""Native-backed profile names plus sparse experimental overrides.

Native `debug describe` data is the only source for base-profile values. The
benchmark/stress matrices own only their deliberate differences.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


_native_profiles: dict[str, dict[str, Any]] = {}


def configure_native_profiles(description: dict[str, Any]) -> None:
    global _native_profiles
    _native_profiles = {item["name"]: dict(item) for item in description["profiles"]}


@dataclass(frozen=True)
class Jam2Profile:
    name: str
    base_profile: str
    overrides: dict[str, Any] = field(default_factory=dict)

    def __getattr__(self, key: str) -> Any:
        if key == "cli_profile": return self.base_profile
        if key in self.overrides: return self.overrides[key]
        if self.base_profile not in _native_profiles:
            raise RuntimeError("native profiles must be configured from `jam2 debug describe` before reading values")
        try:
            return _native_profiles[self.base_profile][key]
        except KeyError as error:
            raise AttributeError(key) from error

    def runtime(self, stream_ms: int) -> dict[str, Any]:
        values = dict(self.overrides)
        values.update({"stats": True, "stats_warmup_ms": 3000, "stats_interval_ms": 5000,
                       "stream_ms": stream_ms, "stream_linger_ms": 500})
        return values

    def metadata(self) -> dict[str, Any]:
        return {
            "profile": self.name,
            "base_profile": self.base_profile,
            "sparse_overrides": dict(self.overrides),
        }


FAST_PROFILE = Jam2Profile("fast", "fast")
MODERATE_PROFILE = Jam2Profile("moderate", "moderate")
SAFE_PROFILE = Jam2Profile("safe", "safe")
AGGRESSIVE_LOCAL_PROFILE = FAST_PROFILE
SAFE_LOCAL_PROFILE = MODERATE_PROFILE


def variant(base: Jam2Profile, suffix: str, **changes: Any) -> Jam2Profile:
    return Jam2Profile(f"{base.name}_{suffix}", base.base_profile,
                       {**base.overrides, **changes})


def adaptive_off_profile(base: Jam2Profile = FAST_PROFILE) -> Jam2Profile:
    return variant(base, "adaptive_off", adaptive_playback_cushion=False)


def jitter_buffer_profile(base: Jam2Profile = FAST_PROFILE, jitter_frames: int = 512,
                          playback_tail_frames: int = 256, adaptive: bool = True) -> Jam2Profile:
    total = jitter_frames + playback_tail_frames
    playback_max = max(total * 2, base.playback_max_frames)
    adaptive_max = max(total * 2, base.adaptive_playback_max_frames)
    return variant(
        base, f"jitter_{jitter_frames}_tail_{playback_tail_frames}_{'on' if adaptive else 'off'}",
        jitter_buffer_frames=jitter_frames,
        jitter_buffer_max_frames=max(jitter_frames * 2, jitter_frames + base.frame_size),
        playback_prefill_frames=playback_tail_frames,
        playout_delay_frames=playback_tail_frames,
        playback_ring_frames=max(base.playback_ring_frames, playback_max, adaptive_max),
        playback_max_frames=playback_max,
        adaptive_playback_cushion=adaptive,
        adaptive_playback_target_frames=playback_tail_frames,
        adaptive_playback_min_frames=playback_tail_frames,
        adaptive_playback_max_frames=adaptive_max,
    )


def latency_matched_prefill_profile(base: Jam2Profile = FAST_PROFILE, total_frames: int = 768,
                                    adaptive: bool = True) -> Jam2Profile:
    playback_max = max(total_frames * 2, base.playback_max_frames)
    adaptive_max = max(total_frames * 2, base.adaptive_playback_max_frames)
    return variant(
        base, f"prefill_{total_frames}_{'on' if adaptive else 'off'}",
        jitter_buffer_frames=0, jitter_buffer_max_frames=0,
        playback_prefill_frames=total_frames, playout_delay_frames=total_frames,
        playback_ring_frames=max(base.playback_ring_frames, playback_max, adaptive_max),
        playback_max_frames=playback_max, adaptive_playback_cushion=adaptive,
        adaptive_playback_target_frames=total_frames,
        adaptive_playback_min_frames=total_frames,
        adaptive_playback_max_frames=adaptive_max,
    )


def wifi_frame_size_profile(audio_buffer_size: int = 64, frame_size: int = 128,
                            jitter_frames: int = 2048, playback_tail_frames: int = 512,
                            jitter_max_frames: int = 4096) -> Jam2Profile:
    base = SAFE_PROFILE
    total = jitter_frames + playback_tail_frames
    playback_max = max(jitter_max_frames + playback_tail_frames, total * 2)
    return Jam2Profile(
        f"wifi_{audio_buffer_size}_{frame_size}_jitter_{jitter_frames}_tail_{playback_tail_frames}_max_{jitter_max_frames}",
        base.base_profile,
        {"audio_buffer_size": audio_buffer_size, "frame_size": frame_size,
         "playback_prefill_frames": playback_tail_frames,
         "playback_ring_frames": max(base.playback_ring_frames, playback_max),
         "playback_max_frames": playback_max,
         "adaptive_playback_target_frames": playback_tail_frames,
         "adaptive_playback_min_frames": playback_tail_frames,
         "adaptive_playback_max_frames": playback_max,
         "playout_delay_frames": playback_tail_frames,
         "jitter_buffer_frames": jitter_frames,
         "jitter_buffer_max_frames": jitter_max_frames},
    )
