if(WIN32)
    file(GLOB JAM2_QT_PREFIX_HINTS
        "C:/Qt/6.*/msvc*_64"
        "C:/Qt/6.*/mingw*_64"
    )
    list(APPEND CMAKE_PREFIX_PATH ${JAM2_QT_PREFIX_HINTS})
endif()

find_package(Qt6 QUIET COMPONENTS Widgets Network Multimedia)

if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "jam2-gui requires Qt 6 Widgets, Network, and Multimedia development packages.\n"
        "Install Qt 6, then reconfigure with a Qt prefix, for example:\n"
        "  cmake -S . -B build -G Ninja -DJAM2_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64\n"
        "If your Qt version/toolchain folder differs, point CMAKE_PREFIX_PATH at that folder."
    )
endif()

set(CMAKE_AUTOMOC ON)

add_executable(jam2-gui
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
    LeadCueModel.hpp
    MainWindow.cpp
    MainWindow.hpp
    SessionController.cpp
    SessionController.hpp
    SharedTrackController.cpp
    SharedTrackController.hpp
    SharedTrackModel.hpp
    main.cpp
)

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

if(WIN32)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET jam2-gui POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                "$<TARGET_FILE:jam2-gui>"
            COMMENT "Deploying Qt runtime files for jam2-gui"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt was not found; jam2-gui may need Qt DLLs copied beside the executable.")
    endif()
endif()
