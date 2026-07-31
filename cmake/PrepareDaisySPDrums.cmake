function(jam2_prepare_daisysp_synthbassdrum daisysp_root output_variable)
    set(source
        "${daisysp_root}/Source/Drums/synthbassdrum.cpp")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR
            "The pinned DaisySP synthetic bass drum source is missing: "
            "${source}")
    endif()

    # DaisySP commit 599511b740f8f3a9b8db72a0642aa45b8a23c3a3 leaves
    # transient_env_ and transient_env_lp_ indeterminate in Init(). The voice
    # then reads transient_env_lp_ on its first triggered sample, making an
    # otherwise deterministic offline drum render depend on heap contents.
    #
    # Keep the pinned vendored source untouched and generate one narrowly
    # corrected translation unit. The exact anchor makes a future source
    # update fail configuration instead of applying this correction to
    # unknown code.
    file(READ "${source}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    set(anchor
"    body_env_lp_          = 0.0f;
    body_env_             = 0.0f;
    body_env_pulse_width_ = 0;")
    set(replacement
"    body_env_lp_          = 0.0f;
    body_env_             = 0.0f;
    transient_env_        = 0.0f;
    transient_env_lp_     = 0.0f;
    body_env_pulse_width_ = 0;")
    string(FIND "${contents}" "${anchor}" anchor_position)
    if(anchor_position EQUAL -1)
        message(FATAL_ERROR
            "The pinned DaisySP SyntheticBassDrum initialization no longer "
            "matches Jam2's reviewed deterministic-state correction.")
    endif()
    string(REPLACE "${anchor}" "${replacement}" contents "${contents}")
    string(REPLACE
        "#include \"synthbassdrum.h\""
        "#include \"Drums/synthbassdrum.h\""
        contents "${contents}")

    set(output_directory
        "${CMAKE_CURRENT_BINARY_DIR}/generated/daisysp")
    set(output
        "${output_directory}/synthbassdrum.cpp")
    file(MAKE_DIRECTORY "${output_directory}")
    file(WRITE "${output}"
        "// Generated from vendored DaisySP with Jam2's reviewed state-init fix.\n"
        "${contents}")
    set_property(
        DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()
