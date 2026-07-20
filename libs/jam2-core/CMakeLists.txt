set(JAM2_AUBIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/aubio")

add_library(aubio_pitch_minimal STATIC
    "${JAM2_AUBIO_ROOT}/src/cvec.c"
    "${JAM2_AUBIO_ROOT}/src/fvec.c"
    "${JAM2_AUBIO_ROOT}/src/mathutils.c"
    "${JAM2_AUBIO_ROOT}/src/pitch/pitchyinfft.c"
    "${JAM2_AUBIO_ROOT}/src/spectral/fft.c"
    "${JAM2_AUBIO_ROOT}/src/spectral/ooura_fft8g.c"
    "${JAM2_AUBIO_ROOT}/src/utils/log.c"
)

set_target_properties(aubio_pitch_minimal PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_include_directories(aubio_pitch_minimal PUBLIC
    "${JAM2_AUBIO_ROOT}/src"
)

target_compile_definitions(aubio_pitch_minimal PRIVATE
    HAVE_C99_VARARGS_MACROS=1
    HAVE_ERRNO_H=1
    HAVE_LIMITS_H=1
    HAVE_MATH_H=1
    HAVE_STDARG_H=1
    HAVE_STDINT_H=1
    HAVE_STDIO_H=1
    HAVE_STDLIB_H=1
    HAVE_STRING_H=1
)

if(MSVC)
    target_compile_definitions(aubio_pitch_minimal PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

add_library(jam2_core STATIC
    src/audio_ring.cpp
    src/common.cpp
    src/metronome.cpp
    src/engine.cpp
    src/network_capture.cpp
    src/network_session.cpp
    src/output_recorder.cpp
    src/peer_mixer.cpp
    src/peer_stream.cpp
    src/pcm16_wav.cpp
    src/pitch_analyzer.cpp
    src/prepared_track_source.cpp
    src/protocol.cpp
    src/session_authority.cpp
    src/track_take_recorder.cpp
    src/stun.cpp
    src/tuning_profile.cpp
    src/udp_socket.cpp
)

target_link_libraries(jam2_core PRIVATE aubio_pitch_minimal)

target_compile_features(jam2_core PUBLIC cxx_std_20)

set_target_properties(jam2_core PROPERTIES
    CXX_EXTENSIONS OFF
)

target_include_directories(jam2_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include/jam2
)

if(MSVC)
    target_compile_options(jam2_core PRIVATE
        /W4
        /permissive-
        /EHsc
    )
else()
    target_compile_options(jam2_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(WIN32)
    if(DEFINED ASIO_SDK_DIR)
        set(_jam2_asio_sdk_dir "${ASIO_SDK_DIR}")
    elseif(DEFINED ENV{ASIO_SDK_DIR})
        set(_jam2_asio_sdk_dir "$ENV{ASIO_SDK_DIR}")
    else()
        set(_jam2_asio_sdk_dir "C:/Tools/ASIO-SDK_2.3.4/ASIOSDK")
    endif()
    set(ASIO_SDK_DIR "${_jam2_asio_sdk_dir}" CACHE PATH "Path to Steinberg ASIO SDK" FORCE)
    unset(_jam2_asio_sdk_dir)
    if(EXISTS "${ASIO_SDK_DIR}/common/asio.h")
        target_include_directories(jam2_core PUBLIC
            "${ASIO_SDK_DIR}/common"
            "${ASIO_SDK_DIR}/host"
            "${ASIO_SDK_DIR}/host/pc"
        )
    else()
        message(WARNING "ASIO SDK not found at ${ASIO_SDK_DIR}; device enumeration can still read registry entries, but ASIO opening will need the SDK.")
    endif()
    target_sources(jam2_core PRIVATE src/audio_device_windows.cpp)
    target_compile_definitions(jam2_core PUBLIC JAM2_PLATFORM_WINDOWS=1)
    target_link_libraries(jam2_core PUBLIC ws2_32 advapi32 ole32 bcrypt)
elseif(APPLE)
    target_sources(jam2_core PRIVATE src/audio_device_macos.cpp)
    target_compile_definitions(jam2_core PUBLIC JAM2_PLATFORM_MACOS=1)
    target_link_libraries(jam2_core PUBLIC
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
    )
else()
    message(FATAL_ERROR "Jam2 supports only Windows and macOS host-native builds.")
endif()
