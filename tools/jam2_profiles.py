#!/usr/bin/env python3

from dataclasses import dataclass, replace


STATS_INTERVAL_MS = 5000
STATS_WARMUP_MS = 3000


@dataclass(frozen=True)
class Jam2Profile:
    name: str
    audio_buffer_size: int = 64
    frame_size: int = 128
    playback_prefill_frames: int = 2048
    playback_ring_frames: int = 8192
    playback_max_frames: int = 4096
    drift_correction: str = "on"
    drift_smoothing: float = 0.02
    drift_deadband_ppm: int = 25
    drift_max_correction_ppm: int = 500
    adaptive_playback_cushion: str = "on"
    adaptive_playback_target_frames: int = 2048
    adaptive_playback_min_frames: int = 2048
    adaptive_playback_max_frames: int = 4096
    adaptive_playback_release_ppm: int = 1000
    metronome: str = "on"
    bpm: int = 120
    metronome_level: float = 0.20
    metronome_mode: str = "shared-grid"
    remote_level: float = 1.0
    sample_time_playout: str = "on"
    playout_delay_frames: int = 2048
    jitter_buffer_frames: int = 0
    jitter_buffer_max_frames: int = 0
    socket_send_buffer: int = 0
    socket_recv_buffer: int = 0
    input_channels: str = ""
    output_channels: str = ""

    def args(self, stream_ms):
        playback_max_frames = max(self.playback_max_frames, self.playback_prefill_frames * 2)
        adaptive_max_frames = max(self.adaptive_playback_max_frames, self.adaptive_playback_target_frames)
        args = [
            "--audio-buffer-size", str(self.audio_buffer_size),
            "--frame-size", str(self.frame_size),
            "--playback-prefill-frames", str(self.playback_prefill_frames),
            "--playback-ring-frames", str(self.playback_ring_frames),
            "--playback-max-frames", str(playback_max_frames),
            "--drift-correction", self.drift_correction,
            "--drift-smoothing", str(self.drift_smoothing),
            "--drift-deadband-ppm", str(self.drift_deadband_ppm),
            "--drift-max-correction-ppm", str(self.drift_max_correction_ppm),
            "--adaptive-playback-cushion", self.adaptive_playback_cushion,
            "--adaptive-playback-target-frames", str(self.adaptive_playback_target_frames),
            "--adaptive-playback-min-frames", str(self.adaptive_playback_min_frames),
            "--adaptive-playback-max-frames", str(adaptive_max_frames),
            "--adaptive-playback-release-ppm", str(self.adaptive_playback_release_ppm),
            "--metronome", self.metronome,
            "--bpm", str(self.bpm),
            "--metronome-level", str(self.metronome_level),
            "--metronome-mode", self.metronome_mode,
            "--remote-level", str(self.remote_level),
            "--sample-time-playout", self.sample_time_playout,
            "--playout-delay-frames", str(self.playout_delay_frames),
            "--jitter-buffer-frames", str(self.jitter_buffer_frames),
            "--jitter-buffer-max-frames", str(self.jitter_buffer_max_frames),
            "--stats", "enabled",
            "--stats-warmup-ms", str(STATS_WARMUP_MS),
            "--stats-interval-ms", str(STATS_INTERVAL_MS),
            "--stream-ms", str(stream_ms),
            "--stream-linger-ms", "500",
            "--machine-readable-startup", "on",
        ]
        if self.socket_send_buffer > 0:
            args.extend(["--socket-send-buffer", str(self.socket_send_buffer)])
        if self.socket_recv_buffer > 0:
            args.extend(["--socket-recv-buffer", str(self.socket_recv_buffer)])
        if self.input_channels:
            args.extend(["--input-channels", self.input_channels])
        if self.output_channels:
            args.extend(["--output-channels", self.output_channels])
        return args

    def metadata(self):
        return {
            "profile": self.name,
            "audio_buffer_size": self.audio_buffer_size,
            "frame_size": self.frame_size,
            "playback_prefill_frames": self.playback_prefill_frames,
            "playback_ring_frames": self.playback_ring_frames,
            "playback_max_frames": max(self.playback_max_frames, self.playback_prefill_frames * 2),
            "drift_correction": self.drift_correction,
            "drift_smoothing": self.drift_smoothing,
            "drift_deadband_ppm": self.drift_deadband_ppm,
            "drift_max_correction_ppm": self.drift_max_correction_ppm,
            "adaptive_playback_cushion": self.adaptive_playback_cushion,
            "adaptive_playback_target_frames": self.adaptive_playback_target_frames,
            "adaptive_playback_min_frames": self.adaptive_playback_min_frames,
            "adaptive_playback_max_frames": max(self.adaptive_playback_max_frames, self.adaptive_playback_target_frames),
            "adaptive_playback_release_ppm": self.adaptive_playback_release_ppm,
            "metronome": self.metronome,
            "bpm": self.bpm,
            "metronome_level": self.metronome_level,
            "metronome_mode": self.metronome_mode,
            "remote_level": self.remote_level,
            "sample_time_playout": self.sample_time_playout,
            "playout_delay_frames": self.playout_delay_frames,
            "jitter_buffer_frames": self.jitter_buffer_frames,
            "jitter_buffer_max_frames": self.jitter_buffer_max_frames,
            "socket_send_buffer": self.socket_send_buffer,
            "socket_recv_buffer": self.socket_recv_buffer,
            "input_channels": self.input_channels,
            "output_channels": self.output_channels,
        }


