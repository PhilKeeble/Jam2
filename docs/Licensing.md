# Licensing

Jam2 is licensed under GPL-3.0-or-later. The complete license text is in the
repository-root `LICENSE` file and is staged beside the public executable.

## Third-Party Code

Jam2 includes Signalsmith libraries under `libs/third_party`:

- `libs/third_party/signalsmith-stretch`
- `libs/third_party/signalsmith-linear`

Their license files are kept beside the source:

- `libs/third_party/signalsmith-stretch/LICENSE.txt`
- `libs/third_party/signalsmith-linear/LICENSE.txt`

Keep those notices with source and binary distributions that include the Signalsmith code.

Jam2 also includes a minimal aubio 0.4.9 pitch-analysis subset under
`libs/third_party/aubio`. Its GPL-3.0-or-later license, pinned upstream commit,
included source boundary, and Jam2's documented low-frequency detector changes
are kept beside the source. The release stages the corresponding aubio and
Signalsmith notices under `release/licenses`.

## Release Checklist

Before publishing a wider release, confirm the staged `LICENSE`,
`THIRD_PARTY_NOTICES.md`, and `release/licenses` files are present.
