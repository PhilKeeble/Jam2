# Vendored DaisySP subset

This directory contains the DaisySP source files used by the Jam2 application
and its `experiments/synth-ab` sound-design workbench. The files are copied
unchanged from DaisySP commit:

`599511b740f8f3a9b8db72a0642aa45b8a23c3a3`

Only the transitive source and header closure required by those two targets is
vendored. Upstream Git metadata, examples, tests, build files, hardware support,
and unused DSP modules are intentionally omitted.

DaisySP is licensed under the MIT License; see [LICENSE](LICENSE).
