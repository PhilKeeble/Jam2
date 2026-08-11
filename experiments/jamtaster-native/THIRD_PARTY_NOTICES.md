# JamTaster Native Lab Third-Party Notices

This experiment commits the narrow source adaptations and final ONNX artifacts
needed to reproduce the native feasibility result. Developer SDK packages for
ONNX Runtime and Eigen remain local in the ignored `.deps/` directory. Nothing
in this experiment is copied into Jam2's release by the current work.

| Component | Pinned version/revision | Licence | Use |
| --- | --- | --- | --- |
| ONNX Runtime | 1.23.2 | MIT | Native inference runtime |
| sevagh/demucs.onnx | `81fa192e6fcc88e35e887f6e6ccce91227f4e6f5` | MIT | HTDemucs ONNX boundary and native STFT reference |
| facebookresearch/demucs | source underlying the pinned conversion fork | MIT | One-time HTDemucs checkpoint loading/export only |
| Eigen | 3.4.0 | MPL-2.0 | Tensor/matrix support required by the Demucs reference implementation |
| mosynthkey/beat_this_cpp | `07ab790a9ec2eda8093d52d249e3ec4f0510ee72` | MIT | Beat This native preprocessing/inference parity reference |
| CPJKU/beat_this | `final0` | MIT for code and published weights | Beat/downbeat model |
| Spotify Basic Pitch | 0.4.0 | Apache-2.0 | Bass note model and official ONNX artifact |
| ptnghia-j/ChordMini | pinned by the existing JamTaster component | MIT repository | Chord model |
| ADTOF-pytorch / original ADTOF | existing component revision | Redistribution unresolved / original CC BY-NC-SA 4.0 | Local drum-analysis comparison only |

Project links:

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

Before any production packaging, copy the applicable full licence texts into
Jam2's release licence directory and re-check the model-weight terms separately
from the source-code terms.

The committed MIT-licensed `third_party/demucs_onnx` source is JamTaster's
adapted version of the pinned upstream revision. Segment context/padding,
reflection padding and half-spectrum handling are corrected; model loading
avoids a redundant in-memory copy; random shifts are reproducibly seeded; and
library-level console output is removed. The original MIT licence is included
alongside the source.

The conversion-only Meta Demucs fork is retained under
`tools/models/third_party/jamtaster_demucs_onnx`. It has been trimmed to the
modules needed to load and convert the pinned checkpoints. Its original MIT
licence and copyright notice are included in that directory. This Python source
is not part of the native runtime.
