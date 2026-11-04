#!/bin/bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

print_error() {
    printf '\nERROR: %s\n' "$1" >&2
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
            printf 'Open a new Terminal, follow the Homebrew PATH instructions, and run: bash ./update.sh\n' >&2
            return 1
        fi
    fi

    eval "$("$brew_bin" shellenv)"
}

# Check the same Qt installation compile.sh will select. Loading the backend
# catches missing/incompatible TLS plugins and their runtime dependencies too.
check_qt_tls() (
    local prefix="$1"
    local qmake="$prefix/bin/qmake"
    local libraries=""
    local plugins=""
    local probe_dir=""

    [[ -n "$prefix" && -x "$qmake" ]] || exit 1
    [[ "$("$qmake" -query QT_VERSION)" == 6.* ]] || exit 1
    for module in Qt6 Qt6Core Qt6Gui Qt6Widgets Qt6Network; do
        [[ -f "$prefix/lib/cmake/$module/${module}Config.cmake" ]] || exit 1
    done
    libraries="$("$qmake" -query QT_INSTALL_LIBS)" || exit 1
    plugins="$("$qmake" -query QT_INSTALL_PLUGINS)" || exit 1
    [[ -d "$libraries" && -d "$plugins/tls" ]] || exit 1

    probe_dir="$(mktemp -d "${TMPDIR:-/tmp}/jam2-qt-tls.XXXXXX")" || exit 1
    trap 'rm -rf "$probe_dir"' EXIT
    TMPDIR="$probe_dir" xcrun clang++ -std=c++17 -O2 \
        -F "$libraries" \
        -I "$libraries/QtCore.framework/Headers" \
        -I "$libraries/QtNetwork.framework/Headers" \
        "$REPO_DIR/tools/macos/QtTlsProbe.cpp" \
        -framework QtNetwork -framework QtCore \
        -Wl,-rpath,"$libraries" -o "$probe_dir/qt-tls-check" || exit 1
    TMPDIR="$probe_dir" "$probe_dir/qt-tls-check" "$plugins"
)

ensure_qt_tls() {
    local prefix="${QT_DIR:-}"

    printf '\nChecking Qt 6 HTTPS/TLS support for in-app updates...\n'
    if [[ -n "$prefix" ]]; then
        if ! check_qt_tls "$prefix"; then
            print_error "QT_DIR has no usable Qt 6 TLS backend: $prefix"
            printf 'Repair this Qt SDK (including its TLS plugins), or unset QT_DIR to use Homebrew Qt.\n' >&2
            return 1
        fi
        return 0
    fi

    load_homebrew
    prefix="$(brew --prefix qtbase 2>/dev/null || true)"
    if check_qt_tls "$prefix"; then
        printf 'Qt HTTPS/TLS support is already available; no installation needed.\n'
        return 0
    fi

    if brew list --versions qtbase >/dev/null 2>&1; then
        printf '\nRepairing the Qt base installation and its TLS dependencies with Homebrew...\n'
        brew reinstall qtbase
    else
        printf '\nInstalling Qt base and its TLS dependencies with Homebrew...\n'
        brew install qtbase
    fi
    prefix="$(brew --prefix qtbase)"
    if ! check_qt_tls "$prefix"; then
        print_error "Qt 6 HTTPS/TLS support is still unavailable after installation: $prefix"
        return 1
    fi
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    print_error "update.sh supports macOS only."
    exit 1
fi

printf '\nChecking Jam2 update prerequisites...\n'

if ! xcode-select -p >/dev/null 2>&1 || ! git --version >/dev/null 2>&1; then
    print_error "Apple Command Line Tools, including Git, are not installed or selected."
    printf 'Starting Apple'\''s Command Line Tools installer...\n'
    xcode-select --install >/dev/null 2>&1 || true
    printf 'Complete the installation dialog, then run: bash ./update.sh\n'
    exit 1
fi

if ! git lfs version >/dev/null 2>&1; then
    load_homebrew
    printf '\nGit LFS was not found. Installing it with Homebrew...\n'
    brew install git-lfs
fi

if ! git -C "$REPO_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    print_error "$REPO_DIR is not a Git working tree."
    exit 1
fi

ensure_qt_tls

printf '\nConfiguring Git LFS...\n'
git -C "$REPO_DIR" lfs install

printf '\nUpdating the current branch...\n'
git -C "$REPO_DIR" pull --ff-only

printf '\nDownloading Git LFS files...\n'
git -C "$REPO_DIR" lfs pull
git -C "$REPO_DIR" lfs fsck

if git -C "$REPO_DIR" lfs ls-files | grep -Eq '^[0-9a-f]+ - '; then
    print_error "Some Git LFS files are still pointers rather than downloaded content."
    git -C "$REPO_DIR" lfs ls-files | grep -E '^[0-9a-f]+ - ' >&2
    exit 1
fi

printf '\nRepository status:\n'
git -C "$REPO_DIR" status --short --branch

printf '\nUPDATE SUCCEEDED. Jam2 is ready to compile with: bash ./compile.sh\n'
