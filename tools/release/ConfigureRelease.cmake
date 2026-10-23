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
    set(JAM2_PACKAGE_RUNTIME_SOURCES "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
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
