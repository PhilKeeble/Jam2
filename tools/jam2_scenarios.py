"""Reusable Jam2 stress scenario catalog and planning helpers."""

from jam2_profiles import (
    FAST_PROFILE,
    MODERATE_PROFILE,
    SAFE_PROFILE,
    adaptive_off_profile,
    jitter_buffer_profile,
    latency_matched_prefill_profile,
    variant,
)
from udp_stress_proxy import DirectionImpairment, ProxyImpairment


def selected_profile(profile_name):
    if profile_name == "fast":
        return FAST_PROFILE
    if profile_name == "moderate":
        return MODERATE_PROFILE
    if profile_name == "safe":
        return SAFE_PROFILE
    raise ValueError(f"unknown profile: {profile_name}")


def selected_os_priorities(values):
    if not values:
        return ["high"]
    if "all" in values:
        return ["off", "high", "realtime"]
    priorities = []
    for value in values:
        if value not in priorities:
            priorities.append(value)
    return priorities


def audio_probe_health_ok(analysis):
    return analysis.get("ok", False) and not analysis.get("tags", [])


def scenario_catalog(base_profile=FAST_PROFILE):
    return {
        "clean-control": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "no injected impairment",
        },
        "clean-fast-control": {
            "profile": FAST_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "fast localhost baseline with no injected impairment",
        },
        "clean-moderate-control": {
            "profile": MODERATE_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "moderate localhost baseline with no injected impairment",
        },
        "clean-safe-control": {
            "profile": SAFE_PROFILE,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "safe localhost baseline with no injected impairment",
        },
        "jitter-20": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "ordered jitter should raise jitter/RTT without proxy packet loss",
        },
        "jitter-50": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "expect": "high ordered jitter should show in jitter/RTT stats and pressure playback",
        },
        "jitter-100": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=100.0)),
            "expect": "extreme ordered jitter should pressure playback depth and adaptive cushion",
        },
        "burst-pause-250": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "short periodic stalls should recover without large lasting damage",
        },
        "burst-pause-500": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(burst_pause_ms=500.0, burst_every_ms=8000.0)),
            "expect": "medium stalls should create visible cushion/underrun pressure",
        },
        "burst-pause-1500": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(burst_pause_ms=1500.0, burst_every_ms=10000.0)),
            "expect": "long stalls should cause obvious underruns but recover afterwards",
        },
        "loss-0.1": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=0.1)),
            "expect": "low loss should appear in sequence loss with limited playback impact",
        },
        "loss-0.5": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=0.5)),
            "expect": "moderate loss should be measurable and may insert missing audio",
        },
        "loss-1.0": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0)),
            "expect": "high loss should create clear missing-frame diagnostics",
        },
        "reorder-small": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=8.0, reorder_percent=2.0, preserve_order=False)),
            "expect": "small reordering should show recovery/loss counters",
        },
        "duplicate-2.0": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(duplicate_percent=2.0)),
            "expect": "exact duplicate datagrams should be rejected and counted without disrupting the stream",
        },
        "corrupt-1.0": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(corrupt_percent=1.0)),
            "expect": "bit-corrupted datagrams should fail fixed framing or authentication and be counted by reason",
        },
        "near-wrap-sequence": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "udp_validation": "near-wrap-sequence",
            "expect": "audio sequence handling should cross uint32 wrap without false loss or a stalled reorder buffer",
        },
        "malformed-udp": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "udp_validation": "malformed",
            "expect": "malformed fixed-header variants should be rejected by reason while both real processes remain streaming",
        },
        "delayed-replay": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "udp_validation": "delayed-replay",
            "expect": "replayed authenticated audio datagrams should be rejected without disrupting fresh traffic",
        },
        "forward-sequence-gap": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "udp_validation": "forward-sequence-gap",
            "expect": "a one-shot excessive authenticated sequence jump should be rejected with bounded work and fresh traffic should continue",
        },
        "extreme-sample-time": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "udp_validation": "extreme-sample-time",
            "expect": "an authenticated uint64-edge audio sample time should be rejected before reorder or playout storage",
        },
        "udp-short-flood": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "udp_validation": "short-flood",
            "expect": "a short-datagram flood should remain bounded by the per-wake work budget while normal audio continues",
        },
        "adaptive-on-pressure": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "comparison run for adaptive cushion enabled under pressure",
        },
        "adaptive-off-pressure": {
            "profile": adaptive_off_profile(base_profile),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "comparison run for adaptive cushion disabled under pressure",
        },
        "jitter-buffer-512-pressure": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "jitter buffer uses most of the latency budget while adaptive cushion remains available under pressure",
        },
        "jitter-buffer-512-max3072-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
                "max_3072",
                jitter_buffer_max_frames=3072),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "same jitter target as the failing pressure case with larger jitter-buffer headroom",
        },
        "jitter-buffer-1024-max3072-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=1024, playback_tail_frames=256, adaptive=True),
                "max_3072",
                jitter_buffer_max_frames=3072),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "larger jitter target and max under the same pressure case",
        },
        "jitter-buffer-2048-max3072-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=2048, playback_tail_frames=256, adaptive=True),
                "max_3072",
                jitter_buffer_max_frames=3072),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "safe-style jitter target under the same pressure case with aggressive playback tail",
        },
        "jitter-buffer-2048-max4096-pressure": {
            "profile": variant(
                jitter_buffer_profile(base_profile, jitter_frames=2048, playback_tail_frames=256, adaptive=True),
                "max_4096",
                jitter_buffer_max_frames=4096),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "safe-style jitter target with enough jitter-buffer headroom for observed pressure depth",
        },
        "prefill-768-pressure": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "latency-matched prefill comparison for jitter-buffer-512-pressure",
        },
        "jitter-buffer-512-adaptive-off-pressure": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=False),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "fixed jitter buffer without adaptive cushion under pressure",
        },
        "prefill-768-adaptive-off-pressure": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=False),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "expect": "fixed prefill without adaptive cushion under pressure",
        },
        "jitter-buffer-1024-jitter-100": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=1024, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=100.0)),
            "expect": "larger jitter buffer should reduce underrun pressure under extreme ordered jitter at the cost of latency",
        },
        "jitter-buffer-512-reorder-small": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=8.0, reorder_percent=2.0, preserve_order=False)),
            "expect": "jitter buffer should work with the reorder path and expose queued/released counters",
        },
        "metronome-shared-grid": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "metronome packets should be exchanged and final alignment should stay valid",
        },
        "metronome-leader-audio": {
            "profile": variant(base_profile, "metro_leader_audio", metronome_mode="leader-audio"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "leader-audio metronome mode should run and exchange metronome state",
        },
        "metronome-listener-compensated": {
            "profile": variant(base_profile, "metro_listener_compensated", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "listener-compensated metronome mode should keep the shared epoch valid while applying local render compensation",
        },
        "metronome-listener-compensated-metro-pulse": {
            "profile": variant(base_profile, "metro_listener_compensated_metro_pulse", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "metro-pulse",
            "server_signal": "metro-pulse",
            "client_signal": "metro-pulse",
            "expect": "listener-compensated metronome should align local compensated clicks to incoming peer pulses generated from the peer metronome epoch",
        },
        "metronome-listener-compensated-pulse-jitter": {
            "profile": variant(base_profile, "metro_listener_compensated_pulse", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "listener-compensated-pulse",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "listener-compensated metronome should track incoming peer pulse timing under ordered jitter",
        },
        "metronome-listener-compensated-pulse-burst": {
            "profile": variant(base_profile, "metro_listener_compensated_pulse_burst", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "listener-compensated-pulse",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "listener-compensated metronome should expose pulse/click tracking under burst pressure",
        },
        "metronome-listener-compensated-pulse-loss": {
            "profile": variant(base_profile, "metro_listener_compensated_pulse_loss", metronome_mode="listener-compensated"),
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0, jitter_ms=20.0)),
            "signal": "listener-compensated-pulse",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "listener-compensated metronome should report pulse/click tracking when packet loss creates missing remote pulses",
        },
        "levels-low": {
            "profile": variant(base_profile, "levels_low", metronome_level=0.05, remote_level=0.50),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "startup metronome and remote levels should be reflected in final CSV state",
        },
        "sample-time-playout-off": {
            "profile": variant(base_profile, "sample_time_off", sample_time_playout="off"),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "non-sample-time playout mode should still stream cleanly and report the requested mode",
        },
        "playout-delay-3072": {
            "profile": variant(
                base_profile,
                "playout_delay_3072",
                playback_max_frames=4096,
                playout_delay_frames=3072,
                adaptive_playback_target_frames=3072,
                adaptive_playback_min_frames=3072,
                adaptive_playback_max_frames=max(4096, base_profile.adaptive_playback_max_frames)),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "expect": "larger playout delay should be applied and visible in depth/latency metrics",
        },
        "drift-max-5ppm": {
            "profile": variant(base_profile, "drift_max_5ppm", drift_deadband_ppm=0, drift_max_correction_ppm=5),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "drift correction should respect a very low max correction cap when real device drift exceeds it",
        },
        "drift-smoothing-fast": {
            "profile": variant(base_profile, "drift_smoothing_fast", drift_smoothing=1.0, drift_deadband_ppm=0),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "fast drift smoothing should run and report drift/resampler data",
        },
        "socket-buffers": {
            "profile": variant(base_profile, "socket_buffers", socket_send_buffer=1048576, socket_recv_buffer=1048576),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "expect": "requested socket buffers should be applied or reported with the OS-adjusted actual values",
        },
        "channels-1-to-1": {
            "profile": variant(base_profile, "channels_1_to_1", input_channels="1", output_channels="1"),
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "expect": "explicit channel selection should run and be visible in CSV metadata",
        },
        "runtime-controls": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=20.0)),
            "commands": [
                {"at_s": 3.0, "side": "server", "line": "metro off"},
                {"at_s": 5.0, "side": "server", "line": "metro on"},
                {"at_s": 7.0, "side": "server", "line": "metro mode listener-compensated"},
                {"at_s": 9.0, "side": "client", "line": "metro level 0.10"},
                {"at_s": 11.0, "side": "client", "line": "remote level 0.75"},
                {"at_s": 13.0, "side": "server", "line": "stats"},
            ],
            "expect": "runtime command path should update final levels/mode while audio continues",
        },
    }


