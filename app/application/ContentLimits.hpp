#pragma once

#include <QtGlobal>

#include "runtime_limits.hpp"

namespace jam2::application::limits {

inline constexpr int kMinimumSampleRate = jam2::limits::kMinimumSampleRate;
inline constexpr int kMaximumSampleRate = jam2::limits::kMaximumSampleRate;

inline constexpr int kMinimumSongSections = 4;
inline constexpr int kMaximumSongSections = 12;
inline constexpr int kMinimumBeatsPerSection = 4;
inline constexpr int kMaximumBeatsPerSection = 512;
inline constexpr int kMaximumBeatLanes = 10;
inline constexpr int kMaximumCellCharacters = 4096;
inline constexpr int kMaximumTitleCharacters = 512;

inline constexpr int kMinimumLooperBankCount = kMinimumSongSections;
inline constexpr int kMaximumLooperBankCount = kMaximumSongSections;
inline constexpr int kMaximumLooperLanesPerBank = 128;
inline constexpr int kMaximumLooperIdCharacters = 80;
inline constexpr int kMaximumLooperNameCharacters = 512;
inline constexpr int kMaximumLooperPathCharacters = 4096;

inline constexpr qint64 kMaximumAssetBytes = 512LL * 1024LL * 1024LL;
// The asset frame limit assumes the smallest valid PCM16 frame (mono). The
// timeline limit covers the longest supported Section at 20 BPM and the
// maximum engine sample rate without accepting arbitrary 64-bit positions.
inline constexpr qint64 kMaximumAssetFrames =
    (kMaximumAssetBytes - 44LL) / 2LL;
inline constexpr qint64 kMaximumLooperTimelineFrames =
    static_cast<qint64>(kMaximumSampleRate) * 3LL * kMaximumBeatsPerSection;
inline constexpr int kMaximumAssetRequests = 64;
inline constexpr int kMaximumAssetChunkBytes = 24 * 1024;
inline constexpr int kMaximumAssetChunks = static_cast<int>(
    (kMaximumAssetBytes + kMaximumAssetChunkBytes - 1) / kMaximumAssetChunkBytes);

} // namespace jam2::application::limits
