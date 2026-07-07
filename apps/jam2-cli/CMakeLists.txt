add_executable(jam2
    main.cpp
)

jam2_set_app_output(jam2)

target_link_libraries(jam2 PRIVATE jam2_core)

if(WIN32)
    target_link_libraries(jam2 PRIVATE Avrt Winmm)
endif()