def standard_suite():
    return [
        "clean-control",
        "jitter-20",
        "jitter-50",
        "jitter-100",
        "burst-pause-250",
        "burst-pause-500",
        "burst-pause-1500",
        "loss-0.1",
        "loss-0.5",
        "loss-1.0",
        "reorder-small",
        "duplicate-2.0",
        "corrupt-1.0",
        "near-wrap-sequence",
        "malformed-udp",
        "delayed-replay",
        "forward-sequence-gap",
        "extreme-sample-time",
        "udp-short-flood",
        "adaptive-on-pressure",
        "adaptive-off-pressure",
        "jitter-buffer-512-pressure",
        "jitter-buffer-512-max3072-pressure",
        "jitter-buffer-1024-max3072-pressure",
        "jitter-buffer-2048-max3072-pressure",
        "jitter-buffer-2048-max4096-pressure",
        "prefill-768-pressure",
        "jitter-buffer-512-adaptive-off-pressure",
        "prefill-768-adaptive-off-pressure",
        "jitter-buffer-1024-jitter-100",
        "jitter-buffer-512-reorder-small",
        "metronome-shared-grid",
        "metronome-leader-audio",
        "metronome-listener-compensated",
        "metronome-listener-compensated-metro-pulse",
        "metronome-listener-compensated-pulse-jitter",
        "metronome-listener-compensated-pulse-burst",
        "metronome-listener-compensated-pulse-loss",
        "levels-low",
        "sample-time-playout-off",
        "playout-delay-3072",
        "drift-max-5ppm",
        "drift-smoothing-fast",
        "socket-buffers",
        "channels-1-to-1",
        "runtime-controls",
    ]


