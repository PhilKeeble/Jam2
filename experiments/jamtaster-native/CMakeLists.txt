cmake_minimum_required(VERSION 3.24)

project(JamTasterNativeLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JAM2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(JAMTASTER_NATIVE_DEPS "${CMAKE_CURRENT_LIST_DIR}/.deps" CACHE PATH
    "Developer-only native dependency root")
set(JAMTASTER_ONNXRUNTIME_ROOT "${JAMTASTER_NATIVE_DEPS}/onnxruntime" CACHE PATH
    "Extracted ONNX Runtime package")

if(NOT EXISTS "${JAMTASTER_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR
        "Set JAMTASTER_ONNXRUNTIME_ROOT to an ONNX Runtime 1.23.2 SDK package")
endif()

find_library(JAMTASTER_ONNXRUNTIME_LIBRARY
    NAMES onnxruntime onnxruntime.1.23.2
    PATHS "${JAMTASTER_ONNXRUNTIME_ROOT}/lib"
    NO_DEFAULT_PATH REQUIRED)

add_library(jamtaster_onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(jamtaster_onnxruntime PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${JAMTASTER_ONNXRUNTIME_ROOT}/include")
if(WIN32)
    set_target_properties(jamtaster_onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${JAMTASTER_ONNXRUNTIME_LIBRARY}"
        IMPORTED_LOCATION "${JAMTASTER_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
else()
    set_target_properties(jamtaster_onnxruntime PROPERTIES
        IMPORTED_LOCATION "${JAMTASTER_ONNXRUNTIME_LIBRARY}")
endif()

add_library(jamtaster_native_core STATIC
    src/Adtof.cpp
    src/BasicPitch.cpp
    src/BeatThis.cpp
    src/ChordMini.cpp
    src/Dsp.cpp
    src/Export.cpp
    src/Hash.cpp
    src/OnnxModel.cpp
    src/Pipeline.cpp
    src/Postprocess.cpp
    src/Wav.cpp
)
target_include_directories(jamtaster_native_core PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_include_directories(jamtaster_native_core SYSTEM PUBLIC
    "${JAM2_ROOT}/libs/third_party"
    "${JAM2_ROOT}/libs/third_party/signalsmith-linear"
    "${JAM2_ROOT}/libs/third_party/signalsmith-stretch"
)
target_link_libraries(jamtaster_native_core PUBLIC jamtaster_onnxruntime)

option(JAMTASTER_NATIVE_ENABLE_DEMUCS
    "Build JamTaster's vendored native HTDemucs adapter" ON)
set(JAMTASTER_DEMUCS_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/demucs_onnx")
set(JAMTASTER_EIGEN_ROOT "${JAMTASTER_NATIVE_DEPS}/eigen" CACHE PATH
    "Eigen 3.4 source root")
if(JAMTASTER_NATIVE_ENABLE_DEMUCS
   AND EXISTS "${JAMTASTER_DEMUCS_ROOT}/demucs.hpp"
   AND EXISTS "${JAMTASTER_EIGEN_ROOT}/Eigen/Dense")
    target_sources(jamtaster_native_core PRIVATE
        src/DemucsAdapter.cpp
        "${JAMTASTER_DEMUCS_ROOT}/dsp.cpp"
        "${JAMTASTER_DEMUCS_ROOT}/model_apply.cpp"
        "${JAMTASTER_DEMUCS_ROOT}/model_inference.cpp"
    )
    target_include_directories(jamtaster_native_core PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/compat"
        "${JAMTASTER_DEMUCS_ROOT}"
    )
    target_include_directories(jamtaster_native_core SYSTEM PRIVATE
        "${JAMTASTER_EIGEN_ROOT}"
    )
    target_compile_definitions(jamtaster_native_core PUBLIC
        JAMTASTER_NATIVE_HAS_DEMUCS=1)
else()
    target_compile_definitions(jamtaster_native_core PUBLIC
        JAMTASTER_NATIVE_HAS_DEMUCS=0)
    message(STATUS "Demucs native adapter disabled: set JAMTASTER_EIGEN_ROOT to Eigen 3.4")
endif()

add_executable(jamtaster_native_lab src/Main.cpp)
target_link_libraries(jamtaster_native_lab PRIVATE jamtaster_native_core)

add_executable(jamtaster_native_tests tests/NativeTests.cpp)
target_link_libraries(jamtaster_native_tests PRIVATE jamtaster_native_core)
if(JAMTASTER_NATIVE_ENABLE_DEMUCS
   AND EXISTS "${JAMTASTER_DEMUCS_ROOT}/demucs.hpp"
   AND EXISTS "${JAMTASTER_EIGEN_ROOT}/Eigen/Dense")
    target_include_directories(jamtaster_native_tests SYSTEM PRIVATE
        "${JAMTASTER_EIGEN_ROOT}")
endif()

if(MSVC)
    target_compile_options(jamtaster_native_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(jamtaster_native_lab PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(jamtaster_native_tests PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jamtaster_native_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(jamtaster_native_lab PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(jamtaster_native_tests PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(jamtaster_native_lab jamtaster_native_tests PROPERTIES
        BUILD_RPATH "${JAMTASTER_ONNXRUNTIME_ROOT}/lib")
endif()

if(WIN32)
    foreach(target jamtaster_native_lab jamtaster_native_tests)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${JAMTASTER_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endif()

enable_testing()
add_test(NAME jamtaster_native_units COMMAND jamtaster_native_tests)
