#!/bin/bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_VERSION=""
INCLUDE_MODELS=OFF
SIGN_IDENTITY=""
PUBLISH=OFF

while (( $# )); do
    case "$1" in
        --version)
            (( $# >= 2 )) || { printf '%s\n' '--version requires major.minor.patch.' >&2; exit 2; }
            RELEASE_VERSION="$2"
            [[ "$RELEASE_VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || {
                printf '%s\n' 'Version must be major.minor.patch without leading zeroes.' >&2; exit 2;
            }
            IFS=. read -r VERSION_MAJOR VERSION_MINOR VERSION_PATCH <<< "$RELEASE_VERSION"
            for VERSION_PART in "$VERSION_MAJOR" "$VERSION_MINOR" "$VERSION_PATCH"; do
                if (( ${#VERSION_PART} > 5 )) || (( VERSION_PART > 65535 )); then
                    printf '%s\n' 'Version components must be at most 65535.' >&2; exit 2
                fi
            done
            shift 2 ;;
        --include-models) INCLUDE_MODELS=ON; shift ;;
        --publish) PUBLISH=ON; shift ;;
        --sign-identity)
            (( $# >= 2 )) && [[ -n "$2" ]] || { printf '%s\n' '--sign-identity requires a signing identity.' >&2; exit 2; }
            SIGN_IDENTITY="$2"; shift 2 ;;
        --help)
            printf '%s\n' 'Usage: ./build-release.sh [--version 0.8.1] [--include-models] [--publish] [--sign-identity "Developer ID Application: ..."]' \
                'Builds a universal Mac app and dist/Jam2-macos.zip; models are an optional separate ZIP.' \
                '--publish requires an explicit version and GitHub CLI authentication (gh auth login).' \
                'Without an identity, local test builds use ad-hoc signing. Notarization is a separate step.'
            exit 0 ;;
        *) printf 'Unknown release option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

[[ "$(uname -s)" == Darwin ]] || { printf '%s\n' 'build-release.sh supports macOS only.' >&2; exit 1; }
if [[ "$PUBLISH" == ON ]]; then
    [[ -n "$RELEASE_VERSION" ]] || { printf '%s\n' '--publish requires an explicit --version.' >&2; exit 2; }
    command -v gh >/dev/null 2>&1 || { printf '%s\n' 'Publishing requires GitHub CLI. Install gh, then run: gh auth login' >&2; exit 1; }
    gh auth status --hostname github.com
fi
export JAM2_RELEASE_VERSION="$RELEASE_VERSION"
export JAM2_RELEASE_UNIVERSAL=1
bash "$REPO_DIR/compile.sh"
CMAKE_BIN="$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' "$REPO_DIR/build/CMakeCache.txt")"
[[ -x "$CMAKE_BIN" ]] || { printf '%s\n' 'Could not locate CMake in the completed build cache.' >&2; exit 1; }
"$CMAKE_BIN" "-DJAM2_PACKAGE_CONFIG=$REPO_DIR/build/release-package.cmake" \
    "-DJAM2_INCLUDE_MODELS=$INCLUDE_MODELS" "-DJAM2_SIGN_IDENTITY=$SIGN_IDENTITY" \
    -P "$REPO_DIR/tools/release/PackageRelease.cmake"
if [[ "$PUBLISH" == ON ]]; then
    "$CMAKE_BIN" "-DJAM2_PACKAGE_CONFIG=$REPO_DIR/build/release-package.cmake" \
        "-DJAM2_INCLUDE_MODELS=$INCLUDE_MODELS" -P "$REPO_DIR/tools/release/PublishRelease.cmake"
fi