def audio_probe_suite(base_profile=FAST_PROFILE):
    return {
        "audio-probe-clean-tone": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment()),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "symmetric tone should record cleanly without injected impairment",
        },
        "audio-probe-jitter-tone": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "symmetric tone should expose audible artifacts under high ordered jitter",
        },
        "audio-probe-loss-server-to-client": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0)),
            "signal": "tone-server-to-client",
            "server_signal": "tone-440",
            "client_signal": "silence",
            "expect": "server-to-client tone should expose directional loss artifacts",
        },
        "audio-probe-loss-client-to-server": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(loss_percent=1.0)),
            "signal": "tone-client-to-server",
            "server_signal": "silence",
            "client_signal": "tone-440",
            "expect": "client-to-server tone should expose directional loss artifacts",
        },
        "audio-probe-adaptive-on-pulse": {
            "profile": base_profile,
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "pulse-1s",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "pulse recording should show whether adaptive cushion masks burst pressure",
        },
        "audio-probe-adaptive-off-pulse": {
            "profile": adaptive_off_profile(base_profile),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "pulse-1s",
            "server_signal": "pulse-1s",
            "client_signal": "pulse-1s",
            "expect": "pulse recording should expose the same burst pressure with adaptive cushion disabled",
        },
        "audio-probe-jitter-buffer-512-tone": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "jitter-buffer tone recording should expose audible artifacts and jitter-buffer stats under high ordered jitter",
        },
        "audio-probe-prefill-768-tone": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0)),
            "signal": "tone-440",
            "server_signal": "tone-440",
            "client_signal": "tone-440",
            "expect": "latency-matched prefill tone recording comparison for jitter-buffer tone analysis",
        },
        "audio-probe-jitter-buffer-512-metronome": {
            "profile": jitter_buffer_profile(base_profile, jitter_frames=512, playback_tail_frames=256, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "metronome-only",
            "server_signal": "silence",
            "client_signal": "silence",
            "expect": "jitter-buffer metronome recording should expose click timing error under Wi-Fi-like pressure",
        },
        "audio-probe-prefill-768-metronome": {
            "profile": latency_matched_prefill_profile(base_profile, total_frames=768, adaptive=True),
            "impairment": ProxyImpairment.both(DirectionImpairment(jitter_ms=50.0, burst_pause_ms=250.0, burst_every_ms=8000.0)),
            "signal": "metronome-only",
            "server_signal": "silence",
            "client_signal": "silence",
            "expect": "latency-matched prefill metronome recording comparison for jitter-buffer timing analysis",
        },
    }


