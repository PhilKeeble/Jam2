function(jam2_require_universal_macos binary)
    # -verify_arch consumes all remaining arguments as architecture names.
    # The input file must precede it, including when the path contains spaces.
    execute_process(COMMAND /usr/bin/lipo "${binary}" -verify_arch arm64 x86_64
        RESULT_VARIABLE architecture_result OUTPUT_QUIET ERROR_VARIABLE architecture_error)
    if(NOT architecture_result STREQUAL "0")
        message(FATAL_ERROR
            "Universal Mac releases require both arm64 and x86_64 in ${binary}.\n"
            "For Qt dependencies, select a universal Qt installation with QT_DIR.\n"
            "${architecture_error}")
    endif()
endfunction()
