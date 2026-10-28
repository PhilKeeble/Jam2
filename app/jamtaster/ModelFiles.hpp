#pragma once
#include <array>

namespace jamtaster::native {
inline constexpr std::array<const char*, 8> kRequiredModelFiles{
    "beat_this.onnx", "basic_pitch.onnx", "chordmini_btc.onnx", "adtof.onnx",
    "htdemucs_ft_0.onnx", "htdemucs_ft_1.onnx", "htdemucs_ft_2.onnx", "htdemucs_ft_3.onnx",
};
}
