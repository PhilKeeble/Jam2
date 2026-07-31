# Third-party notices

Jam2 is distributed under GPL-3.0-or-later. Its source tree and binary
distribution include the following third-party components.

## aubio 0.4.9 pitch subset

Copyright (C) Paul Brossier and aubio contributors.

Jam2 includes the YINFFT pitch detector, the bundled Ooura FFT, and required
aubio support sources from commit
`90bd27a23123fcc524c31787c9c8fc0ae4c79378`. These sources are licensed under
GPL-3.0-or-later. See `libs/third_party/aubio/COPYING` and
`libs/third_party/aubio/README.jam2.md`.

## Signalsmith Stretch

Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.

Licensed under the MIT License. See
`libs/third_party/signalsmith-stretch/LICENSE.txt`.

## Signalsmith Linear

Copyright (c) 2025 Signalsmith Audio.

Licensed under the MIT License. See
`libs/third_party/signalsmith-linear/LICENSE.txt`.

## DaisySP drum-synthesis subset

Copyright (c) 2020 Electrosmith, Corp., with credited Plaits and Soundpipe
components.

Jam2 compiles the ADSR, analogue/synthetic kick and snare, hi-hat, state
variable filter, and oscillator sources from pinned DaisySP commit
`599511b740f8f3a9b8db72a0642aa45b8a23c3a3`. DaisySP and the credited
components are licensed under the MIT License. See
`libs/third_party/DaisySP/LICENSE`.

Jam2 generates a build-local copy of DaisySP's `synthbassdrum.cpp` with two
missing transient-envelope state initializers added to
`SyntheticBassDrum::Init`. This narrow deterministic-state correction does not
change the model or its parameter behaviour; it prevents the first transient
sample from reading indeterminate state. The pinned vendored source remains
unmodified, and configuration fails if the reviewed upstream source anchor no
longer matches.
