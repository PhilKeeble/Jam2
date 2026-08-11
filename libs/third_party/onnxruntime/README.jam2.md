# ONNX Runtime in Jam2

JamTaster uses the official ONNX Runtime 1.23.2 CPU C/C++ release artifacts.
Only the common public headers and platform runtime/link libraries required by
the native worker are vendored.

| Artifact | Official archive SHA-256 | Vendored runtime SHA-256 |
| --- | --- | --- |
| Windows x64 | official `onnxruntime-win-x64-1.23.2.zip` (archive not retained) | `dec964ab1ee36cc9b0ae247d13b376627992fc57dec0454354017ab8fd84f1ea` |
| macOS arm64 | `b4d513ab2b26f088c66891dbbc1408166708773d7cc4163de7bdca0e9bbb7856` | `d306d2bc768540766c7ed8a1e0ff05d2870c77a934ebeee4a7bafa1b732ef299` |
| macOS x86-64 | `d10359e16347b57d9959f7e80a225a5b4a66ed7d7e007274a15cae86836485a6` | `8c9c78de65ea3786f987c0d980e9c1b13a3a5fbc6b3e2965ba05b450e6e4c054` |

The macOS archives are the official GitHub release assets named
`onnxruntime-osx-arm64-1.23.2.tgz` and
`onnxruntime-osx-x86_64-1.23.2.tgz`. The extracted versioned dylibs are stored
without archive symlinks so Git checkouts behave consistently on Windows.

ONNX Runtime is MIT licensed. `LICENSE` and `ThirdPartyNotices.txt` are retained
beside these files and copied into Jam2's release notices.
