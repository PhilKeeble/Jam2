#!/usr/bin/env python3
"""Focused validation for complete Style Mixer handoff persistence."""

from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
import urllib.request
from pathlib import Path
from types import SimpleNamespace


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from workbench_server import DRUM_PIECES, WorkbenchHandler, WorkbenchServer  # noqa: E402


def drum_role() -> dict:
    return {
        "role": "drums",
        "roleName": "Drums",
        "enabled": True,
        "source": "designed",
        "parameterType": "drum-kit",
        "selection": {
            "pieces": {
                piece: {"enabled": True, "source": "selected-kit"}
                for piece in DRUM_PIECES
            }
        },
        "enabledLayers": {piece: {} for piece in DRUM_PIECES},
        "parameters": {"bus": {}, "pieces": {piece: {} for piece in DRUM_PIECES}},
    }


def complete_request() -> dict:
    return {
        "schema": "jam2-style-mix-handoff-v1",
        "profileId": "test-style",
        "styleId": "test",
        "mix": {"masterGain": 0.42, "roles": {"drums": {
            "enabled": True,
            "gain": 1.16,
            "baseGain": 1.0,
            "trimDb": 1.29,
        }}},
        "roles": [drum_role()],
    }


class StyleMixSaveTest(unittest.TestCase):
    def handler(self, experiment: Path):
        handler = WorkbenchHandler.__new__(WorkbenchHandler)
        responses = []
        handler.server = SimpleNamespace(
            renderer=experiment / "build" / "jam2_sound_lab.exe",
            preset_lock=threading.Lock(),
            style_mix_file=experiment / "presets" / "style-mixes.json",
        )
        handler.send_json = lambda status, value: responses.append((status, value))
        return handler, responses

    def test_complete_kit_is_written_as_self_contained_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary:
            experiment = Path(temporary) / "synth-ab"
            handler, responses = self.handler(experiment)
            request = complete_request()
            handler.save_style_mix(request)
            self.assertEqual(200, responses[-1][0])
            document = json.loads(
                (experiment / "presets" / "style-mixes.json").read_text(encoding="utf-8")
            )
            saved = document["styleMixes"]["test-style"]
            self.assertEqual(DRUM_PIECES, set(saved["roles"][0]["parameters"]["pieces"]))
            self.assertTrue(saved["roles"][0]["selection"]["pieces"]["kick"]["enabled"])
            self.assertEqual(1.16, saved["mix"]["roles"]["drums"]["gain"])
            self.assertEqual(1.29, saved["mix"]["roles"]["drums"]["trimDb"])
            self.assertEqual(
                str(experiment / "presets" / "style-mixes.json"),
                responses[-1][1]["absolutePath"],
            )

    def test_incomplete_kit_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            experiment = Path(temporary) / "synth-ab"
            handler, responses = self.handler(experiment)
            request = complete_request()
            del request["roles"][0]["parameters"]["pieces"]["kick"]
            handler.save_style_mix(request)
            self.assertEqual(400, responses[-1][0])
            self.assertFalse((experiment / "presets" / "style-mixes.json").exists())

    def test_http_style_mix_endpoint_routes_and_returns_saved_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            experiment = Path(temporary) / "synth-ab"
            site = experiment / "site"
            renderer = experiment / "build" / "jam2_sound_lab.exe"
            qt_bin = experiment / "qt"
            site.mkdir(parents=True)
            renderer.parent.mkdir(parents=True)
            renderer.touch()
            qt_bin.mkdir(parents=True)
            (qt_bin / "Qt6Core.dll").touch()
            handler = lambda *args, **kwargs: WorkbenchHandler(
                *args, directory=site, **kwargs
            )
            server = WorkbenchServer(
                ("127.0.0.1", 0), handler, site, renderer, qt_bin
            )
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                port = server.server_port
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{port}/api/status", timeout=3
                ) as response:
                    status = json.loads(response.read())
                self.assertIn("style-mix-save", status["capabilities"])
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/api/style-mix",
                    data=json.dumps(complete_request()).encode("utf-8"),
                    headers={
                        "Content-Type": "application/json",
                        "Origin": f"http://127.0.0.1:{port}",
                    },
                    method="POST",
                )
                with urllib.request.urlopen(request, timeout=3) as response:
                    result = json.loads(response.read())
                self.assertTrue(result["ok"])
                self.assertEqual(
                    "presets/style-mixes.json",
                    result["path"].replace("\\", "/"),
                )
                self.assertTrue(Path(result["absolutePath"]).is_file())
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=3)


if __name__ == "__main__":
    unittest.main()
