from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum


class GeneratedIdeaSync(str, Enum):
    FULL = "full"
    CHORDS = "chords"
    BEATS = "beats"
    OFF = "off"


@dataclass(frozen=True)
class JamSyncPolicy:
    track_lanes: bool = True
    auto_share_wavs: bool = True
    global_playback: bool = True
    generated_ideas: GeneratedIdeaSync = GeneratedIdeaSync.FULL
    metronome_state: bool = False
    recordings: bool = True

    def normalized(self) -> "JamSyncPolicy":
        return replace(
            self,
            recordings=self.recordings and self.track_lanes and self.global_playback,
        )

    def allows(self, operation: str) -> bool:
        policy = self.normalized()
        if operation == "track-lane":
            return policy.track_lanes
        if operation == "wav-auto":
            return policy.track_lanes and policy.auto_share_wavs
        if operation == "wav-manual":
            return True
        if operation == "playback":
            return policy.global_playback
        if operation == "recording":
            return policy.recordings
        if operation == "metronome-state":
            return policy.metronome_state
        if operation == "idea-full":
            return policy.generated_ideas is GeneratedIdeaSync.FULL
        if operation == "idea-chords":
            return policy.generated_ideas in (
                GeneratedIdeaSync.FULL, GeneratedIdeaSync.CHORDS)
        if operation == "idea-beats":
            return policy.generated_ideas in (
                GeneratedIdeaSync.FULL, GeneratedIdeaSync.BEATS)
        raise ValueError(f"unknown Jam Sync operation: {operation}")

    def control_message(self, message_type: str, revision: int | None = None) -> dict[str, object]:
        message: dict[str, object] = {
            "type": message_type,
            "track_lanes": self.track_lanes,
            "auto_share_wavs": self.auto_share_wavs,
            "global_playback": self.global_playback,
            "generated_ideas": self.generated_ideas.value,
            "metronome_state": self.metronome_state,
            "recordings": self.normalized().recordings,
        }
        if revision is not None:
            message["revision"] = revision
        return message


@dataclass
class PeerJamSyncState:
    policy: JamSyncPolicy = JamSyncPolicy()
    revision: int = -1

    def adopt(self, policy: JamSyncPolicy, revision: int) -> bool:
        if revision <= self.revision:
            return False
        self.policy = policy.normalized()
        self.revision = revision
        return True

    def local_metronome_enabled(
        self, shared_enabled: bool, *, leader_audio: bool, creator: bool
    ) -> bool:
        if leader_audio and not creator:
            return False
        return shared_enabled
