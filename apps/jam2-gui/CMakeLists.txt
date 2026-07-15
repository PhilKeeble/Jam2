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
        "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJAM2_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64\n"
        "If your Qt version/toolchain folder differs, point CMAKE_PREFIX_PATH at that folder."
    )
endif()

set(CMAKE_AUTOMOC ON)

set(JAM2_GUI_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/jam2.ico")
set(JAM2_GUI_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/jam2.icns")

if(WIN32)
    enable_language(RC)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/Jam2.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Jam2.rc"
        @ONLY
    )
endif()

# The unified Windows binary uses the console subsystem so shells wait for
# command-line invocations. The no-argument GUI path detaches immediately.
add_executable(jam2 MACOSX_BUNDLE
    ApplicationRuntime.cpp
    ApplicationRuntime.hpp
    BeatGridModel.cpp
    BeatGridModel.hpp
    BeatGridWidget.cpp
    BeatGridWidget.hpp
    ControlClient.cpp
    ControlClient.hpp
    ControlProtocol.cpp
    ControlProtocol.hpp
    ControlServer.cpp
    ControlServer.hpp
    ControllerLifecycleValidation.cpp
    ControllerLifecycleValidation.hpp
    GuiLoopbackRecorder.cpp
    GuiLoopbackRecorder.hpp
    LooperProject.cpp
    LooperProject.hpp
    MainWindow.cpp
    MainWindow.hpp
    PlaybackGrid.cpp
    PlaybackGrid.hpp
    PreparedMixRenderer.cpp
    PreparedMixRenderer.hpp
    SessionController.cpp
    SessionController.hpp
    SharedSessionController.cpp
    SharedSessionController.hpp
    SharedTrackController.cpp
    SharedTrackController.hpp
    SharedTrackModel.hpp
    main.cpp
    ../jam2-cli/main.cpp
    ../jam2-cli/cli_entry.hpp
    ../jam2-cli/runtime_entry.hpp
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
        Jam2MacPermissions.hpp
        Jam2MacPermissions.mm
        "${JAM2_GUI_MACOS_ICON}"
    )
    set_target_properties(jam2 PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Jam2"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.jam2.app"
        MACOSX_BUNDLE_ICON_FILE "jam2.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

jam2_set_app_output(jam2)

target_include_directories(jam2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/../jam2-cli"
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
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${JAM2_RELEASE_DIR}/jam2-gui.exe"
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
            "${CMAKE_CURRENT_SOURCE_DIR}/qt.conf"
            "${JAM2_RELEASE_DIR}/qt.conf"
        COMMENT "Staging minimal Qt runtime files for unified jam2"
        VERBATIM
    )
endif()
