#!/usr/bin/env python3

from dataclasses import dataclass

from .profiles import (
    FAST_PROFILE,
    MODERATE_PROFILE,
    SAFE_PROFILE,
    Jam2Profile,
    jitter_buffer_profile,
    latency_matched_prefill_profile,
    variant,
)


CATALOG_VERSION = 1
CASE_STREAM_MS = 30_000
SUITES = ("core", "full")


@dataclass(frozen=True)
class BenchmarkCase:
    case_id: str
    profile: Jam2Profile
    signal: str
    category: str
    purpose: str
    comparator: str = "control-fast-tone-pcm24"
    coordinator_signal: str = ""
    agent_signal: str = ""
    stream_ms: int = CASE_STREAM_MS
    expect_metronome: bool = False
    network_audio_format: str = "pcm24"
    interpretation: str = "network-and-audio"
    repeats: int = 1

    def __post_init__(self):
        if not self.coordinator_signal:
            object.__setattr__(self, "coordinator_signal", self.signal)
        if not self.agent_signal:
            object.__setattr__(self, "agent_signal", self.signal)

    def metadata(self):
        return {
            "catalog_version": CATALOG_VERSION,
            "case_id": self.case_id,
            "category": self.category,
            "purpose": self.purpose,
            "comparator": self.comparator,
            "signal": self.signal,
            "coordinator_signal": self.coordinator_signal,
            "agent_signal": self.agent_signal,
            "primary_runs": 1,
            "stream_ms": self.stream_ms,
            "expect_metronome": self.expect_metronome,
            "network_audio_format": self.network_audio_format,
            "interpretation": self.interpretation,
            "profile": self.profile.metadata(),
        }


def _case(
    case_id: str,
    profile: Jam2Profile,
    signal: str,
    category: str,
    purpose: str,
    **changes,
) -> BenchmarkCase:
    return BenchmarkCase(
        case_id=case_id,
        profile=profile,
        signal=signal,
        category=category,
        purpose=purpose,
        **changes,
    )


def _core_cases() -> list[BenchmarkCase]:
    prefill_on = latency_matched_prefill_profile(
        FAST_PROFILE, total_frames=768, adaptive=True)
    shared_grid = variant(
        MODERATE_PROFILE, "metronome_shared_grid",
        metronome=True, bpm=120, metronome_level=0.20,
        metronome_mode="shared-grid")
    return [
        _case(
            "control-fast-tone-pcm24", FAST_PROFILE, "tone-440", "control",
            "Fast bidirectional PCM24 control used by tuning comparisons.",
            comparator="control-fast-tone-pcm24"),
        _case(
            "signal-fast-pulse-pcm24", FAST_PROFILE, "pulse-1s", "signal",
            "Transient continuity, dropout, missing-audio, and timing reference."),
        _case(
            "profile-moderate-tone-pcm24", MODERATE_PROFILE, "tone-440", "profile",
            "Integrated Moderate latency and resilience tradeoff."),
        _case(
            "profile-safe-tone-pcm24", SAFE_PROFILE, "tone-440", "profile",
            "Integrated Safe latency and resilience tradeoff."),
        _case(
            "adaptive-fast-off-tone-pcm24",
            variant(FAST_PROFILE, "adaptive_off", adaptive_playback_cushion=False),
            "tone-440", "adaptive-buffering",
            "Isolates the contribution of adaptive playback cushioning."),
        _case(
            "buffer-fast-prefill-only-768-adaptive-on-tone-pcm24",
            prefill_on, "tone-440", "buffering",
            "Latency-matched prefill-only strategy versus Fast jitter buffering."),
        _case(
            "frame-fast-128-tone-pcm24",
            variant(FAST_PROFILE, "frame_128", frame_size=128),
            "tone-440", "packet-sizing",
            "128-frame packet interval, bitrate, CPU, and loss sensitivity."),
        _case(
            "format-fast-tone-pcm16", FAST_PROFILE, "tone-440", "wire-format",
            "Matched PCM16 versus PCM24 bandwidth, CPU, and audio measurements.",
            network_audio_format="pcm16"),
        _case(
            "metronome-moderate-shared-grid-pcm24", shared_grid,
            "metronome-only", "metronome",
            "Shared-grid alignment and transport reference.",
            comparator="metronome-moderate-shared-grid-pcm24",
            expect_metronome=True),
    ]


