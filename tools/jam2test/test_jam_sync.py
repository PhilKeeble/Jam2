import unittest
from dataclasses import replace

from jam2test.jam_sync import GeneratedIdeaSync, JamSyncPolicy, PeerJamSyncState


class JamSyncPolicyTests(unittest.TestCase):
    def test_defaults_preserve_existing_sharing_but_leave_metronome_local(self):
        policy = JamSyncPolicy()
        self.assertEqual(
            (
                policy.track_lanes,
                policy.auto_share_wavs,
                policy.generated_ideas,
                policy.global_playback,
                policy.metronome_state,
                policy.recordings,
            ),
            (True, True, GeneratedIdeaSync.FULL, True, False, True),
        )
        self.assertTrue(policy.allows("track-lane"))
        self.assertTrue(policy.allows("wav-auto"))
        self.assertTrue(policy.allows("playback"))
        self.assertTrue(policy.allows("recording"))
        self.assertTrue(policy.allows("idea-full"))
        self.assertFalse(policy.allows("metronome-state"))

    def test_recording_sync_requires_both_lane_and_playback_sync(self):
        for lanes, playback, expected in (
            (True, True, True),
            (False, True, False),
            (True, False, False),
            (False, False, False),
        ):
            with self.subTest(lanes=lanes, playback=playback):
                policy = JamSyncPolicy(track_lanes=lanes, global_playback=playback)
                self.assertEqual(policy.normalized().recordings, expected)
                self.assertEqual(policy.allows("recording"), expected)

    def test_manual_wav_share_survives_every_automatic_setting(self):
        for lanes in (False, True):
            for automatic in (False, True):
                with self.subTest(lanes=lanes, automatic=automatic):
                    policy = JamSyncPolicy(
                        track_lanes=lanes, auto_share_wavs=automatic)
                    self.assertTrue(policy.allows("wav-manual"))
                    self.assertEqual(
                        policy.allows("wav-auto"), lanes and automatic)

    def test_fully_local_policy_suppresses_every_automatic_outbound_route(self):
        policy = JamSyncPolicy(
            track_lanes=False,
            auto_share_wavs=False,
            global_playback=False,
            generated_ideas=GeneratedIdeaSync.OFF,
            metronome_state=False,
            recordings=False,
        )
        for operation in (
            "track-lane",
            "wav-auto",
            "playback",
            "recording",
            "metronome-state",
            "idea-full",
            "idea-chords",
            "idea-beats",
        ):
            with self.subTest(operation=operation):
                self.assertFalse(policy.allows(operation))
        self.assertTrue(policy.allows("wav-manual"))

    def test_generated_idea_modes_route_only_the_selected_content(self):
        expected = {
            GeneratedIdeaSync.FULL: (True, True, True),
            GeneratedIdeaSync.CHORDS: (False, True, False),
            GeneratedIdeaSync.BEATS: (False, False, True),
            GeneratedIdeaSync.OFF: (False, False, False),
        }
        for mode, routes in expected.items():
            with self.subTest(mode=mode):
                policy = JamSyncPolicy(generated_ideas=mode)
                self.assertEqual(
                    (policy.allows("idea-full"),
                     policy.allows("idea-chords"),
                     policy.allows("idea-beats")),
                    routes,
                )

    def test_all_peers_adopt_one_authoritative_policy_and_reject_stale_updates(self):
        creator_policy = JamSyncPolicy(
            auto_share_wavs=False,
            generated_ideas=GeneratedIdeaSync.CHORDS,
            metronome_state=True,
        )
        peers = [PeerJamSyncState() for _ in range(4)]
        self.assertTrue(all(peer.adopt(creator_policy, 7) for peer in peers))
        self.assertTrue(all(peer.policy == creator_policy.normalized() for peer in peers))
        stale = replace(creator_policy, global_playback=False)
        self.assertTrue(all(not peer.adopt(stale, 6) for peer in peers))
        self.assertTrue(all(peer.policy.global_playback for peer in peers))

    def test_leader_audio_never_enables_a_follower_local_click(self):
        creator = PeerJamSyncState()
        follower = PeerJamSyncState()
        self.assertTrue(creator.local_metronome_enabled(
            True, leader_audio=True, creator=True))
        self.assertFalse(follower.local_metronome_enabled(
            True, leader_audio=True, creator=False))
        self.assertTrue(follower.local_metronome_enabled(
            True, leader_audio=False, creator=False))

    def test_control_message_normalizes_invalid_recording_combination(self):
        policy = JamSyncPolicy(track_lanes=False, recordings=True)
        message = policy.control_message("jam.sync.set", revision=3)
        self.assertEqual(message["type"], "jam.sync.set")
        self.assertEqual(message["revision"], 3)
        self.assertFalse(message["recordings"])


if __name__ == "__main__":
    unittest.main()
