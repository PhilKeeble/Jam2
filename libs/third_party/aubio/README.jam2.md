# aubio pitch subset

Jam2 vendors the minimum aubio 0.4.9 source needed by its local real-time
YINFFT pitch detector.

- Upstream: https://github.com/aubio/aubio
- Tag: `0.4.9`
- Commit: `90bd27a23123fcc524c31787c9c8fc0ae4c79378`
- License: GPL-3.0-or-later; see `COPYING`

The included C sources are limited to YINFFT, the bundled Ooura FFT, the
required vector/maths helpers, and aubio logging. Jam2 does not include
aubio's audio or file I/O, command-line tools, Python bindings, tempo/onset
analysis, resampling, or optional codec dependencies.

Jam2 carries two focused changes in `src/pitch/pitchyinfft.c`:

- The low-frequency YINFFT branch records its selected `peak_pos` before
  returning. Without that assignment,
  `aubio_pitchyinfft_get_confidence()` reports zero for low notes.
- The optional psychoacoustic spectral weighting is not applied to the
  squared-magnitude input. Its strong attenuation below 40 Hz biases the
  estimate of a five-string bass low B; the unweighted YIN difference retains
  the fundamental and its harmonic period.

The upstream 0.4.9 copyright header is retained in the modified file.