def expand_scenarios(scenario_ids):
    expanded = []
    for scenario_id in scenario_ids:
        if scenario_id == "adaptive-off-vs-on":
            expanded.extend(["adaptive-on-pressure", "adaptive-off-pressure"])
        elif scenario_id == "burst-loss":
            expanded.extend(["burst-pause-500", "loss-1.0"])
        elif scenario_id == "jitter-buffer-adaptive-pressure-matrix":
            expanded.extend([
                "jitter-buffer-512-pressure",
                "jitter-buffer-512-max3072-pressure",
                "jitter-buffer-1024-max3072-pressure",
                "jitter-buffer-2048-max3072-pressure",
                "jitter-buffer-2048-max4096-pressure",
                "jitter-buffer-512-adaptive-off-pressure",
                "prefill-768-pressure",
            ])
        elif scenario_id == "jitter-buffer-audio-vs-prefill":
            expanded.extend([
                "audio-probe-jitter-buffer-512-tone",
                "audio-probe-prefill-768-tone",
                "audio-probe-jitter-buffer-512-metronome",
                "audio-probe-prefill-768-metronome",
            ])
        else:
            expanded.append(scenario_id)
    return expanded


def scenario_plan(profile_mode, requested_scenarios, include_audio_probes=False):
    base_ids = expand_scenarios(requested_scenarios) if requested_scenarios else standard_suite()
    if profile_mode != "all":
        base_profile = selected_profile(profile_mode)
        catalog = scenario_catalog(base_profile)
        needs_audio_probes = include_audio_probes or any(scenario_id.startswith("audio-probe-") for scenario_id in base_ids)
        if needs_audio_probes:
            probes = audio_probe_suite(base_profile)
            catalog.update(probes)
            if include_audio_probes:
                base_ids = base_ids + list(probes.keys())
        return catalog, base_ids

    planned_catalog = {}
    planned_ids = []
    for profile_name in ("fast", "moderate", "safe"):
        base_profile = selected_profile(profile_name)
        catalog = scenario_catalog(base_profile)
        profile_ids = list(base_ids)
        needs_audio_probes = include_audio_probes or any(scenario_id.startswith("audio-probe-") for scenario_id in profile_ids)
        if needs_audio_probes:
            probes = audio_probe_suite(base_profile)
            catalog.update(probes)
            if include_audio_probes:
                profile_ids.extend(probes.keys())
        for scenario_id in profile_ids:
            planned_id = f"{profile_name}__{scenario_id}"
            scenario = dict(catalog[scenario_id])
            scenario["source_scenario"] = scenario_id
            scenario["profile_family"] = profile_name
            planned_catalog[planned_id] = scenario
            planned_ids.append(planned_id)
    return planned_catalog, planned_ids


def mesh_peer_counts(args_ns):
    counts = args_ns.mesh_peers or [2, 3, 4, 8]
    out = []
    for count in counts:
        if count < 2:
            raise ValueError("--mesh-peers must be at least 2")
        if count not in out:
            out.append(count)
    return out


def mesh_scenario_catalog(base_profile, counts):
    return {
        f"mesh-{count}-clean": {
            "profile": base_profile,
            "mesh_peers": count,
            "signal": "tone-440",
            "expect": f"headless full mesh with {count} peers and no injected network impairment",
        }
        for count in counts
    }


def mesh_scenario_plan(profile_mode, requested_scenarios, counts):
    base_ids = requested_scenarios if requested_scenarios else [f"mesh-{count}-clean" for count in counts]
    if profile_mode != "all":
        catalog = mesh_scenario_catalog(selected_profile(profile_mode), counts)
        return catalog, base_ids

    planned_catalog = {}
    planned_ids = []
    for profile_name in ("fast", "moderate", "safe"):
        catalog = mesh_scenario_catalog(selected_profile(profile_name), counts)
        for scenario_id in base_ids:
            if scenario_id not in catalog:
                planned_ids.append(f"{profile_name}__{scenario_id}")
                continue
            planned_id = f"{profile_name}__{scenario_id}"
            scenario = dict(catalog[scenario_id])
            scenario["source_scenario"] = scenario_id
            scenario["profile_family"] = profile_name
            planned_catalog[planned_id] = scenario
            planned_ids.append(planned_id)
    return planned_catalog, planned_ids



