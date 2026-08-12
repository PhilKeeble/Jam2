# Vendored DaisySP subset

This directory contains the DaisySP source files used by the Jam2 application
and its `experiments/synth-ab` sound-design workbench. The subset is based on
DaisySP commit:

`599511b740f8f3a9b8db72a0642aa45b8a23c3a3`

Jam2 directly patches `Source/Drums/synthbassdrum.cpp` to initialize
`transient_env_` and `transient_env_lp_` in `SyntheticBassDrum::Init()`. The
pinned upstream source leaves those members indeterminate, which makes the
first triggered sample depend on previous memory contents.

Only the transitive source and header closure required by those two targets is
vendored. Upstream Git metadata, examples, tests, build files, hardware support,
and unused DSP modules are intentionally omitted.

DaisySP is licensed under the MIT License; see [LICENSE](LICENSE).