SAFE_LOCAL_PROFILE = Jam2Profile(
    name="safe_64_128_jitter_2048_tail_512",
    audio_buffer_size=64,
    frame_size=128,
    playback_prefill_frames=512,
    playback_ring_frames=8192,
    playback_max_frames=4096,
    adaptive_playback_target_frames=512,
    adaptive_playback_min_frames=512,
    adaptive_playback_max_frames=4096,
    playout_delay_frames=512,
    jitter_buffer_frames=2048,
    jitter_buffer_max_frames=3072,
)
AGGRESSIVE_LOCAL_PROFILE = Jam2Profile(
    name="aggressive_32_64_768",
    audio_buffer_size=32,
    frame_size=64,
    playback_prefill_frames=768,
    playback_ring_frames=4096,
    playback_max_frames=1536,
    adaptive_playback_target_frames=768,
    adaptive_playback_min_frames=768,
    adaptive_playback_max_frames=1536,
    playout_delay_frames=768,
)


def adaptive_off_profile(base=AGGRESSIVE_LOCAL_PROFILE):
    return variant(base, "adaptive_off", adaptive_playback_cushion="off")


def jitter_buffer_profile(base=AGGRESSIVE_LOCAL_PROFILE, jitter_frames=512, playback_tail_frames=256, adaptive=True):
    total_frames = jitter_frames + playback_tail_frames
    adaptive_state = "on" if adaptive else "off"
    playback_max_frames = max(total_frames * 2, base.playback_max_frames)
    adaptive_max_frames = max(total_frames * 2, base.adaptive_playback_max_frames)
    ring_frames = max(base.playback_ring_frames, playback_max_frames, adaptive_max_frames)
    return variant(
        base,
        f"jitter_{jitter_frames}_tail_{playback_tail_frames}_{adaptive_state}",
        jitter_buffer_frames=jitter_frames,
        jitter_buffer_max_frames=max(jitter_frames * 2, jitter_frames + base.frame_size),
        playback_prefill_frames=playback_tail_frames,
        playout_delay_frames=playback_tail_frames,
        playback_ring_frames=ring_frames,
        playback_max_frames=playback_max_frames,
        adaptive_playback_cushion=adaptive_state,
        adaptive_playback_target_frames=playback_tail_frames,
        adaptive_playback_min_frames=playback_tail_frames,
        adaptive_playback_max_frames=adaptive_max_frames)


def latency_matched_prefill_profile(base=AGGRESSIVE_LOCAL_PROFILE, total_frames=768, adaptive=True):
    adaptive_state = "on" if adaptive else "off"
    playback_max_frames = max(total_frames * 2, base.playback_max_frames)
    adaptive_max_frames = max(total_frames * 2, base.adaptive_playback_max_frames)
    ring_frames = max(base.playback_ring_frames, playback_max_frames, adaptive_max_frames)
    return variant(
        base,
        f"prefill_{total_frames}_{adaptive_state}",
        jitter_buffer_frames=0,
        jitter_buffer_max_frames=0,
        playback_prefill_frames=total_frames,
        playout_delay_frames=total_frames,
        playback_ring_frames=ring_frames,
        playback_max_frames=playback_max_frames,
        adaptive_playback_cushion=adaptive_state,
        adaptive_playback_target_frames=total_frames,
        adaptive_playback_min_frames=total_frames,
        adaptive_playback_max_frames=adaptive_max_frames)


def variant(base, suffix, **changes):
    return replace(base, name=f"{base.name}_{suffix}", **changes)
