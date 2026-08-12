# Steinberg VST 3 SDK

Jam2 vendors the source required to implement its private VST3 scanner and
runtime worker. The code is maintained directly inside this repository under
the upstream MIT licence in `LICENSE.txt`.

Upstream: https://github.com/steinbergmedia/vst3sdk

Pinned revisions:

- Aggregate SDK: `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`
- `base`: `fcf9da0bd27a16f7f03773a3a39822f28f5c8477`
- `pluginterfaces`: `4f547e8e102b47de4a8b8aaf343c73b700786372`
- `public.sdk`: `586dc5e6c8012c3e4b01c79389375cbe96bdb1da`

Jam2 integration changes:

- The aggregate repository's nested Git metadata and VSTGUI checkout are
  intentionally omitted. Upstream sample/reference sources remain in the
  vendor tree for review, but Jam2 does not configure, compile, or ship them.
- Jam2 lists the required SDK translation units explicitly in its own CMake
  target. It does not execute or patch around an upstream build system.
- Jam2-specific hosting, process isolation, IPC, MIDI/MPE translation, editor
  ownership, and error handling live in `app/pluginhost`; they are not hidden
  in generated copies of upstream files.
- `public.sdk/source/vst/hosting/module_win32.cpp` uses the compiler's
  `__cpp_char8_t` feature macro for its C++20 filesystem-string conversion so
  the maintained SDK source builds correctly with Jam2's MSVC configuration.
- `public.sdk/source/vst/hosting/module.cpp` bounds negative class counts,
  tolerates delayed factory class registration for up to 500 ms at the
  non-real-time scan/load boundary, and avoids undefined `back()` access when
  a malformed factory advertises a class but rejects every metadata query.
- Unused plug-in examples and wrapper sources remain outside Jam2 build
  targets and are not shipped with the application.

When updating the SDK, re-audit all explicitly compiled sources, platform
module lifecycle code, licences, VST usage requirements, and the Windows and
macOS installed-plug-in validation suite.
