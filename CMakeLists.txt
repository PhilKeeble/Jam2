cmake_minimum_required(VERSION 3.24)

project(Jam2
    VERSION 0.1.0
    DESCRIPTION "Two-person low-latency direct UDP music streaming tool"
    LANGUAGES CXX
)

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Jam2 only supports Release builds" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Jam2 build type" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "Jam2 only supports Release builds. Reconfigure with -DCMAKE_BUILD_TYPE=Release or use a fresh build directory.")
    endif()
endif()

option(JAM2_BUILD_TESTS "Build Jam2 tests" ON)
option(JAM2_BUILD_GUI "Build jam2-gui when its CMake target is present" ON)
option(JAM2_BUILD_CAPTURE "Build jam2-capture when its CMake target is present" ON)

set(JAM2_RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release" CACHE PATH "Directory for distributable app binaries")
set(JAM2_BUILD_BIN_DIR "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Directory for built app binaries before release staging")

function(jam2_create_release_dirs target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${JAM2_RELEASE_DIR}"
            "${JAM2_RELEASE_DIR}/logs"
            "${JAM2_RELEASE_DIR}/captures"
            "${JAM2_RELEASE_DIR}/jam_recordings"
            "${JAM2_RELEASE_DIR}/songs"
            "${JAM2_RELEASE_DIR}/received_tracks"
        VERBATIM
    )
endfunction()

function(jam2_stage_release_binary target)
    jam2_create_release_dirs(${target})
    if(APPLE AND target STREQUAL "jam2-gui")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E remove_directory
                "${JAM2_RELEASE_DIR}/jam2-gui.app"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "$<TARGET_BUNDLE_DIR:${target}>"
                "${JAM2_RELEASE_DIR}/jam2-gui.app"
            VERBATIM
        )
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${target}>"
                "${JAM2_RELEASE_DIR}/$<TARGET_FILE_NAME:${target}>"
            VERBATIM
        )
    endif()
endfunction()

function(jam2_set_app_output target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${JAM2_BUILD_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${JAM2_BUILD_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${JAM2_BUILD_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${JAM2_BUILD_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${JAM2_BUILD_BIN_DIR}"
    )
    jam2_stage_release_binary(${target})
endfunction()

add_subdirectory(libs/jam2-core)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${JAM2_BUILD_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${JAM2_BUILD_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${JAM2_BUILD_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${JAM2_BUILD_BIN_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${JAM2_BUILD_BIN_DIR}")

add_subdirectory(apps/jam2-cli)

if(JAM2_BUILD_CAPTURE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/jam2-capture/CMakeLists.txt")
    add_subdirectory(apps/jam2-capture)
endif()

if(JAM2_BUILD_GUI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/jam2-gui/CMakeLists.txt")
    add_subdirectory(apps/jam2-gui)
endif()

unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL)

include(CTest)

if(JAM2_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
