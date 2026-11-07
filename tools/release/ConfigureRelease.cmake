# Included from app/ after its targets and staging rules exist.
set(JAM2_PACKAGE_APP_FILES
    LICENSE THIRD_PARTY_NOTICES.md
    licenses/aubio-COPYING
    licenses/signalsmith-stretch-LICENSE.txt
    licenses/signalsmith-linear-LICENSE.txt
    licenses/DaisySP-LICENSE.txt
    licenses/VST3-SDK-LICENSE.txt)
list(APPEND JAM2_PACKAGE_APP_FILES licenses/miniz-LICENSE.txt)
if(WIN32)
    set(JAM2_PACKAGE_PLATFORM windows)
    set(JAM2_PACKAGE_ARCHITECTURES "x86_64")
    list(APPEND JAM2_PACKAGE_APP_FILES
        jam2.exe Qt6Core.dll Qt6Gui.dll Qt6Network.dll Qt6Widgets.dll qt.conf
        platforms/qwindows.dll tls/qschannelbackend.dll
        components/jamtaster/jamtaster-worker.exe
        components/jamtaster/onnxruntime.dll
        components/pluginhost/jam2-plugin-worker.exe
        components/pluginhost/licenses/VST3-SDK-LICENSE.txt)
    list(APPEND JAM2_PACKAGE_APP_FILES components/updater/jam2-updater.exe)
    foreach(notice IN ITEMS onnxruntime-LICENSE.txt onnxruntime-ThirdPartyNotices.txt
            eigen-COPYING.MPL2.txt demucs-onnx-LICENSE.txt MODEL_NOTICES.md)
        list(APPEND JAM2_PACKAGE_APP_FILES "components/jamtaster/licenses/${notice}")
    endforeach()
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    # A loaded ASIO driver shares our runtime and may use a newer MSVC STL
    # than Jam2's compiler. Never pin the process to the old toolset's DLLs.
    list(GET CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS 0 toolset_runtime)
    get_filename_component(toolset_runtime_dir "${toolset_runtime}" DIRECTORY)
    set_property(TARGET jam2 PROPERTY JAM2_COMPILER_RUNTIME_DIRECTORY "${toolset_runtime_dir}")
    set(runtime_minimum "14.40.0.0")
    if(CMAKE_CXX_COMPILER_VERSION MATCHES "^19\\.([0-9]+)")
        if("14.${CMAKE_MATCH_1}.0.0" VERSION_GREATER runtime_minimum)
            set(runtime_minimum "14.${CMAKE_MATCH_1}.0.0")
        endif()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/SelectWindowsRuntime.ps1")
    execute_process(COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_LIST_DIR}/SelectWindowsRuntime.ps1"
        -ToolsetDirectory "${toolset_runtime_dir}" -MinimumVersion "${runtime_minimum}"
        OUTPUT_VARIABLE runtime_json ERROR_VARIABLE runtime_error RESULT_VARIABLE runtime_result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT runtime_result EQUAL 0)
        message(FATAL_ERROR "Could not select compatible Windows runtime: ${runtime_error}")
    endif()
    string(JSON runtime_count LENGTH "${runtime_json}")
    math(EXPR runtime_last "${runtime_count} - 1")
    set(JAM2_PACKAGE_RUNTIME_SOURCES "")
    foreach(index RANGE 0 ${runtime_last})
        string(JSON runtime GET "${runtime_json}" ${index})
        file(TO_CMAKE_PATH "${runtime}" runtime)
        list(APPEND JAM2_PACKAGE_RUNTIME_SOURCES "${runtime}")
    endforeach()
    # Refresh on ordinary builds too, including when jam2 does not relink.
    add_custom_target(jam2_windows_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${JAM2_RELEASE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${JAM2_PACKAGE_RUNTIME_SOURCES} "${JAM2_RELEASE_DIR}"
        COMMENT "Staging compatible x64 Microsoft C++ runtime" VERBATIM)
    add_dependencies(jam2 jam2_windows_runtime)
    foreach(runtime IN LISTS JAM2_PACKAGE_RUNTIME_SOURCES)
        get_filename_component(name "${runtime}" NAME)
        list(APPEND JAM2_PACKAGE_APP_FILES "${name}")
    endforeach()
    set(JAM2_PACKAGE_TLS_SOURCE "${QT_PLUGINS_DIR}/tls/qschannelbackend.dll")
elseif(APPLE)
    set(JAM2_PACKAGE_PLATFORM macos)
    set(JAM2_PACKAGE_ARCHITECTURES "arm64;x86_64")
    list(APPEND JAM2_PACKAGE_APP_FILES Jam2.app)
    get_target_property(qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(JAM2_PACKAGE_QT_BIN "${qmake}" DIRECTORY)
else()
    return()
endif()

set(JAM2_PACKAGE_MODEL_NAMES "")
foreach(model IN LISTS JAMTASTER_MODEL_FILES)
    if(model MATCHES "\\.onnx$")
        list(APPEND JAM2_PACKAGE_MODEL_NAMES "${model}")
    endif()
endforeach()
list(SORT JAM2_PACKAGE_MODEL_NAMES)
include("${CMAKE_SOURCE_DIR}/tools/release/ModelManifest.cmake")
foreach(model IN LISTS JAM2_PACKAGE_MODEL_NAMES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/app/jamtaster/models/${model}.json")
endforeach()
jam2_write_model_manifest("${CMAKE_SOURCE_DIR}" "${JAM2_PACKAGE_MODEL_NAMES}"
    "${JAM2_RELEASE_DIR}/models.json")
list(REMOVE_DUPLICATES JAM2_PACKAGE_APP_FILES)
configure_file("${CMAKE_SOURCE_DIR}/tools/release/release-package.cmake.in"
    "${CMAKE_BINARY_DIR}/release-package.cmake" @ONLY)
