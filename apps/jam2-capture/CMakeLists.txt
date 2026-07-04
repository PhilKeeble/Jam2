add_executable(jam2-capture
    main.cpp
)

if(APPLE)
    enable_language(OBJCXX)
    target_sources(jam2-capture PRIVATE
        loopback_capture_macos.hpp
        loopback_capture_macos.mm
    )
endif()

jam2_set_app_output(jam2-capture)

target_link_libraries(jam2-capture PRIVATE jam2_core)

if(WIN32)
    target_link_libraries(jam2-capture PRIVATE propsys)
elseif(APPLE)
    target_link_libraries(jam2-capture PRIVATE
        "-framework Foundation"
        "-framework CoreAudio"
    )
endif()
