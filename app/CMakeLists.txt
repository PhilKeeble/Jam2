if(WIN32)
    file(GLOB JAM2_QT_PREFIX_HINTS
        "C:/Qt/6.*/msvc*_64"
        "C:/Qt/6.*/mingw*_64"
    )
    list(APPEND CMAKE_PREFIX_PATH ${JAM2_QT_PREFIX_HINTS})
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Network)

if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "The Jam2 GUI requires Qt 6 Widgets and Network development packages.\n"
        "Install Qt 6, then reconfigure with a Qt prefix, for example:\n"
        "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64\n"
        "If your Qt version/toolchain folder differs, point CMAKE_PREFIX_PATH at that folder."
    )
endif()

add_library(jam2_automation_contract STATIC
    application/AutomationActionRegistry.cpp
    application/AutomationActionRegistry.hpp
    application/DebugActionValidation.cpp
    application/DebugActionValidation.hpp)
target_compile_features(jam2_automation_contract PUBLIC cxx_std_20)
target_include_directories(jam2_automation_contract PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/application")
target_link_libraries(jam2_automation_contract PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(jam2_automation_contract PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_automation_contract PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(jam2_jam_sync STATIC
    application/JamSyncPolicy.cpp
    application/JamSyncPolicy.hpp)
target_compile_features(jam2_jam_sync PUBLIC cxx_std_20)
target_include_directories(jam2_jam_sync PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/application")
target_link_libraries(jam2_jam_sync PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(jam2_jam_sync PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_jam_sync PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(jam2_midi_input_backend STATIC
    application/MidiInputBackend.cpp
    application/MidiInputBackend.hpp)
target_compile_features(jam2_midi_input_backend PUBLIC cxx_std_20)
target_include_directories(jam2_midi_input_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/application")
target_link_libraries(jam2_midi_input_backend PUBLIC jam2_core)
if(MSVC)
    target_compile_options(jam2_midi_input_backend PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_midi_input_backend PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(jam2_looper_model STATIC
    gui/ConcurrentLooperMerge.cpp
    gui/ConcurrentLooperMerge.hpp
    gui/LooperProject.cpp
    gui/LooperProject.hpp)
target_compile_features(jam2_looper_model PUBLIC cxx_std_20)
target_include_directories(jam2_looper_model PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/application"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui")
target_link_libraries(jam2_looper_model PUBLIC jam2_core Qt6::Core)
if(MSVC)
    target_compile_options(jam2_looper_model PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_looper_model PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(jam2_prepared_mix_lifecycle STATIC
    gui/PreparedMixLifecycle.cpp
    gui/PreparedMixLifecycle.hpp)
target_compile_features(jam2_prepared_mix_lifecycle PUBLIC cxx_std_20)
target_include_directories(jam2_prepared_mix_lifecycle PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/gui")
target_link_libraries(jam2_prepared_mix_lifecycle PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(jam2_prepared_mix_lifecycle PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_prepared_mix_lifecycle PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(jam2_asset_transfer STATIC
    application/AssetChunkProtocol.cpp
    application/AssetChunkProtocol.hpp
    gui/TrackAssetOwnership.cpp
    gui/TrackAssetOwnership.hpp
    gui/AssetTransferService.cpp
    gui/AssetTransferService.hpp)
target_compile_features(jam2_asset_transfer PUBLIC cxx_std_20)
target_include_directories(jam2_asset_transfer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/application"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui")
target_link_libraries(jam2_asset_transfer PUBLIC jam2_core Qt6::Core)
if(MSVC)
    target_compile_options(jam2_asset_transfer PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_asset_transfer PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Build the maintained VST3 SDK checkout directly into Jam2's private plugin
# helper. The public jam2 process never links this target or loads plugins.
set(JAM2_VST3SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/vst3sdk")
if(NOT EXISTS "${JAM2_VST3SDK_ROOT}/LICENSE.txt")
    message(FATAL_ERROR "The maintained VST3 SDK sources are missing")
endif()
set(JAM2_VST3SDK_SOURCES
    "${JAM2_VST3SDK_ROOT}/base/source/baseiids.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/fbuffer.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/fdebug.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/fdynlib.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/fobject.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/fstreamer.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/fstring.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/timer.cpp"
    "${JAM2_VST3SDK_ROOT}/base/source/updatehandler.cpp"
    "${JAM2_VST3SDK_ROOT}/pluginterfaces/base/conststringtable.cpp"
    "${JAM2_VST3SDK_ROOT}/pluginterfaces/base/coreiids.cpp"
    "${JAM2_VST3SDK_ROOT}/pluginterfaces/base/funknown.cpp"
    "${JAM2_VST3SDK_ROOT}/pluginterfaces/base/ustring.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/common/commoniids.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/processdata.cpp"
    "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp")
if(WIN32)
    list(APPEND JAM2_VST3SDK_SOURCES
        "${JAM2_VST3SDK_ROOT}/public.sdk/source/common/threadchecker_win32.cpp"
        "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp")
elseif(APPLE)
    enable_language(OBJCXX)
    set(JAM2_VST3SDK_MAC_MODULE
        "${JAM2_VST3SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm")
    list(APPEND JAM2_VST3SDK_SOURCES
        "${JAM2_VST3SDK_ROOT}/public.sdk/source/common/threadchecker_mac.mm"
        "${JAM2_VST3SDK_MAC_MODULE}")
    # Steinberg's macOS module loader uses ARC by contract and deliberately
    # rejects non-ARC compilation. Keep ARC scoped to that maintained SDK
    # translation unit; Jam2's editor host retains its explicit RAII/manual
    # ownership model.
    set_source_files_properties("${JAM2_VST3SDK_MAC_MODULE}" PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
endif()
add_library(jam2_vst3sdk STATIC ${JAM2_VST3SDK_SOURCES})
target_compile_features(jam2_vst3sdk PUBLIC cxx_std_20)
target_include_directories(jam2_vst3sdk SYSTEM PUBLIC "${JAM2_VST3SDK_ROOT}")
target_compile_definitions(jam2_vst3sdk PUBLIC SMTG_CPP20=1)
if(MSVC)
    target_compile_options(jam2_vst3sdk PRIVATE /W0 /permissive- /EHsc)
endif()
if(WIN32)
    target_link_libraries(jam2_vst3sdk PUBLIC ole32 shell32)
elseif(APPLE)
    target_link_libraries(jam2_vst3sdk PUBLIC
        "-framework Cocoa" "-framework CoreFoundation")
endif()

add_library(jam2_vst3host STATIC pluginhost/Vst3Host.cpp pluginhost/Vst3Host.hpp)
if(APPLE)
    set_source_files_properties(pluginhost/Vst3Host.cpp PROPERTIES LANGUAGE OBJCXX)
endif()
target_compile_features(jam2_vst3host PUBLIC cxx_std_20)
target_include_directories(jam2_vst3host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pluginhost")
target_link_libraries(jam2_vst3host PRIVATE jam2_vst3sdk)
add_library(jam2_plugin_transport STATIC
    pluginhost/PluginProtocol.hpp
    pluginhost/PluginSharedMemory.hpp)
if(WIN32)
    target_sources(jam2_plugin_transport PRIVATE pluginhost/PluginSharedMemoryWindows.cpp)
elseif(APPLE)
    target_sources(jam2_plugin_transport PRIVATE pluginhost/PluginSharedMemoryMac.cpp)
endif()
target_compile_features(jam2_plugin_transport PUBLIC cxx_std_20)
target_include_directories(jam2_plugin_transport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pluginhost")

add_library(jam2_plugin_client STATIC
    pluginhost/PluginAudioBridge.cpp
    pluginhost/PluginAudioBridge.hpp
    pluginhost/PluginHostService.cpp
    pluginhost/PluginHostService.hpp)
target_compile_features(jam2_plugin_client PUBLIC cxx_std_20)
target_include_directories(jam2_plugin_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pluginhost")
target_link_libraries(jam2_plugin_client PUBLIC jam2_core Qt6::Core PRIVATE jam2_plugin_transport)

add_library(jam2_input_plugin_backend STATIC
    application/InputPluginBackend.cpp
    application/InputPluginBackend.hpp)
target_compile_features(jam2_input_plugin_backend PUBLIC cxx_std_20)
target_include_directories(jam2_input_plugin_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/application")
target_link_libraries(jam2_input_plugin_backend PUBLIC
    jam2_core jam2_plugin_client Qt6::Widgets)

add_executable(jam2_plugin_worker pluginhost/PluginWorkerMain.cpp)
set_target_properties(jam2_plugin_worker PROPERTIES OUTPUT_NAME "jam2-plugin-worker")
target_compile_features(jam2_plugin_worker PRIVATE cxx_std_20)
target_link_libraries(jam2_plugin_worker PRIVATE jam2_vst3host jam2_vst3sdk jam2_plugin_transport)
if(MSVC)
    target_compile_options(jam2_vst3host PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(jam2_plugin_transport PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(jam2_plugin_client PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(jam2_input_plugin_backend PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(jam2_plugin_worker PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_vst3host PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(jam2_plugin_transport PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(jam2_plugin_client PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(jam2_input_plugin_backend PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(jam2_plugin_worker PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(JAM2_DAISYSP_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/DaisySP")
if(NOT EXISTS "${JAM2_DAISYSP_ROOT}/Source/Drums/synthbassdrum.cpp")
    message(FATAL_ERROR
        "The vendored DaisySP drum sources are missing from "
        "libs/third_party/DaisySP. Restore the repository checkout, then "
        "configure again.")
endif()

add_library(jam2_daisysp_drums STATIC
    "${JAM2_DAISYSP_ROOT}/Source/Control/adsr.cpp"
    "${JAM2_DAISYSP_ROOT}/Source/Drums/analogbassdrum.cpp"
    "${JAM2_DAISYSP_ROOT}/Source/Drums/analogsnaredrum.cpp"
    "${JAM2_DAISYSP_ROOT}/Source/Drums/hihat.cpp"
    "${JAM2_DAISYSP_ROOT}/Source/Drums/synthbassdrum.cpp"
    "${JAM2_DAISYSP_ROOT}/Source/Drums/synthsnaredrum.cpp"
    "${JAM2_DAISYSP_ROOT}/Source/Filters/svf.cpp"
    "${JAM2_DAISYSP_ROOT}/Source/Synthesis/oscillator.cpp"
)
target_include_directories(jam2_daisysp_drums PUBLIC
    "${JAM2_DAISYSP_ROOT}/Source"
    "${JAM2_DAISYSP_ROOT}/Source/Utility"
)
target_compile_definitions(jam2_daisysp_drums PRIVATE _USE_MATH_DEFINES)
target_compile_features(jam2_daisysp_drums PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(jam2_daisysp_drums PRIVATE /W0)
endif()

set(CMAKE_AUTORCC ON)

set(JAMTASTER_ONNXRUNTIME_ROOT "" CACHE PATH
    "Platform ONNX Runtime 1.23.2 package used by the bundled JamTaster worker")
if(NOT JAMTASTER_ONNXRUNTIME_ROOT)
    if(WIN32)
        set(JAMTASTER_ONNXRUNTIME_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/windows-x64")
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            set(JAMTASTER_ONNXRUNTIME_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/macos-arm64")
        else()
            set(JAMTASTER_ONNXRUNTIME_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/macos-x64")
        endif()
    endif()
endif()
set(JAMTASTER_ONNXRUNTIME_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/include")
if(NOT EXISTS "${JAMTASTER_ONNXRUNTIME_INCLUDE}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "The vendored ONNX Runtime headers are missing")
endif()
find_library(JAMTASTER_ONNXRUNTIME_LIBRARY
    NAMES onnxruntime onnxruntime.1.23.2
    PATHS "${JAMTASTER_ONNXRUNTIME_ROOT}/lib"
    NO_DEFAULT_PATH REQUIRED)
add_library(jamtaster_onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(jamtaster_onnxruntime PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${JAMTASTER_ONNXRUNTIME_INCLUDE}")
if(WIN32)
    set_target_properties(jamtaster_onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${JAMTASTER_ONNXRUNTIME_LIBRARY}"
        IMPORTED_LOCATION "${JAMTASTER_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
else()
    set_target_properties(jamtaster_onnxruntime PROPERTIES
        IMPORTED_LOCATION "${JAMTASTER_ONNXRUNTIME_LIBRARY}")
endif()

set(JAMTASTER_SOURCES
    jamtaster/Adtof.cpp
    jamtaster/BasicPitch.cpp
    jamtaster/BeatThis.cpp
    jamtaster/ChordMini.cpp
    jamtaster/DemucsAdapter.cpp
    jamtaster/Dsp.cpp
    jamtaster/Export.cpp
    jamtaster/Hash.cpp
    jamtaster/OnnxModel.cpp
    jamtaster/Pipeline.cpp
    jamtaster/Postprocess.cpp
    jamtaster/Wav.cpp
    jamtaster/third_party/demucs_onnx/dsp.cpp
    jamtaster/third_party/demucs_onnx/model_apply.cpp
    jamtaster/third_party/demucs_onnx/model_inference.cpp
)
add_library(jamtaster_core STATIC ${JAMTASTER_SOURCES})
target_compile_features(jamtaster_core PUBLIC cxx_std_20)
target_include_directories(jamtaster_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster")
target_include_directories(jamtaster_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/third_party/demucs_onnx")
target_include_directories(jamtaster_core SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/signalsmith-linear"
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/signalsmith-stretch")
target_include_directories(jamtaster_core SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/eigen")
target_compile_definitions(jamtaster_core PUBLIC JAMTASTER_NATIVE_HAS_DEMUCS=1)
target_link_libraries(jamtaster_core PUBLIC jamtaster_onnxruntime)

add_executable(jamtaster_worker jamtaster/WorkerMain.cpp)
set_target_properties(jamtaster_worker PROPERTIES OUTPUT_NAME "jamtaster-worker")
target_compile_features(jamtaster_worker PRIVATE cxx_std_20)
target_link_libraries(jamtaster_worker PRIVATE jamtaster_core)

if(MSVC)
    target_compile_options(jamtaster_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(jamtaster_worker PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jamtaster_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(jamtaster_worker PRIVATE -Wall -Wextra -Wpedantic)
    if(APPLE)
        set_target_properties(jamtaster_worker PROPERTIES
            BUILD_RPATH "${JAMTASTER_ONNXRUNTIME_ROOT}/lib;@executable_path/../Frameworks")
    else()
        set_target_properties(jamtaster_worker PROPERTIES
            BUILD_RPATH "${JAMTASTER_ONNXRUNTIME_ROOT}/lib")
    endif()
endif()

set(JAMTASTER_MODEL_FILES
    adtof.onnx
    adtof.onnx.json
    basic_pitch.onnx
    basic_pitch.onnx.json
    beat_this.onnx
    beat_this.onnx.json
    chordmini_btc.onnx
    chordmini_btc.onnx.json
    htdemucs_ft_0.onnx
    htdemucs_ft_0.onnx.json
    htdemucs_ft_1.onnx
    htdemucs_ft_1.onnx.json
    htdemucs_ft_2.onnx
    htdemucs_ft_2.onnx.json
    htdemucs_ft_3.onnx
    htdemucs_ft_3.onnx.json)
foreach(model IN LISTS JAMTASTER_MODEL_FILES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/models/${model}")
        message(FATAL_ERROR "The bundled JamTaster model is missing: ${model}")
    endif()
endforeach()

set(JAM2_GUI_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/platform/assets/jam2.ico")
set(JAM2_GUI_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/platform/assets/jam2.icns")

if(WIN32)
    enable_language(RC)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/Jam2.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Jam2.rc"
        @ONLY
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_BINARY_DIR}/Jam2.rc"
        PROPERTIES OBJECT_DEPENDS "${JAM2_GUI_WINDOWS_ICON}"
    )
endif()

# The unified Windows binary uses the console subsystem so shells wait for
# command-line invocations. The no-argument GUI path detaches immediately.
add_executable(jam2 MACOSX_BUNDLE
    Jam2Resources.qrc
    application/ApplicationEntrypoint.cpp
    application/ApplicationEntrypoint.hpp
    application/ApplicationRuntime.cpp
    application/ApplicationRuntime.hpp
    application/BoundaryValidation.cpp
    application/BoundaryValidation.hpp
    application/AutomationChannel.cpp
    application/AutomationChannel.hpp
    application/ControlClient.cpp
    application/ControlClient.hpp
    application/ContentLimits.hpp
    application/ControlMessageValidation.cpp
    application/ControlMessageValidation.hpp
    application/ControllerLifecycleValidation.cpp
    application/ControllerLifecycleValidation.hpp
    application/DebugAutomation.cpp
    application/DebugAutomation.hpp
    application/GuiTestAgent.cpp
    application/GuiTestAgent.hpp
    application/MusicCorpusDiagnostic.cpp
    application/MusicCorpusDiagnostic.hpp
    application/NativeTcpTransport.cpp
    application/NativeTcpTransport.hpp
    application/RuntimeHost.cpp
    application/ControlProtocol.cpp
    application/ControlProtocol.hpp
    application/ControlServer.cpp
    application/ControlServer.hpp
    application/RuntimeContracts.hpp
    application/SessionController.cpp
    application/SessionController.hpp
    application/SharedSessionController.cpp
    application/SharedSessionController.hpp
    cli/CliOptions.cpp
    cli/CliOptions.hpp
    cli/CliRecordingSupervisor.cpp
    cli/CliRecordingSupervisor.hpp
    cli/CliRuntimeSupport.hpp
    cli/CliDispatch.cpp
    cli/CliRuntime.hpp
    cli/CliStats.cpp
    cli/CliStats.hpp
    cli/CliEntrypoint.hpp
    cli/NetworkRuntime.cpp
    gui/ArrangementEditorDialog.cpp
    gui/ArrangementEditorDialog.hpp
    gui/BeatGridModel.cpp
    gui/BeatGridModel.hpp
    gui/BeatGridWidget.cpp
    gui/BeatGridWidget.hpp
    gui/CuratedIdeaCatalog.cpp
    gui/CuratedIdeaCatalog.hpp
    gui/CuratedIdeaDialog.cpp
    gui/CuratedIdeaDialog.hpp
    gui/GuiLoopbackRecorder.cpp
    gui/GuiLoopbackRecorder.hpp
    gui/GuiControlMessageRouter.cpp
    gui/GuiControlMessageRouter.hpp
    gui/GuiControlContract.cpp
    gui/GuiControlContract.hpp
    gui/GuiPresentation.cpp
    gui/GuiPresentation.hpp
    gui/InputSourceDialogs.cpp
    gui/InputSourceDialogs.hpp
    gui/JamStorage.cpp
    gui/JamStorage.hpp
    gui/JamSyncDialog.cpp
    gui/JamSyncDialog.hpp
    gui/JamTasterDialog.cpp
    gui/JamTasterDialog.hpp
    gui/LaneRecordingDialog.cpp
    gui/LaneRecordingDialog.hpp
    gui/ListenerCompensationDialog.cpp
    gui/ListenerCompensationDialog.hpp
    gui/LooperAssetMaterializer.cpp
    gui/LooperAssetMaterializer.hpp
    jamtaster/JamTasterService.cpp
    jamtaster/JamTasterService.hpp
    gui/GenerationRecipe.cpp
    gui/GenerationRecipe.hpp
    gui/DetailSectionEdit.cpp
    gui/DetailSectionEdit.hpp
    gui/MainWindow.cpp
    gui/MainWindow.hpp
    gui/MainWindowPages.cpp
    gui/MainWindowPages.hpp
    gui/SessionStartupDialogs.cpp
    gui/SessionStartupDialogs.hpp
    gui/SettingsDialog.cpp
    gui/SettingsDialog.hpp
    gui/MetronomeTransportController.cpp
    gui/MetronomeTransportController.hpp
    gui/MixerStatsViewModel.cpp
    gui/MixerStatsViewModel.hpp
    gui/PerformanceWidgets.cpp
    gui/PerformanceWidgets.hpp
    gui/MusicTheory.cpp
    gui/MusicTheory.hpp
    gui/PlaybackGrid.cpp
    gui/PlaybackGrid.hpp
    gui/PreparedMixRenderer.cpp
    gui/PreparedMixRenderer.hpp
    gui/PreparedMixLifecycle.hpp
    gui/PracticeIdeaDialogs.cpp
    gui/PracticeIdeaDialogs.hpp
    gui/PracticeIdeaController.cpp
    gui/PracticeIdeaController.hpp
    gui/PracticeIdeaGenerator.cpp
    gui/PracticeIdeaGenerator.hpp
    gui/PracticeReferenceRenderer.cpp
    gui/PracticeReferenceRenderer.hpp
    gui/ResearchDrumKit.cpp
    gui/ResearchDrumKit.hpp
    gui/ResearchDrumEngine.cpp
    gui/SharedBankLaunchCoordinator.cpp
    gui/SharedBankLaunchCoordinator.hpp
    gui/ProjectPersistenceCoordinator.cpp
    gui/ProjectPersistenceCoordinator.hpp
    gui/SharedTrackController.cpp
    gui/SharedTrackController.hpp
    gui/SharedTrackModel.hpp
    gui/StyleProfileCatalog.cpp
    gui/StyleProfileCatalog.hpp
    gui/TrackWidgets.hpp
    gui/TrackRecordingWorkflow.cpp
    gui/TrackRecordingWorkflow.hpp
    gui/TrackWorkspaceController.cpp
    gui/TrackWorkspaceController.hpp
    gui/TrackWorkspaceSupport.cpp
    gui/TrackWorkspaceSupport.hpp
    gui/UserPreferences.cpp
    gui/UserPreferences.hpp
    main.cpp
)

file(GLOB JAM2_IDEA_PREVIEW_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/idea-previews/*.wav")
if(JAM2_IDEA_PREVIEW_FILES)
    qt_add_resources(jam2 "jam2_idea_previews"
        PREFIX "/jam2/ideas/previews"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/gui/idea-previews"
        FILES ${JAM2_IDEA_PREVIEW_FILES}
    )
endif()

if(WIN32)
    set_target_properties(jam2 PROPERTIES WIN32_EXECUTABLE FALSE)
    target_sources(jam2 PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/Jam2.rc"
        "${JAM2_GUI_WINDOWS_ICON}"
    )
    set_source_files_properties(
        "${JAM2_GUI_WINDOWS_ICON}"
        PROPERTIES HEADER_FILE_ONLY TRUE
    )
endif()

if(APPLE)
    enable_language(OBJCXX)
    set_source_files_properties("${JAM2_GUI_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(jam2 PRIVATE
        platform/Jam2MacPermissions.hpp
        platform/Jam2MacPermissions.mm
        "${JAM2_GUI_MACOS_ICON}"
    )
    set_target_properties(jam2 PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Jam2"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.jam2.app"
        MACOSX_BUNDLE_ICON_FILE "jam2.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

target_include_directories(jam2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/application"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui"
    "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party"
)

target_link_libraries(jam2 PRIVATE
    jam2_asset_transfer
    jam2_automation_contract
    jam2_input_plugin_backend
    jam2_jam_sync
    jam2_midi_input_backend
    jam2_looper_model
    jam2_prepared_mix_lifecycle
    jam2_core
    jam2_daisysp_drums
    jam2_plugin_client
    Qt6::Network
    Qt6::Widgets
)

if(APPLE)
    target_link_libraries(jam2 PRIVATE
        "-framework AVFoundation"
        "-framework Foundation"
    )
endif()

if(WIN32)
    target_link_libraries(jam2 PRIVATE ole32 propsys Avrt Winmm)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    execute_process(
        COMMAND "${QT_QMAKE_EXECUTABLE}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE QT_PLUGINS_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    add_custom_command(TARGET jam2 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/generic"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/iconengines"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/imageformats"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/networkinformation"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/platforms"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/styles"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/tls"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/translations"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${JAM2_RELEASE_DIR}/multimedia"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${JAM2_RELEASE_DIR}/D3Dcompiler_47.dll"
            "${JAM2_RELEASE_DIR}/dxcompiler.dll"
            "${JAM2_RELEASE_DIR}/dxil.dll"
            "${JAM2_RELEASE_DIR}/libEGL.dll"
            "${JAM2_RELEASE_DIR}/libGLESv2.dll"
            "${JAM2_RELEASE_DIR}/opengl32sw.dll"
            "${JAM2_RELEASE_DIR}/vc_redist.x64.exe"
            "${JAM2_RELEASE_DIR}/icon.png"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:Qt6::Core>"
            "$<TARGET_FILE:Qt6::Gui>"
            "$<TARGET_FILE:Qt6::Widgets>"
            "$<TARGET_FILE:Qt6::Network>"
            "${JAM2_RELEASE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${JAM2_RELEASE_DIR}/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${QT_PLUGINS_DIR}/platforms/qwindows.dll"
            "${JAM2_RELEASE_DIR}/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/qt.conf"
            "${JAM2_RELEASE_DIR}/qt.conf"
        COMMENT "Staging minimal Qt runtime files for unified jam2"
        VERBATIM
    )
endif()

add_dependencies(jam2 jamtaster_worker jam2_plugin_worker)
if(WIN32)
    set(JAMTASTER_RELEASE_ROOT "${JAM2_RELEASE_DIR}/components/jamtaster")
    add_custom_command(TARGET jamtaster_worker POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${JAMTASTER_RELEASE_ROOT}/models"
            "${JAMTASTER_RELEASE_ROOT}/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jamtaster_worker>"
            "${JAMTASTER_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "${JAMTASTER_RELEASE_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/LICENSE"
            "${JAMTASTER_RELEASE_ROOT}/licenses/onnxruntime-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/ThirdPartyNotices.txt"
            "${JAMTASTER_RELEASE_ROOT}/licenses/onnxruntime-ThirdPartyNotices.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/eigen/COPYING.MPL2"
            "${JAMTASTER_RELEASE_ROOT}/licenses/eigen-COPYING.MPL2.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/third_party/demucs_onnx/LICENSE"
            "${JAMTASTER_RELEASE_ROOT}/licenses/demucs-onnx-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/MODEL_NOTICES.md"
            "${JAMTASTER_RELEASE_ROOT}/licenses/MODEL_NOTICES.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${JAMTASTER_MODEL_FILES}
            "${JAMTASTER_RELEASE_ROOT}/models"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/models"
        VERBATIM)
    set(JAM2_PLUGINHOST_RELEASE_ROOT "${JAM2_RELEASE_DIR}/components/pluginhost")
    add_custom_command(TARGET jam2_plugin_worker POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${JAM2_PLUGINHOST_RELEASE_ROOT}/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jam2_plugin_worker>"
            "${JAM2_PLUGINHOST_RELEASE_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${JAM2_VST3SDK_ROOT}/LICENSE.txt"
            "${JAM2_PLUGINHOST_RELEASE_ROOT}/licenses/VST3-SDK-LICENSE.txt"
        VERBATIM)
elseif(APPLE)
    add_custom_command(TARGET jam2 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Helpers"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/models"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/licenses"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jamtaster_worker>"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Helpers/jamtaster-worker"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jamtaster_onnxruntime>"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/LICENSE"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/licenses/onnxruntime-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/onnxruntime/ThirdPartyNotices.txt"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/licenses/onnxruntime-ThirdPartyNotices.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party/eigen/COPYING.MPL2"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/licenses/eigen-COPYING.MPL2.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/third_party/demucs_onnx/LICENSE"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/licenses/demucs-onnx-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/MODEL_NOTICES.md"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/licenses/MODEL_NOTICES.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${JAMTASTER_MODEL_FILES}
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/jamtaster/models"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/jamtaster/models"
        VERBATIM)
    add_custom_command(TARGET jam2 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Helpers"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/pluginhost/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jam2_plugin_worker>"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Helpers/jam2-plugin-worker"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${JAM2_VST3SDK_ROOT}/LICENSE.txt"
            "$<TARGET_BUNDLE_DIR:jam2>/Contents/Resources/pluginhost/licenses/VST3-SDK-LICENSE.txt"
        VERBATIM)
endif()

# Keep release staging last so the completed macOS bundle includes the private
# worker, runtime and models before it is copied into release/.
jam2_set_app_output(jam2)
