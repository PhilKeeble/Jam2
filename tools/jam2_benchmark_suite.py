#!/usr/bin/env python3

from dataclasses import dataclass, field

from jam2_profiles import (
    AGGRESSIVE_LOCAL_PROFILE,
    SAFE_LOCAL_PROFILE,
    Jam2Profile,
    adaptive_off_profile,
    jitter_buffer_profile,
    latency_matched_prefill_profile,
    variant,
    wifi_frame_size_profile,
)


DEFAULT_SIGNALS = ("silence", "tone-440", "pulse-1s")
METRONOME_SIGNALS = ("metronome-only",)


@dataclass(frozen=True)
class BenchmarkCase:
    case_id: str
    profile: Jam2Profile
    signal: str
    server_signal: str = ""
    client_signal: str = ""
    repeats: int = 1
    stream_ms: int = 30000
    expect_metronome: bool = False
    server_args: tuple = field(default_factory=tuple)
    client_args: tuple = field(default_factory=tuple)

    def __post_init__(self):
        if not self.server_signal:
            object.__setattr__(self, "server_signal", self.signal)
        if not self.client_signal:
            object.__setattr__(self, "client_signal", self.signal)

    def metadata(self):
        return {
            "case_id": self.case_id,
            "signal": self.signal,
            "server_signal": self.server_signal,
            "client_signal": self.client_signal,
            "repeats": self.repeats,
            "stream_ms": self.stream_ms,
            "expect_metronome": self.expect_metronome,
            "server_args": list(self.server_args),
            "client_args": list(self.client_args),
            "profile": self.profile.metadata(),
        }


def static_profiles():
    safe = SAFE_LOCAL_PROFILE
    aggressive = AGGRESSIVE_LOCAL_PROFILE
    return [
        safe,
        aggressive,
        adaptive_off_profile(aggressive),
        variant(safe, "sample_time_off", sample_time_playout="off"),
        variant(safe, "playout_1024", playback_prefill_frames=1024, playback_max_frames=2048,
                adaptive_playback_target_frames=1024, adaptive_playback_min_frames=1024,
                adaptive_playback_max_frames=2048, playout_delay_frames=1024),
        variant(safe, "playout_3072", playback_prefill_frames=3072, playback_ring_frames=8192,
                playback_max_frames=6144, adaptive_playback_target_frames=3072,
                adaptive_playback_min_frames=3072, adaptive_playback_max_frames=6144,
                playout_delay_frames=3072),
        latency_matched_prefill_profile(aggressive, total_frames=768, adaptive=True),
        jitter_buffer_profile(aggressive, jitter_frames=512, playback_tail_frames=256, adaptive=True),
        latency_matched_prefill_profile(aggressive, total_frames=768, adaptive=False),
        jitter_buffer_profile(aggressive, jitter_frames=512, playback_tail_frames=256, adaptive=False),
        variant(
            jitter_buffer_profile(aggressive, jitter_frames=1024, playback_tail_frames=256, adaptive=True),
            "max_3072",
            jitter_buffer_max_frames=3072),
        variant(
            jitter_buffer_profile(aggressive, jitter_frames=2048, playback_tail_frames=256, adaptive=True),
            "max_3072",
            jitter_buffer_max_frames=3072),
        variant(
            jitter_buffer_profile(aggressive, jitter_frames=2048, playback_tail_frames=256, adaptive=True),
            "max_4096",
            jitter_buffer_max_frames=4096),
        variant(
            jitter_buffer_profile(aggressive, jitter_frames=2048, playback_tail_frames=256, adaptive=False),
            "max_4096",
            jitter_buffer_max_frames=4096),
        latency_matched_prefill_profile(aggressive, total_frames=2304, adaptive=True),
        latency_matched_prefill_profile(aggressive, total_frames=2304, adaptive=False),
        variant(safe, "drift_off", drift_correction="off"),
        variant(safe, "drift_tight", drift_deadband_ppm=5, drift_smoothing=0.05),
        variant(safe, "socket_large", socket_send_buffer=1048576, socket_recv_buffer=1048576),
    ]


def wifi_diagnostic_profiles():
    aggressive = AGGRESSIVE_LOCAL_PROFILE
    best_aggressive_jitter = variant(
        jitter_buffer_profile(aggressive, jitter_frames=2048, playback_tail_frames=256, adaptive=True),
        "max_3072",
        jitter_buffer_max_frames=3072)
    return {
        "best_aggressive_jitter": best_aggressive_jitter,
        "best_aggressive_jitter_sample_time_off": variant(
            best_aggressive_jitter,
            "sample_time_off",
            sample_time_playout="off"),
        "safe_sample_time_off": variant(
            SAFE_LOCAL_PROFILE,
            "sample_time_off",
            sample_time_playout="off"),
        "safe_socket_large": variant(
            SAFE_LOCAL_PROFILE,
            "socket_large",
            socket_send_buffer=1048576,
            socket_recv_buffer=1048576),
        "wifi_frame_128": wifi_frame_size_profile(
            frame_size=128,
            jitter_frames=2048,
            playback_tail_frames=512,
            jitter_max_frames=4096),
        "wifi_frame_256": wifi_frame_size_profile(
            frame_size=256,
            jitter_frames=2048,
            playback_tail_frames=1024,
            jitter_max_frames=6144),
        "wifi_audio_128_frame_256": wifi_frame_size_profile(
            audio_buffer_size=128,
            frame_size=256,
            jitter_frames=2048,
            playback_tail_frames=1024,
            jitter_max_frames=6144),
        "wifi_frame_256_socket_large": variant(
            wifi_frame_size_profile(
                frame_size=256,
                jitter_frames=2048,
                playback_tail_frames=1024,
                jitter_max_frames=6144),
            "socket_large",
            socket_send_buffer=1048576,
            socket_recv_buffer=1048576),
        "wifi_prefill_3072": latency_matched_prefill_profile(
            SAFE_LOCAL_PROFILE,
            total_frames=3072,
            adaptive=True),
    }


