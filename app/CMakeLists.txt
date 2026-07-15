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

set(CMAKE_AUTOMOC ON)

set(JAM2_GUI_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/platform/assets/jam2.ico")
set(JAM2_GUI_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/platform/assets/jam2.icns")

if(WIN32)
    enable_language(RC)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/Jam2.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Jam2.rc"
        @ONLY
    )
endif()

# The unified Windows binary uses the console subsystem so shells wait for
# command-line invocations. The no-argument GUI path detaches immediately.
add_executable(jam2 MACOSX_BUNDLE
    gui/AssetTransferService.cpp
    gui/AssetTransferService.hpp
    application/ApplicationEntrypoint.cpp
    application/ApplicationEntrypoint.hpp
    application/ApplicationRuntime.cpp
    application/ApplicationRuntime.hpp
    application/AutomationChannel.cpp
    application/AutomationChannel.hpp
    application/ControlClient.cpp
    application/ControlClient.hpp
    application/ContentLimits.hpp
    application/ControlMessageValidation.cpp
    application/ControlMessageValidation.hpp
    application/ControllerLifecycleValidation.cpp
    application/ControllerLifecycleValidation.hpp
    application/DebugActionValidation.cpp
    application/DebugActionValidation.hpp
    application/DebugAutomation.cpp
    application/DebugAutomation.hpp
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
    cli/CliDispatch.cpp
    cli/CliRuntime.hpp
    cli/CliStats.cpp
    cli/CliStats.hpp
    cli/CliEntrypoint.hpp
    cli/NetworkRuntime.cpp
    gui/BeatGridModel.cpp
    gui/BeatGridModel.hpp
    gui/BeatGridWidget.cpp
    gui/BeatGridWidget.hpp
    gui/GuiLoopbackRecorder.cpp
    gui/GuiLoopbackRecorder.hpp
    gui/GuiControlMessageRouter.cpp
    gui/GuiControlMessageRouter.hpp
    gui/GuiPresentation.cpp
    gui/GuiPresentation.hpp
    gui/LooperProject.cpp
    gui/LooperProject.hpp
    gui/MainWindow.cpp
    gui/MainWindow.hpp
    gui/MainWindowPages.cpp
    gui/MainWindowPages.hpp
    gui/MetronomeTransportController.cpp
    gui/MetronomeTransportController.hpp
    gui/MixerStatsViewModel.cpp
    gui/MixerStatsViewModel.hpp
    gui/PlaybackGrid.cpp
    gui/PlaybackGrid.hpp
    gui/PreparedMixRenderer.cpp
    gui/PreparedMixRenderer.hpp
    gui/ProjectPersistenceCoordinator.cpp
    gui/ProjectPersistenceCoordinator.hpp
    gui/SharedTrackController.cpp
    gui/SharedTrackController.hpp
    gui/SharedTrackModel.hpp
    gui/TrackWidgets.hpp
    gui/TrackRecordingWorkflow.cpp
    gui/TrackRecordingWorkflow.hpp
    main.cpp
)

if(WIN32)
    set_target_properties(jam2 PROPERTIES WIN32_EXECUTABLE FALSE)
    target_sources(jam2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Jam2.rc")
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

jam2_set_app_output(jam2)

target_include_directories(jam2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/application"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/../libs/third_party"
)

target_link_libraries(jam2 PRIVATE
    jam2_core
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
