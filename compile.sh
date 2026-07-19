#!/bin/bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CMAKE_BIN=""
CMAKE_VERSION=""
NINJA_BIN=""
NINJA_VERSION=""
QT_DIR="${QT_DIR:-}"

print_error() {
    printf '\nERROR: %s\n' "$1" >&2
}

cmake_version_supported() {
    local version="$1"
    local major=""
    local minor=""

    IFS=. read -r major minor _ <<< "$version"
    [[ "$major" =~ ^[0-9]+$ && "$minor" =~ ^[0-9]+$ ]] || return 1
    (( major > 3 || (major == 3 && minor >= 24) ))
}

validate_qt() {
    local prefix="$1"
    [[ -f "$prefix/lib/cmake/Qt6/Qt6Config.cmake" ]] &&
        [[ -f "$prefix/lib/cmake/Qt6Core/Qt6CoreConfig.cmake" ]] &&
        [[ -f "$prefix/lib/cmake/Qt6Gui/Qt6GuiConfig.cmake" ]] &&
        [[ -f "$prefix/lib/cmake/Qt6Widgets/Qt6WidgetsConfig.cmake" ]] &&
        [[ -f "$prefix/lib/cmake/Qt6Network/Qt6NetworkConfig.cmake" ]]
}

load_homebrew() {
    local brew_bin=""

    if command -v brew >/dev/null 2>&1; then
        brew_bin="$(command -v brew)"
    elif [[ -x /opt/homebrew/bin/brew ]]; then
        brew_bin="/opt/homebrew/bin/brew"
    elif [[ -x /usr/local/bin/brew ]]; then
        brew_bin="/usr/local/bin/brew"
    else
        printf '\nHomebrew was not found. Installing it from the official Homebrew installer...\n'
        if ! command -v curl >/dev/null 2>&1; then
            print_error "curl is required to download the Homebrew installer."
            return 1
        fi
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

        if [[ -x /opt/homebrew/bin/brew ]]; then
            brew_bin="/opt/homebrew/bin/brew"
        elif [[ -x /usr/local/bin/brew ]]; then
            brew_bin="/usr/local/bin/brew"
        else
            print_error "Homebrew installation completed, but brew was not found in a standard location."
            printf 'Open a new Terminal, follow the Homebrew PATH instructions, and run: bash ./compile.sh\n' >&2
            return 1
        fi
    fi

    eval "$("$brew_bin" shellenv)"
}

ensure_cmake() {
    local candidate=""
    local version=""

    if command -v cmake >/dev/null 2>&1; then
        candidate="$(command -v cmake)"
        version="$("$candidate" --version | awk 'NR == 1 { print $3 }')"
        if cmake_version_supported "$version"; then
            CMAKE_BIN="$candidate"
            CMAKE_VERSION="$version"
            return 0
        fi
        printf '\nCMake %s is too old; Jam2 requires CMake 3.24 or newer.\n' "${version:-unknown}"
    fi

    if brew list --versions cmake >/dev/null 2>&1; then
        candidate="$(brew --prefix cmake)/bin/cmake"
        version="$("$candidate" --version | awk 'NR == 1 { print $3 }')"
        if ! cmake_version_supported "$version"; then
            printf 'Upgrading CMake with Homebrew...\n'
            brew upgrade cmake
        fi
    else
        printf '\nCMake was not found. Installing it with Homebrew...\n'
        brew install cmake
    fi

    CMAKE_BIN="$(brew --prefix cmake)/bin/cmake"
    CMAKE_VERSION="$("$CMAKE_BIN" --version | awk 'NR == 1 { print $3 }')"
    if ! cmake_version_supported "$CMAKE_VERSION"; then
        print_error "Homebrew CMake $CMAKE_VERSION is too old; Jam2 requires CMake 3.24 or newer."
        return 1
    fi
}

ensure_ninja() {
    if command -v ninja >/dev/null 2>&1; then
        NINJA_BIN="$(command -v ninja)"
    elif brew list --versions ninja >/dev/null 2>&1; then
        NINJA_BIN="$(brew --prefix ninja)/bin/ninja"
    else
        printf '\nNinja was not found. Installing it with Homebrew...\n'
        brew install ninja
        NINJA_BIN="$(brew --prefix ninja)/bin/ninja"
    fi

    if [[ ! -x "$NINJA_BIN" ]]; then
        print_error "Ninja was installed, but its executable was not found."
        return 1
    fi
    NINJA_VERSION="$("$NINJA_BIN" --version)"
}

ensure_qt() {
    if [[ -n "$QT_DIR" ]]; then
        if ! validate_qt "$QT_DIR"; then
            print_error "QT_DIR does not point to a complete Qt 6 desktop installation: $QT_DIR"
            printf 'The directory must contain the Qt 6 Core, Gui, Widgets, and Network CMake packages.\n' >&2
            return 1
        fi
        return 0
    fi

    if brew list --versions qtbase >/dev/null 2>&1; then
        QT_DIR="$(brew --prefix qtbase)"
    else
        printf '\nQt 6 was not found. Installing the required Qt base modules with Homebrew...\n'
        brew install qtbase
        QT_DIR="$(brew --prefix qtbase)"
    fi

    if ! validate_qt "$QT_DIR"; then
        print_error "Homebrew Qt is missing one or more required packages: $QT_DIR"
        printf 'Try running: brew reinstall qtbase\n' >&2
        return 1
    fi
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    print_error "compile.sh supports macOS only."
    exit 1
fi

printf '\nChecking Jam2 macOS build prerequisites...\n'

if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang >/dev/null 2>&1; then
    print_error "Apple Command Line Tools are not installed or are not selected."
    printf 'Starting Apple'\''s Command Line Tools installer...\n'
    xcode-select --install >/dev/null 2>&1 || true
    printf 'Complete the installation dialog, then run: bash ./compile.sh\n'
    exit 1
fi

load_homebrew
ensure_cmake
ensure_ninja
ensure_qt

export PATH="$(dirname "$CMAKE_BIN"):$(dirname "$NINJA_BIN"):$PATH"

printf '\nPrerequisites found:\n'
printf '  Apple developer tools: %s\n' "$(xcode-select -p)"
printf '  Homebrew:             %s\n' "$(command -v brew)"
printf '  CMake %s:       %s\n' "$CMAKE_VERSION" "$CMAKE_BIN"
printf '  Ninja %s:             %s\n' "$NINJA_VERSION" "$NINJA_BIN"
printf '  Qt 6:                 %s\n' "$QT_DIR"

printf '\nConfiguring Jam2...\n'
if ! "$CMAKE_BIN" \
    -S "$REPO_DIR" \
    -B "$REPO_DIR/build" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_DIR"; then
    printf '\nCONFIGURE FAILED.\n' >&2
    exit 1
fi

printf '\nBuilding Jam2...\n'
if ! "$CMAKE_BIN" --build "$REPO_DIR/build"; then
    printf '\nBUILD FAILED.\n' >&2
    exit 1
fi

printf '\nBUILD SUCCEEDED.\n'