def _full_additions() -> list[BenchmarkCase]:
    prefill_off = latency_matched_prefill_profile(
        FAST_PROFILE, total_frames=768, adaptive=False)
    jitter_large = jitter_buffer_profile(
        FAST_PROFILE, jitter_frames=2048,
        playback_tail_frames=256, adaptive=True)
    leader_audio = variant(
        MODERATE_PROFILE, "metronome_leader_audio",
        metronome=True, bpm=120, metronome_level=0.20,
        metronome_mode="leader-audio")
    listener = variant(
        MODERATE_PROFILE, "metronome_listener_compensated",
        metronome=True, bpm=120, metronome_level=0.20,
        metronome_mode="listener-compensated")
    return [
        _case(
            "signal-fast-silence-pcm24", FAST_PROFILE, "silence", "signal",
            "Unexpected signal, noise, clipping, and recording integrity."),
        _case(
            "direction-coordinator-to-agent-tone-pcm24", FAST_PROFILE,
            "tone-coordinator-to-agent", "direction",
            "Coordinator-to-agent audio integrity and asymmetric behavior.",
            coordinator_signal="tone-440", agent_signal="silence"),
        _case(
            "direction-agent-to-coordinator-tone-pcm24", FAST_PROFILE,
            "tone-agent-to-coordinator", "direction",
            "Agent-to-coordinator audio integrity and asymmetric behavior.",
            coordinator_signal="silence", agent_signal="tone-440"),
        _case(
            "frame-fast-32-tone-pcm24",
            variant(FAST_PROFILE, "frame_32", frame_size=32),
            "tone-440", "packet-sizing",
            "32-frame packet latency versus packet-rate, CPU, and loss cost."),
        _case(
            "frame-fast-256-tone-pcm24",
            variant(FAST_PROFILE, "frame_256", frame_size=256),
            "tone-440", "packet-sizing",
            "256-frame packet efficiency versus packetization latency."),
        _case(
            "callback-fast-64-frame-64-tone-pcm24",
            variant(FAST_PROFILE, "callback_64", audio_buffer_size=64),
            "tone-440", "callback-sizing",
            "64-frame device or synthetic callback against Fast's 32 frames.",
            interpretation="synthetic-callback-or-physical-device"),
        _case(
            "callback-fast-128-frame-64-tone-pcm24",
            variant(FAST_PROFILE, "callback_128", audio_buffer_size=128),
            "tone-440", "callback-sizing",
            "128-frame device or synthetic callback against Fast's 32 frames.",
            interpretation="synthetic-callback-or-physical-device"),
        _case(
            "buffer-fast-prefill-only-768-adaptive-off-tone-pcm24",
            prefill_off, "tone-440", "buffering",
            "Adaptive-on versus fixed latency-matched prefill-only buffering.",
            comparator="buffer-fast-prefill-only-768-adaptive-on-tone-pcm24"),
        _case(
            "buffer-fast-jitter-2048-tail-256-adaptive-on-tone-pcm24",
            jitter_large, "tone-440", "buffering",
            "Larger jitter absorption while retaining Fast packet sizing."),
        _case(
            "sample-time-fast-off-tone-pcm24",
            variant(FAST_PROFILE, "sample_time_off", sample_time_playout=False),
            "tone-440", "sample-time",
            "Sample-time playout contribution to delay accuracy and late handling."),
        _case(
            "drift-correction-fast-off-tone-pcm24",
            variant(FAST_PROFILE, "drift_off", drift_correction=False),
            "tone-440", "drift",
            "Drift correction contribution; physical-device evidence is preferred.",
            interpretation="physical-preferred"),
        _case(
            "socket-buffer-fast-1m-tone-pcm24",
            variant(
                FAST_PROFILE, "socket_1m",
                socket_send_buffer=1_048_576,
                socket_recv_buffer=1_048_576),
            "tone-440", "socket-buffering",
            "Large socket buffers versus default packet loss and work budgeting."),
        _case(
            "os-priority-fast-off-tone-pcm24",
            variant(FAST_PROFILE, "os_priority_off", os_priority="off"),
            "tone-440", "scheduling",
            "Scheduling without Jam2 priority elevation.",
            interpretation="synthetic-scheduler-or-physical-device"),
        _case(
            "os-priority-fast-realtime-tone-pcm24",
            variant(FAST_PROFILE, "os_priority_realtime", os_priority="realtime"),
            "tone-440", "scheduling",
            "Realtime scheduling request versus the default high priority.",
            interpretation="synthetic-scheduler-or-physical-device"),
        _case(
            "metronome-moderate-leader-audio-pcm24", leader_audio,
            "metronome-only", "metronome",
            "Leader-audio source selection, delivery, and click integrity.",
            comparator="metronome-moderate-shared-grid-pcm24",
            expect_metronome=True),
        _case(
            "metronome-moderate-listener-compensated-pcm24", listener,
            "metronome-only", "metronome",
            "Listener compensation target, convergence, and alignment.",
            comparator="metronome-moderate-shared-grid-pcm24",
            expect_metronome=True),
    ]


def benchmark_cases(suite: str = "core") -> list[BenchmarkCase]:
    if suite not in SUITES:
        raise ValueError("benchmark suite must be core or full")
    cases = _core_cases()
    if suite == "full":
        cases.extend(_full_additions())
    return cases


def case_by_id(case_id: str, suite: str = "full") -> BenchmarkCase | None:
    return next(
        (case for case in benchmark_cases(suite) if case.case_id == case_id),
        None,
    )
