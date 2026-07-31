#pragma once

namespace jam2::practice {

// The existing +3 dB recipe gain is part of the kit voicing and feeds the
// researched bus. This additional makeup is applied after the bus so the
// accepted synthesis character does not change.
inline constexpr double kGeneratedDrumStemMakeupDb = 8.0;
inline constexpr double kGeneratedDrumStemMakeupLinear =
    2.5118864315095801;

// The requested makeup is baked into the WAV. Keep the generated lane at
// unity so the user retains its full +12 dB upward fader range.
inline constexpr double kGeneratedDrumLaneGainDb = 0.0;

// Samples below the knee remain linear. Louder transients use a rational
// soft knee that approaches the PCM ceiling while retaining a positive slope,
// avoiding both hard clipping and float-precision plateaus.
inline constexpr double kGeneratedDrumSoftLimitThreshold = 0.78;
inline constexpr double kGeneratedDrumSoftLimitCeiling = 0.98;

} // namespace jam2::practice
