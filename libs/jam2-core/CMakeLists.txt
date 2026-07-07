add_library(jam2_core STATIC
    src/audio_ring.cpp
    src/common.cpp
    src/metronome.cpp
    src/output_recorder.cpp
    src/protocol.cpp
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
    if(DEFINED ENV{ASIO_SDK_DIR})
        set(JAM2_ASIO_SDK_DIR "$ENV{ASIO_SDK_DIR}" CACHE PATH "Path to Steinberg ASIO SDK")
    else()
        set(JAM2_ASIO_SDK_DIR "C:/Tools/ASIO-SDK_2.3.4/ASIOSDK" CACHE PATH "Path to Steinberg ASIO SDK")
    endif()
    if(EXISTS "${JAM2_ASIO_SDK_DIR}/common/asio.h")
        target_include_directories(jam2_core PUBLIC
            "${JAM2_ASIO_SDK_DIR}/common"
            "${JAM2_ASIO_SDK_DIR}/host"
            "${JAM2_ASIO_SDK_DIR}/host/pc"
        )
    else()
        message(WARNING "ASIO SDK not found at ${JAM2_ASIO_SDK_DIR}; device enumeration can still read registry entries, but ASIO opening will need the SDK.")
    endif()
    target_sources(jam2_core PRIVATE src/audio_device_windows.cpp)
    target_compile_definitions(jam2_core PUBLIC JAM2_PLATFORM_WINDOWS=1)
    target_link_libraries(jam2_core PUBLIC ws2_32 advapi32 ole32)
elseif(APPLE)
    target_sources(jam2_core PRIVATE src/audio_device_macos.cpp)
    target_compile_definitions(jam2_core PUBLIC JAM2_PLATFORM_MACOS=1)
    target_link_libraries(jam2_core PUBLIC
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
    )
else()
    message(FATAL_ERROR "Jam2 MVP supports only Windows and macOS host-native builds.")
endif()
