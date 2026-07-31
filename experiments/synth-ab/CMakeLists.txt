cmake_minimum_required(VERSION 3.24)

project(Jam2SoundLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTORCC ON)

set(JAM2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(DAISYSP_ROOT "${JAM2_ROOT}/libs/third_party/DaisySP")

if(NOT EXISTS "${DAISYSP_ROOT}/Source/Drums/synthbassdrum.cpp")
    message(FATAL_ERROR
        "The vendored DaisySP sources are missing from "
        "libs/third_party/DaisySP.")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core)

include("${JAM2_ROOT}/cmake/PrepareDaisySPDrums.cmake")
jam2_prepare_daisysp_synthbassdrum(
    "${DAISYSP_ROOT}"
    JAM2_DAISYSP_SYNTHBASSDRUM_SOURCE)

add_library(daisysp_experiment STATIC
    "${DAISYSP_ROOT}/Source/Control/adsr.cpp"
    "${DAISYSP_ROOT}/Source/Drums/analogbassdrum.cpp"
    "${DAISYSP_ROOT}/Source/Drums/analogsnaredrum.cpp"
    "${DAISYSP_ROOT}/Source/Drums/hihat.cpp"
    "${JAM2_DAISYSP_SYNTHBASSDRUM_SOURCE}"
    "${DAISYSP_ROOT}/Source/Drums/synthsnaredrum.cpp"
    "${DAISYSP_ROOT}/Source/Dynamics/crossfade.cpp"
    "${DAISYSP_ROOT}/Source/Effects/chorus.cpp"
    "${DAISYSP_ROOT}/Source/Effects/overdrive.cpp"
    "${DAISYSP_ROOT}/Source/Effects/wavefolder.cpp"
    "src/DaisyLadderCompatibility.cpp"
    "${DAISYSP_ROOT}/Source/Filters/svf.cpp"
    "${DAISYSP_ROOT}/Source/Noise/particle.cpp"
    "${DAISYSP_ROOT}/Source/PhysicalModeling/KarplusString.cpp"
    "${DAISYSP_ROOT}/Source/PhysicalModeling/modalvoice.cpp"
    "${DAISYSP_ROOT}/Source/PhysicalModeling/resonator.cpp"
    "${DAISYSP_ROOT}/Source/PhysicalModeling/stringvoice.cpp"
    "${DAISYSP_ROOT}/Source/Synthesis/fm2.cpp"
    "${DAISYSP_ROOT}/Source/Synthesis/formantosc.cpp"
    "${DAISYSP_ROOT}/Source/Synthesis/oscillator.cpp"
    "${DAISYSP_ROOT}/Source/Synthesis/variablesawosc.cpp"
    "${DAISYSP_ROOT}/Source/Synthesis/variableshapeosc.cpp"
    "${DAISYSP_ROOT}/Source/Synthesis/vosim.cpp"
    "${DAISYSP_ROOT}/Source/Synthesis/zoscillator.cpp"
    "${DAISYSP_ROOT}/Source/Utility/dcblock.cpp"
)
target_include_directories(daisysp_experiment PUBLIC
    "${DAISYSP_ROOT}/Source"
    "${DAISYSP_ROOT}/Source/Utility"
)
target_compile_definitions(daisysp_experiment PRIVATE _USE_MATH_DEFINES)

add_executable(jam2_sound_lab
    src/DaisyShowcase.cpp
    src/SoundDesignMain.cpp
    src/BeatGridModelShim.cpp
    "${JAM2_ROOT}/app/gui/GenerationRecipe.cpp"
    "${JAM2_ROOT}/app/gui/MusicTheory.cpp"
    "${JAM2_ROOT}/app/gui/PracticeIdeaGenerator.cpp"
    "${JAM2_ROOT}/app/gui/PracticeReferenceRenderer.cpp"
    "${JAM2_ROOT}/app/gui/ResearchDrumEngine.cpp"
    "${JAM2_ROOT}/app/gui/ResearchDrumKit.cpp"
    "${JAM2_ROOT}/app/gui/StyleProfileCatalog.cpp"
    ResearchDrums.qrc
)

target_include_directories(jam2_sound_lab PRIVATE
    "${JAM2_ROOT}/app/application"
    "${JAM2_ROOT}/app/gui"
)
target_link_libraries(jam2_sound_lab PRIVATE
    Qt6::Core
    daisysp_experiment
)

if(MSVC)
    target_compile_options(jam2_sound_lab PRIVATE /W4 /permissive-)
    target_compile_options(daisysp_experiment PRIVATE /W0)
else()
    target_compile_options(jam2_sound_lab PRIVATE -Wall -Wextra -Wpedantic)
endif()
