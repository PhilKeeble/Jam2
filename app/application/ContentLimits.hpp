#pragma once

#include <QtGlobal>

#include "runtime_limits.hpp"

namespace jam2::application::limits {

inline constexpr int kMinimumSampleRate = jam2::limits::kMinimumSampleRate;
inline constexpr int kMaximumSampleRate = jam2::limits::kMaximumSampleRate;

inline constexpr int kMaximumSongSections = 64;
inline constexpr int kMinimumBeatsPerSection = 4;
inline constexpr int kMaximumBeatsPerSection = 512;
inline constexpr int kMaximumBeatLanes = 7;
inline constexpr int kMaximumLegacyBeatLanes = 11;
inline constexpr int kMaximumCellCharacters = 4096;
inline constexpr int kMaximumTitleCharacters = 512;
inline constexpr int kMaximumLyricsCharacters = 1024 * 1024;

inline constexpr int kLooperBankCount = 4;
inline constexpr int kMaximumLooperLanesPerBank = 128;
inline constexpr int kMaximumLooperIdCharacters = 80;
inline constexpr int kMaximumLooperNameCharacters = 512;
inline constexpr int kMaximumLooperPathCharacters = 4096;

inline constexpr qint64 kMaximumAssetBytes = 512LL * 1024LL * 1024LL;
inline constexpr int kMaximumAssetRequests = 64;
inline constexpr int kMaximumAssetChunkBytes = 24 * 1024;
inline constexpr int kMaximumAssetChunks = static_cast<int>(
    (kMaximumAssetBytes + kMaximumAssetChunkBytes - 1) / kMaximumAssetChunkBytes);

} // namespace jam2::application::limits
