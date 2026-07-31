// DaisySP currently annotates LadderFilter::ProcessBlock with GCC's
// __attribute__((optimize)) spelling. Keep the pinned upstream source intact
// while allowing the same implementation to compile with MSVC.
#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(arguments)
#endif

#include "../.deps/DaisySP/Source/Filters/ladder.cpp"