def wifi_diagnostic_cases(selected_signals, stream_ms=30000, repeats=1):
    signals = set(selected_signals)
    profiles = wifi_diagnostic_profiles()
    cases = []

    if "tone-440" in signals:
        cases.append(BenchmarkCase(
            case_id=f"{profiles['best_aggressive_jitter'].name}_repeat5_tone-440",
            profile=profiles["best_aggressive_jitter"],
            signal="tone-440",
            stream_ms=stream_ms,
            repeats=5))
        cases.append(BenchmarkCase(
            case_id=f"{profiles['best_aggressive_jitter'].name}_long120s_tone-440",
            profile=profiles["best_aggressive_jitter"],
            signal="tone-440",
            stream_ms=120000,
            repeats=repeats))
        cases.append(BenchmarkCase(
            case_id=f"{profiles['safe_sample_time_off'].name}_repeat5_tone-440",
            profile=profiles["safe_sample_time_off"],
            signal="tone-440",
            stream_ms=stream_ms,
            repeats=5))
        cases.append(BenchmarkCase(
            case_id=f"{profiles['safe_socket_large'].name}_repeat3_tone-440",
            profile=profiles["safe_socket_large"],
            signal="tone-440",
            stream_ms=stream_ms,
            repeats=3))
        for key in ("best_aggressive_jitter", "safe_sample_time_off"):
            profile = profiles[key]
            cases.append(BenchmarkCase(
                case_id=f"{profile.name}_tone-server-to-client",
                profile=profile,
                signal="tone-server-to-client",
                server_signal="tone-440",
                client_signal="silence",
                stream_ms=stream_ms,
                repeats=repeats))
            cases.append(BenchmarkCase(
                case_id=f"{profile.name}_tone-client-to-server",
                profile=profile,
                signal="tone-client-to-server",
                server_signal="silence",
                client_signal="tone-440",
                stream_ms=stream_ms,
                repeats=repeats))

    if "silence" in signals:
        cases.append(BenchmarkCase(
            case_id=f"{profiles['best_aggressive_jitter'].name}_repeat5_silence",
            profile=profiles["best_aggressive_jitter"],
            signal="silence",
            stream_ms=stream_ms,
            repeats=5))

    for key in ("best_aggressive_jitter_sample_time_off", "wifi_frame_128", "wifi_frame_256",
                "wifi_audio_128_frame_256", "wifi_frame_256_socket_large", "wifi_prefill_3072"):
        profile = profiles[key]
        for signal in selected_signals:
            if signal in DEFAULT_SIGNALS:
                cases.append(BenchmarkCase(
                    case_id=f"{profile.name}_{signal}",
                    profile=profile,
                    signal=signal,
                    stream_ms=stream_ms,
                    repeats=repeats))

    return cases


def metronome_profiles():
    base = variant(SAFE_LOCAL_PROFILE, "metro", metronome="on", bpm=120, metronome_level=0.20)
    return [
        variant(base, "shared_grid", metronome_mode="shared-grid"),
        variant(base, "leader_audio", metronome_mode="leader-audio"),
        variant(base, "symmetric_delay", metronome_mode="symmetric-delay"),
        variant(base, "listener_compensated", metronome_mode="listener-compensated"),
    ]


def benchmark_cases(signals=None, include_metronome=True, stream_ms=30000, repeats=1):
    selected_signals = tuple(signals) if signals else DEFAULT_SIGNALS
    cases = []
    profiles = static_profiles()
    for profile in profiles:
        for signal in selected_signals:
            cases.append(BenchmarkCase(
                case_id=f"{profile.name}_{signal}",
                profile=profile,
                signal=signal,
                repeats=repeats,
                stream_ms=stream_ms))
    directional_profiles = profiles[:2]
    if "tone-440" in selected_signals:
        for profile in directional_profiles:
            cases.append(BenchmarkCase(
                case_id=f"{profile.name}_tone-server-to-client",
                profile=profile,
                signal="tone-server-to-client",
                server_signal="tone-440",
                client_signal="silence",
                repeats=repeats,
                stream_ms=stream_ms))
            cases.append(BenchmarkCase(
                case_id=f"{profile.name}_tone-client-to-server",
                profile=profile,
                signal="tone-client-to-server",
                server_signal="silence",
                client_signal="tone-440",
                repeats=repeats,
                stream_ms=stream_ms))
    if include_metronome:
        for profile in metronome_profiles():
            for signal in METRONOME_SIGNALS:
                cases.append(BenchmarkCase(
                    case_id=f"{profile.name}_{signal}",
                    profile=profile,
                    signal=signal,
                    repeats=repeats,
                    stream_ms=stream_ms,
                    expect_metronome=True))
    cases.extend(wifi_diagnostic_cases(selected_signals, stream_ms=stream_ms, repeats=repeats))
    if "tone-440" in selected_signals:
        for os_priority in ("off", "high", "realtime"):
            cases.append(BenchmarkCase(
                case_id=f"fast_os_priority_{os_priority}_tone-440",
                profile=AGGRESSIVE_LOCAL_PROFILE,
                signal="tone-440",
                repeats=repeats,
                stream_ms=stream_ms,
                server_args=("--os-priority", os_priority),
                client_args=("--os-priority", os_priority)))
    return cases


def case_by_id(case_id, cases=None):
    for case in cases or benchmark_cases():
        if case.case_id == case_id:
            return case
    return None
