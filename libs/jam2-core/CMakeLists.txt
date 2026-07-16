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
    src/prepared_track_source.cpp
    src/protocol.cpp
    src/session_authority.cpp
    src/track_take_recorder.cpp
    src/stun.cpp
    src/tuning_profile.cpp
    src/udp_socket.cpp
)

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
