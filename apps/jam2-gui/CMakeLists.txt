if(WIN32)
    file(GLOB JAM2_QT_PREFIX_HINTS
        "C:/Qt/6.*/msvc*_64"
        "C:/Qt/6.*/mingw*_64"
    )
    list(APPEND CMAKE_PREFIX_PATH ${JAM2_QT_PREFIX_HINTS})
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Network Multimedia)

if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "jam2-gui requires Qt 6 Widgets, Network, and Multimedia development packages.\n"
        "Install Qt 6, then reconfigure with a Qt prefix, for example:\n"
        "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJAM2_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64\n"
        "If your Qt version/toolchain folder differs, point CMAKE_PREFIX_PATH at that folder."
    )
endif()

set(CMAKE_AUTOMOC ON)

add_executable(jam2-gui WIN32 MACOSX_BUNDLE
    BeatGridModel.cpp
    BeatGridModel.hpp
    BeatGridWidget.cpp
    BeatGridWidget.hpp
    ControlClient.cpp
    ControlClient.hpp
    ControlServer.cpp
    ControlServer.hpp
    Jam2Process.cpp
    Jam2Process.hpp
    MainWindow.cpp
    MainWindow.hpp
    SessionController.cpp
    SessionController.hpp
    SharedTrackController.cpp
    SharedTrackController.hpp
    SharedTrackModel.hpp
    main.cpp
)

if(APPLE)
    enable_language(OBJCXX)
    target_sources(jam2-gui PRIVATE
        Jam2MacPermissions.hpp
        Jam2MacPermissions.mm
    )
    set_target_properties(jam2-gui PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Jam2"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.jam2.app"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    add_dependencies(jam2-gui jam2)
    add_custom_command(TARGET jam2-gui POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jam2>"
            "$<TARGET_BUNDLE_DIR:jam2-gui>/Contents/MacOS/$<TARGET_FILE_NAME:jam2>"
        VERBATIM
    )
    if(TARGET jam2-capture)
        add_dependencies(jam2-gui jam2-capture)
        add_custom_command(TARGET jam2-gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:jam2-capture>"
                "$<TARGET_BUNDLE_DIR:jam2-gui>/Contents/MacOS/$<TARGET_FILE_NAME:jam2-capture>"
            VERBATIM
        )
    endif()
endif()

jam2_set_app_output(jam2-gui)

target_include_directories(jam2-gui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party"
)

target_link_libraries(jam2-gui PRIVATE
    jam2_core
    Qt6::Multimedia
    Qt6::Network
    Qt6::Widgets
)

if(APPLE)
    target_link_libraries(jam2-gui PRIVATE
        "-framework AVFoundation"
        "-framework Foundation"
    )
endif()

if(WIN32)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    execute_process(
        COMMAND "${QT_QMAKE_EXECUTABLE}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE QT_PLUGINS_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    add_custom_command(TARGET jam2-gui POST_BUILD
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
            "$<TARGET_FILE:Qt6::Multimedia>"
            "${JAM2_RELEASE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${JAM2_RELEASE_DIR}/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${QT_PLUGINS_DIR}/platforms/qwindows.dll"
            "${JAM2_RELEASE_DIR}/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/qt.conf"
            "${JAM2_RELEASE_DIR}/qt.conf"
        COMMENT "Staging minimal Qt runtime files for jam2-gui"
        VERBATIM
    )
    if(EXISTS "${QT_PLUGINS_DIR}/multimedia")
        add_custom_command(TARGET jam2-gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${QT_PLUGINS_DIR}/multimedia"
                "${JAM2_RELEASE_DIR}/multimedia"
            COMMENT "Staging Qt multimedia plugins for jam2-gui"
            VERBATIM
        )
    endif()
endif()
