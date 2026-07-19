#pragma once

#include <QColor>

namespace jam2::gui::theme {

inline constexpr QColor windowBg{5, 8, 9};
inline constexpr QColor panelBg{11, 16, 17};
inline constexpr QColor panelRaised{16, 23, 25};
inline constexpr QColor editorBg{7, 11, 12};
inline constexpr QColor border{53, 66, 71};
inline constexpr QColor borderStrong{92, 110, 114};
inline constexpr QColor text{233, 230, 220};
inline constexpr QColor textStrong{255, 252, 244};
inline constexpr QColor textMuted{156, 169, 171};
inline constexpr QColor accent{102, 212, 207};
inline constexpr QColor accentSoft{25, 48, 49};
inline constexpr QColor nebulaBlue{86, 164, 244};
inline constexpr QColor nebulaPurple{164, 111, 218};
inline constexpr QColor nebulaCoral{255, 125, 134};
inline constexpr QColor nebulaRed{201, 47, 88};
inline constexpr QColor selection{91, 57, 77};
inline constexpr QColor buttonBg{18, 27, 29};
inline constexpr QColor buttonHover{28, 40, 43};
inline constexpr QColor clipBg{38, 25, 39};
inline constexpr QColor waveform{255, 125, 134};
inline constexpr QColor gridBar{232, 164, 74};
inline constexpr QColor gridBeat{77, 93, 97};
inline constexpr QColor playhead{232, 164, 74};
inline constexpr QColor record{225, 73, 117};
inline constexpr QColor success{73, 213, 181};
inline constexpr QColor warning{230, 174, 82};
inline constexpr QColor danger{240, 104, 136};
inline constexpr QColor meterBg{7, 11, 12};
inline constexpr QColor meterSafe{102, 212, 207};
inline constexpr QColor meterWarn{232, 164, 74};

inline QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

} // namespace jam2::gui::theme
