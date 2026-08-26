set(JAM2_TEST_BIN_DIR "${CMAKE_BINARY_DIR}/tests")

# Imported CMake targets are directory scoped unless promoted globally. Tests
# are a sibling of app/, so resolve the Qt components they link directly.
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

if(WIN32)
    set(JAM2_TEST_PUBLIC_EXECUTABLE
        "${JAM2_RELEASE_DIR}/jam2${CMAKE_EXECUTABLE_SUFFIX}")
elseif(APPLE)
    set(JAM2_TEST_PUBLIC_EXECUTABLE
        "${JAM2_RELEASE_DIR}/Jam2.app/Contents/MacOS/jam2")
endif()

function(jam2_configure_test_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${JAM2_TEST_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${JAM2_TEST_BIN_DIR}"
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_executable(jam2_core_input_tests EXCLUDE_FROM_ALL
    unit/core/CoreInputTests.cpp)
jam2_configure_test_target(jam2_core_input_tests)
target_link_libraries(jam2_core_input_tests PRIVATE jam2_core)
add_test(NAME jam2_core_input_units
    COMMAND $<TARGET_FILE:jam2_core_input_tests>)
set_tests_properties(jam2_core_input_units PROPERTIES
    LABELS "unit;core;network;performance;four-peer"
    TIMEOUT 30)

add_executable(jam2_core_boundary_tests EXCLUDE_FROM_ALL
    unit/core/CoreBoundaryTests.cpp)
jam2_configure_test_target(jam2_core_boundary_tests)
target_link_libraries(jam2_core_boundary_tests PRIVATE jam2_core)
add_test(NAME jam2_core_boundary_units
    COMMAND $<TARGET_FILE:jam2_core_boundary_tests>)
set_tests_properties(jam2_core_boundary_units PROPERTIES
    LABELS "unit;core;network"
    TIMEOUT 30)

add_executable(jam2_pcm16_wav_tests EXCLUDE_FROM_ALL
    unit/core/Pcm16WavTests.cpp)
jam2_configure_test_target(jam2_pcm16_wav_tests)
target_link_libraries(jam2_pcm16_wav_tests PRIVATE jam2_core)
add_test(NAME jam2_pcm16_wav_units
    COMMAND $<TARGET_FILE:jam2_pcm16_wav_tests>)
set_tests_properties(jam2_pcm16_wav_units PROPERTIES
    LABELS "unit;core;shared-content"
    TIMEOUT 30)

add_executable(jam2_audio_device_processing_tests EXCLUDE_FROM_ALL
    unit/core/AudioDeviceProcessingTests.cpp)
jam2_configure_test_target(jam2_audio_device_processing_tests)
target_link_libraries(jam2_audio_device_processing_tests PRIVATE jam2_core)
add_test(NAME jam2_audio_device_processing_units
    COMMAND $<TARGET_FILE:jam2_audio_device_processing_tests>)
set_tests_properties(jam2_audio_device_processing_units PROPERTIES
    LABELS "unit;core;audio;performance;metronome"
    TIMEOUT 30)

add_executable(jam2_transport_timing_tests EXCLUDE_FROM_ALL
    unit/core/TransportTimingTests.cpp)
jam2_configure_test_target(jam2_transport_timing_tests)
target_link_libraries(jam2_transport_timing_tests PRIVATE jam2_core)
add_test(NAME jam2_transport_timing_units
    COMMAND $<TARGET_FILE:jam2_transport_timing_tests>)
set_tests_properties(jam2_transport_timing_units PROPERTIES
    LABELS "unit;core;performance;metronome;transport"
    TIMEOUT 30)

add_executable(jam2_cli_boundary_tests EXCLUDE_FROM_ALL
    unit/application/CliBoundaryTests.cpp
    "${CMAKE_SOURCE_DIR}/app/cli/CliDispatch.cpp"
    "${CMAKE_SOURCE_DIR}/app/cli/CliOptions.cpp"
    "${CMAKE_SOURCE_DIR}/app/cli/CliRecordingSupervisor.cpp"
    "${CMAKE_SOURCE_DIR}/app/cli/CliStats.cpp")
jam2_configure_test_target(jam2_cli_boundary_tests)
target_include_directories(jam2_cli_boundary_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_SOURCE_DIR}/app/cli"
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(jam2_cli_boundary_tests PRIVATE jam2_core Qt6::Core)
add_dependencies(jam2_cli_boundary_tests jam2)
add_test(NAME jam2_cli_boundary_units
    COMMAND $<TARGET_FILE:jam2_cli_boundary_tests>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_cli_boundary_units PROPERTIES
    LABELS "unit;application;cli"
    TIMEOUT 45)
if(WIN32)
    set_tests_properties(jam2_cli_boundary_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_output_recorder_tests EXCLUDE_FROM_ALL
    unit/core/OutputRecorderTests.cpp)
jam2_configure_test_target(jam2_output_recorder_tests)
target_link_libraries(jam2_output_recorder_tests PRIVATE jam2_core Qt6::Core)
add_test(NAME jam2_output_recorder_units
    COMMAND $<TARGET_FILE:jam2_output_recorder_tests>)
set_tests_properties(jam2_output_recorder_units PROPERTIES
    LABELS "unit;core;audio;recording;wav"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_output_recorder_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_application_boundary_tests EXCLUDE_FROM_ALL
    unit/application/ApplicationBoundaryTests.cpp
    "${CMAKE_SOURCE_DIR}/app/application/ApplicationRuntime.cpp"
    "${CMAKE_SOURCE_DIR}/app/application/AutomationChannel.cpp"
    "${CMAKE_SOURCE_DIR}/app/application/ControlProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/app/application/NativeTcpTransport.cpp"
    "${CMAKE_SOURCE_DIR}/app/application/RuntimeHost.cpp")
jam2_configure_test_target(jam2_application_boundary_tests)
target_include_directories(jam2_application_boundary_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application")
target_link_libraries(jam2_application_boundary_tests PRIVATE jam2_core Qt6::Core)
add_test(NAME jam2_application_boundary_units
    COMMAND $<TARGET_FILE:jam2_application_boundary_tests>)
set_tests_properties(jam2_application_boundary_units PROPERTIES
    LABELS "unit;application;automation;network"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_application_boundary_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_debug_diagnostic_tests EXCLUDE_FROM_ALL
    unit/application/DebugDiagnosticTests.cpp
    "${CMAKE_SOURCE_DIR}/app/application/AssetChunkProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/app/application/ControlProtocol.cpp")
jam2_configure_test_target(jam2_debug_diagnostic_tests)
target_include_directories(jam2_debug_diagnostic_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(jam2_debug_diagnostic_tests PRIVATE jam2_core Qt6::Core)
add_dependencies(jam2_debug_diagnostic_tests jam2)
add_test(NAME jam2_debug_help_units
    COMMAND $<TARGET_FILE:jam2_debug_diagnostic_tests>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}" help)
set_tests_properties(jam2_debug_help_units PROPERTIES
    LABELS "unit;application;debug;network"
    TIMEOUT 40)
add_test(NAME jam2_debug_fuzz_rejected_units
    COMMAND $<TARGET_FILE:jam2_debug_diagnostic_tests>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}" fuzz-rejected)
set_tests_properties(jam2_debug_fuzz_rejected_units PROPERTIES
    LABELS "unit;application;debug;fuzz;network"
    TIMEOUT 70)
add_test(NAME jam2_debug_fuzz_accepted_units
    COMMAND $<TARGET_FILE:jam2_debug_diagnostic_tests>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}" fuzz-accepted)
set_tests_properties(jam2_debug_fuzz_accepted_units PROPERTIES
    LABELS "unit;application;debug;fuzz;network"
    TIMEOUT 60)
add_test(NAME jam2_debug_lifecycle_units
    COMMAND $<TARGET_FILE:jam2_debug_diagnostic_tests>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}" lifecycle)
set_tests_properties(jam2_debug_lifecycle_units PROPERTIES
    LABELS "unit;application;debug;network;lifecycle"
    TIMEOUT 20)
add_test(NAME jam2_music_corpus_units
    COMMAND $<TARGET_FILE:jam2_debug_diagnostic_tests>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}" corpus)
set_tests_properties(jam2_music_corpus_units PROPERTIES
    LABELS "unit;application;debug;jamtaster;gui"
    TIMEOUT 120)
if(WIN32)
    set_tests_properties(
        jam2_debug_help_units jam2_debug_fuzz_rejected_units
        jam2_debug_fuzz_accepted_units jam2_debug_lifecycle_units
        jam2_music_corpus_units
        PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_automation_contract_tests EXCLUDE_FROM_ALL
    unit/application/AutomationContractTests.cpp)
jam2_configure_test_target(jam2_automation_contract_tests)
target_link_libraries(jam2_automation_contract_tests PRIVATE
    jam2_automation_contract Qt6::Core)
add_test(NAME jam2_automation_contract_units
    COMMAND $<TARGET_FILE:jam2_automation_contract_tests>)
set_tests_properties(jam2_automation_contract_units PROPERTIES
    LABELS "unit;application;automation"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_automation_contract_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_worker_entrypoint_tests EXCLUDE_FROM_ALL
    unit/application/WorkerEntrypointTests.cpp)
jam2_configure_test_target(jam2_worker_entrypoint_tests)
target_link_libraries(jam2_worker_entrypoint_tests PRIVATE
    Qt6::Core)
target_include_directories(jam2_worker_entrypoint_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
add_dependencies(jam2_worker_entrypoint_tests
    jamtaster_worker jam2_plugin_worker)
add_test(NAME jam2_worker_entrypoint_units
    COMMAND $<TARGET_FILE:jam2_worker_entrypoint_tests>
        $<TARGET_FILE:jamtaster_worker>
        $<TARGET_FILE:jam2_plugin_worker>)
set_tests_properties(jam2_worker_entrypoint_units PROPERTIES
    LABELS "unit;application;worker;plugin;jamtaster"
    TIMEOUT 45)
if(WIN32)
    set_tests_properties(jam2_worker_entrypoint_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:jamtaster_onnxruntime>")
endif()

add_executable(jam2_jam_sync_policy_tests EXCLUDE_FROM_ALL
    unit/application/JamSyncPolicyTests.cpp)
jam2_configure_test_target(jam2_jam_sync_policy_tests)
target_link_libraries(jam2_jam_sync_policy_tests PRIVATE
    jam2_jam_sync Qt6::Core)
add_test(NAME jam2_jam_sync_policy_units
    COMMAND $<TARGET_FILE:jam2_jam_sync_policy_tests>)
set_tests_properties(jam2_jam_sync_policy_units PROPERTIES
    LABELS "unit;application;jam-sync"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_jam_sync_policy_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_midi_input_backend_tests EXCLUDE_FROM_ALL
    unit/application/MidiInputBackendTests.cpp)
jam2_configure_test_target(jam2_midi_input_backend_tests)
target_link_libraries(jam2_midi_input_backend_tests PRIVATE
    jam2_midi_input_backend)
add_test(NAME jam2_midi_input_backend_units
    COMMAND $<TARGET_FILE:jam2_midi_input_backend_tests>)
set_tests_properties(jam2_midi_input_backend_units PROPERTIES
    LABELS "unit;application;gui;midi"
    TIMEOUT 30)

add_executable(jam2_input_plugin_backend_tests EXCLUDE_FROM_ALL
    unit/application/InputPluginBackendTests.cpp)
jam2_configure_test_target(jam2_input_plugin_backend_tests)
target_link_libraries(jam2_input_plugin_backend_tests PRIVATE
    jam2_input_plugin_backend Qt6::Widgets)
add_test(NAME jam2_input_plugin_backend_units
    COMMAND $<TARGET_FILE:jam2_input_plugin_backend_tests>)
set_tests_properties(jam2_input_plugin_backend_units PROPERTIES
    LABELS "unit;application;gui;plugin"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_input_plugin_backend_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
endif()

add_executable(jam2_looper_project_tests EXCLUDE_FROM_ALL
    unit/gui/LooperProjectTests.cpp)
jam2_configure_test_target(jam2_looper_project_tests)
target_link_libraries(jam2_looper_project_tests PRIVATE
    jam2_looper_model Qt6::Core)
add_test(NAME jam2_looper_project_units
    COMMAND $<TARGET_FILE:jam2_looper_project_tests>)
set_tests_properties(jam2_looper_project_units PROPERTIES
    LABELS "unit;gui;shared-content"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_looper_project_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_prepared_mix_lifecycle_tests EXCLUDE_FROM_ALL
    unit/gui/PreparedMixLifecycleTests.cpp)
jam2_configure_test_target(jam2_prepared_mix_lifecycle_tests)
target_link_libraries(jam2_prepared_mix_lifecycle_tests PRIVATE
    jam2_prepared_mix_lifecycle Qt6::Core)
add_test(NAME jam2_prepared_mix_lifecycle_units
    COMMAND $<TARGET_FILE:jam2_prepared_mix_lifecycle_tests>)
set_tests_properties(jam2_prepared_mix_lifecycle_units PROPERTIES
    LABELS "unit;gui;performance;shared-content;persistence"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_prepared_mix_lifecycle_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_project_persistence_tests EXCLUDE_FROM_ALL
    unit/gui/ProjectPersistenceCoordinatorTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/ProjectPersistenceCoordinator.cpp")
jam2_configure_test_target(jam2_project_persistence_tests)
target_include_directories(jam2_project_persistence_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_project_persistence_tests PRIVATE Qt6::Core)
add_test(NAME jam2_project_persistence_units
    COMMAND $<TARGET_FILE:jam2_project_persistence_tests>)
set_tests_properties(jam2_project_persistence_units PROPERTIES
    LABELS "unit;gui;shared-content;persistence;wav"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_project_persistence_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_workspace_state_tests EXCLUDE_FROM_ALL
    unit/gui/WorkspaceStateTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/GuiPresentation.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/JamStorage.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/SharedTrackController.cpp")
jam2_configure_test_target(jam2_workspace_state_tests)
target_include_directories(jam2_workspace_state_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_workspace_state_tests PRIVATE
    jam2_looper_model Qt6::Core Qt6::Widgets)
add_test(NAME jam2_workspace_state_units
    COMMAND $<TARGET_FILE:jam2_workspace_state_tests>)
set_tests_properties(jam2_workspace_state_units PROPERTIES
    LABELS "unit;gui;shared-content;persistence;model"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_tests_properties(jam2_workspace_state_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
endif()

add_executable(jam2_gui_loopback_recorder_tests EXCLUDE_FROM_ALL
    unit/gui/GuiLoopbackRecorderTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/GuiLoopbackRecorder.cpp")
jam2_configure_test_target(jam2_gui_loopback_recorder_tests)
target_include_directories(jam2_gui_loopback_recorder_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_gui_loopback_recorder_tests PRIVATE
    jam2_core Qt6::Core)
if(WIN32)
    target_link_libraries(jam2_gui_loopback_recorder_tests PRIVATE ole32 propsys)
endif()
add_test(NAME jam2_gui_loopback_recorder_units
    COMMAND $<TARGET_FILE:jam2_gui_loopback_recorder_tests>)
set_tests_properties(jam2_gui_loopback_recorder_units PROPERTIES
    LABELS "unit;gui;audio;recording;performance;wav"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_gui_loopback_recorder_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_track_workspace_support_tests EXCLUDE_FROM_ALL
    unit/gui/TrackWorkspaceSupportTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/GuiLoopbackRecorder.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/LooperAssetMaterializer.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/TrackWorkspaceSupport.cpp")
jam2_configure_test_target(jam2_track_workspace_support_tests)
target_include_directories(jam2_track_workspace_support_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_track_workspace_support_tests PRIVATE
    jam2_core jam2_looper_model Qt6::Core)
if(WIN32)
    target_link_libraries(jam2_track_workspace_support_tests PRIVATE ole32 propsys)
endif()
add_test(NAME jam2_track_workspace_support_units
    COMMAND $<TARGET_FILE:jam2_track_workspace_support_tests>)
set_tests_properties(jam2_track_workspace_support_units PROPERTIES
    LABELS "unit;gui;shared-content;wav;performance"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_track_workspace_support_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_track_recording_workflow_tests EXCLUDE_FROM_ALL
    unit/gui/TrackRecordingWorkflowTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/TrackRecordingWorkflow.cpp")
jam2_configure_test_target(jam2_track_recording_workflow_tests)
target_include_directories(jam2_track_recording_workflow_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_track_recording_workflow_tests PRIVATE
    jam2_core Qt6::Core)
add_test(NAME jam2_track_recording_workflow_units
    COMMAND $<TARGET_FILE:jam2_track_recording_workflow_tests>)
set_tests_properties(jam2_track_recording_workflow_units PROPERTIES
    LABELS "unit;gui;audio;recording;performance;transport"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_track_recording_workflow_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_metronome_transport_controller_tests EXCLUDE_FROM_ALL
    unit/gui/MetronomeTransportControllerTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/MetronomeTransportController.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/PlaybackGrid.cpp")
jam2_configure_test_target(jam2_metronome_transport_controller_tests)
target_include_directories(jam2_metronome_transport_controller_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_metronome_transport_controller_tests PRIVATE
    jam2_core Qt6::Core)
add_test(NAME jam2_metronome_transport_controller_units
    COMMAND $<TARGET_FILE:jam2_metronome_transport_controller_tests>)
set_tests_properties(jam2_metronome_transport_controller_units PROPERTIES
    LABELS "unit;gui;metronome;epoch;transport;performance"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_metronome_transport_controller_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_shared_bank_launch_tests EXCLUDE_FROM_ALL
    unit/gui/SharedBankLaunchCoordinatorTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/SharedBankLaunchCoordinator.cpp")
jam2_configure_test_target(jam2_shared_bank_launch_tests)
target_include_directories(jam2_shared_bank_launch_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_shared_bank_launch_tests PRIVATE
    jam2_core Qt6::Core)
add_test(NAME jam2_shared_bank_launch_units
    COMMAND $<TARGET_FILE:jam2_shared_bank_launch_tests>)
set_tests_properties(jam2_shared_bank_launch_units PROPERTIES
    LABELS "unit;gui;network;performance;metronome;epoch;transport"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_shared_bank_launch_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_gui_model_boundary_tests EXCLUDE_FROM_ALL
    unit/gui/GuiModelBoundaryTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/BeatGridModel.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GenerationRecipe.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GuiPresentation.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/MixerStatsViewModel.cpp")
jam2_configure_test_target(jam2_gui_model_boundary_tests)
target_include_directories(jam2_gui_model_boundary_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_gui_model_boundary_tests PRIVATE
    jam2_core Qt6::Core Qt6::Widgets)
add_test(NAME jam2_gui_model_boundary_units
    COMMAND $<TARGET_FILE:jam2_gui_model_boundary_tests>)
set_tests_properties(jam2_gui_model_boundary_units PROPERTIES
    LABELS "unit;gui;model"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_tests_properties(jam2_gui_model_boundary_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
endif()

add_executable(jam2_gui_widget_boundary_tests EXCLUDE_FROM_ALL
    unit/gui/GuiWidgetBoundaryTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/AudioDeviceUiSupport.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/BeatGridModel.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/BeatGridWidget.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/ConnectionGuidance.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GenerationRecipe.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GuiControlContract.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GuiInteractionPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GuiPresentation.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/MusicTheory.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/PerformanceWidgets.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/SessionStartupDialogs.cpp")
jam2_configure_test_target(jam2_gui_widget_boundary_tests)
target_include_directories(jam2_gui_widget_boundary_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_gui_widget_boundary_tests PRIVATE
    jam2_core Qt6::Core Qt6::Widgets)
add_test(NAME jam2_gui_widget_boundary_units
    COMMAND $<TARGET_FILE:jam2_gui_widget_boundary_tests>)
set_tests_properties(jam2_gui_widget_boundary_units PROPERTIES
    LABELS "unit;gui;widget;performance"
    TIMEOUT 45
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_tests_properties(jam2_gui_widget_boundary_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
endif()

add_executable(jam2_practice_idea_boundary_tests EXCLUDE_FROM_ALL
    unit/gui/PracticeIdeaBoundaryTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/BeatGridModel.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/CuratedIdeaCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/CuratedIdeaDialog.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GenerationRecipe.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/GuiControlContract.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/MusicTheory.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/PracticeIdeaController.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/PracticeIdeaDialogs.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/PracticeIdeaGenerator.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/PracticeReferenceRenderer.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/ResearchDrumEngine.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/ResearchDrumKit.cpp"
    "${CMAKE_SOURCE_DIR}/app/gui/StyleProfileCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/app/Jam2Resources.qrc")
jam2_configure_test_target(jam2_practice_idea_boundary_tests)
set_target_properties(jam2_practice_idea_boundary_tests PROPERTIES AUTORCC ON)
file(GLOB JAM2_TEST_IDEA_PREVIEW_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/app/gui/idea-previews/*.wav")
qt_add_resources(jam2_practice_idea_boundary_tests "jam2_test_idea_previews"
    PREFIX "/jam2/ideas/previews"
    BASE "${CMAKE_SOURCE_DIR}/app/gui/idea-previews"
    FILES ${JAM2_TEST_IDEA_PREVIEW_FILES})
target_include_directories(jam2_practice_idea_boundary_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_practice_idea_boundary_tests PRIVATE
    jam2_core jam2_daisysp_drums jam2_looper_model Qt6::Core Qt6::Widgets)
add_test(NAME jam2_practice_idea_boundary_units
    COMMAND $<TARGET_FILE:jam2_practice_idea_boundary_tests>)
set_tests_properties(jam2_practice_idea_boundary_units PROPERTIES
    LABELS "unit;gui;practice;audio;dialog"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_tests_properties(jam2_practice_idea_boundary_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
endif()

add_executable(jam2_user_preferences_tests EXCLUDE_FROM_ALL
    unit/gui/UserPreferencesTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/UserPreferences.cpp")
jam2_configure_test_target(jam2_user_preferences_tests)
target_include_directories(jam2_user_preferences_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_user_preferences_tests PRIVATE Qt6::Core)
add_test(NAME jam2_user_preferences_units
    COMMAND $<TARGET_FILE:jam2_user_preferences_tests>)
set_tests_properties(jam2_user_preferences_units PROPERTIES
    LABELS "unit;gui;preferences;persistence"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_user_preferences_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_detail_section_edit_tests EXCLUDE_FROM_ALL
    unit/gui/DetailSectionEditTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/DetailSectionEdit.cpp")
jam2_configure_test_target(jam2_detail_section_edit_tests)
target_include_directories(jam2_detail_section_edit_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_detail_section_edit_tests PRIVATE Qt6::Widgets)
add_test(NAME jam2_detail_section_edit_units
    COMMAND $<TARGET_FILE:jam2_detail_section_edit_tests>)
set_tests_properties(jam2_detail_section_edit_units PROPERTIES
    LABELS "unit;gui;widget;editing"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_tests_properties(jam2_detail_section_edit_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
endif()

add_executable(jam2_asset_transfer_tests EXCLUDE_FROM_ALL
    unit/gui/AssetTransferTests.cpp)
jam2_configure_test_target(jam2_asset_transfer_tests)
target_link_libraries(jam2_asset_transfer_tests PRIVATE
    jam2_asset_transfer Qt6::Core)
add_test(NAME jam2_asset_transfer_units
    COMMAND $<TARGET_FILE:jam2_asset_transfer_tests>)
set_tests_properties(jam2_asset_transfer_units PROPERTIES
    LABELS "unit;gui;shared-content;asset"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_asset_transfer_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_plugin_bridge_tests EXCLUDE_FROM_ALL
    unit/plugin/PluginBridgeTests.cpp)
jam2_configure_test_target(jam2_plugin_bridge_tests)
target_link_libraries(jam2_plugin_bridge_tests PRIVATE
    jam2_plugin_client jam2_plugin_transport Qt6::Core)
add_test(NAME jam2_plugin_bridge_units
    COMMAND $<TARGET_FILE:jam2_plugin_bridge_tests>)
set_tests_properties(jam2_plugin_bridge_units PROPERTIES
    LABELS "unit;plugin"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_plugin_bridge_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jamtaster_tests EXCLUDE_FROM_ALL
    unit/jamtaster/NativeTests.cpp)
jam2_configure_test_target(jamtaster_tests)
target_link_libraries(jamtaster_tests PRIVATE jamtaster_core)
target_include_directories(jamtaster_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/third_party/eigen")
get_target_property(
    JAM2_TEST_ONNXRUNTIME_LIBRARY
    jamtaster_onnxruntime
    IMPORTED_LOCATION)
get_filename_component(
    JAM2_TEST_ONNXRUNTIME_DIR
    "${JAM2_TEST_ONNXRUNTIME_LIBRARY}"
    DIRECTORY)
if(APPLE)
    set(JAM2_TEST_JAMTASTER_MODELS_DIR
        "${JAM2_RELEASE_DIR}/Jam2.app/Contents/Resources/jamtaster/models")
    set(JAM2_TEST_JAMTASTER_WORKER
        "${JAM2_RELEASE_DIR}/Jam2.app/Contents/Helpers/jamtaster-worker")
else()
    set(JAM2_TEST_JAMTASTER_MODELS_DIR
        "${JAM2_RELEASE_DIR}/components/jamtaster/models")
    set(JAM2_TEST_JAMTASTER_WORKER
        "${JAM2_RELEASE_DIR}/components/jamtaster/jamtaster-worker${CMAKE_EXECUTABLE_SUFFIX}")
endif()
if(APPLE)
    set_target_properties(jamtaster_tests PROPERTIES
        BUILD_RPATH "${JAM2_TEST_ONNXRUNTIME_DIR}")
endif()
add_test(NAME jamtaster_native_units
    COMMAND $<TARGET_FILE:jamtaster_tests>)
set_tests_properties(jamtaster_native_units PROPERTIES
    LABELS "unit;jamtaster"
    TIMEOUT 120)
if(WIN32)
    set_tests_properties(jamtaster_native_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${JAM2_TEST_ONNXRUNTIME_DIR}")
endif()

add_executable(jam2_jamtaster_model_tests EXCLUDE_FROM_ALL
    unit/jamtaster/ModelBoundaryTests.cpp)
jam2_configure_test_target(jam2_jamtaster_model_tests)
target_link_libraries(jam2_jamtaster_model_tests PRIVATE jamtaster_core)
target_include_directories(jam2_jamtaster_model_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/third_party/eigen")
if(WIN32)
    # Windows searches System32 before PATH for a DLL that is not beside the
    # executable. Keep the test bound to the imported JamTaster runtime rather
    # than an unrelated system-wide ONNX Runtime with an older API.
    add_custom_command(TARGET jam2_jamtaster_model_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jamtaster_onnxruntime>"
            "$<TARGET_FILE_DIR:jam2_jamtaster_model_tests>"
        VERBATIM)
endif()
add_test(NAME jam2_jamtaster_model_units
    COMMAND $<TARGET_FILE:jam2_jamtaster_model_tests>
        "${JAM2_TEST_JAMTASTER_MODELS_DIR}")
set_tests_properties(jam2_jamtaster_model_units PROPERTIES
    LABELS "unit;jamtaster;model"
    TIMEOUT 120)
if(WIN32)
    set_tests_properties(jam2_jamtaster_model_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${JAM2_TEST_ONNXRUNTIME_DIR}")
endif()

add_executable(jam2_jamtaster_demucs_tests EXCLUDE_FROM_ALL
    unit/jamtaster/DemucsBoundaryTests.cpp)
jam2_configure_test_target(jam2_jamtaster_demucs_tests)
target_link_libraries(jam2_jamtaster_demucs_tests PRIVATE jamtaster_core)
target_include_directories(jam2_jamtaster_demucs_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/third_party/eigen")
if(WIN32)
    add_custom_command(TARGET jam2_jamtaster_demucs_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jamtaster_onnxruntime>"
            "$<TARGET_FILE_DIR:jam2_jamtaster_demucs_tests>"
        VERBATIM)
endif()
add_test(NAME jam2_jamtaster_demucs_units
    COMMAND $<TARGET_FILE:jam2_jamtaster_demucs_tests>
        "${JAM2_TEST_JAMTASTER_MODELS_DIR}")
set_tests_properties(jam2_jamtaster_demucs_units PROPERTIES
    LABELS "unit;jamtaster;model;separation"
    TIMEOUT 300)
if(WIN32)
    set_tests_properties(jam2_jamtaster_demucs_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${JAM2_TEST_ONNXRUNTIME_DIR}")
endif()

add_executable(jam2_jamtaster_pipeline_tests EXCLUDE_FROM_ALL
    unit/jamtaster/PipelineBoundaryTests.cpp)
jam2_configure_test_target(jam2_jamtaster_pipeline_tests)
target_link_libraries(jam2_jamtaster_pipeline_tests PRIVATE jamtaster_core)
target_include_directories(jam2_jamtaster_pipeline_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/third_party/eigen")
if(WIN32)
    add_custom_command(TARGET jam2_jamtaster_pipeline_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jamtaster_onnxruntime>"
            "$<TARGET_FILE_DIR:jam2_jamtaster_pipeline_tests>"
        VERBATIM)
endif()
add_test(NAME jam2_jamtaster_pipeline_units
    COMMAND $<TARGET_FILE:jam2_jamtaster_pipeline_tests>
        "${JAM2_TEST_JAMTASTER_MODELS_DIR}")
set_tests_properties(jam2_jamtaster_pipeline_units PROPERTIES
    LABELS "unit;jamtaster;model;pipeline"
    TIMEOUT 180)
if(WIN32)
    set_tests_properties(jam2_jamtaster_pipeline_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${JAM2_TEST_ONNXRUNTIME_DIR}")
endif()

add_executable(jam2_jamtaster_worker_protocol_tests EXCLUDE_FROM_ALL
    unit/jamtaster/WorkerProtocolBoundaryTests.cpp)
jam2_configure_test_target(jam2_jamtaster_worker_protocol_tests)
target_include_directories(jam2_jamtaster_worker_protocol_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(jam2_jamtaster_worker_protocol_tests PRIVATE
    jamtaster_core Qt6::Core)
target_include_directories(jam2_jamtaster_worker_protocol_tests SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/third_party/eigen")
add_dependencies(jam2_jamtaster_worker_protocol_tests jamtaster_worker)
add_test(NAME jam2_jamtaster_worker_protocol_units
    COMMAND $<TARGET_FILE:jam2_jamtaster_worker_protocol_tests>
        "${JAM2_TEST_JAMTASTER_WORKER}"
        "${JAM2_TEST_JAMTASTER_MODELS_DIR}")
set_tests_properties(jam2_jamtaster_worker_protocol_units PROPERTIES
    LABELS "unit;jamtaster;worker;model;pipeline"
    TIMEOUT 120)
if(WIN32)
    set_tests_properties(jam2_jamtaster_worker_protocol_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_jamtaster_service_worker EXCLUDE_FROM_ALL
    support/JamtasterServiceWorker.cpp)
jam2_configure_test_target(jam2_jamtaster_service_worker)
target_link_libraries(jam2_jamtaster_service_worker PRIVATE Qt6::Core)

add_executable(jam2_jamtaster_service_tests EXCLUDE_FROM_ALL
    unit/jamtaster/ServiceBoundaryTests.cpp
    "${CMAKE_SOURCE_DIR}/app/jamtaster/JamTasterService.cpp")
jam2_configure_test_target(jam2_jamtaster_service_tests)
target_include_directories(jam2_jamtaster_service_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_SOURCE_DIR}/app/jamtaster")
target_link_libraries(jam2_jamtaster_service_tests PRIVATE Qt6::Core)
add_dependencies(jam2_jamtaster_service_tests jam2_jamtaster_service_worker)
add_test(NAME jam2_jamtaster_service_units
    COMMAND $<TARGET_FILE:jam2_jamtaster_service_tests>
        $<TARGET_FILE:jam2_jamtaster_service_worker>)
set_tests_properties(jam2_jamtaster_service_units PROPERTIES
    LABELS "unit;jamtaster;service;worker"
    TIMEOUT 60)
if(WIN32)
    set_tests_properties(jam2_jamtaster_service_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_jamtaster_dialog_tests EXCLUDE_FROM_ALL
    unit/gui/JamTasterDialogBoundaryTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/JamTasterDialog.cpp"
    "${CMAKE_SOURCE_DIR}/app/jamtaster/JamTasterService.cpp")
jam2_configure_test_target(jam2_jamtaster_dialog_tests)
target_include_directories(jam2_jamtaster_dialog_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_SOURCE_DIR}/app/gui"
    "${CMAKE_SOURCE_DIR}/app/jamtaster")
target_link_libraries(jam2_jamtaster_dialog_tests PRIVATE Qt6::Core Qt6::Widgets)
add_dependencies(jam2_jamtaster_dialog_tests jam2_jamtaster_service_worker)
add_test(NAME jam2_jamtaster_dialog_units
    COMMAND $<TARGET_FILE:jam2_jamtaster_dialog_tests>
        $<TARGET_FILE:jam2_jamtaster_service_worker>)
set_tests_properties(jam2_jamtaster_dialog_units PROPERTIES
    LABELS "unit;gui;jamtaster;dialog;worker"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32)
    set_tests_properties(jam2_jamtaster_dialog_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
endif()

add_executable(jam2_jamtaster_project_support_tests EXCLUDE_FROM_ALL
    unit/gui/JamTasterProjectSupportTests.cpp
    "${CMAKE_SOURCE_DIR}/app/gui/JamTasterProjectSupport.cpp")
jam2_configure_test_target(jam2_jamtaster_project_support_tests)
target_include_directories(jam2_jamtaster_project_support_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/app/application"
    "${CMAKE_SOURCE_DIR}/app/gui")
target_link_libraries(jam2_jamtaster_project_support_tests PRIVATE
    jam2_core Qt6::Core)
add_test(NAME jam2_jamtaster_project_support_units
    COMMAND $<TARGET_FILE:jam2_jamtaster_project_support_tests>)
set_tests_properties(jam2_jamtaster_project_support_units PROPERTIES
    LABELS "unit;gui;jamtaster;model;project"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_jamtaster_project_support_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_plugin_worker_tests EXCLUDE_FROM_ALL
    integration/plugin/PluginWorkerTests.cpp)
jam2_configure_test_target(jam2_plugin_worker_tests)
target_link_libraries(jam2_plugin_worker_tests PRIVATE
    jam2_plugin_client Qt6::Core)
add_dependencies(jam2_plugin_worker_tests jam2_plugin_worker)

add_executable(jam2_plugin_device_tests EXCLUDE_FROM_ALL
    hardware/plugin/PluginDeviceTests.cpp)
jam2_configure_test_target(jam2_plugin_device_tests)
target_link_libraries(jam2_plugin_device_tests PRIVATE
    jam2_input_plugin_backend Qt6::Widgets)
add_dependencies(jam2_plugin_device_tests jam2_plugin_worker)

add_executable(jam2_midi_instrument_device_tests EXCLUDE_FROM_ALL
    hardware/plugin/MidiInstrumentDeviceTests.cpp)
jam2_configure_test_target(jam2_midi_instrument_device_tests)
target_link_libraries(jam2_midi_instrument_device_tests PRIVATE
    jam2_input_plugin_backend Qt6::Widgets)
add_dependencies(jam2_midi_instrument_device_tests jam2_plugin_worker)
if(WIN32)
    add_custom_target(jam2_hardware_plugin_worker_stage
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            $<TARGET_FILE_DIR:jam2_plugin_device_tests>
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            $<TARGET_FILE:jam2_plugin_worker>
            $<TARGET_FILE_DIR:jam2_midi_instrument_device_tests>
        DEPENDS jam2_plugin_worker
        VERBATIM)
elseif(APPLE)
    add_custom_target(jam2_hardware_plugin_worker_stage
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:jam2_midi_instrument_device_tests>/../Helpers"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            $<TARGET_FILE:jam2_plugin_worker>
            "$<TARGET_FILE_DIR:jam2_midi_instrument_device_tests>/../Helpers/jam2-plugin-worker"
        DEPENDS jam2_plugin_worker
        VERBATIM)
endif()
if(TARGET jam2_hardware_plugin_worker_stage)
    add_dependencies(
        jam2_plugin_device_tests jam2_hardware_plugin_worker_stage)
    add_dependencies(
        jam2_midi_instrument_device_tests jam2_hardware_plugin_worker_stage)
endif()

if(JAM2_HARDWARE_PROFILE)
    if(NOT EXISTS "${JAM2_HARDWARE_PROFILE}")
        message(FATAL_ERROR
            "JAM2_HARDWARE_PROFILE does not exist: ${JAM2_HARDWARE_PROFILE}")
    endif()
    file(READ "${JAM2_HARDWARE_PROFILE}" JAM2_HARDWARE_PROFILE_JSON)

    function(jam2_hardware_profile_field output key)
        string(JSON field_value ERROR_VARIABLE field_error
            GET "${JAM2_HARDWARE_PROFILE_JSON}" "${key}")
        if(NOT field_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR
                "Invalid Jam2 hardware profile field '${key}': ${field_error}")
        endif()
        set(${output} "${field_value}" PARENT_SCOPE)
    endfunction()

    jam2_hardware_profile_field(JAM2_HARDWARE_SCHEMA schema)
    jam2_hardware_profile_field(JAM2_HARDWARE_PLUGIN_PATH plugin_path)
    jam2_hardware_profile_field(JAM2_HARDWARE_DEVICE_ID audio_device_id)
    jam2_hardware_profile_field(JAM2_HARDWARE_INPUT_CHANNEL input_channel)
    jam2_hardware_profile_field(JAM2_HARDWARE_BUFFER_FRAMES buffer_frames)

    if(NOT JAM2_HARDWARE_SCHEMA STREQUAL "jam2-hardware-profile-v1" AND
            NOT JAM2_HARDWARE_SCHEMA STREQUAL "jam2-hardware-profile-v2")
        message(FATAL_ERROR
            "Hardware profile schema must be 'jam2-hardware-profile-v1' or "
            "'jam2-hardware-profile-v2'.")
    endif()
    set(JAM2_HARDWARE_SECOND_INPUT_CHANNEL 0)
    set(JAM2_HARDWARE_UNSUPPORTED_BUFFER_FRAMES 0)
    set(JAM2_HARDWARE_INSTRUMENT_PLUGIN_PATH "")
    set(JAM2_HARDWARE_MIDI_DEVICE_NAME "")
    if(JAM2_HARDWARE_SCHEMA STREQUAL "jam2-hardware-profile-v2")
        jam2_hardware_profile_field(
            JAM2_HARDWARE_SECOND_INPUT_CHANNEL second_input_channel)
        jam2_hardware_profile_field(
            JAM2_HARDWARE_UNSUPPORTED_BUFFER_FRAMES unsupported_buffer_frames)
        jam2_hardware_profile_field(
            JAM2_HARDWARE_INSTRUMENT_PLUGIN_PATH instrument_plugin_path)
        jam2_hardware_profile_field(
            JAM2_HARDWARE_MIDI_DEVICE_NAME midi_device_name)
    endif()
    if(NOT EXISTS "${JAM2_HARDWARE_PLUGIN_PATH}")
        message(FATAL_ERROR
            "Hardware profile plugin_path does not exist: ${JAM2_HARDWARE_PLUGIN_PATH}")
    endif()
    foreach(numeric_field IN ITEMS
            JAM2_HARDWARE_DEVICE_ID
            JAM2_HARDWARE_INPUT_CHANNEL
            JAM2_HARDWARE_BUFFER_FRAMES
            JAM2_HARDWARE_SECOND_INPUT_CHANNEL
            JAM2_HARDWARE_UNSUPPORTED_BUFFER_FRAMES)
        if(NOT ${numeric_field} MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "Hardware profile ${numeric_field} must be a non-negative integer.")
        endif()
    endforeach()
    if(JAM2_HARDWARE_INPUT_CHANNEL LESS 1)
        message(FATAL_ERROR "Hardware profile input_channel must be at least 1.")
    endif()
    if(JAM2_HARDWARE_SCHEMA STREQUAL "jam2-hardware-profile-v2")
        if(JAM2_HARDWARE_SECOND_INPUT_CHANNEL LESS 1 OR
                JAM2_HARDWARE_SECOND_INPUT_CHANNEL EQUAL JAM2_HARDWARE_INPUT_CHANNEL)
            message(FATAL_ERROR
                "Hardware profile second_input_channel must be a different positive channel.")
        endif()
        if(JAM2_HARDWARE_UNSUPPORTED_BUFFER_FRAMES LESS 1 OR
                JAM2_HARDWARE_UNSUPPORTED_BUFFER_FRAMES GREATER 4096)
            message(FATAL_ERROR
                "Hardware profile unsupported_buffer_frames must be between 1 and 4096.")
        endif()
        if(NOT EXISTS "${JAM2_HARDWARE_INSTRUMENT_PLUGIN_PATH}")
            message(FATAL_ERROR
                "Hardware profile instrument_plugin_path does not exist: "
                "${JAM2_HARDWARE_INSTRUMENT_PLUGIN_PATH}")
        endif()
        if(JAM2_HARDWARE_MIDI_DEVICE_NAME STREQUAL "")
            message(FATAL_ERROR
                "Hardware profile midi_device_name must not be empty.")
        endif()
    endif()
    if(JAM2_HARDWARE_BUFFER_FRAMES LESS 1 OR
            JAM2_HARDWARE_BUFFER_FRAMES GREATER 4096)
        message(FATAL_ERROR
            "Hardware profile buffer_frames must be between 1 and 4096.")
    endif()

    add_test(NAME jam2_hardware_plugin_device
        COMMAND $<TARGET_FILE:jam2_plugin_device_tests>
            "${JAM2_HARDWARE_PLUGIN_PATH}"
            "${JAM2_HARDWARE_DEVICE_ID}"
            "${JAM2_HARDWARE_INPUT_CHANNEL}"
            "${JAM2_HARDWARE_BUFFER_FRAMES}"
            "${JAM2_HARDWARE_SECOND_INPUT_CHANNEL}"
            "${JAM2_HARDWARE_UNSUPPORTED_BUFFER_FRAMES}")
    set_tests_properties(jam2_hardware_plugin_device PROPERTIES
        LABELS "hardware;plugin;audio-device;interactive"
        RUN_SERIAL TRUE
        TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(jam2_hardware_plugin_device PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
    endif()

    if(JAM2_HARDWARE_SCHEMA STREQUAL "jam2-hardware-profile-v2")
        add_test(NAME jam2_hardware_midi_instrument_device
            COMMAND $<TARGET_FILE:jam2_midi_instrument_device_tests>
                "${JAM2_HARDWARE_INSTRUMENT_PLUGIN_PATH}"
                "${JAM2_HARDWARE_DEVICE_ID}"
                "${JAM2_HARDWARE_INPUT_CHANNEL}"
                "${JAM2_HARDWARE_BUFFER_FRAMES}"
                "${JAM2_HARDWARE_MIDI_DEVICE_NAME}")
        set_tests_properties(jam2_hardware_midi_instrument_device PROPERTIES
            LABELS "hardware;plugin;audio-device;midi;instrument;interactive"
            RUN_SERIAL TRUE
            TIMEOUT 600
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        if(WIN32)
            set_tests_properties(jam2_hardware_midi_instrument_device PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
        endif()
    endif()

    if(WIN32)
        set(JAM2_HARDWARE_PUBLIC_EXECUTABLE "${JAM2_RELEASE_DIR}/jam2.exe")
    elseif(APPLE)
        set(JAM2_HARDWARE_PUBLIC_EXECUTABLE
            "${JAM2_RELEASE_DIR}/Jam2.app/Contents/MacOS/jam2")
    endif()
    add_test(NAME jam2_hardware_audio_device_capabilities
        COMMAND "${JAM2_HARDWARE_PUBLIC_EXECUTABLE}" test-device
            "${JAM2_HARDWARE_DEVICE_ID}")
    set_tests_properties(jam2_hardware_audio_device_capabilities PROPERTIES
        LABELS "hardware;audio-device;capabilities"
        RUN_SERIAL TRUE
        TIMEOUT 30)
endif()

add_library(jam2_test_process_support STATIC EXCLUDE_FROM_ALL
    support/AutomationProcess.cpp
    support/AutomationProcess.hpp
    support/FourPeerCoordinator.cpp
    support/FourPeerCoordinator.hpp
    support/LoopbackPortReservations.cpp
    support/LoopbackPortReservations.hpp
    support/TestTiming.hpp)
target_compile_features(jam2_test_process_support PUBLIC cxx_std_20)
target_include_directories(jam2_test_process_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(jam2_test_process_support PUBLIC Qt6::Core Qt6::Network)
if(MSVC)
    target_compile_options(jam2_test_process_support PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_test_process_support PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(jam2_four_peer_coordinator_tests EXCLUDE_FROM_ALL
    unit/support/FourPeerCoordinatorTests.cpp)
jam2_configure_test_target(jam2_four_peer_coordinator_tests)
target_link_libraries(jam2_four_peer_coordinator_tests PRIVATE
    jam2_test_process_support Qt6::Core)
add_test(NAME jam2_four_peer_coordinator_units
    COMMAND $<TARGET_FILE:jam2_four_peer_coordinator_tests>)
set_tests_properties(jam2_four_peer_coordinator_units PROPERTIES
    LABELS "unit;support;four-peer;artifacts"
    TIMEOUT 30)
if(WIN32)
    set_tests_properties(jam2_four_peer_coordinator_units PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_library(jam2_test_network_support STATIC EXCLUDE_FROM_ALL
    support/ArtifactReader.cpp
    support/ArtifactReader.hpp
    support/UdpImpairmentProxy.cpp
    support/UdpImpairmentProxy.hpp
    support/UdpSecurityFixtures.cpp
    support/UdpSecurityFixtures.hpp)
target_compile_features(jam2_test_network_support PUBLIC cxx_std_20)
target_include_directories(jam2_test_network_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(jam2_test_network_support PUBLIC jam2_core Qt6::Core Qt6::Network)
if(MSVC)
    target_compile_options(jam2_test_network_support PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(jam2_test_network_support PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(jam2_four_gui_agent_smoke EXCLUDE_FROM_ALL
    integration/gui/FourGuiAgentSmoke.cpp)
jam2_configure_test_target(jam2_four_gui_agent_smoke)
target_link_libraries(jam2_four_gui_agent_smoke PRIVATE
    jam2_test_process_support Qt6::Core)
add_executable(jam2_four_gui_modal_integration EXCLUDE_FROM_ALL
    integration/gui/FourGuiModalIntegration.cpp)
jam2_configure_test_target(jam2_four_gui_modal_integration)
target_link_libraries(jam2_four_gui_modal_integration PRIVATE
    jam2_test_process_support Qt6::Core)
add_executable(jam2_four_jam_sync_integration EXCLUDE_FROM_ALL
    integration/gui/FourJamSyncIntegration.cpp)
jam2_configure_test_target(jam2_four_jam_sync_integration)
target_link_libraries(jam2_four_jam_sync_integration PRIVATE
    jam2_test_process_support Qt6::Core Qt6::Network)
add_test(NAME jam2_four_jam_sync_integration
    COMMAND $<TARGET_FILE:jam2_four_jam_sync_integration>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_four_jam_sync_integration PROPERTIES
    LABELS "integration;gui;four-peer;jam-sync"
    TIMEOUT 180)
if(WIN32)
    set_tests_properties(jam2_four_jam_sync_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_four_shared_content_integration EXCLUDE_FROM_ALL
    integration/gui/FourSharedContentIntegration.cpp)
jam2_configure_test_target(jam2_four_shared_content_integration)
target_link_libraries(jam2_four_shared_content_integration PRIVATE
    jam2_test_process_support Qt6::Core Qt6::Network)
add_test(NAME jam2_four_shared_content_integration
    COMMAND $<TARGET_FILE:jam2_four_shared_content_integration>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_four_shared_content_integration PROPERTIES
    LABELS "integration;gui;four-peer;shared-content"
    TIMEOUT 180)
if(WIN32)
    set_tests_properties(jam2_four_shared_content_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_four_wav_interruption_integration EXCLUDE_FROM_ALL
    integration/gui/FourWavInterruptionIntegration.cpp)
jam2_configure_test_target(jam2_four_wav_interruption_integration)
target_link_libraries(jam2_four_wav_interruption_integration PRIVATE
    jam2_test_process_support Qt6::Core Qt6::Network)
add_test(NAME jam2_four_wav_interruption_integration
    COMMAND $<TARGET_FILE:jam2_four_wav_interruption_integration>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_four_wav_interruption_integration PROPERTIES
    LABELS "integration;gui;four-peer;shared-content;interruption"
    TIMEOUT 300)
if(WIN32)
    set_tests_properties(jam2_four_wav_interruption_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_four_performance_integration EXCLUDE_FROM_ALL
    integration/gui/FourPerformanceIntegration.cpp)
jam2_configure_test_target(jam2_four_performance_integration)
target_link_libraries(jam2_four_performance_integration PRIVATE
    jam2_test_process_support Qt6::Core Qt6::Network)
add_test(NAME jam2_four_performance_integration
    COMMAND $<TARGET_FILE:jam2_four_performance_integration>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_four_performance_integration PROPERTIES
    LABELS "integration;gui;four-peer;performance;fake-audio"
    TIMEOUT 180)
if(WIN32)
    set_tests_properties(jam2_four_performance_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
add_test(NAME jam2_four_gui_agent_integration
    COMMAND $<TARGET_FILE:jam2_four_gui_agent_smoke>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_four_gui_agent_integration PROPERTIES
    LABELS "integration;gui;four-peer;automation"
    TIMEOUT 120)
if(WIN32)
    set_tests_properties(jam2_four_gui_agent_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
add_test(NAME jam2_four_gui_modal_integration
    COMMAND $<TARGET_FILE:jam2_four_gui_modal_integration>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_four_gui_modal_integration PROPERTIES
    LABELS "integration;gui;four-peer;automation;modal"
    TIMEOUT 180)
if(WIN32)
    set_tests_properties(jam2_four_gui_modal_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
add_test(NAME jam2_four_session_dialog_integration
    COMMAND $<TARGET_FILE:jam2_four_gui_modal_integration>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}" startup-only)
set_tests_properties(jam2_four_session_dialog_integration PROPERTIES
    LABELS "integration;gui;four-peer;automation;modal;session-startup"
    TIMEOUT 180)
if(WIN32)
    set_tests_properties(jam2_four_session_dialog_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_four_metronome_impairment EXCLUDE_FROM_ALL
    integration/network/FourMetronomeImpairmentIntegration.cpp)
jam2_configure_test_target(jam2_four_metronome_impairment)
target_link_libraries(jam2_four_metronome_impairment PRIVATE
    jam2_test_network_support jam2_test_process_support Qt6::Core Qt6::Network)

add_executable(jam2_four_session_command_integration EXCLUDE_FROM_ALL
    integration/network/FourSessionCommandIntegration.cpp)
jam2_configure_test_target(jam2_four_session_command_integration)
target_link_libraries(jam2_four_session_command_integration PRIVATE
    jam2_core jam2_test_network_support jam2_test_process_support Qt6::Core Qt6::Network)
add_test(NAME jam2_four_session_command_integration
    COMMAND $<TARGET_FILE:jam2_four_session_command_integration>
        "${JAM2_TEST_PUBLIC_EXECUTABLE}")
set_tests_properties(jam2_four_session_command_integration PROPERTIES
    LABELS "integration;four-peer;network;automation;performance;metronome;epoch;transport;fake-audio"
    TIMEOUT 45)
if(WIN32)
    set_tests_properties(jam2_four_session_command_integration PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_executable(jam2_native_validation_integration EXCLUDE_FROM_ALL
    integration/network/NativeValidationIntegration.cpp)
jam2_configure_test_target(jam2_native_validation_integration)
target_link_libraries(jam2_native_validation_integration PRIVATE Qt6::Core)
target_include_directories(jam2_native_validation_integration PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
foreach(operation IN ITEMS boundaries controller-lifecycle)
    if(operation STREQUAL "boundaries")
        set(native_operation "validate.boundaries")
    else()
        set(native_operation "validate.controller-lifecycle")
    endif()
    set(test_name "jam2_native_${operation}_validation")
    add_test(NAME "${test_name}"
        COMMAND $<TARGET_FILE:jam2_native_validation_integration>
            "${JAM2_TEST_PUBLIC_EXECUTABLE}" "${native_operation}")
    set_tests_properties("${test_name}" PROPERTIES
        LABELS "integration;network;security;validation;capacity-heavy"
        TIMEOUT 150)
    if(WIN32)
        set_tests_properties("${test_name}" PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endforeach()

set(JAM2_METRONOME_TEST_MODES
    shared-grid
    leader-audio
    listener-compensated)
set(JAM2_METRONOME_TEST_CONDITIONS
    clean
    delay
    jitter
    loss
    duplication
    reordering
    burst-loss)
foreach(mode IN LISTS JAM2_METRONOME_TEST_MODES)
    foreach(condition IN LISTS JAM2_METRONOME_TEST_CONDITIONS)
        string(REPLACE "-" "_" mode_id "${mode}")
        string(REPLACE "-" "_" condition_id "${condition}")
        set(test_name "jam2_metronome_${mode_id}_${condition_id}")
        add_test(NAME "${test_name}"
            COMMAND $<TARGET_FILE:jam2_four_metronome_impairment>
                "${JAM2_TEST_PUBLIC_EXECUTABLE}" "${mode}" "${condition}")
        set_tests_properties("${test_name}" PROPERTIES
            LABELS "integration;four-peer;performance;metronome;epoch;network-impairment"
            TIMEOUT 150)
        if(WIN32)
            set_tests_properties("${test_name}" PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endforeach()
endforeach()

foreach(security_condition IN ITEMS security sequence-security)
    string(REPLACE "-" "_" security_id "${security_condition}")
    set(test_name "jam2_four_udp_${security_id}")
    add_test(NAME "${test_name}"
        COMMAND $<TARGET_FILE:jam2_four_metronome_impairment>
            "${JAM2_TEST_PUBLIC_EXECUTABLE}" "shared-grid" "${security_condition}")
    set_tests_properties("${test_name}" PROPERTIES
        LABELS "integration;four-peer;network;security;udp"
        TIMEOUT 150)
    if(WIN32)
        set_tests_properties("${test_name}" PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endforeach()

add_custom_target(jam2_tests_unit
    DEPENDS
        jam2
        jam2_automation_contract_tests
        jam2_jam_sync_policy_tests
        jam2_midi_input_backend_tests
        jam2_input_plugin_backend_tests
        jam2_four_peer_coordinator_tests
        jam2_worker_entrypoint_tests
        jam2_looper_project_tests
        jam2_prepared_mix_lifecycle_tests
        jam2_project_persistence_tests
        jam2_workspace_state_tests
        jam2_gui_loopback_recorder_tests
        jam2_track_workspace_support_tests
        jam2_track_recording_workflow_tests
        jam2_metronome_transport_controller_tests
        jam2_shared_bank_launch_tests
        jam2_asset_transfer_tests
        jam2_core_input_tests
        jam2_core_boundary_tests
        jam2_pcm16_wav_tests
        jam2_output_recorder_tests
        jam2_audio_device_processing_tests
        jam2_transport_timing_tests
        jam2_cli_boundary_tests
        jam2_application_boundary_tests
        jam2_debug_diagnostic_tests
        jam2_gui_model_boundary_tests
        jam2_gui_widget_boundary_tests
        jam2_practice_idea_boundary_tests
        jam2_user_preferences_tests
        jam2_detail_section_edit_tests
        jam2_plugin_bridge_tests
        jamtaster_tests
        jam2_jamtaster_model_tests
        jam2_jamtaster_demucs_tests
        jam2_jamtaster_pipeline_tests
        jam2_jamtaster_worker_protocol_tests
        jam2_jamtaster_service_tests
        jam2_jamtaster_dialog_tests
        jam2_jamtaster_project_support_tests)

add_custom_target(jam2_tests_plugin
    DEPENDS
        jam2
        jam2_input_plugin_backend_tests
        jam2_plugin_bridge_tests
        jam2_worker_entrypoint_tests)

add_custom_target(jam2_tests_hardware
    DEPENDS
        jam2
        jam2_plugin_device_tests
        jam2_midi_instrument_device_tests)

add_custom_target(jam2_tests_gui
    DEPENDS
        jam2
        jam2_midi_input_backend_tests
        jam2_input_plugin_backend_tests
        jam2_looper_project_tests
        jam2_prepared_mix_lifecycle_tests
        jam2_project_persistence_tests
        jam2_workspace_state_tests
        jam2_gui_loopback_recorder_tests
        jam2_track_workspace_support_tests
        jam2_track_recording_workflow_tests
        jam2_metronome_transport_controller_tests
        jam2_shared_bank_launch_tests
        jam2_asset_transfer_tests
        jam2_debug_diagnostic_tests
        jam2_gui_model_boundary_tests
        jam2_gui_widget_boundary_tests
        jam2_practice_idea_boundary_tests
        jam2_user_preferences_tests
        jam2_detail_section_edit_tests
        jam2_jamtaster_dialog_tests
        jam2_jamtaster_project_support_tests
        jam2_four_gui_agent_smoke
        jam2_four_gui_modal_integration
        jam2_four_jam_sync_integration
        jam2_four_shared_content_integration
        jam2_four_wav_interruption_integration
        jam2_four_performance_integration)

add_custom_target(jam2_tests_performance
    DEPENDS
        jam2
        jam2_core_input_tests
        jam2_audio_device_processing_tests
        jam2_transport_timing_tests
        jam2_gui_widget_boundary_tests
        jam2_gui_loopback_recorder_tests
        jam2_track_workspace_support_tests
        jam2_track_recording_workflow_tests
        jam2_metronome_transport_controller_tests
        jam2_prepared_mix_lifecycle_tests
        jam2_shared_bank_launch_tests
        jam2_four_performance_integration
        jam2_four_session_command_integration
        jam2_four_metronome_impairment)

add_custom_target(jam2_tests_jam_sync
    DEPENDS
        jam2
        jam2_jam_sync_policy_tests
        jam2_four_jam_sync_integration)

add_custom_target(jam2_tests_shared_content
    DEPENDS
        jam2
        jam2_looper_project_tests
        jam2_prepared_mix_lifecycle_tests
        jam2_project_persistence_tests
        jam2_workspace_state_tests
        jam2_track_workspace_support_tests
        jam2_asset_transfer_tests
        jam2_four_shared_content_integration
        jam2_four_wav_interruption_integration)

add_custom_target(jam2_tests_network
    DEPENDS
        jam2
        jam2_core_boundary_tests
        jam2_application_boundary_tests
        jam2_debug_diagnostic_tests
        jam2_shared_bank_launch_tests
        jam2_four_session_command_integration
        jam2_four_metronome_impairment
        jam2_native_validation_integration)

add_custom_target(jam2_tests_all
    DEPENDS
        jam2_tests_unit
        jam2_tests_gui
        jam2_tests_performance
        jam2_tests_network)
if(JAM2_HARDWARE_PROFILE)
    add_dependencies(jam2_tests_all jam2_tests_hardware)
endif()

# Keep every automated test's temporary state inside a private directory below
# the build tree. Per-test namespaces prevent parallel CTests from colliding on
# fixed temporary names while still letting a test's Jam2 children share their
# coordinator-owned workspace. Individual fixtures remove successful data; the
# build scripts remove the complete root after the requested gate succeeds and
# retain it after failure.
set(JAM2_TEST_ARTIFACT_ROOT "${CMAKE_BINARY_DIR}/test-artifacts")
file(MAKE_DIRECTORY "${JAM2_TEST_ARTIFACT_ROOT}")
get_property(JAM2_ALL_TESTS DIRECTORY PROPERTY TESTS)
set(JAM2_RESET_TEST_ARTIFACT_SCRIPT
    "file(REMOVE_RECURSE [==[${JAM2_TEST_ARTIFACT_ROOT}]==])\n")
set(JAM2_TEST_ARTIFACT_INDEX
    "# CTest name to private artifact directory\n")
foreach(test_name IN LISTS JAM2_ALL_TESTS)
    # Keep the private namespace short. JamTaster's maintained output layout
    # includes a source hash and converted-song assets; repeating a descriptive
    # CTest name here can exceed Win32's legacy path limit even though the test
    # and product paths are otherwise valid. The index retains the readable
    # mapping needed to inspect a failed parallel run.
    string(MD5 test_artifact_hash "${test_name}")
    string(SUBSTRING "${test_artifact_hash}" 0 8 test_artifact_key)
    set(test_artifact_root "${JAM2_TEST_ARTIFACT_ROOT}/t-${test_artifact_key}")
    file(MAKE_DIRECTORY "${test_artifact_root}")
    string(APPEND JAM2_TEST_ARTIFACT_INDEX
        "${test_name}=t-${test_artifact_key}\n")
    string(APPEND JAM2_RESET_TEST_ARTIFACT_SCRIPT
        "file(MAKE_DIRECTORY [==[${test_artifact_root}]==])\n")
    set_property(TEST "${test_name}" APPEND PROPERTY
        ENVIRONMENT_MODIFICATION
            "JAM2_TEST_ARTIFACT_ROOT=set:${test_artifact_root}"
            "TEMP=set:${test_artifact_root}"
            "TMP=set:${test_artifact_root}"
            "TMPDIR=set:${test_artifact_root}")
endforeach()
file(GENERATE
    OUTPUT "${JAM2_TEST_ARTIFACT_ROOT}/index.txt"
    CONTENT "${JAM2_TEST_ARTIFACT_INDEX}")
set(JAM2_RESET_TEST_ARTIFACT_SCRIPT_PATH
    "${CMAKE_BINARY_DIR}/ResetTestArtifactWorkspace.cmake")
file(GENERATE
    OUTPUT "${JAM2_RESET_TEST_ARTIFACT_SCRIPT_PATH}"
    CONTENT "${JAM2_RESET_TEST_ARTIFACT_SCRIPT}")

# CTest TIMEOUT is only a final deadman for a wedged process. Product behavior
# is judged by events, state, frames and artifacts inside each test, never by
# finishing before this ceiling. Use one platform-neutral minimum per workload
# class so a contended or older host is not a product failure.
cmake_host_system_information(RESULT JAM2_TEST_LOGICAL_CORES
    QUERY NUMBER_OF_LOGICAL_CORES)
if(NOT JAM2_TEST_LOGICAL_CORES MATCHES "^[1-9][0-9]*$")
    set(JAM2_TEST_LOGICAL_CORES 1)
endif()
if(JAM2_TEST_LOGICAL_CORES LESS_EQUAL 2)
    set(JAM2_HEAVY_TEST_PROCESSORS 1)
elseif(JAM2_TEST_LOGICAL_CORES GREATER 5)
    set(JAM2_HEAVY_TEST_PROCESSORS 4)
else()
    math(EXPR JAM2_HEAVY_TEST_PROCESSORS "${JAM2_TEST_LOGICAL_CORES} - 1")
endif()

foreach(test_name IN LISTS JAM2_ALL_TESTS)
    get_property(test_labels TEST "${test_name}" PROPERTY LABELS)
    list(FIND test_labels "four-peer" four_peer_label)
    list(FIND test_labels "jamtaster" jamtaster_label)
    list(FIND test_labels "capacity-heavy" capacity_heavy_label)
    get_property(base_timeout TEST "${test_name}" PROPERTY TIMEOUT)
    if(four_peer_label GREATER -1)
        set_tests_properties("${test_name}" PROPERTIES
            PROCESSORS "${JAM2_HEAVY_TEST_PROCESSORS}")
        if(NOT base_timeout OR base_timeout LESS 900)
            set_tests_properties("${test_name}" PROPERTIES TIMEOUT 900)
        endif()
    elseif(jamtaster_label GREATER -1)
        # ONNX/model and helper-process cases can consume several cores and
        # substantial memory. Account for them as one heavy workload instead
        # of scheduling every JamTaster case at once on a small host.
        set_tests_properties("${test_name}" PROPERTIES
            PROCESSORS "${JAM2_HEAVY_TEST_PROCESSORS}")
        if(NOT base_timeout OR base_timeout LESS 600)
            set_tests_properties("${test_name}" PROPERTIES TIMEOUT 600)
        endif()
    elseif(capacity_heavy_label GREATER -1)
        set_tests_properties("${test_name}" PROPERTIES
            PROCESSORS "${JAM2_HEAVY_TEST_PROCESSORS}")
        if(NOT base_timeout OR base_timeout LESS 300)
            set_tests_properties("${test_name}" PROPERTIES TIMEOUT 300)
        endif()
    elseif(NOT base_timeout OR base_timeout LESS 300)
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 300)
    endif()
endforeach()

if(JAM2_ENABLE_COVERAGE)
    # Coverage instrumentation and an unoptimized app are intentionally slower
    # than the distribution build. Keep the behavioral deadlines inside each
    # test unchanged, but allow the test processes enough wall-clock time to
    # report their own precise failure instead of being killed by CTest first.
    foreach(test_name IN LISTS JAM2_ALL_TESTS)
        get_property(test_timeout TEST "${test_name}" PROPERTY TIMEOUT)
        if(test_timeout)
            math(EXPR coverage_timeout "${test_timeout} * 3")
            set_tests_properties("${test_name}" PROPERTIES
                TIMEOUT "${coverage_timeout}")
        endif()
    endforeach()
endif()
