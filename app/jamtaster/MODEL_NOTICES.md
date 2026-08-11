# JamTaster third-party and model notices

JamTaster bundles a native ONNX pipeline. Python conversion sources under
`tools/models` are development tools and are not part of the application
runtime or release package.

| Component | Pinned version/revision | Licence | Use |
| --- | --- | --- | --- |
| ONNX Runtime | 1.23.2 | MIT | Native CPU inference runtime |
| sevagh/demucs.onnx | `81fa192e6fcc88e35e887f6e6ccce91227f4e6f5` | MIT | Adapted HTDemucs ONNX inference and native STFT code |
| facebookresearch/demucs | checkpoint/conversion source | MIT | HTDemucs FT model definitions and four source checkpoints |
| Eigen | 3.4.0 | MPL-2.0 | Tensor/matrix support in the Demucs adapter |
| mosynthkey/beat_this_cpp | `07ab790a9ec2eda8093d52d249e3ec4f0510ee72` | MIT | Native preprocessing parity reference |
| CPJKU/beat_this | `final0` | MIT code and published weights | Beat/downbeat model |
| Spotify Basic Pitch | 0.4.0 | Apache-2.0 | Bass-note ONNX model |
| ptnghia-j/ChordMini | pinned converted checkpoint | MIT repository | Chord model |
| ADTOF-pytorch / original ADTOF | pinned converted checkpoint | Redistribution unresolved; original material is CC BY-NC-SA 4.0 | Drum transcription model |

Project sources:

- <https://github.com/microsoft/onnxruntime>
- <https://github.com/sevagh/demucs.onnx>
- <https://github.com/facebookresearch/demucs>
- <https://gitlab.com/libeigen/eigen>
- <https://github.com/mosynthkey/beat_this_cpp>
- <https://github.com/CPJKU/beat_this>
- <https://github.com/spotify/basic-pitch>
- <https://github.com/ptnghia-j/ChordMini>
- <https://github.com/xavriley/ADTOF-pytorch>
- <https://github.com/MZehren/ADTOF>

The bundled `third_party/demucs_onnx` tree is JamTaster's adapted version of
the pinned upstream revision. Segment context/padding, reflection padding and
half-spectrum handling are corrected; model loading avoids a redundant memory
copy; random shifts are reproducibly seeded; and library console output is
removed. Its original MIT licence remains beside the source.

The conversion-only Demucs fork lives under
`tools/models/third_party/jamtaster_demucs_onnx`. It is trimmed to the modules
required to load and convert the pinned checkpoints, and retains its MIT
licence and copyright notice.

The ADTOF ONNX artifact is intentionally ignored by Git until its model-weight
redistribution terms are resolved. Local builds can use the validated artifact,
but a public Jam2 package must not include it without completing that review or
replacing it with a redistributable drum model.
