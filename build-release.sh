#!/bin/bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_VERSION=""
INCLUDE_MODELS=OFF
SIGN_IDENTITY="${JAM2_SIGN_IDENTITY:-}"
NOTARY_PROFILE="${JAM2_NOTARY_PROFILE:-}"
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
        --notarize-profile)
            (( $# >= 2 )) && [[ -n "$2" ]] || { printf '%s\n' '--notarize-profile requires a Keychain profile name.' >&2; exit 2; }
            NOTARY_PROFILE="$2"; shift 2 ;;
        --help)
            printf '%s\n' 'Usage: ./build-release.sh [--version 0.8.1] [--include-models] [--publish] [--sign-identity "Developer ID Application: ..."] [--notarize-profile jam2-notary]' \
                'Builds a universal Mac app and dist/Jam2-macos.zip; models are an optional separate ZIP.' \
                '--publish requires an explicit version and GitHub CLI authentication (gh auth login).' \
                'Without an identity, local test builds use ad-hoc signing.' \
                'A Developer ID identity enables hardened runtime signing and secure timestamps.' \
                '--notarize-profile submits to Apple, staples the ticket and creates the final ZIP before publishing.' \
                'Defaults: JAM2_SIGN_IDENTITY and JAM2_NOTARY_PROFILE (names only; credentials stay in Keychain).' \
                'One-time setup: xcrun notarytool store-credentials "jam2-notary" (interactive; never pass a password here).'
            exit 0 ;;
        *) printf 'Unknown release option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

[[ "$(uname -s)" == Darwin ]] || { printf '%s\n' 'build-release.sh supports macOS only.' >&2; exit 1; }
if [[ -n "$NOTARY_PROFILE" && ( -z "$SIGN_IDENTITY" || "$SIGN_IDENTITY" == "-" ) ]]; then
    printf '%s\n' 'Notarization requires --sign-identity "Developer ID Application: ..." (or JAM2_SIGN_IDENTITY).' >&2
    exit 2
fi
if [[ -n "$NOTARY_PROFILE" ]]; then
    xcrun --find notarytool >/dev/null
    xcrun --find stapler >/dev/null
    # Validate the saved credentials before spending time compiling. No secrets
    # are read by this script or written into the build cache or artifacts.
    xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null
fi
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
# The packager invokes this hook after deployment and before hashing/archiving.
# Local ad-hoc builds continue to use the packager's normal signing path.
RELEASE_WORK="$(mktemp -d "$REPO_DIR/build/.macos-release.XXXXXX")"
trap 'rm -rf "$RELEASE_WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$RELEASE_WORK/finalize.cmake" <<'CMAKE'
set(entitlements "${JAM2_RELEASE_WORK}/plugin-host.plist")
file(WRITE "${entitlements}" [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.cs.disable-library-validation</key><true/>
</dict></plist>
]=])
set(audio_entitlements "${JAM2_RELEASE_WORK}/audio-input.plist")
file(WRITE "${audio_entitlements}" [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.device.audio-input</key><true/>
</dict></plist>
]=])
# Nested code first; grant foreign-library loading only to the VST host worker.
foreach(target IN LISTS sign_targets)
    set(options --timestamp --options runtime)
    if(target STREQUAL "${bundle}/Contents/Helpers/jam2-plugin-worker")
        list(APPEND options --entitlements "${entitlements}")
    endif()
    execute_process(COMMAND /usr/bin/codesign --force --sign "${JAM2_SIGN_IDENTITY}"
        ${options} "${target}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND /usr/bin/codesign --force --sign "${JAM2_SIGN_IDENTITY}"
    --timestamp --options runtime --entitlements "${audio_entitlements}" "${bundle}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND /usr/bin/codesign --verify --deep --strict "${bundle}"
    COMMAND_ERROR_IS_FATAL ANY)

if(NOT JAM2_NOTARY_PROFILE STREQUAL "")
    set(submission "${JAM2_PACKAGE_OUTPUT}/Jam2-notarization.json")
    set(log "${JAM2_PACKAGE_OUTPUT}/Jam2-notarization-log.json")
    file(REMOVE "${log}")
    execute_process(COMMAND /usr/bin/ditto -c -k --sequesterRsrc --keepParent
        "${bundle}" "${JAM2_RELEASE_WORK}/notarization.zip" COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS "Submitting Jam2 to Apple; waiting for notarization. Result: ${submission}")
    execute_process(COMMAND /usr/bin/xcrun notarytool submit "${JAM2_RELEASE_WORK}/notarization.zip"
        --keychain-profile "${JAM2_NOTARY_PROFILE}" --wait --output-format json
        OUTPUT_VARIABLE result RESULT_VARIABLE submitted ERROR_VARIABLE submit_error)
    write_json("${submission}" "${result}")
    string(JSON identifier ERROR_VARIABLE id_error GET "${result}" id)
    string(JSON status ERROR_VARIABLE status_error GET "${result}" status)
    if(id_error STREQUAL "NOTFOUND")
        execute_process(COMMAND /usr/bin/xcrun notarytool log "${identifier}"
            --keychain-profile "${JAM2_NOTARY_PROFILE}" "${log}" RESULT_VARIABLE logged)
        if(NOT logged EQUAL 0)
            message(FATAL_ERROR "Could not retrieve notarization log for ${identifier}; rerun notarytool log before publishing")
        endif()
        file(READ "${log}" log_contents)
        message(STATUS "Apple notarization log: ${log}\n${log_contents}")
    endif()
    if(NOT submitted EQUAL 0 OR NOT id_error STREQUAL "NOTFOUND" OR NOT status_error STREQUAL "NOTFOUND" OR NOT status STREQUAL "Accepted")
        message(FATAL_ERROR "Notarization was not accepted (${status}). ${submit_error} See ${submission} and ${log}")
    endif()
    execute_process(COMMAND /usr/bin/xcrun stapler staple "${bundle}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND /usr/bin/xcrun stapler validate "${bundle}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND /usr/bin/codesign --verify --deep --strict "${bundle}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND /usr/sbin/spctl --assess --type execute --verbose=2 "${bundle}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()

CMAKE
FINALIZE_SCRIPT=""
if [[ -n "$SIGN_IDENTITY" && "$SIGN_IDENTITY" != "-" ]]; then
    FINALIZE_SCRIPT="$RELEASE_WORK/finalize.cmake"
fi
"$CMAKE_BIN" "-DJAM2_PACKAGE_CONFIG=$REPO_DIR/build/release-package.cmake" \
    "-DJAM2_INCLUDE_MODELS=$INCLUDE_MODELS" "-DJAM2_SIGN_IDENTITY=$SIGN_IDENTITY" \
    "-DJAM2_NOTARY_PROFILE=$NOTARY_PROFILE" "-DJAM2_RELEASE_WORK=$RELEASE_WORK" \
    "-DJAM2_MACOS_FINALIZE_SCRIPT=$FINALIZE_SCRIPT" \
    -P "$REPO_DIR/tools/release/PackageRelease.cmake"
if [[ "$PUBLISH" == ON ]]; then
    "$CMAKE_BIN" "-DJAM2_PACKAGE_CONFIG=$REPO_DIR/build/release-package.cmake" \
        "-DJAM2_INCLUDE_MODELS=$INCLUDE_MODELS" -P "$REPO_DIR/tools/release/PublishRelease.cmake"
fi
