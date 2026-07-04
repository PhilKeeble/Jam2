cmake_minimum_required(VERSION 3.24)

project(Jam2
    VERSION 0.1.0
    DESCRIPTION "Two-person low-latency direct UDP music streaming tool"
    LANGUAGES CXX
)

option(JAM2_BUILD_TESTS "Build Jam2 tests" ON)
option(JAM2_BUILD_GUI "Build jam2-gui when its CMake target is present" ON)
option(JAM2_BUILD_CAPTURE "Build jam2-capture when its CMake target is present" ON)

set(JAM2_RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release" CACHE PATH "Directory for distributable app binaries")

function(jam2_set_app_output target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${JAM2_RELEASE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${JAM2_RELEASE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${JAM2_RELEASE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${JAM2_RELEASE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${JAM2_RELEASE_DIR}"
    )
endfunction()

add_subdirectory(libs/jam2-core)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${JAM2_RELEASE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${JAM2_RELEASE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${JAM2_RELEASE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${JAM2_RELEASE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${JAM2_RELEASE_DIR}")

add_subdirectory(apps/jam2-cli)

if(JAM2_BUILD_GUI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/jam2-gui/CMakeLists.txt")
    add_subdirectory(apps/jam2-gui)
endif()

if(JAM2_BUILD_CAPTURE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/jam2-capture/CMakeLists.txt")
    add_subdirectory(apps/jam2-capture)
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
