#!/usr/bin/env python3

from dataclasses import dataclass

from jam2_profiles import AGGRESSIVE_LOCAL_PROFILE, SAFE_LOCAL_PROFILE, Jam2Profile, adaptive_off_profile, variant


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
        variant(safe, "drift_off", drift_correction="off"),
        variant(safe, "drift_tight", drift_deadband_ppm=5, drift_smoothing=0.05),
        variant(safe, "socket_large", socket_send_buffer=1048576, socket_recv_buffer=1048576),
    ]


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
    return cases


def case_by_id(case_id, cases=None):
    for case in cases or benchmark_cases():
        if case.case_id == case_id:
            return case
    return None
