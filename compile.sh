#!/bin/bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CMAKE_BIN=""
CMAKE_VERSION=""
NINJA_BIN=""
NINJA_VERSION=""
QT_DIR="${QT_DIR:-}"
BUILD_TESTING="OFF"
TEST_SUITE=""
TEST_TARGET=""
TEST_LABEL=""
TEST_SHOW_GUI="0"
TEST_NAME=""
TEST_ARTIFACT_ROOT=""
HARDWARE_PROFILE=""

print_error() {
    printf '\nERROR: %s\n' "$1" >&2
}

while (( $# > 0 )); do
    case "$1" in
        --tests-full)
            BUILD_TESTING="ON"
            TEST_SUITE="full"
            shift
            ;;
        --tests)
            if (( $# < 2 )); then
                print_error "--tests requires a suite name."
                exit 2
            fi
            BUILD_TESTING="ON"
            TEST_SUITE="$2"
            shift 2
            ;;
        --show-gui)
            TEST_SHOW_GUI="1"
            shift
            ;;
        --test-name)
            if (( $# < 2 )); then
                print_error "--test-name requires one exact CTest name."
                exit 2
            fi
            TEST_NAME="$2"
            shift 2
            ;;
        --hardware-profile)
            if (( $# < 2 )); then
                print_error "--hardware-profile requires a JSON profile path."
                exit 2
            fi
            HARDWARE_PROFILE="$2"
            shift 2
            ;;
        *)
            print_error "Unknown compile option: $1"
            printf 'Supported options: --tests unit, --tests plugin, --tests hardware, --tests gui, --tests jam-sync, --tests shared-content, --tests performance, --tests network, --tests full, --tests-full, --test-name NAME, --show-gui, --hardware-profile PATH\n' >&2
            exit 2
            ;;
    esac
done

case "$TEST_SUITE" in
    "") ;;
    unit)
        TEST_TARGET="jam2_tests_unit"
        TEST_LABEL="unit"
        ;;
    plugin)
        TEST_TARGET="jam2_tests_plugin"
        TEST_LABEL="plugin"
        ;;
    hardware)
        TEST_TARGET="jam2_tests_hardware"
        TEST_LABEL="hardware"
        ;;
    gui)
        TEST_TARGET="jam2_tests_gui"
        TEST_LABEL="gui"
        ;;
    jam-sync)
        TEST_TARGET="jam2_tests_jam_sync"
        TEST_LABEL="jam-sync"
        ;;
    shared-content)
        TEST_TARGET="jam2_tests_shared_content"
        TEST_LABEL="shared-content"
        ;;
    performance)
        TEST_TARGET="jam2_tests_performance"
        TEST_LABEL="performance"
        ;;
    network)
        TEST_TARGET="jam2_tests_network"
        TEST_LABEL="^network$"
        ;;
    full)
        TEST_TARGET="jam2_tests_all"
        ;;
    *)
        print_error "Test suite '$TEST_SUITE' is not implemented yet."
        printf 'Available suites: unit, plugin, hardware, gui, jam-sync, shared-content, performance, network, full\n' >&2
        exit 2
        ;;
esac

if [[ "$TEST_SHOW_GUI" == "1" && "$TEST_SUITE" != "gui" && "$TEST_SUITE" != "full" ]]; then
    print_error "--show-gui requires the gui or full test suite."
    exit 2
fi
if [[ -n "$TEST_NAME" && -z "$TEST_TARGET" ]]; then
    print_error "--test-name requires --tests SUITE or --tests-full."
    exit 2
fi
if [[ "$TEST_SUITE" == "hardware" && -z "$HARDWARE_PROFILE" ]]; then
    print_error "--tests hardware requires --hardware-profile PATH."
    exit 2
fi
if [[ -n "$HARDWARE_PROFILE" && "$TEST_SUITE" != "hardware" && "$TEST_SUITE" != "full" ]]; then
    print_error "--hardware-profile requires --tests hardware or --tests-full."
    exit 2
fi
if [[ -n "$HARDWARE_PROFILE" ]]; then
    if [[ ! -f "$HARDWARE_PROFILE" ]]; then
        print_error "Hardware profile was not found: $HARDWARE_PROFILE"
        exit 2
    fi
    HARDWARE_PROFILE="$(cd "$(dirname "$HARDWARE_PROFILE")" && pwd -P)/$(basename "$HARDWARE_PROFILE")"
fi

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

if [[ -n "$TEST_TARGET" ]]; then
    TEST_ARTIFACT_ROOT="$REPO_DIR/build/test-artifacts"
    if ! "$CMAKE_BIN" -E remove_directory "$TEST_ARTIFACT_ROOT" ||
        ! "$CMAKE_BIN" -E make_directory "$TEST_ARTIFACT_ROOT"; then
        print_error "Could not initialize the test artifact workspace: $TEST_ARTIFACT_ROOT"
        exit 1
    fi
fi

printf '\nConfiguring Jam2...\n'
if ! "$CMAKE_BIN" \
    -S "$REPO_DIR" \
    -B "$REPO_DIR/build" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_DIR" \
    -DBUILD_TESTING="$BUILD_TESTING" \
    -DJAM2_HARDWARE_PROFILE:FILEPATH="$HARDWARE_PROFILE"; then
    printf '\nCONFIGURE FAILED.\n' >&2
    exit 1
fi

printf '\nBuilding Jam2...\n'
if [[ -n "$TEST_TARGET" ]]; then
    BUILD_COMMAND=("$CMAKE_BIN" --build "$REPO_DIR/build" --target "$TEST_TARGET")
else
    BUILD_COMMAND=("$CMAKE_BIN" --build "$REPO_DIR/build")
fi
if ! "${BUILD_COMMAND[@]}"; then
    printf '\nBUILD FAILED.\n' >&2
    exit 1
fi

if [[ -n "$TEST_TARGET" ]]; then
    printf '\nRunning Jam2 %s tests...\n' "$TEST_SUITE"
    export JAM2_TEST_SHOW_GUI="$TEST_SHOW_GUI"
    CTEST_BIN="$(dirname "$CMAKE_BIN")/ctest"
    if [[ ! -x "$CTEST_BIN" ]]; then
        print_error "CTest was not found next to CMake: $CTEST_BIN"
        exit 1
    fi
    if [[ -n "$TEST_NAME" ]]; then
        TEST_COMMAND=("$CTEST_BIN" --test-dir "$REPO_DIR/build" --output-on-failure --no-tests=error -R "^${TEST_NAME}$")
    elif [[ -n "$TEST_LABEL" ]]; then
        TEST_COMMAND=("$CTEST_BIN" --test-dir "$REPO_DIR/build" --output-on-failure -L "$TEST_LABEL")
    else
        TEST_COMMAND=("$CTEST_BIN" --test-dir "$REPO_DIR/build" --output-on-failure)
    fi
    if ! "${TEST_COMMAND[@]}"; then
        printf '\nTESTS FAILED.\n' >&2
        exit 1
    fi
    if ! "$CMAKE_BIN" -E remove_directory "$TEST_ARTIFACT_ROOT"; then
        print_error "Tests passed, but their artifact workspace could not be removed: $TEST_ARTIFACT_ROOT"
        exit 1
    fi
    printf '\nBUILD AND TESTS SUCCEEDED.\n'
else
    printf '\nBUILD SUCCEEDED.\n'
fi
