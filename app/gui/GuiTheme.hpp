#pragma once

#include <QColor>

namespace jam2::gui::theme {

inline constexpr QColor windowBg{5, 6, 7};
inline constexpr QColor panelBg{13, 15, 16};
inline constexpr QColor panelRaised{23, 26, 28};
inline constexpr QColor editorBg{0, 0, 0};
inline constexpr QColor border{89, 98, 105};
inline constexpr QColor borderStrong{137, 149, 156};
inline constexpr QColor text{228, 233, 236};
inline constexpr QColor textStrong{255, 255, 255};
inline constexpr QColor textMuted{164, 175, 181};
inline constexpr QColor accent{65, 159, 129};
inline constexpr QColor accentSoft{18, 62, 50};
inline constexpr QColor selection{37, 95, 114};
inline constexpr QColor buttonBg{34, 40, 44};
inline constexpr QColor buttonHover{48, 56, 61};
inline constexpr QColor clipBg{22, 63, 84};
inline constexpr QColor waveform{240, 152, 158};
inline constexpr QColor gridBar{131, 144, 153};
inline constexpr QColor gridBeat{66, 74, 79};
inline constexpr QColor playhead{255, 64, 95};
inline constexpr QColor record{168, 107, 111};
inline constexpr QColor success{83, 193, 132};
inline constexpr QColor warning{255, 194, 79};
inline constexpr QColor danger{240, 152, 158};
inline constexpr QColor meterBg{18, 21, 22};
inline constexpr QColor meterSafe{84, 227, 148};
inline constexpr QColor meterWarn{255, 184, 62};

inline QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

} // namespace jam2::gui::theme
